// Copyright (c) 2022 Vladimir Sadov
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
#include "SatoriMarkChunk.h"
#include "SatoriAllocationContext.h"
#include "SatoriFinalizationQueue.h"
#include "../gcscan.h"

#ifdef memcpy
#undef memcpy
#endif //memcpy

//#define TIMED

static const int MIN_GEN1_BUDGET = 10;
static const int MIN_GEN2_BUDGET = 40;

void ToggleWriteBarrier(bool concurrent, bool eeSuspended)
{
    WriteBarrierParameters args = {};
    args.operation = concurrent ?
        WriteBarrierOp::StartConcurrentMarkingSatori :
        WriteBarrierOp::StopConcurrentMarkingSatori;

    args.is_runtime_suspended = eeSuspended;
    GCToEEInterface::StompWriteBarrier(&args);
}

void SatoriRecycler::Initialize(SatoriHeap* heap)
{
    m_helpersGate = new (nothrow) GCEvent;
    m_helpersGate->CreateAutoEventNoThrow(false);
    m_gateSignaled = 0;
    m_activeHelpers= 0;
    m_totalHelpers = 0;

    m_noWorkSince = 0;

    m_perfCounterFrequencyMHz = GCToOSInterface::QueryPerformanceFrequency() / 1000;

    m_heap = heap;
    m_trimmer = new SatoriTrimmer(heap);

    m_ephemeralRegions = new SatoriRegionQueue(QueueKind::RecyclerEphemeral);
    m_ephemeralFinalizationTrackingRegions = new SatoriRegionQueue(QueueKind::RecyclerEphemeralFinalizationTracking);
    m_tenuredRegions = new SatoriRegionQueue(QueueKind::RecyclerTenured);
    m_tenuredFinalizationTrackingRegions = new SatoriRegionQueue(QueueKind::RecyclerTenuredFinalizationTracking);

    m_finalizationScanCompleteRegions = new SatoriRegionQueue(QueueKind::RecyclerFinalizationScanComplete);
    m_finalizationPendingRegions = new SatoriRegionQueue(QueueKind::RecyclerFinalizationPending);

    m_stayingRegions = new SatoriRegionQueue(QueueKind::RecyclerStaying);
    m_relocatingRegions = new SatoriRegionQueue(QueueKind::RecyclerRelocating);
    m_relocatedRegions = new SatoriRegionQueue(QueueKind::RecyclerRelocated);
    m_relocatedToHigherGenRegions = new SatoriRegionQueue(QueueKind::RecyclerRelocatedToHigherGen);

    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        m_relocationTargets[i] = new SatoriRegionQueue(QueueKind::RecyclerRelocationTarget);
    }

    m_deferredSweepRegions = new SatoriRegionQueue(QueueKind::RecyclerDeferredSweep);
    m_deferredSweepCount = 0;
    m_gen1AddedSinceLastCollection = 0;
    m_gen2AddedSinceLastCollection = 0;

    m_reusableRegions = new SatoriRegionQueue(QueueKind::RecyclerReusable);
    m_reusableRegionsAlternate = new SatoriRegionQueue(QueueKind::RecyclerReusable);
    m_demotedRegions = new SatoriRegionQueue(QueueKind::RecyclerDemoted);

    m_workQueue = new SatoriMarkChunkQueue();
    m_gcState = GC_STATE_NONE;
    m_isBarrierConcurrent = false;

    m_gcCount[0] = 0;
    m_gcCount[1] = 0;
    m_gcCount[2] = 0;

    m_condemnedGeneration = 0;

    m_occupancy[0] = 0;
    m_occupancy[1] = 0;
    m_occupancy[2] = 0;
    m_occupancyAcc[0] = 0;
    m_occupancyAcc[1] = 0;
    m_occupancyAcc[2] = 0;

    // assume gen2 just happens and collected everything

    // when gen1 happens
    m_gen1CountAtLastGen2 = 0;
    m_gen1Budget = MIN_GEN1_BUDGET;
    m_gen2Budget = MIN_GEN2_BUDGET;
    m_gen2BudgetBytes = m_gen2Budget * Satori::REGION_SIZE_GRANULARITY;
    m_prevCondemnedGeneration = 2;

    m_activeHelperFn = nullptr;
    m_rootScanTicket = 0;
    m_cardScanTicket = 0;
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
        auto activeHelper = VolatileLoadWithoutBarrier(&recycler->m_activeHelperFn);
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

int SatoriRecycler::CondemnedGeneration()
{
    return m_condemnedGeneration;
}

size_t SatoriRecycler::Gen1RegionCount()
{
    return m_ephemeralFinalizationTrackingRegions->Count() +
        m_ephemeralRegions->Count() +
        m_demotedRegions->Count();
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
    return m_reusableRegions->TryPop();
}

void SatoriRecycler::PushToEphemeralQueues(SatoriRegion* region)
{
    if (region->IsDemoted())
    {
        m_demotedRegions->Push(region);
    }
    else
    {
        (region->EverHadFinalizables() ? m_ephemeralFinalizationTrackingRegions : m_ephemeralRegions)->Push(region);
    }
}

// NOTE: recycler owns nursery regions only temporarily for the duration of GC when mutators are stopped.
//       we do not keep them because an active nursery region may change its finalizability classification
//       concurrently by allocating a finalizable object.
void SatoriRecycler::AddEphemeralRegion(SatoriRegion* region)
{
    _ASSERTE(region->AllocStart() == 0);
    _ASSERTE(region->AllocRemaining() == 0);
    _ASSERTE(region->Generation() == 0 || region->Generation() == 1);
    _ASSERTE(!region->HasMarksSet());

    PushToEphemeralQueues(region);
    Interlocked::ExchangeAdd64(&m_gen1AddedSinceLastCollection, region->Size() >> Satori::REGION_BITS);
    if (region->IsEscapeTracking())
    {
        _ASSERTE(IsBlockingPhase());
        _ASSERTE(!region->HasPinnedObjects());
        region->ClearMarks();
    }

    // When concurrent marking is allowed we may have marks already.
    // Demoted regions could be pre-marked
    region->Verify(/* allowMarked */ region->IsDemoted() || SatoriUtil::IsConcurrent());

    // the region has just done allocating, so real occupancy will be known only after we sweep.
    // for now put 0 and treat the region as completely full
    // that is a fair estimate for detached regions anyways and for nursery regions
    // it does not matter, since they will not participate in relocations
    region->SetOccupancy(0, 0);

    if (region->IsAttachedToContext())
    {
        _ASSERTE(IsBlockingPhase());
        m_condemnedNurseryRegionsCount++;
    }
}

