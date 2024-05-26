// Copyright (c) 2024 Vladimir Sadov
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
#include "../gceventstatus.h"

#include "SatoriHandlePartitioner.h"
#include "SatoriHeap.h"
#include "SatoriTrimmer.h"
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriRecycler.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"
#include "SatoriWorkChunk.h"
#include "SatoriAllocationContext.h"
#include "SatoriFinalizationQueue.h"
#include "../gcscan.h"

//#pragma optimize("", off)

#ifdef memcpy
#undef memcpy
#endif //memcpy

//#define TIMED

static const int MIN_GEN1_BUDGET = 2 * Satori::REGION_SIZE_GRANULARITY;

void ToggleWriteBarrier(bool concurrent, bool eeSuspended)
{
    WriteBarrierParameters args = {};
    args.operation = concurrent ?
        WriteBarrierOp::StartConcurrentMarkingSatori :
        WriteBarrierOp::StopConcurrentMarkingSatori;

    args.is_runtime_suspended = eeSuspended;
    GCToEEInterface::StompWriteBarrier(&args);
}

static SatoriRegionQueue* AllocQueue(QueueKind kind)
{
    const size_t align = 64;
#ifdef _MSC_VER
    void* buffer = _aligned_malloc(sizeof(SatoriRegionQueue), align);
#else
    void* buffer = malloc(sizeof(SatoriRegionQueue) + align);
    buffer = (void*)ALIGN_UP((size_t)buffer, align);
#endif
    return new(buffer)SatoriRegionQueue(kind);
}

void SatoriRecycler::Initialize(SatoriHeap* heap)
{
    m_helpersGate = new (nothrow) GCEvent;
    m_helpersGate->CreateAutoEventNoThrow(false);
    m_gateSignaled = 0;
    m_activeHelpers= 0;
    m_totalHelpers = 0;

    m_noWorkSince = 0;

    m_perfCounterTicksPerMilli = GCToOSInterface::QueryPerformanceFrequency() / 1000;
    m_perfCounterTicksPerMicro = GCToOSInterface::QueryPerformanceFrequency() / 1000000;

    m_heap = heap;
    m_trimmer = new (nothrow) SatoriTrimmer(heap);

    m_ephemeralRegions = AllocQueue(QueueKind::RecyclerEphemeral);
    m_ephemeralFinalizationTrackingRegions = AllocQueue(QueueKind::RecyclerEphemeralFinalizationTracking);
    m_tenuredRegions = AllocQueue(QueueKind::RecyclerTenured);
    m_tenuredFinalizationTrackingRegions = AllocQueue(QueueKind::RecyclerTenuredFinalizationTracking);

    m_finalizationPendingRegions = AllocQueue(QueueKind::RecyclerFinalizationPending);

    m_stayingRegions = AllocQueue(QueueKind::RecyclerStaying);
    m_relocatingRegions = AllocQueue(QueueKind::RecyclerRelocating);
    m_relocatedRegions = AllocQueue(QueueKind::RecyclerRelocated);
    m_relocatedToHigherGenRegions = AllocQueue(QueueKind::RecyclerRelocatedToHigherGen);

    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        m_relocationTargets[i] = AllocQueue(QueueKind::RecyclerRelocationTarget);
    }

    m_deferredSweepRegions = AllocQueue(QueueKind::RecyclerDeferredSweep);
    m_deferredSweepCount = 0;
    m_gen1AddedSinceLastCollection = 0;
    m_gen2AddedSinceLastCollection = 0;

    m_reusableRegions = AllocQueue(QueueKind::RecyclerReusable);
    m_ephemeralWithUnmarkedDemoted = AllocQueue(QueueKind::RecyclerDemoted);
    m_reusableRegionsAlternate = AllocQueue(QueueKind::RecyclerReusable);

    m_workList = new (nothrow) SatoriWorkList();
    m_gcState = GC_STATE_NONE;
    m_isBarrierConcurrent = false;

    m_gcCount[0] = 0;
    m_gcCount[1] = 0;
    m_gcCount[2] = 0;

    m_condemnedGeneration = 0;

    m_relocatableEphemeralEstimate = 0;
    m_relocatableTenuredEstimate = 0;
    m_promotionEstimate = 0;

    m_occupancy[0] = 0;
    m_occupancy[1] = 0;
    m_occupancy[2] = 0;
    m_occupancyAcc[0] = 0;
    m_occupancyAcc[1] = 0;
    m_occupancyAcc[2] = 0;

    m_gen1CountAtLastGen2 = 0;
    m_gen1Budget = MIN_GEN1_BUDGET;
    m_totalBudget = MIN_GEN1_BUDGET;
    m_totalLimit = m_totalBudget;
    m_prevCondemnedGeneration = 2;

    m_activeHelperFn = nullptr;
    m_rootScanTicket = 0;
    m_cardScanTicket = 0;
    m_concurrentCardsDone = false;
    m_concurrentHandlesDone = false;
    m_ccStackMarkingThreadsNum = 0;

    m_isLowLatencyMode = SatoriUtil::IsLowLatencyMode();

    for (int i = 0; i < 2; i++)
    {
        m_gcStartMillis[i] = m_gcDurationMillis[i] = 0;
    }

    m_lastEphemeralGcInfo = { 0 };
    m_lastTenuredGcInfo   = { 0 };
    m_CurrentGcInfo = nullptr;
}

void SatoriRecycler::ShutDown()
{
    m_activeHelperFn = nullptr;
}

/* static */
void SatoriRecycler::HelperThreadFn(void* param)
{
    SatoriRecycler* recycler = (SatoriRecycler*)param;
    Interlocked::Increment(&recycler->m_activeHelpers);

    for (;;)
    {
        Interlocked::Decrement(&recycler->m_activeHelpers);

        uint32_t waitResult = recycler->m_helpersGate->Wait(1000, FALSE);
        if (waitResult != WAIT_OBJECT_0)
        {
            Interlocked::Decrement(&recycler->m_totalHelpers);
            return;
        }

        recycler->m_gateSignaled = 0;
        Interlocked::Increment(&recycler->m_activeHelpers);
        auto activeHelper = recycler->m_activeHelperFn;
        if (activeHelper)
        {
            (recycler->*activeHelper)();
        }
    }
}

// not interlocked. this is not done concurrently. 
void SatoriRecycler::IncrementRootScanTicket()
{
    m_rootScanTicket++;
    // make sure the ticket is not 0
    if (!m_rootScanTicket)
    {
        m_rootScanTicket++;
    }
}

void SatoriRecycler::IncrementCardScanTicket()
{
    m_cardScanTicket++;
    // make sure the ticket is not 0
    if (!m_cardScanTicket)
    {
        m_cardScanTicket++;
    }
}

int SatoriRecycler::GetRootScanTicket()
{
    return m_rootScanTicket;
}

uint8_t SatoriRecycler::GetCardScanTicket()
{
    return m_cardScanTicket;
}

int64_t SatoriRecycler::GetCollectionCount(int gen)
{
    return m_gcCount[gen];
}

int SatoriRecycler::GetCondemnedGeneration()
{
    return m_condemnedGeneration;
}

size_t SatoriRecycler::Gen1RegionCount()
{
    return m_ephemeralFinalizationTrackingRegions->Count() +
        m_ephemeralRegions->Count() +
        m_ephemeralWithUnmarkedDemoted->Count();
}

size_t SatoriRecycler::Gen2RegionCount()
{
    return m_tenuredFinalizationTrackingRegions->Count() + m_tenuredRegions->Count();
}

size_t SatoriRecycler::RegionCount()
{
    return Gen1RegionCount() + Gen2RegionCount();
}

SatoriRegion* SatoriRecycler::TryGetReusable()
{
    SatoriRegion* reusable = m_reusableRegions->TryPop();
    if (reusable)
    {
        reusable->OccupancyAtReuse() = reusable->Occupancy();
    }

    return  reusable;
}

SatoriRegion* SatoriRecycler::TryGetReusableForLarge()
{
    SatoriRegion* reusable = m_reusableRegions->TryDequeueIfHasFreeSpaceInTopBucket();
    if (reusable)
    {
        reusable->OccupancyAtReuse() = reusable->Occupancy();
    }

    return  reusable;
}

void SatoriRecycler::PushToEphemeralQueues(SatoriRegion* region)
{
    if (region->HasUnmarkedDemotedObjects())
    {
        m_ephemeralWithUnmarkedDemoted->Push(region);
    }
    else
    {
        if (IsRelocationCandidate(region))
        {
            Interlocked::Increment(&m_relocatableEphemeralEstimate);
        }

        if (IsPromotionCandidate(region))
        {
            Interlocked::Increment(&m_promotionEstimate);
        }

        if (region->HasFinalizables())
        {
            m_ephemeralFinalizationTrackingRegions->Push(region);
        }
        else
        {
            m_ephemeralRegions->Push(region);
        }
    }
}

void SatoriRecycler::PushToTenuredQueues(SatoriRegion* region)
{
    if (IsRelocationCandidate(region))
    {
        Interlocked::Increment(&m_relocatableTenuredEstimate);
    }

    if (region->HasFinalizables())
    {
        m_tenuredFinalizationTrackingRegions->Push(region);
    }
    else
    {
        m_tenuredRegions->Push(region);
    }
}

// NOTE: recycler owns nursery regions only temporarily for the duration of GC when mutators are stopped.
//       we do not keep them because an active nursery region may change its finalizability classification
//       concurrently by allocating a finalizable object.
void SatoriRecycler::AddEphemeralRegion(SatoriRegion* region)
{
    _ASSERTE(region->GetAllocStart() == 0);
    _ASSERTE(region->GetAllocRemaining() == 0);
    _ASSERTE(region->Generation() == 0 || region->Generation() == 1);
    _ASSERTE(!region->HasMarksSet());
    _ASSERTE(!region->DoNotSweep());
    _ASSERTE(!region->IsAttachedToAllocatingOwner() || IsBlockingPhase());

    PushToEphemeralQueues(region);
    size_t allocatedBytes = region->Occupancy() - region->OccupancyAtReuse();
    _ASSERTE(allocatedBytes <= region->Occupancy());
    region->OccupancyAtReuse() = 0;
    Interlocked::ExchangeAdd64(&m_gen1AddedSinceLastCollection, allocatedBytes);
    if (region->IsEscapeTracking())
    {
        _ASSERTE(IsBlockingPhase());
        _ASSERTE(!region->HasPinnedObjects());
        region->ClearMarks();
    }

    // When concurrent marking is allowed we may have marks already.
    // Demoted regions could be pre-marked
#ifdef DEBUG
    if (!region->MaybeAllocatingAcquire())
    {
        region->Verify(/* allowMarked */ region->IsDemoted() || SatoriUtil::IsConcurrent());
    }
#endif
}

void SatoriRecycler::AddTenuredRegion(SatoriRegion* region)
{
    _ASSERTE(region->GetAllocStart() == 0);
    _ASSERTE(region->GetAllocRemaining() == 0);
    _ASSERTE(!region->IsEscapeTracking());
    _ASSERTE(!region->HasMarksSet());
    _ASSERTE(!region->DoNotSweep());

    region->Verify(/* allowMarked */ SatoriUtil::IsConcurrent() && SatoriUtil::IsConservativeMode());
    PushToTenuredQueues(region);
    _ASSERTE(region->Generation() == 2);
    Interlocked::ExchangeAdd64(&m_gen2AddedSinceLastCollection, region->Occupancy());
    region->RearmCardsForTenured();
}

size_t SatoriRecycler::GetNowMillis()
{
    int64_t t = GCToOSInterface::QueryPerformanceCounter();
    return (size_t)(t / m_perfCounterTicksPerMilli);
}

size_t SatoriRecycler::IncrementGen0Count()
{
    // duration of Gen0 is typically << msec, so we will not record that.
    m_gcStartMillis[0] = GetNowMillis();
    return Interlocked::Increment((size_t*)&m_gcCount[0]);
}

void SatoriRecycler::TryStartGC(int generation, gc_reason reason)
{
    int newState = SatoriUtil::IsConcurrent() ? GC_STATE_CONCURRENT : GC_STATE_BLOCKING;
    if (m_gcState == GC_STATE_NONE &&
        Interlocked::CompareExchange(&m_gcState, newState, GC_STATE_NONE) == GC_STATE_NONE)
    {
        m_CurrentGcInfo = generation == 2 ? &m_lastTenuredGcInfo :&m_lastEphemeralGcInfo;
        m_CurrentGcInfo->m_condemnedGeneration = (uint8_t)generation;
        m_CurrentGcInfo->m_concurrent = SatoriUtil::IsConcurrent();

        FIRE_EVENT(GCTriggered, (uint32_t)reason);

        m_trimmer->SetStopSuggested();

        // Here we have a short window when other threads may notice that gc is in progress,
        // but will find no helping opportunities until we set m_condemnedGeneration.
        // that is ok.

        // in concurrent case there is an extra pass over stacks and handles
        // which happens before the blocking stage
        if (newState == GC_STATE_CONCURRENT)
        {
            m_ccStackMarkState = CC_MARK_STATE_NONE;
            IncrementRootScanTicket();
            m_concurrentCardsDone = false;
            m_concurrentHandlesDone = false;
            SatoriHandlePartitioner::StartNextScan();
            m_activeHelperFn = &SatoriRecycler::ConcurrentHelp;
        }

        IncrementCardScanTicket();

        // publishing condemned generation will enable helping.
        // it should happen after the writes above.
        // just to make sure it is all published when the barrier is updated, which is fully synchronizing.
        VolatileStore(&m_condemnedGeneration, generation);

        // if we are doing blocking GC, just go and start it
        if (newState == GC_STATE_BLOCKING)
        {
            BlockingCollect();
        }
    }
}

bool IsHelperThread()
{
    return GCToEEInterface::WasCurrentThreadCreatedByGC();
}

int64_t SatoriRecycler::HelpQuantum()
{
    return m_perfCounterTicksPerMilli / 8;  // 1/8 msec
}

bool SatoriRecycler::HelpOnceCore()
{
    if (m_condemnedGeneration == 0)
    {
        // GC has not started yet, come again later.
        return true;
    }

    if (m_ccStackMarkState == CC_MARK_STATE_MARKING)
    {
        _ASSERTE(m_isBarrierConcurrent);
        // help with marking stacks and f-queue, this is urgent since EE is stopped for this.
        BlockingMarkForConcurrentHelper();
    }

    int64_t timeStamp = GCToOSInterface::QueryPerformanceCounter();
    int64_t deadline = timeStamp + HelpQuantum();

    // this should be done before scanning stacks or cards
    // since the regions must be swept before we can use FindObject
    if (m_ccStackMarkState == CC_MARK_STATE_NONE)
    {
        if (DrainDeferredSweepQueueConcurrent(deadline))
        {
            return true;
        }
    }

    // make sure the barrier is toggled to concurrent before marking
    if (!m_isBarrierConcurrent)
    {
        // toggling is a ProcessWide fence.
        ToggleWriteBarrier(true, /* eeSuspended */ false);
        m_isBarrierConcurrent = true;
    }

    if (m_ccStackMarkState != CC_MARK_STATE_DONE)
    {
        MarkOwnStackOrDrainQueuesConcurrent(deadline);
    }

    // if stacks are not marked yet, start suspending EE
    if (m_ccStackMarkState == CC_MARK_STATE_NONE)
    {
        // only one thread will win and drive this stage, others may help.
        BlockingMarkForConcurrent();
    }

    if (m_ccStackMarkState == CC_MARK_STATE_SUSPENDING_EE && !IsHelperThread())
    {
        // this is a mutator thread and we are suspending them, leave and suspend.
        return true;
    }

    if (MarkDemotedAndDrainQueuesConcurrent(deadline))
    {
        return true;
    }

    if (!m_concurrentHandlesDone)
    {
        bool moreWork = MarkHandles(deadline);
        if (!moreWork)
        {
            m_concurrentHandlesDone = true;
        }

        return true;
    }

    if (!m_concurrentCardsDone)
    {
        if (m_condemnedGeneration == 1 ?
            MarkThroughCardsConcurrent(deadline) :
            ScanDirtyCardsConcurrent(deadline))
        {
            return true;
        }
        else
        {
            m_concurrentCardsDone = true;
        }
    }

    if (m_ccStackMarkState == CC_MARK_STATE_MARKING)
    {
        _ASSERTE(IsHelperThread());
        // do not leave, come back and help
        return true;
    }

    // if queues are empty we see no more work
    return DrainMarkQueuesConcurrent(nullptr, deadline);
}

