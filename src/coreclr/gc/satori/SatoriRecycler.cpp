// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRecycler.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriHandlePartitioner.h"
#include "SatoriHeap.h"
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

#define ENABLE_CONCURRENT true
//#define ENABLE_CONCURRENT false

#define ENABLE_RELOCATION true
//#define ENABLE_RELOCATION false

//#define TIMED

static GCEvent* m_helpersGate;
volatile static int m_gateSignaled;
volatile static int m_activeHelpers;
volatile static int m_totalHelpers;

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

    m_heap = heap;

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
    m_regionsAddedSinceLastCollection = 0;

    m_reusableRegions = new SatoriRegionQueue(QueueKind::RecyclerReusable);
    m_demotedRegions = new SatoriRegionQueue(QueueKind::RecyclerDemoted);

    m_workList = new SatoriMarkChunkQueue();
    m_gcState = GC_STATE_NONE;
    m_isBarrierConcurrent = false;

    m_gen1Count = m_gen2Count = 0;
    m_condemnedGeneration = 0;

    m_prevCondemnedGeneration = 2;
    m_gen1CountAtLastGen2 = 0;

    // when gen1 happens
    m_gen1MinorBudget = 6; // 2 * GCToOSInterface::GetTotalProcessorCount();

    // when gen1 promotes
    m_gen1Budget = m_gen1MinorBudget * 2;

    // when gen2 happens
    m_gen2Budget = 4;

    m_activeHelperFn = nullptr;
    m_rootScanTicket = 0;
    m_cardScanTicket = 0;
}

void SatoriRecycler::ShutDown()
{
    m_activeHelperFn = nullptr;
}