void SatoriRecycler::AddTenuredRegion(SatoriRegion* region)
{
    _ASSERTE(region->AllocStart() == 0);
    _ASSERTE(region->AllocRemaining() == 0);
    _ASSERTE(!region->IsEscapeTracking());
    _ASSERTE(!region->HasMarksSet());

    region->Verify();
    (region->EverHadFinalizables() ? m_tenuredFinalizationTrackingRegions : m_tenuredRegions)->Push(region);
    Interlocked::ExchangeAdd64(&m_gen2AddedSinceLastCollection, region->Size() >> Satori::REGION_BITS);

    _ASSERTE(region->Generation() == 1);
    region->SetGeneration(2);
    region->RearmCardsForTenured();
}

size_t SatoriRecycler::GetNowMillis()
{
    int64_t t = GCToOSInterface::QueryPerformanceCounter();
    return (size_t)(t / m_perfCounterFrequencyMHz);
}

size_t SatoriRecycler::IncrementGen0Count()
{
    m_gcStartMillis[0] = GetNowMillis();
    return Interlocked::Increment((size_t*)&m_gcCount[0]);
}

void SatoriRecycler::TryStartGC(int generation, gc_reason reason)
{
    int newState = SatoriUtil::IsConcurrent() ? GC_STATE_CONCURRENT : GC_STATE_BLOCKING;
    if (Interlocked::CompareExchange(&m_gcState, newState, GC_STATE_NONE) == GC_STATE_NONE)
    {
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
            SatoriHandlePartitioner::StartNextScan();
            m_activeHelperFn = &SatoriRecycler::ConcurrentHelp;
        }

        // card scan is incremental and one pass spans concurrent and blocking stages
        if (generation != 2)
        {
            IncrementCardScanTicket();
        }

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
    return GCToEEInterface::GetThread() == nullptr;
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

        if (m_ccStackMarkState == CC_MARK_STATE_MARKING && IsHelperThread())
        {
            // before trying other things let other marking threads go ahead
            // we do this curtesy because EE is stopped and we do not want to delay marking threads.
            GCToOSInterface::YieldThread(0);
        }
    }

    int64_t timeStamp = GCToOSInterface::QueryPerformanceCounter();
    int64_t deadline = timeStamp + m_perfCounterFrequencyMHz / 8; // 1/8 msec.

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
        // toggling is a PW fence.
        ToggleWriteBarrier(true, /* eeSuspended */ false);
        m_isBarrierConcurrent = true;
    }

    if (MarkOwnStackAndDrainQueues(deadline))
    {
        return true;
    }

    if (MarkHandles(deadline))
    {
        return true;
    }

    if (m_ccStackMarkState == CC_MARK_STATE_NONE)
    {
        // only one thread will win and drive this stage, others may help.
        BlockingMarkForConcurrent();
    }

    if (m_condemnedGeneration == 1)
    {
        if (MarkThroughCardsConcurrent(deadline))
        {
            return true;
        }
    }

    if (m_ccStackMarkState != CC_MARK_STATE_DONE)
    {
        return true;
    }

    // if queues are empty we see no more work
    return !m_workQueue->IsEmpty();
}

class MarkContext
{
    friend class SatoriRecycler;

public:
    MarkContext(SatoriRecycler* recycler)
        : m_markChunk()
    {
        m_recycler = recycler;
        m_condemnedGeneration = recycler->m_condemnedGeneration;
        m_heap = recycler->m_heap;
    }

    void PushToMarkQueues(SatoriObject* o)
    {
        if (m_markChunk && m_markChunk->TryPush(o))
        {
            return;
        }

        m_recycler->PushToMarkQueuesSlow(m_markChunk, o);
    }

private:
    int m_condemnedGeneration;
    SatoriMarkChunk* m_markChunk;
    SatoriHeap* m_heap;
    SatoriRecycler* m_recycler;
};

void SatoriRecycler::BlockingMarkForConcurrentHelper()
{
    Interlocked::Increment(&m_ccMarkingThreadsNum);
    // check state again it could have changed if there were no marking threads
    if (m_ccStackMarkState == CC_MARK_STATE_MARKING)
    {
        MarkAllStacksFinalizationAndDemotedRoots();
    }

    Interlocked::Decrement(&m_ccMarkingThreadsNum);
}

/* static */
void SatoriRecycler::MarkDemotedAttachedRegionsFn(gc_alloc_context* gcContext, void* param)
{
    SatoriAllocationContext* context = (SatoriAllocationContext*)gcContext;
    MarkContext* markContext = (MarkContext*)param;
    SatoriRecycler* recycler = markContext->m_recycler;

    SatoriRegion* region = context->RegularRegion();
    if (region && region->IsDemoted())
    {
        recycler->MarkDemoted(region, *markContext);
    }
}

void SatoriRecycler::BlockingMarkForConcurrent()
{
    if (Interlocked::CompareExchange(&m_ccStackMarkState, CC_MARK_STATE_SUSPENDING_EE, CC_MARK_STATE_NONE) == CC_MARK_STATE_NONE)
    {
        GCToEEInterface::SuspendEE(SUSPEND_FOR_GC_PREP);

        // swap reusable and alternate so that we could filter through reusables.
        // the swap needs to be done when EE is stopped, but before marking has started.
        SatoriRegionQueue* alternate = m_reusableRegionsAlternate;
        _ASSERTE(alternate->Count() == 0);
        m_reusableRegionsAlternate = m_reusableRegions;
        m_reusableRegions = alternate;

        // signal to everybody to start marking roots
        VolatileStore((int*)&m_ccStackMarkState, CC_MARK_STATE_MARKING);

        // mark demoted regions if any attached to thread contexts
        MarkContext c(this);
        GCToEEInterface::GcEnumAllocContexts(MarkDemotedAttachedRegionsFn, &c);
        if (c.m_markChunk != nullptr)
        {
            m_workQueue->Push(c.m_markChunk);
        }

        // now join everybody else and mark some roots
        MarkAllStacksFinalizationAndDemotedRoots();

        // done, wait for marking to finish and restart EE
        Interlocked::Exchange(&m_ccStackMarkState, CC_MARK_STATE_DONE);
        while (m_ccMarkingThreadsNum)
        {
            // since we are waiting anyways, try helping
            if (!HelpOnceCore())
            {
                YieldProcessor();
            }
        }

        GCToEEInterface::RestartEE(false);
    }
}

void SatoriRecycler::HelpOnce()
{
    _ASSERTE(GCToEEInterface::GetThread() != nullptr);

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
                // TODO: VS make help quantum a const or macro.
                if (!m_activeHelpers &&
                    (time - m_noWorkSince) > m_perfCounterFrequencyMHz / 8 * 2) // 4 help quantums
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
}

void SatoriRecycler::ConcurrentHelp()
{
    while ((m_gcState == GC_STATE_CONCURRENT) && HelpOnceCore());
}

void SatoriRecycler::MaybeAskForHelp()
{
    if (m_activeHelperFn && m_activeHelpers < SatoriHandlePartitioner::PartitionCount())
    {
        AskForHelp();
    }
}