class MarkContext
{
    friend class SatoriRecycler;

public:
    MarkContext(SatoriRecycler* recycler)
        : m_WorkChunk()
    {
        m_recycler = recycler;
        m_condemnedGeneration = recycler->m_condemnedGeneration;
        m_heap = recycler->m_heap;
    }

    void PushToMarkQueues(SatoriObject* o)
    {
        if (m_WorkChunk && m_WorkChunk->TryPush(o))
        {
            return;
        }

        m_recycler->PushToMarkQueuesSlow(m_WorkChunk, o);
    }

private:
    int m_condemnedGeneration;
    SatoriWorkChunk* m_WorkChunk;
    SatoriHeap* m_heap;
    SatoriRecycler* m_recycler;
};

void SatoriRecycler::BlockingMarkForConcurrentHelper()
{
    Interlocked::Increment(&m_ccStackMarkingThreadsNum);
    // check state again it could have changed if there were no marking threads
    if (m_ccStackMarkState == CC_MARK_STATE_MARKING)
    {
        MarkAllStacksFinalizationAndDemotedRoots();
    }

    Interlocked::Decrement(&m_ccStackMarkingThreadsNum);
}

/* static */
void SatoriRecycler::ConcurrentPhasePrepFn(gc_alloc_context* gcContext, void* param)
{
    SatoriAllocationContext* context = (SatoriAllocationContext*)gcContext;
    MarkContext* markContext = (MarkContext*)param;
    SatoriRecycler* recycler = markContext->m_recycler;

    SatoriRegion* region = context->RegularRegion();
    if (region)
    {
        if (region->IsEscapeTracking())
        {
            region->StopEscapeTracking();
        }

        if (region->HasUnmarkedDemotedObjects())
        {
            recycler->MarkDemoted(region, markContext);
        }
    }

    region = context->LargeRegion();
    if (region && region->HasUnmarkedDemotedObjects())
    {
        recycler->MarkDemoted(region, markContext);
    }
}

void SatoriRecycler::BlockingMarkForConcurrent()
{
    if (Interlocked::CompareExchange(&m_ccStackMarkState, CC_MARK_STATE_SUSPENDING_EE, CC_MARK_STATE_NONE) == CC_MARK_STATE_NONE)
    {
        size_t blockingStart = GCToOSInterface::QueryPerformanceCounter();
        GCToEEInterface::SuspendEE(SUSPEND_FOR_GC_PREP);

        // swap reusable and alternate so that we could filter through reusables.
        // the swap needs to be done when EE is stopped, but before marking has started.
        SatoriRegionQueue* alternate = m_reusableRegionsAlternate;
        _ASSERTE(alternate->IsEmpty());
        m_reusableRegionsAlternate = m_reusableRegions;
        m_reusableRegions = alternate;

        // signal to everybody to start marking roots
        VolatileStore((int*)&m_ccStackMarkState, CC_MARK_STATE_MARKING);

        // mark demoted regions if any attached to thread contexts
        MarkContext c(this);
        GCToEEInterface::GcEnumAllocContexts(ConcurrentPhasePrepFn, &c);
        if (c.m_WorkChunk != nullptr)
        {
            m_workList->Push(c.m_WorkChunk);
        }

        // now join everybody else and mark some roots
        MarkAllStacksFinalizationAndDemotedRoots();

        // done, wait for marking to finish and restart EE
        Interlocked::Exchange(&m_ccStackMarkState, CC_MARK_STATE_DONE);
        while (m_ccStackMarkingThreadsNum)
        {
            // since we are waiting anyways, try helping
            if (!HelpOnceCore())
            {
                YieldProcessor();
            }
        }

        size_t blockingDuration = (GCToOSInterface::QueryPerformanceCounter() - blockingStart);
        m_CurrentGcInfo->m_pauseDurations[1] = blockingDuration / m_perfCounterTicksPerMicro;

        GCToEEInterface::RestartEE(false);
    }
}

void SatoriRecycler::HelpOnce()
{
    _ASSERTE(!IsHelperThread());

    if (m_gcState != GC_STATE_NONE)
    {
        if (m_gcState == GC_STATE_CONCURRENT)
        {
            bool moreWork = HelpOnceCore();
            int64_t time = GCToOSInterface::QueryPerformanceCounter();

            if (moreWork)
            {
                m_noWorkSince = time;
            }
            else
            {
                if (!m_activeHelpers &&
                    (time - m_noWorkSince) > HelpQuantum() * 4) // 4 help quantums without work returned
                {
                    // we see no concurrent work, initiate blocking stage
                    if (Interlocked::CompareExchange(&m_gcState, GC_STATE_BLOCKING, GC_STATE_CONCURRENT) == GC_STATE_CONCURRENT)
                    {
                        m_activeHelperFn = nullptr;
                        BlockingCollect();
                    }
                }
            }
        }

        GCToEEInterface::GcPoll();
    }
    else if (!m_deferredSweepRegions->IsEmpty())
    {
        int64_t timeStamp = GCToOSInterface::QueryPerformanceCounter();
        int64_t deadline = timeStamp + HelpQuantum();
        DrainDeferredSweepQueueConcurrent(deadline);
    }
}

void SatoriRecycler::ConcurrentHelp()
{
    // helpers have deadline too, just to come here and check the stage.
    while ((m_gcState == GC_STATE_CONCURRENT) && HelpOnceCore());
}

int SatoriRecycler::MaxHelpers()
{
    int helperCount = SatoriUtil::MaxHelpersCount();
    if (helperCount < 0)
    {
        int cpuCount = GCToOSInterface::GetTotalProcessorCount();

        // TUNING: should this be more dynamic? check CPU load and such.
        helperCount = cpuCount - 1;
    }

    return helperCount;
}

void SatoriRecycler::MaybeAskForHelp()
{
    if (m_activeHelperFn && m_activeHelpers < MaxHelpers())
    {
        AskForHelp();
    }
}

void SatoriRecycler::AskForHelp()
{
    int totalHelpers = m_totalHelpers;
    if (m_activeHelpers >= totalHelpers &&
        totalHelpers < MaxHelpers() &&
        Interlocked::CompareExchange(&m_totalHelpers, totalHelpers + 1, totalHelpers) == totalHelpers)
    {
        if (!GCToEEInterface::CreateThread(HelperThreadFn, this, false, "Satori GC Helper Thread"))
        {
            Interlocked::Decrement(&m_totalHelpers);
            return;
        }
    }

    if (!m_gateSignaled && Interlocked::CompareExchange(&m_gateSignaled, 1, 0) == 0)
    {
        m_helpersGate->Set();
    }
}

void SatoriRecycler::Collect(int generation, bool force, bool blocking)
{
    // only blocked & forced GC is strictly observable - via finalization.
    // otherwise it is kind of a suggestion to maybe start collecting

    if (!force)
    {
        // just check if it is time
        MaybeTriggerGC(gc_reason::reason_induced_noforce);
        return;
    }

    int64_t& collectionNumRef = (generation == 1 ? m_gcCount[1] : m_gcCount[2]);
    int64_t desiredCollectionNum = collectionNumRef + 1;

    // If we need to block for GC, a GC that is already in progress does not count.
    // It may have marked what is not alive at the time of the Collect call and
    // that is observably incorrect.
    // We will wait until the completion of the next one.
    if (m_gcState != GC_STATE_NONE)
    {
        desiredCollectionNum++;
    }

    do
    {
        TryStartGC(generation, gc_reason::reason_induced);
        HelpOnce();
    } while (blocking && (desiredCollectionNum > collectionNumRef));
}

bool SatoriRecycler::IsBlockingPhase()
{
    return m_gcState == GC_STATE_BLOCKED;
}

//TUNING: We use a very simplistic approach for GC triggering here.
//        There could be a lot of room to improve in this area:
//        - could consider current CPU/memory load and adjust accordingly
//        - could collect and use past history of the program behavior
//        - could consider user input as to favor latency or throughput
//        - ??

// we target 1/EPH_SURV_TARGET ephemeral survival rate
#define EPH_SURV_TARGET 4

// when we do not know, we estimate 1/10 of total heap to be ephemeral.
#define EPH_RATIO 10

// do gen2 when total doubles
#define GEN2_THRESHOLD 2

void SatoriRecycler::MaybeTriggerGC(gc_reason reason)
{
    int generation = 0;

    if (m_gen1AddedSinceLastCollection > m_gen1Budget)
    {
        generation = 1;
    }

    size_t currentAddedEstimate = m_gen2AddedSinceLastCollection +
        m_gen1AddedSinceLastCollection / EPH_SURV_TARGET;

    if (currentAddedEstimate > m_totalBudget)
    {
        generation = 2;
    }

    // just make sure gen2 happens eventually. 
    if (m_gcCount[1] - m_gen1CountAtLastGen2 > 16)
    {
        generation = 2;
    }

    if (generation != 0)
    {
        TryStartGC(generation, reason);
    }

    HelpOnce();
}

size_t GetAvailableMemory()
{
    uint64_t available;
    GCToOSInterface::GetMemoryStatus(0, nullptr, &available, nullptr);

    return available;
}

void SatoriRecycler::AdjustHeuristics()
{
    // ocupancies as of last collection
    size_t occupancy = GetTotalOccupancy();
    size_t ephemeralOccupancy = m_occupancy[1] + m_occupancy[0];

    if (m_prevCondemnedGeneration == 2)
    {
        m_totalLimit = occupancy * GEN2_THRESHOLD;
    }

    size_t currentTotalEstimate = occupancy +
        m_gen2AddedSinceLastCollection +
        m_gen1AddedSinceLastCollection / EPH_SURV_TARGET;

    m_totalBudget = m_totalLimit > currentTotalEstimate ?
        max(MIN_GEN1_BUDGET, m_totalLimit - currentTotalEstimate) :
        MIN_GEN1_BUDGET;

    // we will try not to use the last 10%
    size_t available = GetAvailableMemory() * 9 / 10;
    m_totalBudget = min(m_totalBudget, available);

    // we look for 1 / EPH_SURV_TARGET ephemeral survivorship, thus budget is ephemeralOccupancy * EPH_SURV_TARGET
    // we compute that based on actual ephemeralOccupancy or (occupancy / EPH_RATIO / 2), whichever is larger
    // and limit that to MIN_GEN1_BUDGET
    size_t minGen1 = max(MIN_GEN1_BUDGET, occupancy * EPH_SURV_TARGET / EPH_RATIO / 2);

    size_t newGen1Budget = max(minGen1, ephemeralOccupancy * EPH_SURV_TARGET);

    // smooth the budget a bit
    // TUNING: using exponential smoothing with alpha == 1/2. is it a good smooth/lag balance?
    m_gen1Budget = (m_gen1Budget + newGen1Budget) / 2;

    if (m_condemnedGeneration == 2)
    {
        // we expect heap size to reduce and will readjust the limit at the next gc,
        // but if heap doubles we do gen2 again.
        m_totalBudget = min(currentTotalEstimate, available);

        // we will also lower gen1 budget, so that a large gen1 budget does not lead to
        // perpetual gen2s if heap collapses. Normally gen1 budget will be less than gen2.
        m_gen1Budget = max(MIN_GEN1_BUDGET, m_totalBudget / 8);
    }

    // now figure if we will promote
    m_promoteAllRegions = false;
    size_t promotionEstimate = m_promotionEstimate;
    m_promotionEstimate = 0;

    if (m_condemnedGeneration == 2)
    {
        m_promoteAllRegions = true;
        m_gen1CountAtLastGen2 = (int)m_gcCount[1];
    }
    else
    {
        if (promotionEstimate > Gen1RegionCount() / 2)
        {
            m_promoteAllRegions = true;
        }
    }
}

void SatoriRecycler::BlockingCollect()
{
    if (m_condemnedGeneration == 2)
    {
        BlockingCollect2();
    }
    else
    {
        BlockingCollect1();
    }
}

NOINLINE
void SatoriRecycler::BlockingCollect1()
{
    size_t blockingStart = GCToOSInterface::QueryPerformanceCounter();

    // stop other threads.
    GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

    BlockingCollectImpl();

    size_t blockingDuration = (GCToOSInterface::QueryPerformanceCounter() - blockingStart);
    m_CurrentGcInfo->m_pauseDurations[0] = blockingDuration / m_perfCounterTicksPerMicro;
    m_gcDurationMillis[1] = blockingDuration / m_perfCounterTicksPerMicro;
    m_CurrentGcInfo = nullptr;

    // restart VM
    GCToEEInterface::RestartEE(true);
}

NOINLINE
void SatoriRecycler::BlockingCollect2()
{
    size_t blockingStart = GCToOSInterface::QueryPerformanceCounter();

    // stop other threads.
    GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

    BlockingCollectImpl();

    size_t blockingDuration = (GCToOSInterface::QueryPerformanceCounter() - blockingStart);
    m_CurrentGcInfo->m_pauseDurations[0] = blockingDuration / m_perfCounterTicksPerMicro;
    m_gcDurationMillis[2] = blockingDuration / m_perfCounterTicksPerMicro;
    m_CurrentGcInfo = nullptr;

    // restart VM
    GCToEEInterface::RestartEE(true);
}