void SatoriRecycler::HelperThreadFn(void* param)
{
    SatoriRecycler* recycler = (SatoriRecycler*)param;
    Interlocked::Increment(&m_activeHelpers);

    for (;;)
    {
        Interlocked::Decrement(&m_activeHelpers);

        uint32_t waitResult = m_helpersGate->Wait(1000, FALSE);
        if (waitResult != WAIT_OBJECT_0)
        {
            Interlocked::Decrement(&m_totalHelpers);
            return;
        }

        m_gateSignaled = 0;
        Interlocked::Increment(&m_activeHelpers);
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
    switch (gen)
    {
    case 0:
        goto fallthrough;
    case 1:
        fallthrough:
        return m_gen1Count;
    default:
        return m_gen2Count;
    }
}

int SatoriRecycler::CondemnedGeneration()
{
    return m_condemnedGeneration;
}

size_t SatoriRecycler::Gen1RegionCount()
{
    return m_ephemeralFinalizationTrackingRegions->Count() +
        m_ephemeralRegions->Count();
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
    SatoriRegion* r = m_reusableRegions->TryPop();
    if (r)
    {
        Interlocked::Decrement(&m_regionsAddedSinceLastCollection);
        printf(r->IsDemoted() ? "*" : "+");
    }

    return r;
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
    Interlocked::Increment(&m_regionsAddedSinceLastCollection);
    if (region->IsEscapeTracking())
    {
        _ASSERTE(IsBlockingPhase());
        _ASSERTE(!region->HasPinnedObjects());
        region->ClearMarks();
    }

    // When concurrent marking is allowed we may have marks already.
    // Demoted regions could be pre-marked
    region->Verify(/* allowMarked */ region->IsDemoted() || ENABLE_CONCURRENT);

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

    _ASSERTE(region->Generation() == 1);

    // always promote these, we always keep them.
    // this write must happen after all allocations have concluded,
    // previous adding to a queue ensures such ordering.
    region->SetGeneration(2);
}

void SatoriRecycler::TryStartGC(int generation)
{
    int newState = ENABLE_CONCURRENT ? GC_STATE_CONCURRENT : GC_STATE_BLOCKING;
    if (Interlocked::CompareExchange(&m_gcState, newState, GC_STATE_NONE) == GC_STATE_NONE)
    {
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
        // work has not started yet, come again later.
        return true;
    }

    if (m_ccStackMarkState == CC_MARK_STATE_MARKING)
    {
        _ASSERTE(m_isBarrierConcurrent);
        // help with marking stacks and f-queue, this is urgent since EE is stopped for this.
        MarkAllStacksAndFinalizationQueueHelper();

        if (m_ccStackMarkState == CC_MARK_STATE_MARKING && IsHelperThread())
        {
            // before trying other things let other marking threads go ahead
            // we do this curtesy because EE is stopped and we do not want to delay marking threads.
            GCToOSInterface::YieldThread(0);
        }
    }

    int64_t deadline = GCToOSInterface::QueryPerformanceCounter() +
        GCToOSInterface::QueryPerformanceFrequency() / 10000; // 0.1 msec.

    // this should be done before scanning stacks or cards
    // since the regions must be swept before we can FindObject
    if (DrainDeferredSweepQueueConcurrent(deadline))
    {
        return true;
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

    //TODO: VS helping on user thread should be more strictly bounded
    //      esp. when doing gen2 since we may see very large object arrays
    //      * sweep and cards seem fine-grained enough
    //      * handles are generally fast, but could take time since handles are often misbalanced
    //        can't do much about it, perhaps just skip handles when not on a helper thread
    //      * drain could have a special version for user threads that skips large objects

    if (MarkHandles(deadline))
    {
        return true;
    }

    if (m_ccStackMarkState == CC_MARK_STATE_NONE)
    {
        StartMarkingAllStacksAndFinalizationQueue();
    }

    if (m_condemnedGeneration == 1)
    {
        if (MarkThroughCardsConcurrent(deadline))
        {
            return true;
        }
    }

    if (DrainMarkQueuesConcurrent(nullptr, deadline))
    {
        return true;
    }

    if (m_ccStackMarkState != CC_MARK_STATE_DONE)
    {
        return true;
    }

    // no more work
    return false;
}

void SatoriRecycler::MarkAllStacksAndFinalizationQueueHelper()
{
    Interlocked::Increment(&m_ccMarkingThreadsNum);
    // check state again it could have changed if there were no marking threads
    if (m_ccStackMarkState == CC_MARK_STATE_MARKING)
    {
        MarkAllStacksFinalizationAndDemotedRoots();
    }

    Interlocked::Decrement(&m_ccMarkingThreadsNum);
}

void SatoriRecycler::StartMarkingAllStacksAndFinalizationQueue()
{
    if (Interlocked::CompareExchange(&m_ccStackMarkState, CC_MARK_STATE_SUSPENDING_EE, CC_MARK_STATE_NONE) == CC_MARK_STATE_NONE)
    {
        GCToEEInterface::SuspendEE(SUSPEND_FOR_GC_PREP);
        m_ccStackMarkState = CC_MARK_STATE_MARKING;
        MarkAllStacksFinalizationAndDemotedRoots();
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

bool SatoriRecycler::HelpOnce()
{
    _ASSERTE(GCToEEInterface::GetThread() != nullptr);

    bool moreWork = false;

    if (m_gcState != GC_STATE_NONE)
    {
        if (m_gcState == GC_STATE_CONCURRENT)
        {
            moreWork = HelpOnceCore();
            if (!moreWork && !m_activeHelpers)
            {
                // we see no concurrent work, initiate blocking stage
                if (Interlocked::CompareExchange(&m_gcState, GC_STATE_BLOCKING, GC_STATE_CONCURRENT) == GC_STATE_CONCURRENT)
                {
                    m_activeHelperFn = nullptr;
                    BlockingCollect();
                }
            }
        }

        GCToEEInterface::GcPoll();
    }

    return moreWork;
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
        totalHelpers < SatoriHandlePartitioner::PartitionCount() &&
        Interlocked::CompareExchange(&m_totalHelpers, totalHelpers + 1, totalHelpers) == totalHelpers)
    {
        GCToEEInterface::CreateThread(HelperThreadFn, this, false, "Satori GC Helper Thread");
    }

    if (!m_gateSignaled && Interlocked::CompareExchange(&m_gateSignaled, 1, 0) == 0)
    {
        m_helpersGate->Set();
    }
}

void SatoriRecycler::MaybeTriggerGC()
{
    if (m_regionsAddedSinceLastCollection > m_gen1MinorBudget)
    {
        int generation = Gen2RegionCount() > m_gen2Budget ? 2 : 1;

        // just make sure gen2 happens eventually. 
        if (m_gen1Count - m_gen1CountAtLastGen2 > 64)
        {
            generation = 2;
        }

        TryStartGC(generation);
    }

    HelpOnce();
}

void SatoriRecycler::Collect(int generation, bool force, bool blocking)
{
    // only blocked & forced GC is strictly observable - via finalization.
    // otherwise it is kind of a suggestion to maybe start collecting

    if (!force)
    {
        // just check if it is time
        MaybeTriggerGC();
        return;
    }

    int64_t& collectionNumRef = (generation == 1 ? m_gen1Count : m_gen2Count);
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
        TryStartGC(generation);
        HelpOnce();
    } while (blocking && (desiredCollectionNum > collectionNumRef));
}

bool SatoriRecycler::IsBlockingPhase()
{
    return m_gcState == GC_STATE_BLOCKED;
}

void SatoriRecycler::BlockingCollect()
{
    // stop other threads.
    GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

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
    m_isRelocating = ENABLE_RELOCATION;

    RunWithHelp(&SatoriRecycler::DrainDeferredSweepQueue);

    _ASSERTE(m_deferredSweepRegions->IsEmpty());

    size_t gen2Count = Gen2RegionCount();
    if (m_prevCondemnedGeneration == 2)
    {
        // next gen2 is when gen2 grows by 20%
        m_gen2Budget = max(4, gen2Count + gen2Count / 4);
#ifdef TIMED
        printf("Gen2 budget:%zd \n", m_gen2Budget);
#endif
    }

    m_isPromotingAllRegions = m_allowPromotingRelocations = false;
    if (m_condemnedGeneration == 2)
    {
        m_isPromotingAllRegions = true;
        m_gen1CountAtLastGen2 = (int)m_gen1Count;
    }
    else
    {
        size_t gen1CountAfterLastCollection = Gen1RegionCount() - m_regionsAddedSinceLastCollection;
        _ASSERTE(gen1CountAfterLastCollection >= 0);

        m_allowPromotingRelocations = gen1CountAfterLastCollection > m_gen1Budget;
    }

    // tell EE that we are starting
    // this needs to be called on a "GC" thread while EE is stopped.
    GCToEEInterface::GcStartWork(m_condemnedGeneration, max_generation);

    //#ifdef TIMED
    //printf("GenStarting%i , allow promoting relocations: %d \n", m_condemnedGeneration, m_allowPromotingRelocations);
    //#endif

    m_condemnedNurseryRegionsCount = 0;
    DeactivateAllStacks();

    m_condemnedRegionsCount = m_condemnedGeneration == 2 ?
        RegionCount() :
        Gen1RegionCount();

    BlockingMark();
    Plan();
    Relocate();
    Update();

    m_gen1Count++;
    if (m_condemnedGeneration == 2)
    {
        m_gen2Count++;
    }

    // we may still have some deferred sweeping to do, but
    // that is unobservable to EE, so tell EE that we are done
    GCToEEInterface::GcDone(m_condemnedGeneration);

    //#ifdef TIMED
    //printf("GenDone%i , relocating: %d , allow promoting relocations: %d \n", m_condemnedGeneration, m_isRelocating, m_allowPromotingRelocations);
    //#endif

#ifdef TIMED
    time = (GCToOSInterface::QueryPerformanceCounter() - time) * 1000000 / GCToOSInterface::QueryPerformanceFrequency();
    printf("%zu\n", time);
#endif

    m_prevCondemnedGeneration = m_condemnedGeneration;
    m_condemnedGeneration = 0;
    m_gcState = GC_STATE_NONE;
    m_deferredSweepCount = m_deferredSweepRegions->Count();
    m_regionsAddedSinceLastCollection = 0;

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
    } while (!m_workList->IsEmpty() || revisitCards);
}

void SatoriRecycler::MarkNewReachable()
{
    //TODO: VS we are assuming that empty work queue implies no dirty cards.
    //         we need to guarantee that we have at least some chunks.
    //         Note: finalization trackers use chunks too and may consume all the budget.
    //               we may just require that finalizer's api leaves at least one spare.
    // can also have an "overflow" flag, set it on overflow, reset when? (when we assert NO_WORK?)
    // or, simplest of all, just clean cards once unconditionally.
    while (!m_workList->IsEmpty())
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
#if !ENABLE_CONCURRENT
    // in concurrent case the current stack is unlikely to have anything unmarked
    // so no advantage to mark it early
    MarkOwnStackAndDrainQueues();
#endif

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

void SatoriRecycler::DeactivateFn(gc_alloc_context* gcContext, void* param)
{
    SatoriAllocationContext* context = (SatoriAllocationContext*)gcContext;
    SatoriRecycler* recycler = (SatoriRecycler*)param;

    context->Deactivate(recycler, /*detach*/ recycler->m_isPromotingAllRegions);
}

void SatoriRecycler::DeactivateAllStacks()
{
    GCToEEInterface::GcEnumAllocContexts(DeactivateFn, m_heap->Recycler());
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

void SatoriRecycler::PushToMarkQueuesSlow(SatoriMarkChunk*& currentMarkChunk, SatoriObject* o)
{
    _ASSERTE(o->ContainingRegion()->Generation() <= m_condemnedGeneration);

    if (currentMarkChunk)
    {
        m_workList->Push(currentMarkChunk);
        MaybeAskForHelp();
    }

#ifdef _DEBUG
    // Limit worklist in debug/chk.
    // This is just to force more overflows. Otherwise they are rather rare.
    currentMarkChunk = nullptr;
    if (m_workList->Count() < 10)
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
        // check for pinning here
        if (o->IsUnmovable())
        {
            o->ContainingRegion()->HasPinnedObjects() = true;
        }
    }
}

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
        // and assing 0+ gen to the region.
        // So check here in the opposite order - first that region is 0+ gen and then that the size is stil right.
        if (isConservative &&
            (containingRegion->GenerationAcquire() < 0 || location >= containingRegion->End()))
        {
            return;
        }

        // Concurrent FindObject is unsafe in active regions. While ref may be in a real obj,
        // the path to it from the first obj or prev indexed may cross unparsable ranges.
        // The check must acquire to be sure we check before actually doing FindObject.
        if (containingRegion->IsAttachedToContextAcquire())
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
        if (containingRegion->IsEscapeTrackingAcquire())
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

#ifdef FEATURE_CONSERVATIVE_GC
                if (GCConfig::GetConservativeGC())
                    GCToEEInterface::GcScanCurrentStackRoots(IsBlockingPhase() ? MarkFn<true> : MarkFnConcurrent<true>, &sc);
                else
#endif
                    GCToEEInterface::GcScanCurrentStackRoots(IsBlockingPhase() ? MarkFn<false> : MarkFnConcurrent<false>, &sc);
            }
        }
    }

    bool revisit = false;
    if (IsBlockingPhase())
    {
        DrainMarkQueues(c.m_markChunk);
    }
    else
    {
        revisit = DrainMarkQueuesConcurrent(c.m_markChunk, deadline);
    }

    return revisit;
}