void SatoriRecycler::AskForHelp()
{
    int totalHelpers = m_totalHelpers;
    if (m_activeHelpers >= totalHelpers &&
        totalHelpers < SatoriUtil::MaxHelpersCount() &&
        Interlocked::CompareExchange(&m_totalHelpers, totalHelpers + 1, totalHelpers) == totalHelpers)
    {
        GCToEEInterface::CreateThread(HelperThreadFn, this, false, "Satori GC Helper Thread");
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
    if (m_gcState)
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
void SatoriRecycler::MaybeTriggerGC(gc_reason reason)
{
    int generation = 0;

    if (m_gen1AddedSinceLastCollection > m_gen1Budget)
    {
        generation = 1;
    }

    if (m_gen1AddedSinceLastCollection + m_gen2AddedSinceLastCollection > m_gen2Budget)
    {
        generation = 2;
    }

    // just make sure gen2 happens eventually. 
    if (m_gcCount[1] - m_gen1CountAtLastGen2 > 64)
    {
        generation = 2;
    }

    if (generation != 0)
    {
        TryStartGC(generation, reason);
    }

    HelpOnce();
}

void SatoriRecycler::AdjustHeuristics()
{
    size_t occupancy = GetTotalOccupancy();
    size_t ephemeralOccupancy = m_occupancy[1] + m_occupancy[0];

    if (m_prevCondemnedGeneration == 2)
    {
        // we do gen2 if occupancy doubles
        m_gen2BudgetBytes = occupancy * 2;
    }

    // if prev promoted, gen1 occupancy will be low, just keep the same budget.
    if (!m_promoteAllRegions)
    {
        // we look for ~20% ephemeral survivorship, also
        // at least 1/2 gen2 budget or gen1 min.
        size_t minGen1 = max(MIN_GEN1_BUDGET, m_gen2Budget / 2);
        size_t newGen1Budget = max(minGen1, ephemeralOccupancy * 4 / Satori::REGION_SIZE_GRANULARITY);

        // TUNING: using exponential smoothing with alpha == 1/2. is it a good smooth/lag balance?
        m_gen1Budget = (m_gen1Budget + newGen1Budget) / 2;
    }

    m_promoteAllRegions = m_allowPromotingRelocations = false;

    if (m_condemnedGeneration == 2)
    {
        m_promoteAllRegions = true;
        m_gen1CountAtLastGen2 = (int)m_gcCount[1];

        // gen2 budget has been reached.
        // we expect heap size to reduce and will readjust the goal at the next gc,
        // but if heap doubles we do gen2 again.
        m_gen2Budget = max(MIN_GEN2_BUDGET, occupancy / Satori::REGION_SIZE_GRANULARITY);
        // we will also lower gen1 budget, so that a large gen1 bugget does not lead to
        // perpetual gen2s if heap collapses. Normally gen1 budget will be less than gen2.
        m_gen1Budget = min(m_gen1Budget, m_gen2Budget);
    }
    else
    {
        // gen2 byte goal is not changing, but adjust the budget to occupancy.
        // ideally occupancy changes only slightly after each gen1
        // if we have reached the goal, next gen1 should be gen2
        m_gen2Budget = m_gen2BudgetBytes > occupancy ?
            max(MIN_GEN2_BUDGET, (m_gen2BudgetBytes - occupancy) / Satori::REGION_SIZE_GRANULARITY) :
            MIN_GEN2_BUDGET;

        if (ephemeralOccupancy * 20 > occupancy)
        {
            m_allowPromotingRelocations = true;
        }

        if (ephemeralOccupancy * 10 > occupancy)
        {
            m_promoteAllRegions = true;
        }
    }
}

void SatoriRecycler::BlockingCollect()
{
    // stop other threads.
    GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

    FIRE_EVENT(GCStart_V2, (int)m_gcCount[0], m_condemnedGeneration, reason_empty, gc_etw_type_ngc);

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
    printf("GenStarting%i , allow promoting relocations: %d \n", m_condemnedGeneration, m_allowPromotingRelocations);
#endif

    m_condemnedNurseryRegionsCount = 0;
    DeactivateAllStacks();

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

    // we are done with gen0 here, update the occupancy
    m_occupancy[0] = m_occupancyAcc[0];

    // we may still have some deferred sweeping to do, but
    // that is unobservable to EE, so tell EE that we are done
    GCToEEInterface::GcDone(m_condemnedGeneration);

#ifdef TIMED
    printf("GenDone%i , relocating: %d \n", m_condemnedGeneration, m_isRelocating);
#endif

#ifdef TIMED
    time = (GCToOSInterface::QueryPerformanceCounter() - time) * 1000000 / GCToOSInterface::QueryPerformanceFrequency();
    printf("%zu\n", time);
#endif

    FIRE_EVENT(GCEnd_V1, int(m_gcCount[0] - 1), m_condemnedGeneration);

    m_prevCondemnedGeneration = m_condemnedGeneration;
    m_condemnedGeneration = 0;
    m_gcState = GC_STATE_NONE;
    m_deferredSweepCount = m_deferredSweepRegions->Count();
    m_gen1AddedSinceLastCollection = 0;
    m_gen2AddedSinceLastCollection = 0;

    if (SatoriUtil::IsConcurrent() && m_deferredSweepCount > 0)
    {
        m_activeHelperFn = &SatoriRecycler::DrainDeferredSweepQueueHelp;
        MaybeAskForHelp();
    }
    else
    {
        // no deferred sweep, can update occupancy earlier (this is optional)
        m_occupancy[1] = m_occupancyAcc[1];
        m_occupancy[2] = m_occupancyAcc[2];
    }

    m_trimmer->SetOkToRun();

    // restart VM
    GCToEEInterface::RestartEE(true);
}

void SatoriRecycler::RunWithHelp(void(SatoriRecycler::* method)())
{
    m_activeHelperFn = method;
    (this->*method)();
    m_activeHelperFn = nullptr;
    // make sure everyone sees the new Fn before waiting for helpers to drain.
    MemoryBarrier();
    while (m_activeHelpers > 0)
    {
        // TODO: VS should find something more useful to do than mmpause,
        //       or perhaps Sleep(0) after a few spins?
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
    bool revisitCards;
    do
    {
        DrainMarkQueues();
        revisitCards = CleanCards();
    } while (!m_workQueue->IsEmpty() || revisitCards);
}

void SatoriRecycler::MarkNewReachable()
{
    // it is nearly impossible to have dirty cards and empty work queue.
    // basically only if we completely ran out of chunks.
    // we will just check for simplicity.
    while (!m_workQueue->IsEmpty() || HasDirtyCards())
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
    // so no advantage to mark it early
    if (!SatoriUtil::IsConcurrent())
    {
        MarkOwnStackAndDrainQueues();
    }

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
    _ASSERTE(m_workQueue->IsEmpty());

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

void SatoriRecycler::DeactivateAllStacks()
{
    m_currentAllocBytesLiveThreads = 0;
    GCToEEInterface::GcEnumAllocContexts(DeactivateFn, m_heap->Recycler());
    m_totalAllocBytes = m_currentAllocBytesLiveThreads + m_currentAllocBytesDeadThreads;
}

void SatoriRecycler::PushToMarkQueuesSlow(SatoriMarkChunk*& currentMarkChunk, SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion()->Generation() <= m_condemnedGeneration);

    if (currentMarkChunk)
    {
        m_workQueue->Push(currentMarkChunk);
        MaybeAskForHelp();
    }

#ifdef _DEBUG
    // Limit work queue in debug/chk.
    // This is just to force more overflows. Otherwise they are very rare.
    currentMarkChunk = nullptr;
    if (m_workQueue->Count() < 10)
#endif
    {
        currentMarkChunk = m_heap->Allocator()->TryGetMarkChunk();
    }

    if (currentMarkChunk)
    {
        currentMarkChunk->Push(o);
    }
    else
    {
        // handle mark overflow by dirtying the cards
        o->DirtyCardsForContent();

        // since this o will not be popped from the mark queue,
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

    MarkContext* context = (MarkContext*)sc->_unused1;
    SatoriObject* o = (SatoriObject*)location;

    if (flags & GC_CALL_INTERIOR)
    {
        // byrefs may point to stack, use checked here
        SatoriRegion* containingRegion = context->m_heap->RegionForAddressChecked(location);
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

    if (o->ContainingRegion()->Generation() <= context->m_condemnedGeneration)
    {
        if (!o->IsMarked())
        {
            o->SetMarkedAtomic();
            context->PushToMarkQueues(o);
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
        MarkContext* context = (MarkContext*)sc->_unused1;
        // byrefs may point to stack, use checked here
        SatoriRegion* containingRegion = context->m_heap->RegionForAddressChecked(location);
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

    ptrdiff_t ptr = ((ptrdiff_t*)o)[-1];
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
    MarkContext* context = (MarkContext*)sc->_unused1;
    SatoriObject* o = (SatoriObject*)location;

    if (flags & GC_CALL_INTERIOR)
    {
        // byrefs may point to stack, use checked here
        containingRegion = context->m_heap->RegionForAddressChecked(location);
        if (!containingRegion)
        {
            return;
        }

        // since this is concurrent, in a conservative case an allocation could have caused a split
        // that shortened the found region and the region no longer matches the ref (which means the ref is not real).
        // Note that the split could only happen before we are done with allocator queue (which is synchronising)
        // and assign 0+ gen to the region.
        // So check here in the opposite order - first that region is 0+ gen and then that the size is stil right.
        if (isConservative &&
            (containingRegion->GenerationAcquire() < 0 || location >= containingRegion->End()))
        {
            return;
        }

        // Concurrent FindObject is unsafe in active regions. While ref may be in a real obj,
        // the path to it from the first obj or prev indexed may cross unparsable ranges.
        // The check must acquire to be sure we check before actually doing FindObject.
        if (containingRegion->MaybeAttachedToContextAcquire())
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
        containingRegion = o->ContainingRegion();
        // can't mark in regions which are tracking escapes, bitmap is in use
        if (containingRegion->MaybeEscapeTrackingAcquire())
        {
            return;
        }
    }

    // no need for fences here. a published obj cannot become gen2 concurrently. 
    if (containingRegion->Generation() <= context->m_condemnedGeneration)
    {
        if (!o->IsMarked())
        {
            o->SetMarkedAtomic();
            context->PushToMarkQueues(o);
        }

        if (flags & GC_CALL_PINNED)
        {
            o->ContainingRegion()->HasPinnedObjects() = true;
        }
    }
};

bool SatoriRecycler::MarkOwnStackAndDrainQueues(int64_t deadline)
{
    MarkContext c = MarkContext(this);
    gc_alloc_context* aContext = GCToEEInterface::GetAllocContext();

    bool isBlockingPhase = IsBlockingPhase();

    // NB: helper threads do not have contexts.
    if (aContext)
    {
        ScanContext sc;
        sc.promotion = TRUE;
        sc._unused1 = &c;

        int threadScanTicket = VolatileLoadWithoutBarrier(&aContext->alloc_count);
        int currentScanTicket = GetRootScanTicket();
        if (threadScanTicket != currentScanTicket)
        {
            // claim our own stack for scanning
            if (Interlocked::CompareExchange(&aContext->alloc_count, currentScanTicket, threadScanTicket) == threadScanTicket)
            {
                MaybeAskForHelp();

                if (SatoriUtil::IsConservativeMode())
                    GCToEEInterface::GcScanCurrentStackRoots(isBlockingPhase ? MarkFn<true> : MarkFnConcurrent<true>, &sc);
                else
                    GCToEEInterface::GcScanCurrentStackRoots(isBlockingPhase ? MarkFn<false> : MarkFnConcurrent<false>, &sc);
            }
        }
    }

    // in blocking case we go through demoted together with marking all stacks
    // in concurrent case we do it here, since going through demoted does not need EE stopped.
    if (!isBlockingPhase && (!m_demotedRegions->IsEmpty()))
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_demotedRegions->TryPop()))
        {
            MarkDemoted(curRegion, c);
            (curRegion->EverHadFinalizables() ? m_ephemeralFinalizationTrackingRegions : m_ephemeralRegions)->Push(curRegion);

            if (deadline && ((GCToOSInterface::QueryPerformanceCounter() - deadline) > 0))
            {
                if (c.m_markChunk != nullptr)
                {
                    m_workQueue->Push(c.m_markChunk);
                }

                return true;
            }
        }
    }

    bool revisit = false;
    if (isBlockingPhase)
    {
        DrainMarkQueues(c.m_markChunk);
    }
    else
    {
        revisit = DrainMarkQueuesConcurrent(c.m_markChunk, deadline);
    }

    return revisit;
}

void SatoriRecycler::MarkDemoted(SatoriRegion* curRegion, MarkContext& c)
{
    _ASSERTE(curRegion->Generation() == 1);

    SatoriMarkChunk* gen2Objects = curRegion->DemotedObjects();
    if (m_condemnedGeneration == 1)
    {
        curRegion->HasPinnedObjects() = true;
        for (int i = 0; i < gen2Objects->Count(); i++)
        {
            SatoriObject* o = gen2Objects->Item(i);
            o->SetMarkedAtomic();
            c.PushToMarkQueues(o);
        }
    }
    else
    {
        curRegion->DemotedObjects() = nullptr;
        gen2Objects->Clear();
        m_heap->Allocator()->ReturnMarkChunk(gen2Objects);
    }
}

void SatoriRecycler::MarkAllStacksFinalizationAndDemotedRoots()
{
    // mark roots for all stacks
    ScanContext sc;
    sc.promotion = TRUE;
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    bool isBlockingPhase = IsBlockingPhase();

    // TODO: VS ask for help? (also check the update counterpart)
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
                    c.PushToMarkQueues(o);
                }
            }
        );
    }

    // going through demoted queue does not require EE stopped,
    // so in concurrent case we will do that later
    if (isBlockingPhase)
    {
        SatoriRegion* curRegion;
        while ((curRegion = m_demotedRegions->TryPop()))
        {
            MarkDemoted(curRegion, c);
            (curRegion->EverHadFinalizables() ? m_ephemeralFinalizationTrackingRegions : m_ephemeralRegions)->Push(curRegion);
        }
    }
    else
    {
        SatoriRegion* curRegion;
        while ((curRegion = m_reusableRegionsAlternate->TryPop()))
        {
            if (curRegion->IsDemoted())
            {
                MarkDemoted(curRegion, c);
            }

            m_reusableRegions->Enqueue(curRegion);
        }
    }

    if (c.m_markChunk != nullptr)
    {
        m_workQueue->Push(c.m_markChunk);
    }
}

