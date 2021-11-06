// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRecycler.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriTrimmer.h"
#include "SatoriHeap.h"
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

SatoriTrimmer::SatoriTrimmer(SatoriHeap* heap)
{
    m_lastGen2Count = 0;
    m_heap  = heap;
    m_state = TRIMMER_STATE_RUNNING;

    m_gate = new (nothrow) GCEvent;
    m_gate->CreateAutoEventNoThrow(false);

    GCToEEInterface::CreateThread(LoopFn, this, false, "Satori GC Trimmer Thread");
}

void SatoriTrimmer::LoopFn(void* inst)
{
    SatoriTrimmer* ths = (SatoriTrimmer*)inst;
    ths->Loop();
}

void SatoriTrimmer::Loop()
{
    while (true)
    {
        int64_t curGen2 = m_heap->Recycler()->GetCollectionCount(2);
        while (curGen2 == m_heap->Recycler()->GetCollectionCount(2))
        {
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOPPED, TRIMMER_STATE_RUNNING);
            StopAndWait();
        }

        m_heap->ForEachPage(
            [&](SatoriPage* page)
            {
                page->ForEachRegion(
                    [&](SatoriRegion* region)
                    {
                        SatoriQueue<SatoriRegion>* queue = region->ContainingQueue();
                        if (queue && queue->Kind() == QueueKind::Allocator)
                        {
                            if (region->CanDecommit() || region->CanCoalesceWithNext())
                            {
                                if (queue->TryRemove(region))
                                {
                                    region->TryCoalesceWithNext();
                                    if (region->TryDecommit())
                                    {
                                        m_heap->Allocator()->AddRegion(region);
                                    }
                                    else
                                    {
                                        m_heap->Allocator()->ReturnRegion(region);
                                    }
                                }
                            }
                        }

                        if (m_state != TRIMMER_STATE_RUNNING)
                        {
                            StopAndWait();
                        }
                    }
                );
            }
        );
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
                GCToOSInterface::Sleep(100);
                if (m_state != state)
                {
                    goto tryAgain;
                }
            }

            if (Interlocked::CompareExchange(&m_state, TRIMMER_STATE_BLOCKED, state) == state)
            {
                m_gate->Wait(INFINITE, false);
            }
            continue;
        case TRIMMER_STATE_RUNNING:
            return;
        default:
            __UNREACHABLE();
            return;
        }
    }
}

void SatoriTrimmer::SetOkToRun()
{
    int state = m_state;
    if (state == TRIMMER_STATE_BLOCKED)
    {
        // trimmer can't get out of BlOCKED by itself, ordinary assignment is ok
        m_state = TRIMMER_STATE_OK_TO_RUN;
        m_gate->Set();
        return;
    }

    if (state == TRIMMER_STATE_STOPPED)
    {
        Interlocked::CompareExchange(&m_state, TRIMMER_STATE_OK_TO_RUN, state);
        return;
    }

    if (state == TRIMMER_STATE_STOP_SUGGESTED)
    {
        Interlocked::CompareExchange(&m_state, TRIMMER_STATE_RUNNING, state);
    }
}

void SatoriTrimmer::SetStopSuggested()
{
    while (true)
    {
        int state = m_state;
        if (state == TRIMMER_STATE_OK_TO_RUN &&
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOPPED, state) == state)
        {
            break;
        }
        else if (state == TRIMMER_STATE_RUNNING &&
            Interlocked::CompareExchange(&m_state, TRIMMER_STATE_STOP_SUGGESTED, state) == state)
        {
            break;
        }
        else
        {
            break;
        }
    }

    _ASSERTE(m_state <= TRIMMER_STATE_STOP_SUGGESTED);
}

void SatoriTrimmer::WaitForStop()
{
    _ASSERTE(m_state <= TRIMMER_STATE_STOP_SUGGESTED);

    int cycles = 0;
    while (m_state == TRIMMER_STATE_STOP_SUGGESTED)
    {
        YieldProcessor();
        if ((++cycles % 127) == 0)
        {
            GCToOSInterface::YieldThread(0);
        }
    }

    _ASSERTE(m_state <= TRIMMER_STATE_STOPPED);
}