void SatoriRecycler::BlockingCollectImpl()
{
    FIRE_EVENT(GCStart_V2, (uint32_t)GlobalGcIndex() + 1, (uint32_t)m_condemnedGeneration, (uint32_t)reason_empty, (uint32_t)gc_etw_type_ngc);
    m_gcStartMillis[m_condemnedGeneration] = GetNowMillis();

#ifdef TIMED
    size_t time = GCToOSInterface::QueryPerformanceCounter();
#endif

    // we should not normally have active workers here.
    // just in case we support forcing blocking stage for Collect or OOM situations
    while (m_activeHelpers > 0)
    {
        // since we are waiting for concurrent helpers to stop, we could as well try helping
        if (!HelpOnceCore())
        {
            YieldProcessor();
        }
    }

    if (m_isBarrierConcurrent)
    {
        ToggleWriteBarrier(false, /* eeSuspended */ true);
        m_isBarrierConcurrent = false;
    }

    m_gcState = GC_STATE_BLOCKED;

    // assume that we will relocate. we will rethink later.
    m_isRelocating = m_condemnedGeneration == 2 ? SatoriUtil::IsRelocatingInGen2() : SatoriUtil::IsRelocatingInGen1();
    if (IsLowLatencyMode())
    {
        m_isRelocating = false;
    }

    RunWithHelp(&SatoriRecycler::DrainDeferredSweepQueue);

    // all sweeping should be done by now
    m_occupancy[1] = m_occupancyAcc[1];
    m_occupancy[2] = m_occupancyAcc[2];

    m_occupancyAcc[0] = 0;
    m_occupancyAcc[1] = 0;

    _ASSERTE(m_deferredSweepRegions->IsEmpty());

    // now we know survivorship after the last GC
    // and we can figure what we want to do in this GC and when we will do the next one
    AdjustHeuristics();

    // tell EE that we are starting
    // this needs to be called on a "GC" thread while EE is stopped.
    GCToEEInterface::GcStartWork(m_condemnedGeneration, max_generation);

#ifdef TIMED
    if (m_condemnedGeneration == 2)
    {
        printf("GenStarting%i \n", m_condemnedGeneration);
    }
#endif

    // this will make the heap officially parseable.
    DeactivateAllocatingRegions();

    m_condemnedRegionsCount = m_condemnedGeneration == 2 ?
        RegionCount() :
        Gen1RegionCount();

    BlockingMark();
    Plan();
    Relocate();
    Update();

    m_gcCount[0]++;
    m_gcCount[1]++;
    if (m_condemnedGeneration == 2)
    {
        m_gcCount[2]++;
    }

    m_CurrentGcInfo->m_index = GlobalGcIndex();

    // we may still have some deferred sweeping to do, but
    // that is unobservable to EE, so tell EE that we are done
    GCToEEInterface::GcDone(m_condemnedGeneration);

#ifdef TIMED
    if (m_condemnedGeneration == 2)
    {
        printf("GenDone. Gen: %i , relocating: %d ", m_condemnedGeneration, m_isRelocating);
        time = (GCToOSInterface::QueryPerformanceCounter() - time) * 1000000 / GCToOSInterface::QueryPerformanceFrequency();
        printf("usec: %zu\n", time);
    }
#endif

    FIRE_EVENT(GCEnd_V1, (uint32_t)GlobalGcIndex(), (uint32_t)m_condemnedGeneration);

//void FireGCHeapStats_V2(uint64_t generationSize0,
//    uint64_t totalPromotedSize0,
//    uint64_t generationSize1,
//    uint64_t totalPromotedSize1,
//    uint64_t generationSize2,
//    uint64_t totalPromotedSize2,
//    uint64_t generationSize3,
//    uint64_t totalPromotedSize3,
//    uint64_t generationSize4,
//    uint64_t totalPromotedSize4,
//    uint64_t finalizationPromotedSize,
//    uint64_t finalizationPromotedCount,
//    uint32_t pinnedObjectCount,
//    uint32_t sinkBlockCount,
//    uint32_t gcHandleCount);

    FIRE_EVENT(GCHeapStats_V2,
        m_occupancy[0], m_occupancy[0],
        m_occupancy[1], m_occupancy[1],
        m_occupancy[2], m_occupancy[2],
        (size_t)0, (size_t)0,
        (size_t)0, (size_t)0,
        (size_t)0,
        (size_t)m_heap->FinalizationQueue()->Count(),
        (uint32_t)0,
        (uint32_t)0,
        (uint32_t)0);

    m_prevCondemnedGeneration = m_condemnedGeneration;
    m_condemnedGeneration = 0;
    m_gcState = GC_STATE_NONE;
    m_gen1AddedSinceLastCollection = 0;
    m_gen2AddedSinceLastCollection = 0;

    if (SatoriUtil::IsConcurrent() && !m_deferredSweepRegions->IsEmpty())
    {
        m_deferredSweepCount = m_deferredSweepRegions->Count();
        m_activeHelperFn = &SatoriRecycler::DrainDeferredSweepQueueHelp;
        MaybeAskForHelp();
    }
    else
    {
        // no deferred sweep, can update occupancy earlier (this is optional)
        m_occupancy[1] = m_occupancyAcc[1];
        m_occupancy[2] = m_occupancyAcc[2];
    }

    // we are done with gen0 here, update the occupancy
    m_occupancy[0] = m_occupancyAcc[0];

    m_trimmer->SetOkToRun();
}

void SatoriRecycler::RunWithHelp(void(SatoriRecycler::* method)())
{
    m_activeHelperFn = method;
    do
    {
        (this->*method)();
        YieldProcessor();
    } while (m_activeHelpers > 0);

    m_activeHelperFn = nullptr;
    // make sure everyone sees the new Fn before waiting for helpers to drain.
    MemoryBarrier();
    while (m_activeHelpers > 0)
    {
        // TUNING: are we wasting too many cycles here?
        //         should we find something more useful to do than mmpause,
        //         or perhaps Sleep(0) after a few spins?
        YieldProcessor();
    }
}

void SatoriRecycler::BlockingMark()
{
    // tell EE we will be marking
    // NB: we may have done some marking concurrently already, but anything that watches
    //     for GC marking only cares about blocking/final marking phase.
    GCToEEInterface::BeforeGcScanRoots(m_condemnedGeneration, /* is_bgc */ false, /* is_concurrent */ false);

    MarkStrongReferences();
    ASSERT_NO_WORK();
    DependentHandlesScan();
    ASSERT_NO_WORK();

    // Tell EE we have done marking strong references before scanning finalizables.
    // What actually happens here is detaching COM wrappers when exposed object is not reachable.
    // The object may stay around for finalization and become F-reachable, so the check needs to happen here.
    ScanContext sc;
    GCToEEInterface::AfterGcScanRoots(m_condemnedGeneration, max_generation, &sc);

    ShortWeakPtrScan();
    ScanFinalizables();
    MarkNewReachable();
    ASSERT_NO_WORK();
    LongWeakPtrScan();
}

void SatoriRecycler::DrainAndCleanWorker()
{
    do
    {
        DrainMarkQueues();
        CleanCards();
    } while (!m_workList->IsEmpty() || HasDirtyCards());
}

void SatoriRecycler::MarkNewReachable()
{
    // it is nearly impossible to have dirty cards and empty work queue.
    // basically only if we completely ran out of chunks.
    // we will just check for simplicity.
    while (!m_workList->IsEmpty() || HasDirtyCards())
    {
        RunWithHelp(&SatoriRecycler::DrainAndCleanWorker);
        DependentHandlesRescan();
    }
}

void SatoriRecycler::DependentHandlesScan()
{
    DependentHandlesInitialScan();
    MarkNewReachable();
}

void SatoriRecycler::MarkStrongReferences()
{
    // stack and handles do not track dirtying writes,
    // therefore we must rescan even after concurrent mark completed its work
    IncrementRootScanTicket();
    SatoriHandlePartitioner::StartNextScan();
    RunWithHelp(&SatoriRecycler::MarkStrongReferencesWorker);
}

void SatoriRecycler::MarkStrongReferencesWorker()
{
    // in concurrent case the current stack is unlikely to have anything unmarked
    // it is still preferred to look at own stack on the same thread.
    // this will also ask for helpers.
    MarkOwnStackAndDrainQueues();
    MarkHandles();
    MarkAllStacksFinalizationAndDemotedRoots();

    if (m_condemnedGeneration == 1)
    {
        DrainMarkQueues();
        MarkThroughCards();
    }

    DrainAndCleanWorker();
}

void SatoriRecycler::ASSERT_NO_WORK()
{
    _ASSERTE(m_workList->IsEmpty());

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            _ASSERTE(page->CardState() < Satori::CardState::PROCESSING);
        }
    );
}

void SatoriRecycler::ReportThreadAllocBytes(int64_t bytes, bool isLive)
{
    if (isLive)
    {
        _ASSERTE(IsBlockingPhase());
        m_currentAllocBytesLiveThreads += bytes;
    }
    else
    {
        Interlocked::ExchangeAdd64(&m_currentAllocBytesDeadThreads, bytes);
    }
}

int64_t SatoriRecycler::GetTotalAllocatedBytes()
{
    return m_totalAllocBytes;
}

/* static */
void SatoriRecycler::DeactivateFn(gc_alloc_context* gcContext, void* param)
{
    SatoriAllocationContext* context = (SatoriAllocationContext*)gcContext;
    SatoriRecycler* recycler = (SatoriRecycler*)param;

    context->Deactivate(recycler, /*detach*/ recycler->m_promoteAllRegions);
    recycler->ReportThreadAllocBytes(context->alloc_bytes + context->alloc_bytes_uoh, /*islive*/ true);
}

void SatoriRecycler::DeactivateAllocatingRegions()
{
    m_currentAllocBytesLiveThreads = 0;
    GCToEEInterface::GcEnumAllocContexts(DeactivateFn, m_heap->Recycler());
    m_totalAllocBytes = m_currentAllocBytesLiveThreads + m_currentAllocBytesDeadThreads;

    // make shared regions parseable, in case we have byrefs pointing to them.
    m_heap->Allocator()->DeactivateSharedRegions(m_promoteAllRegions);
}

void SatoriRecycler::PushToMarkQueuesSlow(SatoriWorkChunk*& currentWorkChunk, SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion()->Generation() <= m_condemnedGeneration);

    if (currentWorkChunk)
    {
        m_workList->Push(currentWorkChunk);
        MaybeAskForHelp();
    }

    currentWorkChunk = m_heap->Allocator()->TryGetWorkChunk();
    if (currentWorkChunk)
    {
        currentWorkChunk->Push(o);
    }
    else
    {
        // handle mark overflow by dirtying the cards
        o->DirtyCardsForContent();

        // since this o will not be popped from the work queue,
        // check for unmovable here
        if (o->IsUnmovable())
        {
            o->ContainingRegion()->HasPinnedObjects() = true;
        }
    }
}

/* static */
template <bool isConservative>
void SatoriRecycler::MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    size_t location = (size_t)*ppObject;
    if (location == 0)
    {
        return;
    }

    MarkContext* markContext = (MarkContext*)sc->_unused1;
    SatoriObject* o = (SatoriObject*)location;

    if (flags & GC_CALL_INTERIOR)
    {
        // byrefs may point to stack, use checked here
        SatoriRegion* containingRegion = markContext->m_heap->RegionForAddressChecked(location);
        if (!containingRegion ||
            (isConservative && (containingRegion->Generation() < 0)))
        {
            return;
        }

        o = containingRegion->FindObject(location);
        if (isConservative && o->IsFree())
        {
            return;
        }
    }
    else if (o->IsExternal())
    {
        return;
    }

    if (o->ContainingRegion()->Generation() <= markContext->m_condemnedGeneration)
    {
        if (!o->IsMarked())
        {
            o->SetMarkedAtomic();
            markContext->PushToMarkQueues(o);
        }

        if (flags & GC_CALL_PINNED)
        {
            o->ContainingRegion()->HasPinnedObjects() = true;
        }
    }
};

/* static */
template <bool isConservative>
void SatoriRecycler::UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    size_t location = (size_t)*ppObject;
    if (location == 0)
    {
        return;
    }

    SatoriObject* o = (SatoriObject*)location;
    if (flags & GC_CALL_INTERIOR)
    {
        MarkContext* markContext = (MarkContext*)sc->_unused1;
        // byrefs may point to stack, use checked here
        SatoriRegion* containingRegion = markContext->m_heap->RegionForAddressChecked(location);
        if (!containingRegion ||
            (isConservative && (containingRegion->Generation() < 0)))
        {
            return;
        }

        o = containingRegion->FindObject(location);
        if (isConservative && o->IsFree())
        {
            return;
        }
    }

    ptrdiff_t ptr = *((ptrdiff_t*)o - 1);
    if (ptr < 0)
    {
        ptr = -ptr;
        if (flags & GC_CALL_INTERIOR)
        {
            *ppObject = (PTR_Object)(location + (ptr - o->Start()));
        }
        else
        {
            *ppObject = (PTR_Object)ptr;
        }
    }
};

template <bool isConservative>
void SatoriRecycler::MarkFnConcurrent(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    size_t location = (size_t)VolatileLoadWithoutBarrier(ppObject);
    if (location == 0)
    {
        return;
    }

#ifdef DEBUG_DestroyedHandleValue
    // we can race with destroy handle during concurrent scan
    if (location == (size_t)DEBUG_DestroyedHandleValue)
        return;
#endif //DEBUG_DestroyedHandleValue

    SatoriRegion* containingRegion;
    MarkContext* markContext = (MarkContext*)sc->_unused1;
    SatoriObject* o = (SatoriObject*)location;

    if (flags & GC_CALL_INTERIOR)
    {
        // byrefs may point to stack, use checked here
        containingRegion = markContext->m_heap->RegionForAddressChecked(location);
        if (!containingRegion)
        {
            return;
        }

        // since this is concurrent, in a conservative case an allocation could have caused a split
        // that shortened the found region and the region no longer contains the ref (which means the ref is not real).
        // Note that the split could only happen before we are done with allocator queue (which is synchronising)
        // and assign 0+ gen to the region.
        // So check here in the opposite order - first that region is 0+ gen and then that the size is still right.
        if (isConservative &&
            (containingRegion->GenerationAcquire() < 0 || location >= containingRegion->End()))
        {
            return;
        }

        // Concurrent FindObject is unsafe in active regions. While ref may be in a real obj,
        // the path to it from the first obj or prev indexed may cross unparsable ranges.
        // The check must acquire to be sure we check before actually doing FindObject.
        if (containingRegion->MaybeAllocatingAcquire())
        {
            return;
        }

        o = containingRegion->FindObject(location);
        if (isConservative && o->IsFree())
        {
            return;
        }
    }
    else
    {
        if (o->IsExternal())
        {
            return;
        }

        containingRegion = o->ContainingRegion();
        // can't mark in regions which are tracking escapes, bitmap is in use
        if (containingRegion->MaybeEscapeTrackingAcquire())
        {
            return;
        }
    }

    // no need for fences here. a published obj cannot become gen2 concurrently. 
    if (containingRegion->Generation() <= markContext->m_condemnedGeneration)
    {
        if (!o->IsMarked())
        {
            o->SetMarkedAtomic();
            markContext->PushToMarkQueues(o);
        }

        if (flags & GC_CALL_PINNED)
        {
            o->ContainingRegion()->HasPinnedObjects() = true;
        }
    }
};

bool SatoriRecycler::MarkDemotedAndDrainQueuesConcurrent(int64_t deadline)
{
    _ASSERTE(!IsBlockingPhase());

    MarkContext markContext = MarkContext(this);

    // in blocking case we go through demoted together with marking all stacks
    // in concurrent case we do it here, since going through demoted does not need EE stopped.
    SatoriRegion* curRegion = m_ephemeralWithUnmarkedDemoted->TryPop();
    if (curRegion)
    {
        MaybeAskForHelp();
        do
        {
            MarkDemoted(curRegion, &markContext);
            PushToEphemeralQueues(curRegion);

            if (deadline && ((GCToOSInterface::QueryPerformanceCounter() - deadline) > 0))
            {
                if (markContext.m_WorkChunk != nullptr)
                {
                    m_workList->Push(markContext.m_WorkChunk);
                }

                return true;
            }
        } while ((curRegion = m_ephemeralWithUnmarkedDemoted->TryPop()));
    }

    return DrainMarkQueuesConcurrent(markContext.m_WorkChunk, deadline);
}