bool SatoriRecycler::DrainMarkQueuesConcurrent(SatoriMarkChunk* srcChunk, int64_t deadline)
{
    if (!srcChunk)
    {
        srcChunk = m_workQueue->TryPop();
    }

    if (!m_workQueue->IsEmpty())
    {
        MaybeAskForHelp();
    }

    // just a crude measure of work performed to remind us to check for the deadline
    size_t objectCount = 0;
    SatoriMarkChunk* dstChunk = nullptr;
    SatoriObject* o = nullptr;

    auto markChildFn = [&](SatoriObject** ref)
    {
        SatoriObject* child = VolatileLoadWithoutBarrier(ref);
        if (child)
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

            parentRegion->ContainingPage()->DirtyCardForAddressUnordered((size_t)ref);
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
                m_workQueue->Push(srcChunk);
                if (dstChunk)
                {
                    m_workQueue->Push(dstChunk);
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
            SatoriMarkChunk* tmp = srcChunk;
            srcChunk = dstChunk;
            dstChunk = tmp;
        }
        else
        {
            m_heap->Allocator()->ReturnMarkChunk(srcChunk);
            srcChunk = m_workQueue->TryPop();
        }
    }

    if (dstChunk)
    {
        _ASSERTE(dstChunk->Count() == 0);
        m_heap->Allocator()->ReturnMarkChunk(dstChunk);
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
            SatoriMarkChunk* chunk = m_heap->Allocator()->TryGetMarkChunk();
            if (chunk == nullptr)
            {
                o->ContainingRegion()->ContainingPage()->DirtyCardsForRange(start, remains);
                break;
            }

            size_t len = min(Satori::REGION_SIZE_GRANULARITY, remains);
            chunk->SetRange(o, start, start + len);
            start += len;
            remains -= len;
            m_workQueue->Push(chunk);
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
            SatoriMarkChunk* chunk = m_heap->Allocator()->TryGetMarkChunk();
            if (chunk == nullptr)
            {
                return false;
            }

            size_t len = min(Satori::REGION_SIZE_GRANULARITY, remains);
            chunk->SetRange(o, start, start + len);
            start += len;
            remains -= len;
            m_workQueue->Push(chunk);
        }

        // done with current object
        _ASSERTE(remains == 0);
    }

    return true;
}