void SatoriRecycler::MarkAllStacksFinalizationAndDemotedRoots()
{
    // mark roots for all stacks
    ScanContext sc;
    sc.promotion = TRUE;
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    bool isBlockingPhase = IsBlockingPhase();

#ifdef FEATURE_CONSERVATIVE_GC
    if (GCConfig::GetConservativeGC())
        //generations are meaningless here, so we pass -1
        GCToEEInterface::GcScanRoots(isBlockingPhase ? MarkFn<true> : MarkFnConcurrent<true>, -1, -1, &sc);
    else
#endif
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
                if (!isBlockingPhase && o->ContainingRegion()->IsEscapeTrackingAcquire())
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

    SatoriRegion* curRegion;
    while ((curRegion = m_demotedRegions->TryPop()))
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

        if (curRegion->IsReusable() && !IsBlockingPhase())
        {
            m_reusableRegions->Push(curRegion);
        }
        else
        {
            (curRegion->EverHadFinalizables() ? m_ephemeralFinalizationTrackingRegions : m_ephemeralRegions)->Push(curRegion);
        }
    }

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

bool SatoriRecycler::DrainMarkQueuesConcurrent(SatoriMarkChunk* srcChunk, int64_t deadline)
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
    SatoriMarkChunk* dstChunk = nullptr;
    while (srcChunk)
    {
        objectCount += srcChunk->Count();
        // drain srcChunk to dst chunk
        while (srcChunk->Count() > 0)
        {
            // every once in a while check for the deadline
            // the number here is to
            // - amortize cost of QueryPerformanceCounter() and
            // - establish the minimum amount of work per help quantum
            if (deadline && objectCount > 4096)
            {
                objectCount = 0;
                // printf(".");
                if ((GCToOSInterface::QueryPerformanceCounter() - deadline) > 0)
                {
                    // printf("+");
                    m_workList->Push(srcChunk);
                    if (dstChunk)
                    {
                        m_workList->Push(dstChunk);
                    }
                    return true;
                }
            }

            SatoriObject* o = srcChunk->Pop();
            _ASSERTE(o->IsMarked());
            if (o->IsUnmovable())
            {
                o->ContainingRegion()->HasPinnedObjects() = true;
            }

            o->ForEachObjectRef(
                [&](SatoriObject** ref)
                {
                    SatoriObject* child = VolatileLoadWithoutBarrier(ref);
                    if (child)
                    {
                        objectCount++;
                        SatoriRegion* childRegion = child->ContainingRegion();
                        if (!childRegion->IsEscapeTrackingAcquire())
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
                },
                /* includeCollectibleAllocator */ true
            );
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
            srcChunk = m_workList->TryPop();
        }
    }

    if (dstChunk)
    {
        _ASSERTE(dstChunk->Count() == 0);
        m_heap->Allocator()->ReturnMarkChunk(dstChunk);
    }

    return false;
}