void SatoriRecycler::MarkOwnStackAndDrainQueues()
{
    MarkContext markContext = MarkContext(this);

    if (!IsHelperThread())
    {
        gc_alloc_context* aContext = GCToEEInterface::GetAllocContext();
        int threadScanTicket = VolatileLoadWithoutBarrier(&aContext->alloc_count);
        int currentScanTicket = GetRootScanTicket();
        if (threadScanTicket != currentScanTicket)
        {
            // claim our own stack for scanning
            if (Interlocked::CompareExchange(&aContext->alloc_count, currentScanTicket, threadScanTicket) == threadScanTicket)
            {
                MaybeAskForHelp();
                MarkOwnStack(aContext, &markContext);

                // in concurrent prep stage we do not drain after self-scanning as we prefer to suspend quickly
                if (!IsBlockingPhase())
                {
                    if (markContext.m_WorkChunk != nullptr)
                    {
                        m_workList->Push(markContext.m_WorkChunk);
                    }

                    return;
                }
            }
        }
    }

    DrainMarkQueues(markContext.m_WorkChunk);
}

void SatoriRecycler::MarkOwnStackOrDrainQueuesConcurrent(int64_t deadline)
{
    MarkContext markContext = MarkContext(this);

    if (!IsHelperThread())
    {
        gc_alloc_context* aContext = GCToEEInterface::GetAllocContext();
        int threadScanTicket = VolatileLoadWithoutBarrier(&aContext->alloc_count);
        int currentScanTicket = GetRootScanTicket();
        if (threadScanTicket != currentScanTicket)
        {
            // claim our own stack for scanning
            if (Interlocked::CompareExchange(&aContext->alloc_count, currentScanTicket, threadScanTicket) == threadScanTicket)
            {
                MaybeAskForHelp();
                MarkOwnStack(aContext, &markContext);

                // in concurrent prep stage we do not drain after self-scanning as we prefer to suspend quickly
                if (!IsBlockingPhase())
                {
                    if (markContext.m_WorkChunk != nullptr)
                    {
                        m_workList->Push(markContext.m_WorkChunk);
                    }

                    return;
                }
            }
        }
    }

    DrainMarkQueuesConcurrent(markContext.m_WorkChunk, deadline);
}

void SatoriRecycler::MarkOwnStack(gc_alloc_context* aContext, MarkContext* markContext)
{
    bool isBlockingPhase = IsBlockingPhase();
    if (!isBlockingPhase)
    {
        SatoriRegion* region = ((SatoriAllocationContext*)aContext)->RegularRegion();
        if (region && region->IsEscapeTracking())
        {
            region->StopEscapeTracking();
        }
    }

    ScanContext sc;
    sc.promotion = TRUE;
    sc._unused1 = markContext;

    if (SatoriUtil::IsConservativeMode())
        GCToEEInterface::GcScanCurrentStackRoots(isBlockingPhase ? MarkFn<true> : MarkFnConcurrent<true>, &sc);
    else
        GCToEEInterface::GcScanCurrentStackRoots(isBlockingPhase ? MarkFn<false> : MarkFnConcurrent<false>, &sc);
}

void SatoriRecycler::MarkDemoted(SatoriRegion* curRegion, MarkContext* markContext)
{
    _ASSERTE(curRegion->Generation() == 1);
    curRegion->HasUnmarkedDemotedObjects() = false;

    if (m_condemnedGeneration == 1)
    {
        _ASSERTE(!curRegion->MaybeEscapeTrackingAcquire());
        curRegion->HasPinnedObjects() = true;

        SatoriWorkChunk* gen2Objects = curRegion->DemotedObjects();
        while (gen2Objects)
        {
            for (int i = 0; i < gen2Objects->Count(); i++)
            {
                SatoriObject* o = gen2Objects->Item(i);
                o->SetMarkedAtomic();
                markContext->PushToMarkQueues(o);
            }

            gen2Objects = gen2Objects->Next();
        }
    }
    else
    {
        curRegion->FreeDemotedTrackers();
    }
}

void SatoriRecycler::MarkAllStacksFinalizationAndDemotedRoots()
{
    // mark roots for all stacks
    ScanContext sc;
    sc.promotion = TRUE;
    MarkContext markContext = MarkContext(this);
    sc._unused1 = &markContext;

    bool isBlockingPhase = IsBlockingPhase();

    if (SatoriUtil::IsConservativeMode())
        //generations are meaningless here, so we pass -1
        GCToEEInterface::GcScanRoots(isBlockingPhase ? MarkFn<true> : MarkFnConcurrent<true>, -1, -1, &sc);
    else
        GCToEEInterface::GcScanRoots(isBlockingPhase ? MarkFn<false> : MarkFnConcurrent<false>, -1, -1, &sc);

    SatoriFinalizationQueue* fQueue = m_heap->FinalizationQueue();
    if (fQueue->TryUpdateScanTicket(this->GetRootScanTicket()) &&
        fQueue->HasItems())
    {
        fQueue->ForEachObjectRef(
            [&](SatoriObject** ppObject)
            {
                SatoriObject* o = *ppObject;

                // can't mark in regions which are tracking escapes, bitmap is in use
                if (!isBlockingPhase && o->ContainingRegion()->MaybeEscapeTrackingAcquire())
                {
                    return;
                }

                if (!o->IsMarkedOrOlderThan(m_condemnedGeneration))
                {
                    o->SetMarkedAtomic();
                    markContext.PushToMarkQueues(o);
                }
            }
        );
    }

    // EE is stopped here for root marking, but this could be either
    // a part of the blocking phase or a part of concurrent GC 
    if (isBlockingPhase)
    {
        SatoriRegion* curRegion = m_ephemeralWithUnmarkedDemoted->TryPop();
        if (curRegion)
        {
            MaybeAskForHelp();
            do
            {
                MarkDemoted(curRegion, &markContext);
                PushToEphemeralQueues(curRegion);
            } while ((curRegion = m_ephemeralWithUnmarkedDemoted->TryPop()));
        }
    }
    else
    {
        // going through demoted queue does not require EE stopped,
        // so in concurrent case we will do that later.
        // but there could be demoted regions in reusable queue
        // we can mark tenured objects in those.
        SatoriRegion* curRegion = m_reusableRegionsAlternate->TryPop();
        if (curRegion)
        {
            MaybeAskForHelp();
            do
            {
                if (curRegion->HasUnmarkedDemotedObjects() &&
                    curRegion->ReusableFor() != SatoriRegion::ReuseLevel::Gen0)
                {
                    MarkDemoted(curRegion, &markContext);
                }

                if (curRegion->HasFreeSpaceInTopBucket())
                {
                    m_reusableRegions->Enqueue(curRegion);
                }
                else
                {
                    m_reusableRegions->Push(curRegion);
                }
            } while ((curRegion = m_reusableRegionsAlternate->TryPop()));
        }
    }

    if (markContext.m_WorkChunk != nullptr)
    {
        m_workList->Push(markContext.m_WorkChunk);
    }
}

bool SatoriRecycler::DrainMarkQueuesConcurrent(SatoriWorkChunk* srcChunk, int64_t deadline)
{
    if (!srcChunk)
    {
        srcChunk = m_workList->TryPop();
    }

    if (!m_workList->IsEmpty())
    {
        MaybeAskForHelp();
    }

    // just a crude measure of work performed to remind us to check for the deadline
    size_t objectCount = 0;
    SatoriWorkChunk* dstChunk = nullptr;
    SatoriObject* o = nullptr;

    auto markChildFn = [&](SatoriObject** ref)
    {
        SatoriObject* child = VolatileLoadWithoutBarrier(ref);
        if (child && !child->IsExternal())
        {
            objectCount++;
            SatoriRegion* childRegion = child->ContainingRegion();
            if (!childRegion->MaybeEscapeTrackingAcquire())
            {
                if (!child->IsMarkedOrOlderThan(m_condemnedGeneration))
                {
                    child->SetMarkedAtomic();
                    if (!dstChunk || !dstChunk->TryPush(child))
                    {
                        this->PushToMarkQueuesSlow(dstChunk, child);
                    }
                }
                return;
            }

            // cannot mark the child in a thread local region. just mark the ref as dirty to visit later.
            // if ref is outside of the containing region, it is a fake ref to collectible allocator.
            // dirty the MT location as if it points to the allocator object.
            // technically it does reference the allocator, by indirection.
            SatoriRegion* parentRegion = o->ContainingRegion();
            if ((size_t)ref - parentRegion->Start() > parentRegion->Size())
            {
                ref = (SatoriObject**)o->Start();
            }

            parentRegion->ContainingPage()->DirtyCardForAddressConcurrent((size_t)ref);
        }
    };

    while (srcChunk)
    {
        if (srcChunk->IsRange())
        {
            size_t start, end;
            srcChunk->GetRange(o, start, end);
            srcChunk->Clear();
            // mark children in the range
            o->ForEachObjectRef(markChildFn, start, end);
        }
        else
        {
            objectCount += srcChunk->Count();
            // drain srcChunk to dst chunk
            while (srcChunk->Count() > 0)
            {
                o = srcChunk->Pop();
                SatoriUtil::Prefetch(srcChunk->Peek());

                _ASSERTE(o->IsMarked());
                if (o->IsUnmovable())
                {
                    o->ContainingRegion()->HasPinnedObjects() = true;
                }

                // do not get engaged with huge objects, reschedule them as child ranges.
                if (o->ContainingRegion()->Size() > Satori::REGION_SIZE_GRANULARITY)
                {
                    ScheduleMarkAsChildRanges(o);
                    continue;
                }

                o->ForEachObjectRef(markChildFn, /* includeCollectibleAllocator */ true);
            }
        }

        // every once in a while check for the deadline
        // the objectCount number here is to
        // - amortize cost of QueryPerformanceCounter() and
        // - establish the minimum amount of work per help quantum
        if (deadline && objectCount > 4096)
        {
            if ((GCToOSInterface::QueryPerformanceCounter() - deadline) > 0)
            {
                m_workList->Push(srcChunk);
                if (dstChunk)
                {
                    m_workList->Push(dstChunk);
                }
                return true;
            }

            objectCount = 0;
        }

        // done with srcChunk       
        // if we have nonempty dstChunk (i.e. produced more work),
        // swap src and dst and continue
        if (dstChunk && dstChunk->Count() > 0)
        {
            SatoriWorkChunk* tmp = srcChunk;
            srcChunk = dstChunk;
            dstChunk = tmp;
        }
        else
        {
            m_heap->Allocator()->ReturnWorkChunk(srcChunk);
            srcChunk = m_workList->TryPop();
        }
    }

    if (dstChunk)
    {
        _ASSERTE(dstChunk->Count() == 0);
        m_heap->Allocator()->ReturnWorkChunk(dstChunk);
    }

    return false;
}

void SatoriRecycler::ScheduleMarkAsChildRanges(SatoriObject* o)
{
    if (o->RawGetMethodTable()->ContainsPointersOrCollectible())
    {
        size_t start = o->Start();
        size_t remains = o->Size();
        while (remains > 0)
        {
            SatoriWorkChunk* chunk = m_heap->Allocator()->TryGetWorkChunk();
            if (chunk == nullptr)
            {
                o->ContainingRegion()->ContainingPage()->DirtyCardsForRange(start, start + remains);
                remains = 0;
                break;
            }

            size_t len = min(Satori::REGION_SIZE_GRANULARITY, remains);
            chunk->SetRange(o, start, start + len);
            start += len;
            remains -= len;
            m_workList->Push(chunk);
        }

        // done with current object
        _ASSERTE(remains == 0);
    }
}

bool SatoriRecycler::ScheduleUpdateAsChildRanges(SatoriObject* o)
{
    if (o->RawGetMethodTable()->ContainsPointers())
    {
        size_t start = o->Start() + sizeof(size_t);
        size_t remains = o->Size() - sizeof(size_t);
        while (remains > 0)
        {
            SatoriWorkChunk* chunk = m_heap->Allocator()->TryGetWorkChunk();
            if (chunk == nullptr)
            {
                return false;
            }

            size_t len = min(Satori::REGION_SIZE_GRANULARITY, remains);
            chunk->SetRange(o, start, start + len);
            start += len;
            remains -= len;
            m_workList->Push(chunk);
        }

        // done with current object
        _ASSERTE(remains == 0);
    }

    return true;
}

void SatoriRecycler::DrainMarkQueues(SatoriWorkChunk* srcChunk)
{
    if (!srcChunk)
    {
        srcChunk = m_workList->TryPop();
    }

    if (!m_workList->IsEmpty())
    {
        MaybeAskForHelp();
    }

    SatoriWorkChunk* dstChunk = nullptr;

    auto markChildFn = [&](SatoriObject** ref)
    {
        SatoriObject* child = *ref;
        if (child &&
            !child->IsExternal() &&
            !child->IsMarkedOrOlderThan(m_condemnedGeneration))
        {
            child->SetMarkedAtomic();
            // put more work, if found, into dstChunk
            if (!dstChunk || !dstChunk->TryPush(child))
            {
                this->PushToMarkQueuesSlow(dstChunk, child);
            }
        }
    };

    while (srcChunk)
    {
        if (srcChunk->IsRange())
        {
            SatoriObject* o;
            size_t start, end;
            srcChunk->GetRange(o, start, end);
            srcChunk->Clear();
            // mark children in the range
            o->ForEachObjectRef(markChildFn, start, end);
        }
        else
        {
            // mark objects in the chunk
            while (srcChunk->Count() > 0)
            {
                SatoriObject* o = srcChunk->Pop();
                SatoriUtil::Prefetch(srcChunk->Peek());

                _ASSERTE(o->IsMarked());
                if (o->IsUnmovable())
                {
                    o->ContainingRegion()->HasPinnedObjects() = true;
                }

                // do not get engaged with huge objects, reschedule them as child ranges.
                if (o->ContainingRegion()->Size() > Satori::REGION_SIZE_GRANULARITY)
                {
                    ScheduleMarkAsChildRanges(o);
                    continue;
                }

                o->ForEachObjectRef(markChildFn, /* includeCollectibleAllocator */ true);
            }
        }

        // done with srcChunk
        // if we have nonempty dstChunk (i.e. produced more work),
        // swap src and dst and continue
        if (dstChunk && dstChunk->Count() > 0)
        {
            SatoriWorkChunk* tmp = srcChunk;
            srcChunk = dstChunk;
            dstChunk = tmp;
        }
        else
        {
            m_heap->Allocator()->ReturnWorkChunk(srcChunk);
            srcChunk = m_workList->TryPop();
        }
    }

    if (dstChunk)
    {
        _ASSERTE(dstChunk->Count() == 0);
        m_heap->Allocator()->ReturnWorkChunk(dstChunk);
    }
}

// Just a number that is likely be different for different threads
// making the same call.
// mix in the GC index to not get stuck with the same combination, in case it is not good.
size_t ThreadSpecificNumber(int64_t gcIndex)
{
    size_t result = (((size_t)&result ^ (size_t)gcIndex) * 11400714819323198485llu) >> 32;
    return result;
}

int64_t SatoriRecycler::GlobalGcIndex()
{
    return m_gcCount[0];
}

struct ConcurrentCardsRestart
{
    int64_t gcIndex;
    SatoriPage* page;
    size_t ii;
    size_t offset;
};