void SatoriRecycler::DrainMarkQueues(SatoriMarkChunk* srcChunk)
{
    if (!srcChunk)
    {
        srcChunk = m_workQueue->TryPop();
    }

    if (!m_workQueue->IsEmpty())
    {
        MaybeAskForHelp();
    }

    SatoriMarkChunk* dstChunk = nullptr;

    auto markChildFn = [&](SatoriObject** ref)
    {
        SatoriObject* child = *ref;
        if (child && !child->IsMarkedOrOlderThan(m_condemnedGeneration))
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
            SatoriMarkChunk* tmp = srcChunk;
            srcChunk = dstChunk;
            dstChunk = tmp;
        }
        else
        {
            m_heap->Allocator()->ReturnMarkChunk(srcChunk);
            srcChunk = m_workQueue->TryPop();
        }
    }

    if (dstChunk)
    {
        _ASSERTE(dstChunk->Count() == 0);
        m_heap->Allocator()->ReturnMarkChunk(dstChunk);
    }
}

// Just a number that is likely be different for different threads
// making the same call.
size_t ThreadSpecificNumber()
{
    size_t result = ((size_t)&result * 11400714819323198485llu) >> 32;
    return result;
}

bool SatoriRecycler::MarkThroughCardsConcurrent(int64_t deadline)
{
    SatoriMarkChunk* dstChunk = nullptr;
    bool revisit = false;

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
                // add thread specific offset, to separate somewhat what threads read
                size_t offset = ThreadSpecificNumber();
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
                        //    a region with remembered/dirty marks must be there and cannot be destroyed.
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        // we should not be marking when there could be dead objects
                        _ASSERTE(!region->HasMarksSet());

                        // sometimes we set cards without checking dst generation, but REMEMBERED only has meaning in tenured
                        if (region->Generation() != 2)
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
                                // We do not clean cards in concurrent mode since barrier can dirty cards ahead of writes.
                                // on x64 that is not the case, but we will keep the same logic.
                            } while (++j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j]);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);
                            do
                            {
                                o->ForEachObjectRef(
                                    [&](SatoriObject** ref)
                                    {
                                        SatoriObject* child = VolatileLoadWithoutBarrier(ref);
                                        if (child)
                                        {
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

                                            // cannot mark stuff in thread local regions. just mark as dirty to visit later.
                                            // if ref is outside of the containing region, it is a fake ref to collectible allocator.
                                            // dirty the MT location as if it points to the allocator object
                                            // technically it does reference the allocator, by indirection.
                                            SatoriRegion* parentRegion = o->ContainingRegion();
                                            if ((size_t)ref - parentRegion->Start() > parentRegion->Size())
                                            {
                                                ref = (SatoriObject**)o->Start();
                                            }

                                            parentRegion->ContainingPage()->DirtyCardForAddressUnordered((size_t)ref);
                                        }
                                    }, start, end);
                                o = o->Next();
                            } while (o->Start() < objLimit);
                        }

                        if (deadline && (GCToOSInterface::QueryPerformanceCounter() - deadline > 0))
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
        m_workQueue->Push(dstChunk);
    }

    return revisit;
}

void SatoriRecycler::MarkThroughCards()
{
    SatoriMarkChunk* dstChunk = nullptr;

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
                size_t offset = ThreadSpecificNumber();
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
                        if (region->Generation() != 2)
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
                                        if (child && !child->IsMarkedOrOlderThan(m_condemnedGeneration))
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
        m_workQueue->Push(dstChunk);
    }
}

bool SatoriRecycler::HasDirtyCards()
{
    bool isDirty = false;
    m_heap->ForEachPageUntil([&](SatoriPage* page) { return page->CardState() == Satori::CardState::DIRTY ? (isDirty = true) : false; });
    return isDirty;
}

