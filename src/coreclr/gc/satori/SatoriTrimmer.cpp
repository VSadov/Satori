// Copyright (c) 2025 Vladimir Sadov
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// SatoriRecycler.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriUtil.h"
#include "SatoriTrimmer.h"
#include "SatoriAllocator.h"
#include "SatoriHeap.h"
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

SatoriTrimmer::SatoriTrimmer(SatoriHeap* heap)
{
    m_lastGen2Count = 0;
    m_lastAgeRotation = SatoriUtil::GetTimeStamp();
    m_heap  = heap;
    m_state = TRIMMER_STATE_STOPPED;

    m_event = new (nothrow) GCEvent;
    m_event->CreateAutoEventNoThrow(false);

    m_sleepGate = new (nothrow) SatoriGate();

    if (SatoriUtil::IsTrimmingEnabled())
    {
        GCToEEInterface::CreateThread(LoopFn, this, false, "Satori GC Trimmer Thread");
    }
}

// A commit coarser than a region cannot be sparse, so then there is nothing to decommit.
static bool CanDecommitGranules()
{
    return SatoriUtil::CommitGranularity() <= Satori::REGION_SIZE_GRANULARITY;
}

void SatoriTrimmer::Pause(int msec)
{
    m_paused = true;
    m_sleepGate->TimedWait(msec);
    m_paused = false;
}

void SatoriTrimmer::Unpause()
{
    m_sleepGate->WakeOne();
}

void SatoriTrimmer::LoopFn(void* inst)
{
    SatoriTrimmer* ths = (SatoriTrimmer*)inst;
    ths->Loop();
}

void SatoriTrimmer::Loop()
{
    int64_t lastGen2 = m_heap->Recycler()->GetCollectionCount(2);
    int64_t roundStart = 0;
    const int64_t roundInterval = SatoriUtil::GetTimeStampFrequency() * 5;

    // The last round saw granules that are free and committed, and could not trim them, since they were not free on the round before,
    // or have not been free for the minimum free age. A later round can, if they stay free.
    bool leftWork = false;

    // The GCs of any generation that the trimmer has seen, and when it saw the last new one.
    // A process that has seen no GC for a round interval is idle.
    int64_t lastGcCount = m_heap->Recycler()->GetCollectionCount(0);
    int64_t lastGcSeenTs = SatoriUtil::GetTimeStamp();

    while (true)
    {
        // A round is due after a gen2 that came since the last round started, since that is when memory is freed in bulk.
        // A round is also due when the last round left work for this one, but only in an idle process: where GCs keep coming,
        // the granules that this round would find are about to be taken again, and the gen2 gate is the one that counts.
        // Without the second condition the memory that the last gen2 freed would be seen once and never decommitted,
        // in a process that stops allocating and has no more gen2.
        // The rounds are at least 5 sec apart.
        while (true)
        {
            int64_t now = SatoriUtil::GetTimeStamp();
            int64_t newGen2 = m_heap->Recycler()->GetCollectionCount(2);
            int64_t gcCount = m_heap->Recycler()->GetCollectionCount(0);
            if (gcCount != lastGcCount)
            {
                lastGcCount = gcCount;
                lastGcSeenTs = now;
            }

            bool gen2Due = lastGen2 != newGen2;
            int64_t dueTs = roundStart + roundInterval;
            if (!gen2Due && leftWork)
            {
                dueTs = max(dueTs, lastGcSeenTs + roundInterval);
            }

            int64_t waitTicks = dueTs - now;
            if (waitTicks <= 0 && (gen2Due || leftWork))
            {
                lastGen2 = newGen2;
                break;
            }

            // Idle throttling must not revoke a GC's run permission. Only a GC-requested stop
            // needs another SetOkToRun before the trimmer can continue.
            // The wait also ends in time for the next rotation of the age snapshots, so that their period is what it is meant to be.
            int64_t untilRotation = MaybeRotateAges();
            int64_t waitFor = min(waitTicks <= 0 ? untilRotation : min(waitTicks, untilRotation), roundInterval);
            int waitMsec = (int)(waitFor * 1000 / SatoriUtil::GetTimeStampFrequency() + 1);
            Pause(waitMsec);
            StopAndWait();
        }

        roundStart = SatoriUtil::GetTimeStamp();
        leftWork = false;

        if (!CanDecommitGranules())
        {
            continue;
        }

        // Regions in the cache are not free granules, and the trimmer only decommits free granules.
        // The ones that nobody has taken by the next round will be decommitted then.
        // The round starts with the permission to run. Without this call the first round could start while a GC is in progress
        // (the state is STOPPED, if a gen2 completed between the two reads of its count at the start), and in the conservative mode
        // a GC marks only while the trimmer is not active.
        StopAndWait();
        MaybeRotateAges();
        m_heap->Allocator()->DrainRegionCache();
        if (SatoriUtil::IsConservativeMode())
        {
            // The regions that were returned while a thread may have scanned a stack are parked (see SatoriAllocator::ParkRegion).
            // Nothing else would free them before the next GC.
            FreeParkedRegions();
        }

        m_heap->ForEachPage(
            [&](SatoriPage* page)
            {
                // limit the rate of scanning to 1 page/msec.
                Pause(1);
                if (m_state != TRIMMER_STATE_RUNNING)
                {
                    StopAndWait();
                }

                int64_t lastGen1 = m_heap->Recycler()->GetCollectionCount(1);

                // Only the granules that were free on the previous round too, and have been free for the minimum free age, so that memory
                // that a busy process is about to take again is not decommitted. Adjacent granules are decommitted together.
                size_t granule = page->NextGranuleToTrim(0, /* onlyIfSeenFree */ true);
                while (granule != SIZE_MAX)
                {
                    size_t count;
                    if (page->TryTrimRun(granule, Satori::TRIM_RUN_GRANULES, /* onlyIfSeenFree */ true, count))
                    {
                        // limit the decommit rate to 1 call/10 msec.
                        Pause(10);
                    }

                    // also we will pause for 1 sec if there was a GC - to further reduce the churn
                    // if the app is allocation-active.
                    int64_t newGen1 = m_heap->Recycler()->GetCollectionCount(1);
                    if (newGen1 != lastGen1)
                    {
                        lastGen1 = newGen1;
                        Pause(1000);
                    }

                    if (m_state != TRIMMER_STATE_RUNNING)
                    {
                        StopAndWait();
                    }

                    granule = page->NextGranuleToTrim(granule + count, /* onlyIfSeenFree */ true);
                }

                page->RememberFreeGranules();

                // what this round saw, and a later round can trim, when the granules are old enough
                if (page->NextGranuleToTrim(0, /* onlyIfSeenFree */ true, /* ignoreAge */ true) != SIZE_MAX)
                {
                    leftWork = true;
                }
            }
        );
    }
}