thread_local
ConcurrentCardsRestart t_concurrentCardState;

bool SatoriRecycler::MarkThroughCardsConcurrent(int64_t deadline)
{
    SatoriWorkChunk* dstChunk = nullptr;
    bool revisit = false;

    int64_t gcIndex = GlobalGcIndex();
    ConcurrentCardsRestart* pRestart = &t_concurrentCardState;
    if (pRestart->gcIndex != gcIndex)
    {
        pRestart->gcIndex = gcIndex;
        pRestart->page = nullptr;
        pRestart->ii = 0;
        pRestart->offset = ThreadSpecificNumber(GlobalGcIndex());
    }

    m_heap->ForEachPageUntil(
        [&](SatoriPage* page)
        {
            int8_t pageState = page->CardState();
            if (pageState != Satori::CardState::BLANK)
            {
                int8_t currentScanTicket = GetCardScanTicket();
                if (page->ScanTicket() == currentScanTicket)
                {
                    // this is not a timeout, continue to next page
                    return false;
                }

                // there is unfinished page. Maybe should ask for help
                MaybeAskForHelp();

                size_t groupCount = page->CardGroupCount();

                // if restarting with the same page, continue with the last ii and offset
                size_t offset = pRestart->offset;
                size_t ii = pRestart->ii;
                if (pRestart->page != page)
                {
                    // it is a new page
                    pRestart->page = page;
                    ii = 0;
                }

                while (ii < groupCount)
                {
                    size_t i = (ii + offset) % groupCount;
                    ii ++;

                    int8_t groupState = page->CardGroupState(i);
                    if (groupState >= Satori::CardState::REMEMBERED)
                    {
                        int8_t groupTicket = page->CardGroupScanTicket(i);
                        if (groupTicket == currentScanTicket)
                        {
                            continue;
                        }

                        // claim the group as complete
                        page->CardGroupScanTicket(i) = currentScanTicket;

                        // now we have to finish, since we have clamed the group
                        // NB: two threads may claim the same group and do overlapping work
                        //     that is correct, but redundant. We could claim using interlocked operation
                        //     and avoid that, but such collisions appear to be too rare to worry about.
                        //     It may be worth watching this in the future though.

                        //NB: It is safe to get a region even if region map may be changing because
                        //    a region with remembered/dirty marks must be there and cannot be destroyed.
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        // we should not be marking when there could be dead objects
                        _ASSERTE(!region->HasMarksSet());

                        // sometimes we set cards without checking dst generation, but REMEMBERED only has meaning in tenured
                        if (region->Generation() < 2)
                        {
                            // This is optimization. Not needed for correctness.
                            // If not dirty, we wipe the group, to not look at this again in the next scans.
                            // It must use interlocked, even if not concurrent - in case it gets dirty by parallel mark (it can since it is gen1)
                            // wiping actual cards does not matter, we will look at them only if group is dirty,
                            // and then cleaner will reset them appropriately.
                            // there is no marking work in this region, so ticket is also irrelevant. it will be wiped if region gets promoted
                            if (groupState == Satori::CardState::REMEMBERED)
                            {
                                Interlocked::CompareExchange(&page->CardGroupState(i), Satori::CardState::BLANK, Satori::CardState::REMEMBERED);
                            }

                            continue;
                        }

                        _ASSERTE(groupTicket == 0 || currentScanTicket - groupTicket <= 2);
                        const int8_t resetValue = Satori::CardState::REMEMBERED;
                        int8_t* cards = page->CardsForGroup(i);

                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for 0
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == 0)
                            {
                                j += sizeof(size_t) - 1;
                                continue;
                            }

                            // skip empty cards
                            if (!cards[j])
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                cards[j] = resetValue;
                            } while (++j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j]);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);

                            // NOTE: We could do this only if we cleaned any of the cards, but it does not seem worth checking that.
                            //       Most likely we cleaned something.
                            // read marks after cleaning cards
                            MemoryBarrier();

                            do
                            {
                                o->ForEachObjectRef(
                                    [&](SatoriObject** ref)
                                    {
                                        SatoriObject* child = VolatileLoadWithoutBarrier(ref);
                                        if (child && !child->IsExternal())
                                        {
                                            SatoriRegion* childRegion = child->ContainingRegion();
                                            if (!childRegion->MaybeEscapeTrackingAcquire())
                                            {
                                                if (!child->IsMarkedOrOlderThan(1))
                                                {
                                                    child->SetMarkedAtomic();
                                                    if (!dstChunk || !dstChunk->TryPush(child))
                                                    {
                                                        this->PushToMarkQueuesSlow(dstChunk, child);
                                                    }
                                                }
                                                return;
                                            }

                                            // cannot mark stuff in thread local regions. just mark as dirty to visit later.
                                            // if ref is outside of the containing region, it is a fake ref to collectible allocator.
                                            // dirty the MT location as if it points to the allocator object
                                            // technically it does reference the allocator, by indirection.
                                            SatoriRegion* parentRegion = o->ContainingRegion();
                                            if ((size_t)ref - parentRegion->Start() > parentRegion->Size())
                                            {
                                                ref = (SatoriObject**)o->Start();
                                            }

                                            parentRegion->ContainingPage()->DirtyCardForAddressConcurrent((size_t)ref);
                                        }
                                    }, start, end);
                                o = o->Next();
                            } while (o->Start() < objLimit);
                        }

                        _ASSERTE(deadline != 0);
                        if (GCToOSInterface::QueryPerformanceCounter() - deadline > 0)
                        {
                            // timed out, there could be more work
                            // save where we would restart if we see this page again
                            pRestart->ii = ii;
                            revisit = true;
                            return true;
                        }
                    }
                }

                // All groups/cards are accounted in this page - either visited or claimed.
                // No marking work is left here, set the ticket to indicate that.
                page->ScanTicket() = currentScanTicket;
            }

            // not a timeout, continue iterating
            return false;
        }
    );

    if (dstChunk)
    {
        m_workList->Push(dstChunk);
    }

    return revisit;
}

bool SatoriRecycler::ScanDirtyCardsConcurrent(int64_t deadline)
{
    _ASSERTE(m_condemnedGeneration == 2);

    SatoriWorkChunk* dstChunk = nullptr;
    bool revisit = false;

    int64_t gcIndex = GlobalGcIndex();
    ConcurrentCardsRestart* pRestart = &t_concurrentCardState;
    if (pRestart->gcIndex != gcIndex)
    {
        pRestart->gcIndex = gcIndex;
        pRestart->page = nullptr;
        pRestart->ii = 0;
        pRestart->offset = ThreadSpecificNumber(GlobalGcIndex());
    }

    m_heap->ForEachPageUntil(
        [&](SatoriPage* page)
        {
            int8_t pageState = page->CardState();
            if (pageState == Satori::CardState::DIRTY)
            {
                int8_t currentScanTicket = GetCardScanTicket();
                if (page->ScanTicket() == currentScanTicket)
                {
                    // this is not a timeout, continue to next page
                    return false;
                }

                // there is unfinished page. Maybe should ask for help
                MaybeAskForHelp();

                size_t groupCount = page->CardGroupCount();

                // if restarting with the same page, continue with the last ii and offset
                size_t offset = pRestart->offset;
                size_t ii = pRestart->ii;
                if (pRestart->page != page)
                {
                    // it is a new page
                    pRestart->page = page;
                    ii = 0;
                }

                while (ii < groupCount)
                {
                    size_t i = (ii + offset) % groupCount;
                    ii++;

                    int8_t groupState = page->CardGroupState(i);
                    if (groupState == Satori::CardState::DIRTY)
                    {
                        int8_t groupTicket = page->CardGroupScanTicket(i);
                        if (groupTicket == currentScanTicket)
                        {
                            continue;
                        }

                        // claim the group as complete, now it is ours
                        page->CardGroupScanTicket(i) = currentScanTicket;

                        //NB: It is safe to get a region even if region map may be changing because
                        //    a region with dirty marks must be there and cannot be destroyed.
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        // we should not be marking when there could be dead objects
                        _ASSERTE(!region->HasMarksSet());

                        const int8_t resetValue = region->Generation() >= 2 ? Satori::CardState::REMEMBERED : Satori::CardState::EPHEMERAL;

                        // allocating region is not parseable.
                        if (region->MaybeAllocatingAcquire())
                        {
                            continue;
                        }

                        int8_t* cards = page->CardsForGroup(i);

                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for 0
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == 0)
                            {
                                j += sizeof(size_t) - 1;
                                continue;
                            }

                            // skip nondirty
                            if (cards[j] != Satori::CardState::DIRTY)
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                cards[j] = resetValue;
                            } while (++j < Satori::CARD_BYTES_IN_CARD_GROUP &&
                                cards[j] == Satori::CardState::DIRTY);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);

                            // read marks after cleaning cards
                            MemoryBarrier();

                            do
                            {
                                o = region->SkipUnmarked(o, objLimit);
                                if (o->Start() == objLimit)
                                {
                                    break;
                                }

                                // if (o->IsMarked())
                                {
                                    o->ForEachObjectRef(
                                        [&](SatoriObject** ref)
                                        {
                                            SatoriObject* child = VolatileLoadWithoutBarrier(ref);
                                            if (child && !child->IsExternal())
                                            {
                                                SatoriRegion* childRegion = child->ContainingRegion();
                                                if (!childRegion->MaybeEscapeTrackingAcquire())
                                                {
                                                    if (!child->IsMarkedOrOlderThan(2))
                                                    {
                                                        child->SetMarkedAtomic();
                                                        if (!dstChunk || !dstChunk->TryPush(child))
                                                        {
                                                            this->PushToMarkQueuesSlow(dstChunk, child);
                                                        }
                                                    }

                                                    return;
                                                }

                                                // cannot mark stuff in thread local regions. just mark as dirty to visit later.
                                                // if ref is outside of the containing region, it is a fake ref to collectible allocator.
                                                // dirty the MT location as if it points to the allocator object
                                                // technically it does reference the allocator, by indirection.
                                                SatoriRegion* parentRegion = o->ContainingRegion();
                                                if ((size_t)ref - parentRegion->Start() > parentRegion->Size())
                                                {
                                                    ref = (SatoriObject**)o->Start();
                                                }

                                                parentRegion->ContainingPage()->DirtyCardForAddressConcurrent((size_t)ref);
                                            }
                                        }, start, end);
                                }
                                o = o->Next();
                            } while (o->Start() < objLimit);
                        }

                        _ASSERTE(deadline != 0);
                        if (GCToOSInterface::QueryPerformanceCounter() - deadline > 0)
                        {
                            // timed out, there could be more work
                            revisit = true;
                            return true;
                        }
                    }
                }

                // All groups/cards are accounted in this page - either visited or claimed.
                // No marking work is left here, set the ticket to indicate that.
                page->ScanTicket() = currentScanTicket;
            }

            // not a timeout, continue iterating
            return false;
        }
    );

    if (dstChunk)
    {
        m_workList->Push(dstChunk);
    }

    return revisit;
}

void SatoriRecycler::MarkThroughCards()
{
    SatoriWorkChunk* dstChunk = nullptr;

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            int8_t pageState = page->CardState();
            if (pageState != Satori::CardState::BLANK)
            {
                int8_t currentScanTicket = GetCardScanTicket();
                if (page->ScanTicket() == currentScanTicket)
                {
                    // continue to next page
                    return;
                }

                // there is unfinished page. Maybe should ask for help
                MaybeAskForHelp();

                size_t groupCount = page->CardGroupCount();
                // add thread specific offset, to separate somewhat what threads read
                size_t offset = ThreadSpecificNumber(GlobalGcIndex());
                for (size_t ii = 0; ii < groupCount; ii++)
                {
                    size_t i = (offset + ii) % groupCount;
                    int8_t groupState = page->CardGroupState(i);
                    if (groupState >= Satori::CardState::REMEMBERED)
                    {
                        int8_t groupTicket = page->CardGroupScanTicket(i);
                        if (groupTicket == currentScanTicket)
                        {
                            continue;
                        }

                        // claim the group as complete, now we have to finish
                        page->CardGroupScanTicket(i) = currentScanTicket;

                        //NB: It is safe to get a region even if region map may be changing because
                        //    a region with remembered/dirty mark must be there and cannot be destroyed.
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        // we should not be marking when there could be dead objects
                        _ASSERTE(!region->HasMarksSet());

                        // sometimes we set cards without checking dst generation, but REMEMBERED only has meaning in tenured
                        if (region->Generation() < 2)
                        {
                            // This is optimization. Not needed for correctness.
                            // If not dirty, we wipe the group, to not look at this again in the next scans.
                            // It must use interlocked, even if not concurrent - in case it gets dirty by parallel mark (it can since it is gen1)
                            // wiping actual cards does not matter, we will look at them only if group is dirty,
                            // and then cleaner will reset them appropriately.
                            // there is no marking work in this region, so ticket is also irrelevant. it will be wiped if region gets promoted
                            if (groupState == Satori::CardState::REMEMBERED)
                            {
                                Interlocked::CompareExchange(&page->CardGroupState(i), Satori::CardState::BLANK, Satori::CardState::REMEMBERED);
                            }

                            continue;
                        }

                        _ASSERTE(groupTicket == 0 || currentScanTicket - groupTicket <= 2);
                        const int8_t resetValue = Satori::CardState::REMEMBERED;
                        int8_t* cards = page->CardsForGroup(i);

                        // clean the group if dirty, but must do that before reading the cards.
                        if (groupState == Satori::CardState::DIRTY)
                        {
                            Interlocked::CompareExchange(&page->CardGroupState(i), resetValue, Satori::CardState::DIRTY);
                        }

                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for 0
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == 0)
                            {
                                j += sizeof(size_t) - 1;
                                continue;
                            }

                            // skip empty cards
                            if (!cards[j])
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                if (cards[j] != resetValue)
                                {
                                    // clean the card since it is going to be visited and marked through.
                                    // order relative to visiting the objects is unimportant since fields are not changing
                                    cards[j] = resetValue;
                                }
                                j++;
                            } while (j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j]);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);
                            do
                            {
                                o->ForEachObjectRef(
                                    [&](SatoriObject** ref)
                                    {
                                        SatoriObject* child = VolatileLoadWithoutBarrier(ref);
                                        if (child &&
                                            !child->IsExternal() &&
                                            !child->IsMarkedOrOlderThan(1))
                                        {
                                            child->SetMarkedAtomic();
                                            if (!dstChunk || !dstChunk->TryPush(child))
                                            {
                                                this->PushToMarkQueuesSlow(dstChunk, child);
                                            }
                                        }
                                    }, start, end);
                                o = o->Next();
                            } while (o->Start() < objLimit);
                        }
                    }
                }

                // All groups/cards are accounted in this page - either visited or claimed.
                // No updating work is left for this page, set the ticket to indicate that.
                page->ScanTicket() = currentScanTicket;
            }
        }
    );

    if (dstChunk)
    {
        m_workList->Push(dstChunk);
    }
}

bool SatoriRecycler::HasDirtyCards()
{
    bool isDirty = false;
    m_heap->ForEachPageUntil([&](SatoriPage* page) { return page->CardState() == Satori::CardState::DIRTY ? (isDirty = true) : false; });
    return isDirty;
}