// cleaning is not concurrent, but could be parallel
bool SatoriRecycler::CleanCards()
{
    SatoriMarkChunk* dstChunk = nullptr;
    bool revisit = false;

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            // NOTE: we may concurrently make cards dirty due to mark stack overflow.
            // Thus the page must be checked first, then group, then cards.
            // Dirtying due to overflow will have to do writes in the opposite order. (the barrier dirtying can be unordered though)
            int8_t pageState = page->CardState();
            if (pageState >= Satori::CardState::PROCESSING)
            {
                size_t groupCount = page->CardGroupCount();

                // there is unfinished page. Maybe should ask for help
                MaybeAskForHelp();

                // get a thread specific offset, to separate somewhat what different threads work on to reduce sharing
                size_t offset = ThreadSpecificNumber();

                // move the page to PROCESSING state and make sure it does not reorder past reading the card group states
                Interlocked::Exchange(&page->CardState(), Satori::CardState::PROCESSING);

                for (size_t ii = 0; ii < groupCount; ii++)
                {
                    size_t i = (offset + ii) % groupCount;
                    int8_t groupState = page->CardGroupState(i);
                    if (groupState == Satori::CardState::DIRTY)
                    {
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        const int8_t resetValue = region->Generation() == 2 ? Satori::CardState::REMEMBERED : Satori::CardState::EPHEMERAL;

                        // clean the group, but must do that before reading the cards.
                        if (Interlocked::CompareExchange(&page->CardGroupState(i), resetValue, Satori::CardState::DIRTY) != Satori::CardState::DIRTY)
                        {
                            // at this point in time the card group is no longer dirty, try the next one
                            continue;
                        }

                        bool considerAllMarked = region->Generation() > m_condemnedGeneration;

                        _ASSERTE(Satori::CardState::EPHEMERAL == -1);
                        const size_t unsetValue = region->Generation() == 2 ? 0 : -1;

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
                                if (considerAllMarked || o->IsMarked())
                                {
                                    o->ForEachObjectRef(
                                        [&](SatoriObject** ref)
                                        {
                                            SatoriObject* child = *ref;
                                            if (child && !child->IsMarkedOrOlderThan(m_condemnedGeneration))
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

                // we do not see more cleaning work so clean the page state, unless the page went dirty while we were working on it
                // in such case record a missed clean to revisit the whole deal. 
                int8_t origState = Interlocked::CompareExchange(&page->CardState(), Satori::CardState::REMEMBERED, Satori::CardState::PROCESSING);
                _ASSERTE(origState != Satori::CardState::BLANK);
                revisit |= origState == Satori::CardState::DIRTY;
            }
        }
    );

    if (dstChunk)
    {
        m_workQueue->Push(dstChunk);
    }

    return revisit;
}

void SatoriRecycler::UpdatePointersThroughCards()
{
    SatoriMarkChunk* dstChunk = nullptr;
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
                size_t offset = ThreadSpecificNumber();
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
                        _ASSERTE(region->Generation() == 2);

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
                                        if (child)
                                        {
                                            ptrdiff_t ptr = ((ptrdiff_t*)child)[-1];
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

            sc.thread_number = p;
            GCScan::GcScanHandles(
                IsBlockingPhase() ? MarkFn</*isConservative*/ false> : MarkFnConcurrent</*isConservative*/ false>,
                m_condemnedGeneration,
                2,
                &sc);
        },
        deadline
    );

    if (c.m_markChunk != nullptr)
    {
        m_workQueue->Push(c.m_markChunk);
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

    if (c.m_markChunk != nullptr)
    {
        GCToEEInterface::EnableFinalization(true);
        m_workQueue->Push(c.m_markChunk);
    }
}

void SatoriRecycler::ScanFinalizableRegions(SatoriRegionQueue* queue, MarkContext* c)
{
    if (!queue->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* region;
        while ((region = queue->TryPop()))
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
                                // after regular finalizables scheduled in this GC
                                hasPendingCF = true;
                                (size_t&)finalizable |= Satori::FINALIZATION_PENDING;
                            }
                            else
                            {
                                finalizable->SetMarkedAtomic();
                                c->PushToMarkQueues(finalizable);

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

            (hasPendingCF ? m_finalizationPendingRegions : m_finalizationScanCompleteRegions)->Push(region);
        }
    }
}

void SatoriRecycler::QueueCriticalFinalizablesWorker()
{
    if (!m_finalizationPendingRegions->IsEmpty())
    {
        MaybeAskForHelp();
        MarkContext c = MarkContext(this);

        SatoriRegion* region;
        while ((region = m_finalizationPendingRegions->TryPop()))
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

            m_finalizationScanCompleteRegions->Push(region);
        }

        if (c.m_markChunk != nullptr)
        {
            GCToEEInterface::EnableFinalization(true);
            m_workQueue->Push(c.m_markChunk);
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

    if (c.m_markChunk != nullptr)
    {
        m_workQueue->Push(c.m_markChunk);
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

    if (c.m_markChunk != nullptr)
    {
        m_workQueue->Push(c.m_markChunk);
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
        // no need for context. we do not create more work here.
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

void SatoriRecycler::FreeRelocatedRegionsWorker()
{
    if (!m_relocatedRegions->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_relocatedRegions->TryPop()))
        {
            _ASSERTE(!curRegion->HasPinnedObjects());
            curRegion->ClearMarks();

            if (SatoriUtil::IsConcurrent())
            {
                curRegion->HasMarksSet() = false;
                curRegion->SetOccupancy(0, 0);
                m_deferredSweepRegions->Enqueue(curRegion);
            }
            else
            {
                curRegion->MakeBlank();
                m_heap->Allocator()->ReturnRegion(curRegion);
            }
        }
    }
}

void SatoriRecycler::Plan()
{
    RunWithHelp(&SatoriRecycler::PlanWorker);
}

void SatoriRecycler::PlanWorker()
{
    PlanRegions(m_ephemeralRegions);
    PlanRegions(m_finalizationScanCompleteRegions);

    if (m_condemnedGeneration == 2)
    {
        m_occupancyAcc[2] = 0;
        PlanRegions(m_tenuredRegions);
        PlanRegions(m_tenuredFinalizationTrackingRegions);
    }
}

void SatoriRecycler::PlanRegions(SatoriRegionQueue* regions)
{
    SatoriRegion* curRegion = regions->TryPop();

    if (!curRegion)
    {
        return;
    }

    // TODO: VS it is not profitable to parallelize planning
    //       consider moving to concurrent stage
    // MaybeAskForHelp();

    do
    {
        _ASSERTE(curRegion->Generation() <= m_condemnedGeneration);
        // we have just marked all condemened generations
        _ASSERTE(!curRegion->HasMarksSet());
        curRegion->HasMarksSet() = true;

        // nursery regions do not participate in relocations
        if (!m_isRelocating || curRegion->IsAttachedToContext())
        {
            m_stayingRegions->Push(curRegion);
            continue;
        }

        // pinned cannot be evacuated
        if (curRegion->HasPinnedObjects())
        {
            AddRelocationTarget(curRegion);
            continue;
        }

        // freshly added region with unknown occupancy
        if (curRegion->Occupancy() == 0)
        {
            _ASSERTE(curRegion->Generation() == 1);
            AddRelocationTarget(curRegion);
            continue;
        }

        // select evacuation candidates and relocation targets according to sizes.
        if (curRegion->Occupancy() == 0 ||
            curRegion->Occupancy() > Satori::REGION_SIZE_GRANULARITY / 2)
        {
            AddRelocationTarget(curRegion);
        }
        else
        {
            m_relocatingRegions->Push(curRegion);
        }
    } while ((curRegion = regions->TryPop()));
};

void SatoriRecycler::AddRelocationTarget(SatoriRegion* region)
{
    size_t maxFree = region->MaxAllocEstimate();

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
    _ASSERTE(bucket < Satori::FREELIST_COUNT);

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

    if (existingRegionOnly)
    {
        return nullptr;
    }

    _ASSERTE(SatoriRegion::RegionSizeForAlloc(allocSize) == Satori::REGION_SIZE_GRANULARITY);
    SatoriRegion* newRegion = m_heap->Allocator()->GetRegion(Satori::REGION_SIZE_GRANULARITY);
    if (newRegion)
    {
        newRegion->SetGeneration(m_allowPromotingRelocations ? 2 : m_condemnedGeneration);
        if (newRegion->Generation() == 2)
        {
            newRegion->RearmCardsForTenured();
        }
    }

    return newRegion;
}

void SatoriRecycler::Relocate()
{
    // TUNING: 
    // If we can reduce condemned generation by 12-25% regions, then do relocations.
    // We do not consider nursery regions in the benefit/cost ratio here.
    // They are not relocatable and generally have very few objects too.
    size_t desiredRelocating = (m_condemnedRegionsCount - m_condemnedNurseryRegionsCount) / 4;

    if (m_relocatingRegions->Count() < desiredRelocating)
    {
        m_isRelocating = false;
    }

    RunWithHelp(&SatoriRecycler::RelocateWorker);
}

void SatoriRecycler::AddTenuredRegionsToPlan(SatoriRegionQueue* regions)
{
    if (!regions->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = regions->TryPop()))
        {
            // we have just marked all condemened generations
            _ASSERTE(!curRegion->HasMarksSet());
            // condemned regions should go through Plan
            _ASSERTE(curRegion->Generation() > m_condemnedGeneration);

            if (m_allowPromotingRelocations && m_isRelocating)
            {
                AddRelocationTarget(curRegion);
            }
            else
            {
                m_stayingRegions->Push(curRegion);
            }
        }
    }
};

void SatoriRecycler::RelocateWorker()
{
    if (m_condemnedGeneration != 2 &&
        (m_promoteAllRegions || (m_allowPromotingRelocations && m_isRelocating)))
    {
        m_occupancyAcc[2] = 0;
        AddTenuredRegionsToPlan(m_tenuredRegions);
        AddTenuredRegionsToPlan(m_tenuredFinalizationTrackingRegions);
    }

    if (!m_relocatingRegions->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_relocatingRegions->TryPop()))
        {
            if (m_isRelocating)
            {
                RelocateRegion(curRegion);
            }
            else
            {
                m_stayingRegions->Push(curRegion);
            }
        }
    }
}