// Decommits all free granules, and the regions from the cache with them.
void SatoriTrimmer::TrimNow()
{
    _ASSERTE(m_heap->Recycler()->IsBlockingPhase());

    if (!CanDecommitGranules())
    {
        return;
    }

    // The trimmer was asked to stop when the GC started. Make sure it did, so that we do not run two things at once.
    WaitForStop();

    m_heap->Allocator()->DrainRegionCache();

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            // the world is stopped, there is no use in waiting for the granules to stay free.
            size_t granule = page->NextGranuleToTrim(0, /* onlyIfSeenFree */ false);
            while (granule != SIZE_MAX)
            {
                size_t count;
                page->TryTrimRun(granule, Satori::TRIM_RUN_GRANULES, /* onlyIfSeenFree */ false, count);
                granule = page->NextGranuleToTrim(granule + count, /* onlyIfSeenFree */ false);
            }

            page->RememberFreeGranules();
        }
    );
}

// The minimum free age (DOTNET_gcTrimMinFreeAge) without a stamp per granule. Every period E the pages rotate their two age snapshots
// (SatoriPage::RotateAges): "aged" gets "aging", and "aging" gets the granules that the last look saw free. The trimmer takes a granule
// for its age only if it is in "aged", that is it was seen free before the rotation before the last one: at least E ago.
// It is first in "aged" one to two periods after the look that first saw it free, so with E = gcTrimMinFreeAge the effective
// minimum age is between the setting and twice that. E is at least one round interval (5 s).
// The check is one comparison of the clock, made by the trimmer when it runs. No rotation happens while the trimmer is stopped:
// the snapshots then age by the time of the stop, which is the safe direction.
// Returns the time until the next rotation, in ticks (INT64_MAX if the age is off).
int64_t SatoriTrimmer::MaybeRotateAges()
{
    size_t minAge = SatoriUtil::TrimMinFreeAgeSeconds();
    if (minAge == 0)
    {
        return INT64_MAX;
    }

    int64_t now = SatoriUtil::GetTimeStamp();
    int64_t period = (int64_t)max(minAge, (size_t)5) * SatoriUtil::GetTimeStampFrequency();
    if (now - m_lastAgeRotation < period)
    {
        return m_lastAgeRotation + period - now;
    }

    m_lastAgeRotation = now;
    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            page->RotateAges();
        }
    );

    return period;
}