// cleaning is not concurrent, but could be parallel
void SatoriRecycler::CleanCards()
{
    SatoriWorkChunk* dstChunk = nullptr;

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            // NOTE: we may concurrently make cards dirty due to mark stack overflow.
            // Thus the page must be checked first, then group, then cards.
            // Dirtying due to overflow will have to do writes in the opposite order. (the barrier dirtying can be unordered though)
            int8_t pageState = page->CardState();
            if (pageState >= Satori::CardState::PROCESSING)
            {
                // move the page to PROCESSING state and make sure it does not reorder past reading the card group states
                pageState = Interlocked::CompareExchange(& page->CardState(), Satori::CardState::PROCESSING, Satori::CardState::DIRTY);
                if (pageState < Satori::CardState::PROCESSING)
                {
                    return;
                }

                // there is unfinished page. Maybe should ask for help
                MaybeAskForHelp();

                size_t groupCount = page->CardGroupCount();
                // add thread specific offset, to separate somewhat what threads read
                size_t offset = ThreadSpecificNumber(GlobalGcIndex());

                for (size_t ii = 0; ii < groupCount; ii++)
                {
                    size_t i = (offset + ii) % groupCount;
                    int8_t groupState = page->CardGroupState(i);
                    if (groupState == Satori::CardState::DIRTY)
                    {
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        const int8_t resetValue = region->Generation() >= 2 ? Satori::CardState::REMEMBERED : Satori::CardState::EPHEMERAL;

                        // clean the group, but must do that before reading the cards.
                        if (Interlocked::CompareExchange(&page->CardGroupState(i), resetValue, Satori::CardState::DIRTY) != Satori::CardState::DIRTY)
                        {
                            // at this point in time the card group is no longer dirty, try the next one
                            continue;
                        }

                        bool considerAllMarked = region->Generation() > m_condemnedGeneration;

                        _ASSERTE(Satori::CardState::EPHEMERAL == (int8_t)0x80);
                        const size_t unsetValue = region->Generation() >= 2 ?
                            Satori::CardState::BLANK :
                            0x8080808080808080;

                        int8_t* cards = page->CardsForGroup(i);
                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for unset value
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == unsetValue)
                            {
                                j += sizeof(size_t) - 1;
                                continue;
                            }

                            if (cards[j] != Satori::CardState::DIRTY)
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                // clean the card since it is going to be visited and marked through.
                                cards[j++] = resetValue;
                            } while (j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j] == Satori::CardState::DIRTY);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);

                            // do not allow card cleaning to delay until after checking IsMarked
                            if (!considerAllMarked)
                            {
                                MemoryBarrier();
                            }

                            do
                            {
                                if (!considerAllMarked)
                                {
                                    o = region->SkipUnmarked(o, objLimit);
                                    if (o->Start() == objLimit)
                                    {
                                        break;
                                    }
                                }

                                // if (considerAllMarked || o->IsMarked())
                                {
                                    o->ForEachObjectRef(
                                        [&](SatoriObject** ref)
                                        {
                                            SatoriObject* child = *ref;
                                            if (child &&
                                                !child->IsExternal() &&
                                                !child->IsMarkedOrOlderThan(m_condemnedGeneration))
                                            {
                                                child->SetMarkedAtomic();
                                                if (!dstChunk || !dstChunk->TryPush(child))
                                                {
                                                    this->PushToMarkQueuesSlow(dstChunk, child);
                                                }
                                            }
                                        }, start, end);
                                }
                                o = o->Next();
                            } while (o->Start() < objLimit);
                        }
                    }
                }

                // we do not see more cleaning work so clean the page state, use interlocked in case the page went dirty while we were working on it
                int8_t origState = Interlocked::CompareExchange(&page->CardState(), Satori::CardState::REMEMBERED, Satori::CardState::PROCESSING);
                _ASSERTE(origState != Satori::CardState::BLANK);
            }
        }
    );

    if (dstChunk)
    {
        m_workList->Push(dstChunk);
    }
}

void SatoriRecycler::UpdatePointersThroughCards()
{
    SatoriWorkChunk* dstChunk = nullptr;
    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            int8_t pageState = page->CardState();
            _ASSERTE(pageState < Satori::CardState::PROCESSING);
            if (pageState == Satori::CardState::REMEMBERED)
            {
                int8_t currentScanTicket = GetCardScanTicket();
                if (page->ScanTicket() == currentScanTicket)
                {
                    // continue to next page
                    return;
                }

                // there is unfinished page. Maybe should ask for help
                MaybeAskForHelp();

                size_t groupCount = page->CardGroupCount();
                // add thread specific offset, to separate somewhat what threads read
                size_t offset = ThreadSpecificNumber(GlobalGcIndex());
                for (size_t ii = 0; ii < groupCount; ii++)
                {
                    size_t i = (offset + ii) % groupCount;
                    int8_t groupState = page->CardGroupState(i);
                    _ASSERTE(groupState < Satori::CardState::PROCESSING);
                    if (groupState == Satori::CardState::REMEMBERED)
                    {
                        int8_t groupTicket = page->CardGroupScanTicket(i);
                        if (groupTicket == currentScanTicket)
                        {
                            continue;
                        }

                        // claim the group as complete, now we have to finish
                        page->CardGroupScanTicket(i) = currentScanTicket;

                        SatoriRegion* region = page->RegionForCardGroup(i);
                        _ASSERTE(region->Generation() >= 2);

                        _ASSERTE(groupTicket == 0 || currentScanTicket - groupTicket <= 2);
                        int8_t* cards = page->CardsForGroup(i);
                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for 0
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == 0)
                            {
                                j += sizeof(size_t) - 1;
                                continue;
                            }

                            _ASSERTE(cards[j] < Satori::CardState::PROCESSING);
                            if (cards[j] != Satori::CardState::REMEMBERED)
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                _ASSERTE(cards[j] < Satori::CardState::PROCESSING);
                                // wipe the card. if the card is still valid we will set it back shortly.
                                // this is an optimization. we do not need this for correctness.
                                cards[j] = Satori::CardState::BLANK;
                                j++;
                            } while (j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j] == Satori::CardState::REMEMBERED);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);
                            do
                            {
                                o->ForEachObjectRef(
                                    [&](SatoriObject** ppObject)
                                    {
                                        // prevent re-reading o, someone else could be doing the same update.
                                        SatoriObject* child = VolatileLoadWithoutBarrier(ppObject);
                                        if (child && !child->IsExternal())
                                        {
                                            ptrdiff_t ptr = *((ptrdiff_t*)child - 1);
                                            if (ptr < 0)
                                            {
                                                _ASSERTE(child->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                                                child = (SatoriObject*)-ptr;
                                                VolatileStoreWithoutBarrier(ppObject, child);
                                            }

                                            if (child->ContainingRegion()->Generation() < 2)
                                            {
                                                page->SetCardForAddressOnly((size_t)ppObject);
                                            }
                                        }
                                    },
                                    max(start, o->Start() + 1),  // do not include allocator object.
                                    end
                                );
                                o = o->Next();
                            } while (o->Start() < objLimit);
                        }
                    }
                }

                // All groups/cards are accounted in this page - either visited or claimed.
                // No updating work is left for this page, set the ticket to indicate that.
                page->ScanTicket() = currentScanTicket;
            }
        }
    );
}

bool SatoriRecycler::MarkHandles(int64_t deadline)
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc.concurrent = !IsBlockingPhase();
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    bool revisit = SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            // there is work for us, so maybe there is more
            MaybeAskForHelp();
            _ASSERTE(m_condemnedGeneration > 0);

            sc.thread_number = p;
            GCScan::GcScanHandles(
                IsBlockingPhase() ? MarkFn</*isConservative*/ false> : MarkFnConcurrent</*isConservative*/ false>,
                m_condemnedGeneration,
                2,
                &sc);
        },
        deadline
    );

    if (c.m_WorkChunk != nullptr)
    {
        m_workList->Push(c.m_WorkChunk);
    }

    return revisit;
}

void SatoriRecycler::ShortWeakPtrScan()
{
    SatoriHandlePartitioner::StartNextScan();
    RunWithHelp(&SatoriRecycler::ShortWeakPtrScanWorker);
}

void SatoriRecycler::ShortWeakPtrScanWorker()
{
    ScanContext sc;
    sc.promotion = TRUE;
    SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            GCScan::GcShortWeakPtrScan(m_condemnedGeneration, 2, &sc);
        }
    );
}

void SatoriRecycler::LongWeakPtrScan()
{
    SatoriHandlePartitioner::StartNextScan();
    m_syncBlockCacheScanDone = 0;
    RunWithHelp(&SatoriRecycler::LongWeakPtrScanWorker);
}

void SatoriRecycler::LongWeakPtrScanWorker()
{
    ScanContext sc;
    sc.promotion = TRUE;
    SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            GCScan::GcWeakPtrScan(m_condemnedGeneration, 2, &sc);
        }
    );

    if (!m_syncBlockCacheScanDone &&
        Interlocked::CompareExchange(&m_syncBlockCacheScanDone, 1, 0) == 0)
    {
        // scan for deleted entries in the syncblk cache
        // sc is not really used, but must have "promotion == TRUE" if HeapVerify is on.
        GCScan::GcWeakPtrScanBySingleThread(m_condemnedGeneration, 2, &sc);
    }
}

void SatoriRecycler::ScanFinalizables()
{
    m_heap->FinalizationQueue()->ResetOverflow(m_condemnedGeneration);
    RunWithHelp(&SatoriRecycler::ScanAllFinalizableRegionsWorker);
    RunWithHelp(&SatoriRecycler::QueueCriticalFinalizablesWorker);
}

void SatoriRecycler::ScanAllFinalizableRegionsWorker()
{
    MarkContext c = MarkContext(this);

    ScanFinalizableRegions(m_ephemeralFinalizationTrackingRegions, &c);
    _ASSERTE(m_reusableRegions->IsEmpty());
    
    if (m_condemnedGeneration == 2)
    {
        ScanFinalizableRegions(m_tenuredFinalizationTrackingRegions, &c);
    }

    if (c.m_WorkChunk != nullptr)
    {
        GCToEEInterface::EnableFinalization(true);
        m_workList->Push(c.m_WorkChunk);
    }
}

void SatoriRecycler::ScanFinalizableRegions(SatoriRegionQueue* queue, MarkContext* markContext)
{
    SatoriRegion* region = queue->TryPop();
    if (region)
    {
        MaybeAskForHelp();
        do
        {
            _ASSERTE(region->Generation() <= m_condemnedGeneration);

            bool hasPendingCF = false;
            region->ForEachFinalizable(
                [&](SatoriObject* finalizable)
                {
                    _ASSERTE(((size_t)finalizable & Satori::FINALIZATION_PENDING) == 0);

                    // reachable finalizables are not iteresting in any state.
                    // finalizer can be suppressed and re-registered again without creating new trackers.
                    // (this is preexisting behavior)

                    if (!finalizable->IsMarked())
                    {
                        // eager finalization does not respect suppression (preexisting behavior)
                        if (GCToEEInterface::EagerFinalized(finalizable))
                        {
                            finalizable = nullptr;
                        }
                        else if (finalizable->IsFinalizationSuppressed())
                        {
                            // Reset the bit so it will be put back on the queue
                            // if resurrected and re-registered.
                            // NOTE: if finalizer could run only once until re-registered,
                            //       unreachable + suppressed object would not be able to resurrect.
                            //       however, re-registering multiple times may result in multiple finalizer runs.
                            // (this is preexisting behavior)
                            finalizable->UnSuppressFinalization();
                            finalizable = nullptr;
                        }
                        else
                        {
                            // finalizable has just become unreachable
                            if (finalizable->RawGetMethodTable()->HasCriticalFinalizer())
                            {
                                // can't schedule just yet, because CriticalFinalizables must go
                                // after regular finalizables scheduled in the same GC
                                hasPendingCF = true;
                                (size_t&)finalizable |= Satori::FINALIZATION_PENDING;
                            }
                            else
                            {
                                finalizable->SetMarkedAtomic();
                                markContext->PushToMarkQueues(finalizable);

                                if (m_heap->FinalizationQueue()->TryScheduleForFinalization(finalizable))
                                {
                                    // this tracker has served its purpose.
                                    finalizable = nullptr;
                                }
                                else
                                {
                                    m_heap->FinalizationQueue()->SetOverflow(m_condemnedGeneration);
                                }
                            }
                        }
                    }

                    return finalizable;
                }
            );

            if (hasPendingCF)
            {
                m_finalizationPendingRegions->Push(region);
            }
            else
            {
                if (region->Generation() == 2)
                {
                    m_tenuredRegions->Push(region);
                }
                else
                {
                    m_ephemeralRegions->Push(region);
                }
            }
        } while ((region = queue->TryPop()));
    }
}

void SatoriRecycler::QueueCriticalFinalizablesWorker()
{
    SatoriRegion* region = m_finalizationPendingRegions->TryPop();
    if (region)
    {
        MarkContext c = MarkContext(this);
        MaybeAskForHelp();
        do
        {
            region->ForEachFinalizable(
                [&](SatoriObject* finalizable)
                {
                    if ((size_t)finalizable & Satori::FINALIZATION_PENDING)
                    {
                        (size_t&)finalizable &= ~Satori::FINALIZATION_PENDING;
                        finalizable->SetMarkedAtomic();
                        c.PushToMarkQueues(finalizable);

                        if (m_heap->FinalizationQueue()->TryScheduleForFinalization(finalizable))
                        {
                            // this tracker has served its purpose.
                            finalizable = nullptr;
                        }
                        else
                        {
                            m_heap->FinalizationQueue()->SetOverflow(m_condemnedGeneration);
                        }
                    }

                    return finalizable;
                }
            );

            if (region->Generation() == 2)
            {
                m_tenuredRegions->Push(region);
            }
            else
            {
                m_ephemeralRegions->Push(region);
            }
        } while ((region = m_finalizationPendingRegions->TryPop()));


        if (c.m_WorkChunk != nullptr)
        {
            GCToEEInterface::EnableFinalization(true);
            m_workList->Push(c.m_WorkChunk);
        }
    }
}

void SatoriRecycler::DependentHandlesInitialScan()
{
    SatoriHandlePartitioner::StartNextScan();
    RunWithHelp(&SatoriRecycler::DependentHandlesInitialScanWorker);
}

void SatoriRecycler::DependentHandlesInitialScanWorker()
{
    ScanContext sc;
    sc.promotion = TRUE;
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            GCScan::GcDhInitialScan(MarkFn</*isConservative*/ false>, m_condemnedGeneration, 2, &sc);
        }
    );

    if (c.m_WorkChunk != nullptr)
    {
        m_workList->Push(c.m_WorkChunk);
    }
}

void SatoriRecycler::DependentHandlesRescan()
{
    SatoriHandlePartitioner::StartNextScan();
    RunWithHelp(&SatoriRecycler::DependentHandlesRescanWorker);
}

void SatoriRecycler::DependentHandlesRescanWorker()
{
    ScanContext sc;
    sc.promotion = TRUE;
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            if (GCScan::GcDhUnpromotedHandlesExist(&sc))
            {
                GCScan::GcDhReScan(&sc);
            }
        }
    );

    if (c.m_WorkChunk != nullptr)
    {
        m_workList->Push(c.m_WorkChunk);
    }
}

void SatoriRecycler::PromoteHandlesAndFreeRelocatedRegions()
{
    // NB: we may promote some objects in gen1 gc, but it is ok if handles stay young
    if (m_promoteAllRegions)
    {
        SatoriHandlePartitioner::StartNextScan();
    }

    RunWithHelp(&SatoriRecycler::PromoteSurvivedHandlesAndFreeRelocatedRegionsWorker);
}