void SatoriRecycler::RelocateRegion(SatoriRegion* relocationSource)
{
    // the region must be in after marking and before sweeping state
    _ASSERTE(relocationSource->HasMarksSet());
    _ASSERTE(!relocationSource->IsDemoted());

    relocationSource->Verify(true);

    size_t copySize = relocationSource->Occupancy();
    // if half region is contiguously free, relocate only into existing regions,
    // otherwise we would rather make this one a target of relocations.
    bool existingRegionOnly = relocationSource->HasFreeSpaceInTopBucket();
    SatoriRegion* relocationTarget = TryGetRelocationTarget(copySize, existingRegionOnly);

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
    size_t dst = relocationTarget->Allocate(copySize, /*zeroInitialize*/ false);

    // actually relocate src objects into the allocated space.
    size_t dstOrig = dst;
    size_t objLimit = relocationSource->Start() + Satori::REGION_SIZE_GRANULARITY;
    SatoriObject* o = relocationSource->FirstObject();
    // the target is typically marked, unless it is freshly allocated or in a higher generation
    // preserve marked state by copying marks
    bool needToCopyMarks = relocationTarget->HasMarksSet();
    size_t objectsRelocated = 0;
    do
    {
        o = relocationSource->SkipUnmarked(o);
        if (o->Start() >= objLimit)
        {
            break;
        }

        _ASSERTE(o->IsMarked());
        _ASSERTE(!o->IsFree());
        _ASSERTE(!o->IsUnmovable());

        size_t size = o->Size();
        memcpy((void*)(dst - sizeof(size_t)), (void*)(o->Start() - sizeof(size_t)), size);
        // record the new location of the object by storing it in the syncblock space.
        // make it negative so it is different from a normal syncblock.
        ((ptrdiff_t*)o)[-1] = -(ptrdiff_t)dst;

        if (needToCopyMarks)
        {
            ((SatoriObject*)dst)->SetMarked();
        }

        dst += size;
        objectsRelocated++;
        o = (SatoriObject*)(o->Start() + size);
    } while (o->Start() < objLimit);

    size_t used = dst - dstOrig;
    relocationTarget->SetOccupancy(relocationTarget->Occupancy() + used, relocationTarget->ObjCount() + objectsRelocated);
    relocationTarget->StopAllocating(dst);
    // the target may yet have more space and be a target for more relocations.
    AddRelocationTarget(relocationTarget);

    bool relocationIsPromotion = relocationTarget->Generation() > m_condemnedGeneration;
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

void SatoriRecycler::Update()
{
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

    // must run after updating through cards since update may change generations
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

        // TODO: VS ask for help? (also check the mark counterpart)
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
                    ptrdiff_t ptr = ((ptrdiff_t*)o)[-1];
                    if (ptr < 0)
                    {
                        _ASSERTE(o->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                        *ppObject = (SatoriObject*)-ptr;
                    }
                }
            );
        }

        _ASSERTE(c.m_markChunk == nullptr);

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
    if (!m_workQueue->IsEmpty())
    {
        UpdatePointersInObjectRanges();
    }
}

void SatoriRecycler::UpdatePointersInObjectRanges()
{
    MaybeAskForHelp();

    SatoriMarkChunk* srcChunk;
    while ((srcChunk = m_workQueue->TryPop()))
    {
        _ASSERTE(srcChunk->IsRange());
        SatoriObject* o;
        size_t start, end;
        srcChunk->GetRange(o, start, end);
        srcChunk->Clear();
        //TODO: VS rename -> WorkChunk ?
        m_heap->Allocator()->ReturnMarkChunk(srcChunk);

        // mark children in the range
        o->ForEachObjectRef(
            [](SatoriObject** ppObject)
            {
                SatoriObject* child = *ppObject;
                if (child)
                {
                    ptrdiff_t ptr = ((ptrdiff_t*)child)[-1];
                    if (ptr < 0)
                    {
                        _ASSERTE(child->RawGetMethodTable() == ((SatoriObject*)-ptr)->RawGetMethodTable());
                        *ppObject = (SatoriObject*)-ptr;
                    }
                }
            },
            start,
            end);
    }
}