void SatoriRecycler::DrainMarkQueues(SatoriMarkChunk* srcChunk)
{
    if (!srcChunk)
    {
        srcChunk = m_workList->TryPop();
    }

    if (!m_workList->IsEmpty())
    {
        MaybeAskForHelp();
    }

    SatoriMarkChunk* dstChunk = nullptr;
    while (srcChunk)
    {
        // drain srcChunk to dst chunk
        while (srcChunk->Count() > 0)
        {
            SatoriObject* o = srcChunk->Pop();
            _ASSERTE(o->IsMarked());
            if (o->IsUnmovable())
            {
                o->ContainingRegion()->HasPinnedObjects() = true;
            }

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
                },
                /* includeCollectibleAllocator */ true
            );
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
            srcChunk = m_workList->TryPop();
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
                    if (groupState != Satori::CardState::BLANK)
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

                        // barrier sets cards in all generations, but REMEMBERED only has meaning in tenured
                        if (region->Generation() != 2)
                        {
                            // This is optimization. Not needed for correctness.
                            // If not dirty, we wipe the group, to not look at this again in the next scans.
                            // must use interlocked, even if not concurrent - in case it gets dirty by parallel mark (it can since it is gen1)
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
                                if (cards[j] != resetValue)
                                {
                                    {
                                        // We can clean cards in concurrent mode too since writes in barriers are happening before cards,
                                        // but must use interlocked - to make sure we read after the clean
                                        // TUNING: is this profitable? maybe just leave it. dirtying must be rare and the next promotion
                                        // will wipe the cards anyways.
                                        // Interlocked::CompareExchange(&cards[j], resetValue, Satori::CardState::DIRTY);
                                    }
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
                                        if (child)
                                        {
                                            SatoriRegion* childRegion = child->ContainingRegion();
                                            if (!childRegion->IsEscapeTrackingAcquire())
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
        m_workList->Push(dstChunk);
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
                    if (groupState != Satori::CardState::BLANK)
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

                        // barrier sets cards in all generations, but REMEMBERED only has meaning in tenured
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
                                    // order WRT visiting is unimportant since fields are not changing
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
        m_workList->Push(dstChunk);
    }
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
                        const int8_t resetValue = region->Generation() == 2 ? Satori::CardState::REMEMBERED : Satori::CardState::BLANK;

                        // clean the group, but must do that before reading the cards.
                        if (Interlocked::CompareExchange(&page->CardGroupState(i), resetValue, Satori::CardState::DIRTY) != Satori::CardState::DIRTY)
                        {
                            // at this point in time the card group is no longer dirty, try the next one
                            continue;
                        }

                        bool considerAllMarked = region->Generation() > m_condemnedGeneration;
                        int8_t* cards = page->CardsForGroup(i);
                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for 0
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == 0)
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
                                    if (o->IsUnmovable())
                                    {
                                        o->ContainingRegion()->HasPinnedObjects() = true;
                                    }

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
        m_workList->Push(dstChunk);
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
            _ASSERTE(pageState <= Satori::CardState::REMEMBERED);
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
                    _ASSERTE(groupState <= Satori::CardState::REMEMBERED);
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

                            _ASSERTE(cards[j] <= Satori::CardState::REMEMBERED);
                            if (cards[j] == Satori::CardState::BLANK)
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                _ASSERTE(cards[j] <= Satori::CardState::REMEMBERED);
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
        m_workList->Push(c.m_markChunk);
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
        m_workList->Push(c.m_markChunk);
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
            m_workList->Push(c.m_markChunk);
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
        m_workList->Push(c.m_markChunk);
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
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::PromoteHandlesAndFreeRelocatedRegions()
{
    // NB: we may promote some objects in gen1 gc, but it is ok if handles stay young
    if (m_isPromotingAllRegions)
    {
        SatoriHandlePartitioner::StartNextScan();
    }

    RunWithHelp(&SatoriRecycler::PromoteSurvivedHandlesAndFreeRelocatedRegionsWorker);
}

void SatoriRecycler::PromoteSurvivedHandlesAndFreeRelocatedRegionsWorker()
{
    if (m_isPromotingAllRegions)
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

            if (ENABLE_CONCURRENT)
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
    MaybeAskForHelp();
    PlanRegions(m_finalizationScanCompleteRegions);
    PlanRegions(m_ephemeralRegions);

    if (m_allowPromotingRelocations || m_isPromotingAllRegions)
    {
        PlanRegions(m_tenuredFinalizationTrackingRegions);
        PlanRegions(m_tenuredRegions);
    }
}

void SatoriRecycler::PlanRegions(SatoriRegionQueue* regions)
{
    SatoriRegion* curRegion;
    while ((curRegion = regions->TryPop()))
    {
        // we have just marked all condemened generations
        _ASSERTE(!curRegion->HasMarksSet());
        if (curRegion->Generation() <= m_condemnedGeneration)
        {
            curRegion->HasMarksSet() = true;
        }

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

        if (curRegion->Occupancy() == 0)
        {
            _ASSERTE(curRegion->Generation() == 1);

            // TODO: VS we keep marks so that we do not need a special versions of
            // UpdatePointersInPromotedObjects and RelocateRegion, perhaps we should.

            // a newly promoted gen1 region may have a lot of dead objects.
            // we will sweep to improve chances finding relocatable regions.
            curRegion->Sweep</*updatePointers*/ false>(/*keepMarked*/ true);
        }

        if (curRegion->Generation() > m_condemnedGeneration)
        {
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
    }
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

#ifdef TIMED
    printf("relocating:%d ", m_isRelocating);
#endif

    RunWithHelp(&SatoriRecycler::RelocateWorker);
}

void SatoriRecycler::RelocateWorker()
{
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

    if (relocationTarget->IsDemoted())
    {
        printf("#");
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
        size_t size = o->Size();
        if (o->IsMarked())
        {
            _ASSERTE(!o->IsUnmovable());
            _ASSERTE(!o->IsFree());

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
        }
        else
        {
            o = relocationSource->SkipUnmarked(o);
        }
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
    // TODO: VS can this run in above when not promoting?
    RunWithHelp(&SatoriRecycler::UpdateRegionsWorker);

    // promoting handles must be after handles are updated (since update needs to know unpromoted generations).
    // relocated regions can be freed after live regions are updated (since update gets new locations from relocated regions).
    // we will combine these two passes here after both prerequisites are complete.
    PromoteHandlesAndFreeRelocatedRegions();

    if (m_isPromotingAllRegions)
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
        MaybeAskForHelp();

        ScanContext sc;
        sc.promotion = FALSE;
        MarkContext c = MarkContext(this);
        sc._unused1 = &c;

        SatoriHandlePartitioner::ForEachUnscannedPartition(
            [&](int p)
            {
                sc.thread_number = p;
                GCScan::GcScanHandles(UpdateFn</*isConservative*/ false>, m_condemnedGeneration, 2, &sc);
            }
        );

        // TODO: VS ask for help? (also check the mark counterpart)
#ifdef FEATURE_CONSERVATIVE_GC
        if (GCConfig::GetConservativeGC())
            //generations are meaningless here, so we pass -1
            GCToEEInterface::GcScanRoots(UpdateFn<true>, -1, -1, &sc);
        else
#endif
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
    MaybeAskForHelp();

    // update and return target regions
    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        UpdateRegions(m_relocationTargets[i]);
    }

    // update and return staying regions
    UpdateRegions(m_stayingRegions);
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
    SatoriRegion* curRegion;   
    while ((curRegion = queue->TryPop()))
    {
        if (curRegion->Generation() > m_condemnedGeneration)
        {
            // this can only happen when selectively promoting in gen1
            _ASSERTE(m_allowPromotingRelocations);
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
                    if (ENABLE_CONCURRENT)
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
            _ASSERTE(!m_isPromotingAllRegions);
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

            continue;
        }

        if (m_isPromotingAllRegions)
        {
            curRegion->SetGeneration(2);
            curRegion->WipeCards();
        }

        // make sure the region is swept and returned now, or later
        if (!ENABLE_CONCURRENT)
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

    if (curRegion->Generation() == 2)
    {
        if (curRegion->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 8 &&
            curRegion->HasFreeSpaceInTop3Buckets() &&
            curRegion->ObjCount() <= SatoriMarkChunk::Capacity())
        {
            _ASSERTE(curRegion->Size() == Satori::REGION_SIZE_GRANULARITY);
            curRegion->TryDemote();
        }
    }

    // TODO: VS heuristic for reuse may be more aggressive, consider pinning, etc...
    if (curRegion->Generation() == 1 &&
        curRegion->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 8 &&
        curRegion->HasFreeSpaceInTop3Buckets())
    {
        _ASSERTE(curRegion->Size() == Satori::REGION_SIZE_GRANULARITY);
        curRegion->IsReusable() = true;
    }
    else
    {
        curRegion->IsReusable() = false;
    }

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

    if (IsBlockingPhase())
    {
        MaybeAskForHelp();

        SatoriRegion* curRegion;
        while ((curRegion = m_reusableRegions->TryPop()))
        {
            PushToEphemeralQueues(curRegion);
        }
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
        uint32_t spinCount = 0;
        while (m_deferredSweepCount)
        {
            GCToOSInterface::YieldThread(spinCount);
        }
    }

    return m_deferredSweepCount;
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