void SatoriRecycler::PromoteSurvivedHandlesAndFreeRelocatedRegionsWorker()
{
    if (m_promoteAllRegions)
    {
        ScanContext sc;
        sc.promotion = TRUE;
        // no need for scan context. we do not create more work here.
        sc._unused1 = nullptr;

        SatoriHandlePartitioner::ForEachUnscannedPartition(
            [&](int p)
            {
                sc.thread_number = p;
                GCScan::GcPromotionsGranted(m_condemnedGeneration, 2, &sc);
            }
        );
    }

    FreeRelocatedRegionsWorker();
}

void SatoriRecycler::FreeRelocatedRegion(SatoriRegion* curRegion)
{
    _ASSERTE(!curRegion->HasPinnedObjects());
    curRegion->ClearMarks();

    bool isNurseryRegion = curRegion->IsAttachedToAllocatingOwner();
    if (isNurseryRegion)
    {
        curRegion->DetachFromAlocatingOwnerRelease();
    }

    // return nursery regions eagerly
    // there should be a modest number of those, but we may need them soon
    // defer blanking of others
    if (SatoriUtil::IsConcurrent() && !isNurseryRegion)
    {
#if _DEBUG
        curRegion->HasMarksSet() = false;
#endif
        curRegion->DoNotSweep() = true;
        curRegion->SetOccupancy(0, 0);
        m_deferredSweepRegions->Enqueue(curRegion);
    }
    else
    {
        curRegion->MakeBlank();
        m_heap->Allocator()->ReturnRegion(curRegion);
    }
}

void SatoriRecycler::FreeRelocatedRegionsWorker()
{
    SatoriRegion* curRegion = m_relocatedRegions->TryPop();
    if (curRegion)
    {
        MaybeAskForHelp();
        do
        {
            FreeRelocatedRegion(curRegion);
        } while ((curRegion = m_relocatedRegions->TryPop()));
    }
}

void SatoriRecycler::Plan()
{
    _ASSERTE(m_ephemeralFinalizationTrackingRegions->IsEmpty());
    if (m_condemnedGeneration == 2)
    {
        _ASSERTE(m_tenuredFinalizationTrackingRegions->IsEmpty());
    }

#if _DEBUG
    auto setHasMarksFn = [&](SatoriRegion* region)
    {
        _ASSERTE(!region->HasMarksSet());
        region->HasMarksSet() = true;
    };

    // we have just marked all condemened generations
    m_ephemeralRegions->ForEachRegion(setHasMarksFn);
    if (m_condemnedGeneration == 2)
    {
        m_tenuredRegions->ForEachRegion(setHasMarksFn);
    }
#endif

    size_t relocatableEstimate = m_relocatableEphemeralEstimate;
    m_relocatableEphemeralEstimate = 0;

    if (m_condemnedGeneration == 2)
    {
        relocatableEstimate += m_relocatableTenuredEstimate;
        m_occupancyAcc[2] = 0;
        m_relocatableTenuredEstimate = 0;
    }

    // If we do relocation, we are committed to do pointer updates.
    // At an extreme we do not want to relocate one region
    // and then go through 100 regions and update pointers.
    //
    // TUNING: 
    // As crude criteria, we will do relocations if at least 1/4
    // of condemened regions want to participate. And at least 2.
    size_t desiredRelocating = m_condemnedRegionsCount / 4 + 2;

    if (m_isRelocating == false ||
        relocatableEstimate <= desiredRelocating)
    {
        DenyRelocation();
        return;
    }

    // plan relocations
    RunWithHelp(&SatoriRecycler::PlanWorker);

    // the actual relocatable number could be less than the estimate due to pinning,
    // which we know only after marking.
    // check again if it we are still meeting the relocation criteria.
    size_t relocatableActual = m_relocatingRegions->Count();
    _ASSERTE(relocatableActual <= relocatableEstimate);
    if (relocatableActual <= desiredRelocating)
    {
        m_stayingRegions->AppendUnsafe(m_relocatingRegions);
        DenyRelocation();
    }
}

void SatoriRecycler::DenyRelocation()
{
    m_isRelocating = false;

    // put all affected regions into staying queue
    if (m_promoteAllRegions)
    {
        m_occupancyAcc[2] = 0;
        m_relocatableTenuredEstimate = 0;
        m_stayingRegions->AppendUnsafe(m_ephemeralRegions);
        m_stayingRegions->AppendUnsafe(m_tenuredRegions);
        m_stayingRegions->AppendUnsafe(m_tenuredFinalizationTrackingRegions);
    }
    else
    {
        m_stayingRegions->AppendUnsafe(m_ephemeralRegions);
    }
}

void SatoriRecycler::PlanWorker()
{
    PlanRegions(m_ephemeralRegions);

    if (m_condemnedGeneration == 2)
    {
        PlanRegions(m_tenuredRegions);
    }
}

void SatoriRecycler::PlanRegions(SatoriRegionQueue* regions)
{
    _ASSERTE(m_isRelocating);

    SatoriRegion* curRegion = regions->TryPop();
    if (curRegion)
    {
        // TUNING: It is not profitable to parallelize planning.
        //       The main cost is walking the linked list of regions.
        //       It is relatively cheap, but for very large heaps could
        //       add up. By rough estimates planning 1Tb heap could take 100ms+.
        //       Think about this.
        // MaybeAskForHelp();

        do
        {
            _ASSERTE(curRegion->Generation() <= m_condemnedGeneration);

            // select relocation candidates and relocation targets according to sizes.
            if (IsRelocationCandidate(curRegion))
            {
                // when relocating, we want to start with larger regions
                if (curRegion->Occupancy() > Satori::REGION_SIZE_GRANULARITY * 2 / 5)
                {
                    m_relocatingRegions->Push(curRegion);
                }
                else
                {
                    m_relocatingRegions->Enqueue(curRegion);
                }
            }
            else
            {
                AddRelocationTarget(curRegion);
            }
        } while ((curRegion = regions->TryPop()));
    }
};

void SatoriRecycler::AddRelocationTarget(SatoriRegion* region)
{
    size_t maxFree = region->GetMaxAllocEstimate();

    if (maxFree < Satori::MIN_FREELIST_SIZE)
    {
        m_stayingRegions->Push(region);
    }
    else
    {
        DWORD bucket;
        BitScanReverse64(&bucket, maxFree);
        bucket -= Satori::MIN_FREELIST_SIZE_BITS;
        _ASSERTE(bucket >= 0);
        _ASSERTE(bucket < Satori::FREELIST_COUNT);

        // within the same bucket, we'd prefer to fill up pinned ones first
        if (region->HasPinnedObjects())
        {
            m_relocationTargets[bucket]->Push(region);
        }
        else
        {
            m_relocationTargets[bucket]->Enqueue(region);
        }
    }
}

SatoriRegion* SatoriRecycler::TryGetRelocationTarget(size_t allocSize, bool existingRegionOnly)
{
    //make this occasionally fail in debug to be sure we can handle low memory case.
#if _DEBUG
    if (allocSize % 1024 == 0)
    {
        return nullptr;
    }
#endif

    DWORD bucket;
    BitScanReverse64(&bucket, allocSize);

    // we could search through this bucket, which may have a large enough obj,
    // but we will just use the next queue, which guarantees it fits
    bucket++;

    bucket = bucket > Satori::MIN_FREELIST_SIZE_BITS ?
        bucket - Satori::MIN_FREELIST_SIZE_BITS :
        0;

    _ASSERTE(bucket >= 0);

    if(bucket < Satori::FREELIST_COUNT)
    {
        for (; bucket < Satori::FREELIST_COUNT; bucket++)
        {
            SatoriRegionQueue* queue = m_relocationTargets[bucket];
            if (queue)
            {
                SatoriRegion* region = queue->TryPop();
                if (region)
                {
                    size_t allocStart = region->StartAllocating(allocSize);
                    _ASSERTE(allocStart);
                    return region;
                }
            }
        }
    }

    if (existingRegionOnly)
    {
        return nullptr;
    }

    _ASSERTE(SatoriRegion::RegionSizeForAlloc(allocSize) == Satori::REGION_SIZE_GRANULARITY);
    SatoriRegion* newRegion = m_heap->Allocator()->GetRegion(Satori::REGION_SIZE_GRANULARITY);
    if (newRegion)
    {
        newRegion->SetGeneration(m_condemnedGeneration);
        newRegion->DoNotSweep() = true;
        if (newRegion->Generation() == 2)
        {
            newRegion->RearmCardsForTenured();
        }
    }

    return newRegion;
}

void SatoriRecycler::Relocate()
{
    if (m_isRelocating)
    {
        RunWithHelp(&SatoriRecycler::RelocateWorker);
    }
}

void SatoriRecycler::AddTenuredRegionsToPlan(SatoriRegionQueue* regions)
{
    _ASSERTE(m_isRelocating);

    SatoriRegion* curRegion = regions->TryPop();
    if (curRegion)
    {
        MaybeAskForHelp();
        do
        {
            // we mark only condemened generations
            _ASSERTE(!curRegion->HasMarksSet());
            // condemned regions should go through Plan
            _ASSERTE(curRegion->Generation() > m_condemnedGeneration);
            AddRelocationTarget(curRegion);
        } while ((curRegion = regions->TryPop()));
    }
}

void SatoriRecycler::RelocateWorker()
{
    _ASSERTE(m_isRelocating);

    if (m_condemnedGeneration != 2 && m_promoteAllRegions)
    {
        m_occupancyAcc[2] = 0;
        m_relocatableTenuredEstimate = 0;

        AddTenuredRegionsToPlan(m_tenuredRegions);
        AddTenuredRegionsToPlan(m_tenuredFinalizationTrackingRegions);
    }

    SatoriRegion* curRegion = m_relocatingRegions->TryPop();
    if (curRegion)
    {
        MaybeAskForHelp();
        do
        {
            RelocateRegion(curRegion);
        } while ((curRegion = m_relocatingRegions->TryPop()));
    }
}

void SatoriRecycler::RelocateRegion(SatoriRegion* relocationSource)
{
    // the region must be in after marking and before sweeping state
    _ASSERTE(relocationSource->HasMarksSet());
    _ASSERTE(!relocationSource->IsDemoted());

    relocationSource->Verify(true);

    size_t maxBytesToCopy = relocationSource->Occupancy();
    // if half region is contiguously free, relocate only into existing regions,
    // otherwise we would rather make this one a target of relocations.
    bool existingRegionOnly = relocationSource->HasFreeSpaceInTopBucket();
    SatoriRegion* relocationTarget = TryGetRelocationTarget(maxBytesToCopy, existingRegionOnly);

    // could not get a region. we must be low on available memory.
    // we can try using the source region as a target for other relocations.
    if (!relocationTarget)
    {
        AddRelocationTarget(relocationSource);
        return;
    }

    // transfer finalization trackers if we have any
    relocationTarget->TakeFinalizerInfoFrom(relocationSource);

    // allocate space for relocated objects
    size_t dst = relocationTarget->Allocate(maxBytesToCopy, /*zeroInitialize*/ false);

    // actually relocate src objects into the allocated space.
    size_t dstOrig = dst;
    size_t objLimit = relocationSource->Start() + Satori::REGION_SIZE_GRANULARITY;
    SatoriObject* o = relocationSource->FirstObject();

    // the target is typically marked, unless it is freshly allocated or in a higher generation
    // preserve marked state by copying marks
    bool relocationIsPromotion = relocationTarget->Generation() > m_condemnedGeneration;
    bool needToCopyMarks = !(relocationIsPromotion || relocationTarget->DoNotSweep());
    _ASSERTE(relocationTarget->HasMarksSet() == needToCopyMarks);

    size_t objectsRelocated = 0;
    do
    {
        o = relocationSource->SkipUnmarked(o);
        if (o->Start() >= objLimit)
        {
            _ASSERTE(o->Start() == objLimit);
            break;
        }

        _ASSERTE(o->IsMarked());
        _ASSERTE(!o->IsFree());
        _ASSERTE(!o->IsUnmovable());

        size_t size = o->Size();
        memcpy((void*)(dst - sizeof(size_t)), (void*)(o->Start() - sizeof(size_t)), size);
        // record the new location of the object by storing it in the syncblock space.
        // make it negative so it is different from a normal syncblock.
        *((ptrdiff_t*)o - 1) = -(ptrdiff_t)dst;

        if (needToCopyMarks)
        {
            ((SatoriObject*)dst)->SetMarked();
        }

        dst += size;
        objectsRelocated++;
        o = (SatoriObject*)(o->Start() + size);
    } while (o->Start() < objLimit);

    size_t used = dst - dstOrig;
    // we have new marks, but have not swept at this point, so could use less.
    _ASSERTE(used <= maxBytesToCopy);

    relocationTarget->StopAllocating(dst);

    // just update the obj count, if target is not marked this will be correct
    // otherwise sweep will update anyways.
    relocationTarget->SetOccupancy(relocationTarget->Occupancy(), relocationTarget->ObjCount() + objectsRelocated);

    // the target may yet have more space and be a target for more relocations.
    AddRelocationTarget(relocationTarget);

    if (objectsRelocated > 0)
    {
        if (relocationIsPromotion)
        {
            relocationTarget->AcceptedPromotedObjects() = true;
            m_relocatedToHigherGenRegions->Push(relocationSource);
        }
        else
        {
            m_relocatedRegions->Push(relocationSource);
        }
    }
    else
    {
        FreeRelocatedRegion(relocationSource);
    }
}

void SatoriRecycler::Update()
{
    // if we ended up not moving anything, this is no longer a relocating GC.
    m_isRelocating = m_relocatedRegions->Count() > 0 ||
        m_relocatedToHigherGenRegions->Count() > 0;

    m_CurrentGcInfo->m_compaction = m_isRelocating;

    if (m_isRelocating)
    {
        IncrementRootScanTicket();
        SatoriHandlePartitioner::StartNextScan();
        if (m_condemnedGeneration != 2)
        {
            IncrementCardScanTicket();
        }
    }

    RunWithHelp(&SatoriRecycler::UpdateRootsWorker);

    // regions must me updated 
    // after updating through cards since region update may change generations

    _ASSERTE(m_ephemeralRegions->IsEmpty());
    _ASSERTE(m_ephemeralFinalizationTrackingRegions->IsEmpty());
    _ASSERTE(m_occupancyAcc[0] == 0);
    _ASSERTE(m_occupancyAcc[1] == 0);
    if (m_condemnedGeneration == 2 || m_promoteAllRegions)
    {
        _ASSERTE(m_tenuredRegions->IsEmpty());
        _ASSERTE(m_tenuredFinalizationTrackingRegions->IsEmpty());
        _ASSERTE(m_occupancyAcc[2] == 0);
    }

    RunWithHelp(&SatoriRecycler::UpdateRegionsWorker);

    // promoting handles must be after handles are updated (since update needs to know unpromoted generations).
    // relocated regions can be freed after live regions are updated (since update gets new locations from relocated regions).
    // we will combine these two passes here after both prerequisites are complete.
    PromoteHandlesAndFreeRelocatedRegions();

    if (m_promoteAllRegions)
    {
        // the following must be after cards are reset
        m_heap->ForEachPage(
            [](SatoriPage* page)
            {
                page->CardState() = Satori::CardState::BLANK;
                page->ScanTicket() = 0;
            }
        );

        // this is optional, we can allow the ticket to just wrap around,
        // but -1 could be suboptimal for concurrent dirty scans
        m_cardScanTicket = 0;
    }

}