void SatoriRecycler::UpdatePointersInPromotedObjects()
{
    SatoriRegion* curRegion;
    while ((curRegion = m_relocatedToHigherGenRegions->TryPop()))
    {
        curRegion->UpdatePointersInPromotedObjects();
        m_relocatedRegions->Push(curRegion);
    }
}

void SatoriRecycler::UpdateRegions(SatoriRegionQueue* queue)
{
    if (!queue->IsEmpty())
    {
        // there is work for us, so maybe there is more
        MaybeAskForHelp();
    }

    SatoriRegion* curRegion;
    while ((curRegion = queue->TryPop()))
    {
        if (curRegion->Generation() > m_condemnedGeneration)
        {
            // this can only happen when promoting in gen1
            _ASSERTE(m_allowPromotingRelocations || m_promoteAllRegions);
            if (curRegion->AcceptedPromotedObjects())
            {
                curRegion->UpdateFinalizableTrackers();
                curRegion->AcceptedPromotedObjects() = false;
            }
        }
        else if (m_isRelocating)
        {
            if (curRegion->HasMarksSet())
            {
                if (!curRegion->Sweep</*updatePointers*/ true>())
                {
                    // the region is empty and will be returned,
                    // but there is still some cleaning work to defer.
                    if (SatoriUtil::IsConcurrent())
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
            else 
            {
                curRegion->UpdatePointers();
            }

            curRegion->UpdateFinalizableTrackers();
        }

        // recycler owns nursery regions only temporarily, we should not keep them.
        if (curRegion->IsAttachedToContext())
        {
            // when promoting, all nursery regions should be detached
            _ASSERTE(!m_promoteAllRegions);
            if (!m_isRelocating)
            {
                curRegion->Sweep</*updatePointers*/ false>();
            }

            if (curRegion->Occupancy() == 0)
            {
                curRegion->DetachFromContext();
                curRegion->MakeBlank();
                m_heap->Allocator()->ReturnRegion(curRegion);
            }

            RecordOccupancy(curRegion->Generation(), curRegion->Occupancy());
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

        if (!curRegion->HasMarksSet() &&
            curRegion->Occupancy() > 0)
        {
            // no point to defer these, we have swept them already
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
    }
}

void SatoriRecycler::KeepRegion(SatoriRegion* curRegion)
{
    _ASSERTE(curRegion->Occupancy() > 0);
    _ASSERTE(curRegion->Generation() > 0);

    // TUNING: heuristic for reuse could be more aggressive, consider pinning, etc...
    //         the cost here is inability to trace byrefs concurrently, not huge,
    //         byref is rarely the only ref.
    curRegion->ReusableFor() = SatoriRegion::ReuseLevel::None;
    if (curRegion->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 2 &&
        curRegion->HasFreeSpaceInTop3Buckets())
    {
        _ASSERTE(curRegion->Size() == Satori::REGION_SIZE_GRANULARITY);

        // TUNING: heuristic for demoting -  could consider pinning, etc...
        //         the cost here is increasing  gen1, which is supposed to be small.
        if ((curRegion->Generation() == 1) ||
            ((curRegion->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 4) && curRegion->TryDemote()))
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
    if (curRegion->Generation() == 2)
    {
        (curRegion->EverHadFinalizables() ? m_tenuredFinalizationTrackingRegions : m_tenuredRegions)->Push(curRegion);
    }
    else
    {
        if (curRegion->IsReusable())
        {
            _ASSERTE(curRegion->Size() <= Satori::REGION_SIZE_GRANULARITY);
            if (curRegion->IsDemoted())
            {
                m_reusableRegions->Push(curRegion);
            }
            else
            {
                m_reusableRegions->Enqueue(curRegion);
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
    if (!m_deferredSweepRegions->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_deferredSweepRegions->TryPop()))
        {
            SweepAndReturnRegion(curRegion);
            Interlocked::Decrement(&m_deferredSweepCount);
        }
    }

    // we are blocked, we no longer need reusables.
    _ASSERTE(IsBlockingPhase());
    if (!m_reusableRegions->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_reusableRegions->TryPop()))
        {
            curRegion->ReusableFor() = SatoriRegion::ReuseLevel::None;
            PushToEphemeralQueues(curRegion);
        }
    }

    if (SatoriUtil::IsConservativeMode())
    {
        m_trimmer->WaitForStop();
    }
}

bool SatoriRecycler::DrainDeferredSweepQueueConcurrent(int64_t deadline)
{
    bool isHelperGCThread = IsHelperThread();
    if (!m_deferredSweepRegions->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_deferredSweepRegions->TryPop()))
        {
            SweepAndReturnRegion(curRegion);
            Interlocked::Decrement(&m_deferredSweepCount);

            // ignore deadline on helper threads, we can't do anything else anyways.
            if (!isHelperGCThread && deadline && (GCToOSInterface::QueryPerformanceCounter() - deadline > 0))
            {
                break;
            }
        }
    }

    if (isHelperGCThread)
    {
        // no work that we can claim, but we must wait for sweeping to finish
        // let other threads do their things.
        int cycles = 0;
        while (m_deferredSweepCount)
        {
            YieldProcessor();
            if ((++cycles % 127) == 0)
            {
                GCToOSInterface::YieldThread(0);
            }
        }

        if (SatoriUtil::IsConservativeMode())
        {
            m_trimmer->WaitForStop();
        }
    }

    return m_deferredSweepCount;
}

void SatoriRecycler::DrainDeferredSweepQueueHelp()
{
    _ASSERTE(IsHelperThread());

    if (!m_deferredSweepRegions->IsEmpty())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_deferredSweepRegions->TryPop()))
        {
            SweepAndReturnRegion(curRegion);
            Interlocked::Decrement(&m_deferredSweepCount);
        }
    }
}

void SatoriRecycler::SweepAndReturnRegion(SatoriRegion* curRegion)
{
    if ((curRegion->HasMarksSet() && !curRegion->Sweep</*updatePointers*/ false>()) ||
        curRegion->Occupancy() == 0)
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

size_t SatoriRecycler::GetTotalOccupancy()
{
    return m_occupancy[0] +
           m_occupancy[1] +
           m_occupancy[2];
}

size_t SatoriRecycler::GetRecyclerOccupancy()
{
    return m_occupancy[1] + m_occupancy[2];
}

size_t SatoriRecycler::GetGcStartMillis(int generation)
{
    return m_gcStartMillis[generation];
}