// The conservative mode: frees the regions that are parked. A conservative marker may follow a stale pointer to the header of
// a region that it has looked up. The trimmer frees parked regions only between a return from StopAndWait and its next call:
// a conservative GC does not mark until the trimmer has acknowledged its stop (in StopAndWait), and only the end of the GC
// lets the trimmer run again (SetOkToRun). The state may become STOP_SUGGESTED at any moment in between, that is fine.
// The state is checked before every region, and a region is popped only after the check, so nothing is held across an acknowledgement:
// a blocking GC waits for the acknowledgement with the EE suspended.
void SatoriTrimmer::FreeParkedRegions()
{
    SatoriAllocator* allocator = m_heap->Allocator();
    while (true)
    {
        if (VolatileLoad(&m_state) != TRIMMER_STATE_RUNNING)
        {
            StopAndWait();
        }

        if (!allocator->TryFreeOneParkedRegion())
        {
            return;
        }
    }
}

void SatoriTrimmer::StopAndWait()
{
    while (true)
    {
        tryAgain:

        int state = m_state;
        switch (state)
        {
        case TRIMMER_STATE_STOP_SUGGESTED:
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOPPED, state);
            continue;
        case TRIMMER_STATE_OK_TO_RUN:
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_RUNNING, state);
            continue;
        case TRIMMER_STATE_STOPPED:
            for (int i = 0; i < 10; i++)
            {
                Pause(100);
                if (m_state != state)
                {
                    goto tryAgain;
                }
            }

            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_BLOCKED, state) == state)
            {
                m_event->Wait(INFINITE, false);
            }
            continue;
        case TRIMMER_STATE_RUNNING:
            return;
        default:
            UNREACHABLE();
            return;
        }
    }
}

void SatoriTrimmer::SetOkToRun()
{
    while (true)
    {
        int state = m_state;
        switch (state)
        {
        case TRIMMER_STATE_BLOCKED:
            // trimmer can't get out of BLOCKED by itself, ordinary assignment is ok
            m_state = TRIMMER_STATE_OK_TO_RUN;
            m_event->Set();
            return;
        case TRIMMER_STATE_STOPPED:
            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_OK_TO_RUN, state) == state)
            {
                return;
            }
            break;
        case TRIMMER_STATE_STOP_SUGGESTED:
            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_RUNNING, state) == state)
            {
                return;
            }
            break;
        default:
            return;
        }
    }
}

void SatoriTrimmer::SetStopSuggested()
{
    while (true)
    {
        int state = m_state;
        switch (state)
        {
        case TRIMMER_STATE_OK_TO_RUN:
            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOPPED, state) == state)
            {
                Unpause();
                return;
            }
            break;
        case TRIMMER_STATE_RUNNING:
            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOP_SUGGESTED, state) == state)
            {
                Unpause();
                return;
            }
            break;
        default:
            _ASSERTE(m_state <= TRIMMER_STATE_STOP_SUGGESTED);
            Unpause();
            return;
        }
    }
}

void SatoriTrimmer::WaitForStop()
{
    _ASSERTE(m_state <= TRIMMER_STATE_STOP_SUGGESTED);

    int cycles = 0;
    // A conservative marker reads the region map after this. The load must be an acquire (a plain load is not one on arm64),
    // so that the marker sees every free that the trimmer did before its acknowledgement.
    while (VolatileLoad(&m_state) == TRIMMER_STATE_STOP_SUGGESTED)
    {
        if (m_paused)
        {
            Unpause();
        }

        YieldProcessor();
        if ((++cycles & 127) == 0)
        {
            GCToOSInterface::YieldThread(0);
        }
    }

    _ASSERTE(!IsActive());
}

bool SatoriTrimmer::IsActive()
{
    // an acquire, as in WaitForStop: a conservative marker that sees the trimmer inactive reads the region map after this.
    return VolatileLoad(&m_state) > TRIMMER_STATE_STOPPED;
}