void SatoriRecycler::UpdateRootsWorker()
{
    if (m_isRelocating)
    {
        ScanContext sc;
        sc.promotion = FALSE;
        MarkContext c = MarkContext(this);
        sc._unused1 = &c;

        SatoriHandlePartitioner::ForEachUnscannedPartition(
            [&](int p)
            {
                // there is work for us, so maybe there is more
                MaybeAskForHelp();

                sc.thread_number = p;
                GCScan::GcScanHandles(UpdateFn</*isConservative*/ false>, m_condemnedGeneration, 2, &sc);
            }
        );

        if (SatoriUtil::IsConservativeMode())
            //generations are meaningless here, so we pass -1
            GCToEEInterface::GcScanRoots(UpdateFn<true>, -1, -1, &sc);
        else
            GCToEEInterface::GcScanRoots(UpdateFn<false>, -1, -1, &sc);

        SatoriFinalizationQueue* fQueue = m_heap->FinalizationQueue();
        if (fQueue->TryUpdateScanTicket(this->GetRootScanTicket()) &&
            fQueue->HasItems())
        {
            fQueue->ForEachObjectRef(
                [&](SatoriObject** ppObject)
                {
                    SatoriObject* o = *ppObject;
                    ptrdiff_t ptr = *((ptrdiff_t*)o - 1);
                    if (ptr < 0)
                    {
                        _ASSERTE(o->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                        *ppObject = (SatoriObject*)-ptr;
                    }
                }
            );
        }

        _ASSERTE(c.m_WorkChunk == nullptr);

        UpdatePointersInPromotedObjects();

        if (m_condemnedGeneration != 2)
        {
            UpdatePointersThroughCards();
        }
   }
}

void SatoriRecycler::UpdateRegionsWorker()
{
    // update and return target regions
    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        UpdateRegions(m_relocationTargets[i]);
    }

    // update and return staying regions
    UpdateRegions(m_stayingRegions);

    // if we saw large objects we may have ranges to update
    if (!m_workList->IsEmpty())
    {
        UpdatePointersInObjectRanges();
    }
}

void SatoriRecycler::UpdatePointersInObjectRanges()
{
    SatoriWorkChunk* srcChunk = m_workList->TryPop();
    if (srcChunk)
    {
        MaybeAskForHelp();
        do
        {
            _ASSERTE(srcChunk->IsRange());
            SatoriObject* o;
            size_t start, end;
            srcChunk->GetRange(o, start, end);
            srcChunk->Clear();
            m_heap->Allocator()->ReturnWorkChunk(srcChunk);

            // mark children in the range
            o->ForEachObjectRef(
                [](SatoriObject** ppObject)
                {
                    SatoriObject* child = *ppObject;
                    if (child)
                    {
                        ptrdiff_t ptr = *((ptrdiff_t*)child - 1);
                        if (ptr < 0)
                        {
                            _ASSERTE(child->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                            *ppObject = (SatoriObject*)-ptr;
                        }
                    }
                },
                start,
                    end);
        } while ((srcChunk = m_workList->TryPop()));
    }
}

void SatoriRecycler::UpdatePointersInPromotedObjects()
{
    SatoriRegion* curRegion;
    while ((curRegion = m_relocatedToHigherGenRegions->TryPop()))
    {
        m_promoteAllRegions ?
            curRegion->UpdatePointersInPromotedObjects<true>():
            curRegion->UpdatePointersInPromotedObjects<false>();

        m_relocatedRegions->Push(curRegion);
    }
}

void SatoriRecycler::UpdateRegions(SatoriRegionQueue* queue)
{
    SatoriRegion* curRegion = queue->TryPop();
    if (curRegion)
    {
        MaybeAskForHelp();
        do
        {
            if (!m_promoteAllRegions && IsPromotionCandidate(curRegion))
            {
                curRegion->IndividuallyPromote();
            }

            if (curRegion->Generation() > m_condemnedGeneration &&
                !curRegion->IndividuallyPromoted())
            {
                // this can only happen when promoting in gen1
                _ASSERTE(m_promoteAllRegions);
                curRegion->DoNotSweep() = true;
                if (curRegion->AcceptedPromotedObjects())
                {
                    curRegion->UpdateFinalizableTrackers();
                    curRegion->AcceptedPromotedObjects() = false;
                }
            }
            else if (m_isRelocating)
            {
                // this can happen when relocation adds a region.
                if (curRegion->DoNotSweep())
                {
                    curRegion->UpdatePointers();
                }
                else
                {
                    if (!curRegion->Sweep</*updatePointers*/ true>())
                    {
                        bool isNurseryRegion = curRegion->IsAttachedToAllocatingOwner();
                        if (isNurseryRegion)
                        {
                            curRegion->DetachFromAlocatingOwnerRelease();
                        }

                        // return nursery regions eagerly
                        // there should be a modest number of those, but we may need them soon
                        // defer blanking of others
                        if (SatoriUtil::IsConcurrent() && !isNurseryRegion)
                        {
                            m_deferredSweepRegions->Enqueue(curRegion);
                        }
                        else
                        {
                            curRegion->MakeBlank();
                            m_heap->Allocator()->ReturnRegion(curRegion);
                        }

                        continue;
                    }
                }

                curRegion->UpdateFinalizableTrackers();
            }

            // recycler owns nursery regions only temporarily, we should not keep them.
            if (curRegion->IsAttachedToAllocatingOwner())
            {
                // when promoting, all nursery regions should be detached
                _ASSERTE(!m_promoteAllRegions);
                if (!m_isRelocating)
                {
                    curRegion->Sweep</*updatePointers*/ false>();
                }

                if (curRegion->Occupancy() == 0)
                {
                    curRegion->DetachFromAlocatingOwnerRelease();
                    curRegion->MakeBlank();
                    m_heap->Allocator()->ReturnRegion(curRegion);
                }
                else
                {
                    RecordOccupancy(curRegion->Generation(), curRegion->Occupancy());
                    curRegion->DoNotSweep() = false;
                }

                continue;
            }

            if (m_promoteAllRegions)
            {
                curRegion->SetGeneration(2);
                curRegion->RearmCardsForTenured();
            }

            // make sure the region is swept and returned now, or later
            if (!SatoriUtil::IsConcurrent())
            {
                SweepAndReturnRegion(curRegion);
                continue;
            }

            // this happens when relocation allocates a region or promotes.         
            if (curRegion->DoNotSweep() && curRegion->Occupancy() > 0)
            {
                // no point to defer these, we will not do anything more with them
                KeepRegion(curRegion);
                continue;
            }

            // these we can sweep/return later
            if (curRegion->IsReusable())
            {
                m_deferredSweepRegions->Push(curRegion);
            }
            else
            {
                m_deferredSweepRegions->Enqueue(curRegion);
            }
        } while ((curRegion = queue->TryPop()));
    }
}

// ideally, we just reuse the region for allocations.
// the region must have enough free space and not be very fragmented
bool SatoriRecycler::IsReuseCandidate(SatoriRegion* region)
{
    if (!region->HasFreeSpaceInTopNBuckets(Satori::REUSABLE_BUCKETS))
        return false;

    // TUNING: here we are roughly estimating reuse goodness. A better idea?
    //       i.e. 32k max chunk can be not more than 131K (1/16 full)
    //            64k max chunk can be not more than 262K (1/8 full)
    //           128k max chunk can be not more than 524K (1/4 full)
    //           256k max chunk can be not more than   1M (1/2 full)
    //           512k max chunk                    always acceptable
    //             1M max chunk                    always acceptable
    return region->GetMaxAllocEstimate() * 4 > region->Occupancy();
}

// we relocate regions if that would improve their reuse quality.
// compaction might also improve mutator locality, somewhat.
// TUNING: heuristic for reuse could be more aggressive, consider pinning, etc...
//         the cost here is inability to trace byrefs concurrently, not huge,
//         byref is rarely the only ref.
bool SatoriRecycler::IsRelocationCandidate(SatoriRegion* region)
{
    if (region->HasPinnedObjects())
    {
        return false;
    }

    // two half empty may fit two in one, so always try relocating
    if (region->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 2)
    {
        return true;
    }

    // region up to 3/4 will free 524K+ chunk, compact if not reusable
    if (region->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 4 * 3 &&
        !IsReuseCandidate(region))
    {
        return true;
    }


    return false;
}

// regions that were not reused or relocated for a while could be tenured.
// unless it is a reuse candidate
bool SatoriRecycler::IsPromotionCandidate(SatoriRegion* region)
{
    // TUNING: individual promoting heuristic
    // if the region has not seen an allocation for 4 cycles, perhaps should tenure it
    return region->Generation() == 1 &&
        region->SweepsSinceLastAllocation() > 4 &&
        !IsReuseCandidate(region);
}

void SatoriRecycler::KeepRegion(SatoriRegion* curRegion)
{
    _ASSERTE(curRegion->Occupancy() > 0);
    _ASSERTE(curRegion->Generation() > 0);

    curRegion->DoNotSweep() = false;
    curRegion->ReusableFor() = SatoriRegion::ReuseLevel::None;
    if (IsReuseCandidate(curRegion))
    {
        _ASSERTE(curRegion->Size() == Satori::REGION_SIZE_GRANULARITY);
        if ((curRegion->Generation() == 1) || curRegion->TryDemote())
        {
#if _DEBUG
            // just split 50%/50% for testing purposes.
            curRegion->ReusableFor() = curRegion->ObjCount() % 2 == 0 ?
                SatoriRegion::ReuseLevel::Gen0 :
                SatoriRegion::ReuseLevel::Gen1;

#else
            // TUNING: heuristic for gen0
            //         the cost here is inability to trace concurrently at all.
            curRegion->ReusableFor() = curRegion->ObjCount() < (Satori::MAX_ESCAPE_SIZE / 4)?
                                            SatoriRegion::ReuseLevel::Gen0 :
                                            SatoriRegion::ReuseLevel::Gen1;
#endif
            // we can't mark through gen0 candidates concurrently, so we would not put demoted there
            // it would work, but could reduce concurrent gen2 efficiency.
            if (!SatoriUtil::IsThreadLocalGCEnabled() || curRegion->IsDemoted())
            {
                curRegion->ReusableFor() = SatoriRegion::ReuseLevel::Gen1;
            }
        }
    }

    //
    // finally return the region back into recycler queues
    //

    RecordOccupancy(curRegion->Generation(), curRegion->Occupancy());
    if (curRegion->Generation() >= 2)
    {
        PushToTenuredQueues(curRegion);
    }
    else
    {
        if (curRegion->IsReusable())
        {
            _ASSERTE(curRegion->Size() <= Satori::REGION_SIZE_GRANULARITY);
            if (curRegion->HasFreeSpaceInTopBucket())
            {
                m_reusableRegions->Enqueue(curRegion);
            }
            else
            {
                m_reusableRegions->Push(curRegion);
            }
        }
        else
        {
            PushToEphemeralQueues(curRegion);
        }
    }
}

void SatoriRecycler::DrainDeferredSweepQueue()
{
    SatoriRegion* curDeferredRegion = m_deferredSweepRegions->TryPop();
    if (curDeferredRegion)
    {
        MaybeAskForHelp();
        do
        {
            SweepAndReturnRegion(curDeferredRegion);
            Interlocked::Decrement(&m_deferredSweepCount);
        } while ((curDeferredRegion = m_deferredSweepRegions->TryPop()));
    }

    // we are blocked, we no longer need reusables.
    _ASSERTE(IsBlockingPhase());
    SatoriRegion* curReusableRegion = m_reusableRegions->TryPop();
    if (curReusableRegion)
    {
        MaybeAskForHelp();
        do
        {
            curReusableRegion->ReusableFor() = SatoriRegion::ReuseLevel::None;
            PushToEphemeralQueues(curReusableRegion);
        } while ((curReusableRegion = m_reusableRegions->TryPop()));
    }

    if (SatoriUtil::IsConservativeMode())
    {
        m_trimmer->WaitForStop();
    }
}

bool SatoriRecycler::DrainDeferredSweepQueueConcurrent(int64_t deadline)
{
    bool isHelperGCThread = IsHelperThread();

    SatoriRegion* curRegion = m_deferredSweepRegions->TryPop();
    if (curRegion)
    {
        MaybeAskForHelp();
        do
        {
            SweepAndReturnRegion(curRegion);
            Interlocked::Decrement(&m_deferredSweepCount);

            // ignore deadline on helper threads, we can't do anything else anyways.
            if (!isHelperGCThread && deadline && (GCToOSInterface::QueryPerformanceCounter() - deadline > 0))
            {
                break;
            }
        } while ((curRegion = m_deferredSweepRegions->TryPop()));
    }

    // no work that we can claim, but we must wait for sweeping to finish and
    // in conservative mode we can't start marking while trimming may change regions,
    // let other threads do their things.
    int cycles = 0;
    while (m_deferredSweepCount ||
        (SatoriUtil::IsConservativeMode() && m_trimmer->IsActive()))
    {
        // user threads should not wait, just say we have more work
        if (!isHelperGCThread)
        {
            return true;
        }

        YieldProcessor();
        if ((++cycles % 127) == 0)
        {
            GCToOSInterface::YieldThread(0);
        }
    }

    _ASSERTE(m_deferredSweepCount == 0 &&
        (!SatoriUtil::IsConservativeMode() || !m_trimmer->IsActive()));

    return false;
}

void SatoriRecycler::DrainDeferredSweepQueueHelp()
{
    _ASSERTE(IsHelperThread());

    SatoriRegion* curRegion = m_deferredSweepRegions->TryPop();
    if (curRegion)
    {
        MaybeAskForHelp();
        do
        {
            SweepAndReturnRegion(curRegion);
            Interlocked::Decrement(&m_deferredSweepCount);
        } while ((curRegion = m_deferredSweepRegions->TryPop()));
    }
}

void SatoriRecycler::SweepAndReturnRegion(SatoriRegion* curRegion)
{
    if (!curRegion->DoNotSweep())
    {
        curRegion->Sweep</*updatePointers*/ false>();
    }

    if (curRegion->Occupancy() == 0)
    {
        curRegion->MakeBlank();
        m_heap->Allocator()->ReturnRegion(curRegion);
    }
    else
    {
        KeepRegion(curRegion);
    }
}

void SatoriRecycler::RecordOccupancy(int generation, size_t occupancy)
{
    Interlocked::ExchangeAdd64(&m_occupancyAcc[generation], occupancy);
}

size_t SatoriRecycler::GetOccupancy(int i)
{
    if (i < 0 || i > 2)
        return 0;

    return m_occupancy[i];
}

size_t SatoriRecycler::GetTotalOccupancy()
{
    return m_occupancy[0] +
           m_occupancy[1] +
           m_occupancy[2];
}

size_t SatoriRecycler::GetGcStartMillis(int generation)
{
    return m_gcStartMillis[generation];
}

size_t SatoriRecycler::GetGcDurationMillis(int generation)
{
    return m_gcDurationMillis[generation];
}

bool& SatoriRecycler::IsLowLatencyMode()
{
    return m_isLowLatencyMode;
}

