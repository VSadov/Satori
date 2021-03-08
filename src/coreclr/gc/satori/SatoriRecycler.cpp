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

#define COMPACTING true
//#define COMPACTING false

//#define TIMED

static GCEvent* m_backgroundGCGate;
static int m_activeWorkers;

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

    m_workList = new SatoriMarkChunkQueue();
    m_gcState = GC_STATE_NONE;
    m_isBarrierConcurrent = false;

    m_gen1Count = m_gen2Count = 0;
    m_condemnedGeneration = 0;

    m_prevCondemnedGeneration = 2;
    m_gen1CountAtLastGen2 = 0;

    m_gen1MinorBudget = 6;
    m_gen1Budget = 12;
    m_gen2Budget = 4;
}

void SatoriRecycler::BackgroundGcFn(void* param)
{
    SatoriRecycler* recycler = (SatoriRecycler*)param;
    Interlocked::Increment(&m_activeWorkers);

    for (;;)
    {
        // should not be in cooperative mode while blocked
        GCToEEInterface::EnablePreemptiveGC();
        Interlocked::Decrement(&m_activeWorkers);
        m_backgroundGCGate->Wait(INFINITE, FALSE);
        Interlocked::Increment(&m_activeWorkers);
        GCToEEInterface::DisablePreemptiveGC();

        while (recycler->HelpOnce())
        {
        }
    }
}

// not interlocked. this is not done concurrently. 
void SatoriRecycler::IncrementStackScanCount()
{
    m_stackScanCount++;
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

int SatoriRecycler::GetStackScanCount()
{
    return m_stackScanCount;
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
    case 1:
        return m_gen1Count;
    default:
        return m_gen2Count;
    }
}

int SatoriRecycler::CondemnedGeneration()
{
    return m_condemnedGeneration;
}

int SatoriRecycler::Gen1RegionCount()
{
    return m_ephemeralFinalizationTrackingRegions->Count() + m_ephemeralRegions->Count();
}

int SatoriRecycler::Gen2RegionCount()
{
    return m_tenuredFinalizationTrackingRegions->Count() + m_tenuredRegions->Count();
}

int SatoriRecycler::RegionCount()
{
    return Gen1RegionCount() + Gen2RegionCount();
}

// NOTE: recycler owns nursery regions only temporarily for the duration of GC when mutators are stopped
//       we do not keep them always since an active nursery region may change its classification
//       concurrently by becoming gen1 or by allocating a finalizable object.
void SatoriRecycler::AddEphemeralRegion(SatoriRegion* region, bool keep)
{
    _ASSERTE(region->AllocStart() == 0);
    _ASSERTE(region->AllocRemaining() == 0);
    _ASSERTE(region->Generation() == 0 || region->Generation() == 1);
    _ASSERTE(!region->MayHaveDeadObjects());

    (region->EverHadFinalizables() ? m_ephemeralFinalizationTrackingRegions: m_ephemeralRegions)->Push(region);
    Interlocked::Increment(&m_regionsAddedSinceLastCollection);

    // if we want to keep it, we have to promote it.
    if (keep)
    {
        _ASSERTE(region->Generation() == 0);
        // following two writes must happen after all allocations have concluded,
        // previous adding to a queue ensures such ordering.
        region->StopEscapeTracking();
        region->SetGeneration(1);
    }
    else if (region->IsThreadLocal())
    {
        // order is unimportant. noone will touch threadlocal region until STW
        region->ClearMarks();
    }

    // we may have marks already, when concurrent marking is allowed
    region->Verify(/* allowMarked */ ENABLE_CONCURRENT && !region->IsThreadLocal());

    // the region has just done allocating, so real occupancy will be known only after we sweep.
    // for now put 0 and treat the region as completely full
    // that is a fair estimate for promoted regions anyways and for nursery regions
    // it does not matter, since they will not participate in compaction
    region->SetOccupancy(0);
}

void SatoriRecycler::AddTenuredRegion(SatoriRegion* region)
{
    _ASSERTE(region->AllocStart() == 0);
    _ASSERTE(region->AllocRemaining() == 0);
    _ASSERTE(!region->IsThreadLocal());
    _ASSERTE(!region->MayHaveDeadObjects());

    region->Verify();
    (region->EverHadFinalizables() ? m_tenuredFinalizationTrackingRegions : m_tenuredRegions)->Push(region);

    // always promote these, we always keep them.
    _ASSERTE(region->Generation() == 0);

    // must happen after all allocations have concluded,
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
            IncrementStackScanCount();
            SatoriHandlePartitioner::StartNextScan();

            if (!m_backgroundGCGate)
            {
                GCToEEInterface::SuspendEE(SUSPEND_FOR_GC_PREP);

                //TODO: VS this should be published when fully initialized.
                m_backgroundGCGate = new (nothrow) GCEvent;
                m_backgroundGCGate->CreateAutoEventNoThrow(false);

                for (int i = 0; i < 8; i++)
                {
                    GCToEEInterface::CreateThread(BackgroundGcFn, this, true, ".NET BGC");
                }

                GCToEEInterface::RestartEE(false);
            }
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
    }
}

bool SatoriRecycler::HelpImpl()
{
    if (m_condemnedGeneration)
    {
        //TODO: VS helping with gen2 carries some risk of pauses.
        //      although cards seems fine grained regardless and handles are generally fast.
        //      perhaps skip handles and bail from draining when gen2. Similar to what we do in Sweep

        int64_t deadline = GCToOSInterface::QueryPerformanceCounter() +
            GCToOSInterface::QueryPerformanceFrequency() / 10000; // 0.1 msec.

        if (DrainDeferredSweepQueue(deadline))
        {
            return true;
        }

        if (!m_isBarrierConcurrent)
        {
            // toggle the barrier before setting m_condemnedGeneration
            // this is a PW fence
            ToggleWriteBarrier(true, /* eeSuspended */ false);
            m_isBarrierConcurrent = true;
        }

        if (m_condemnedGeneration == 1)
        {
            if (MarkThroughCards(/* isConcurrent */ true, deadline))
            {
                return true;
            }
        }

        if (MarkHandles(deadline))
        {
            return true;
        }

        if (MarkOwnStackAndDrainQueues(deadline))
        {
            return true;
        }

        // no more work
        return false;
    }

    // work has not started yet.
    return true;
}

bool SatoriRecycler::HelpOnce()
{
    bool moreWork = false;

    if (m_gcState != GC_STATE_NONE)
    {
        if (m_gcState == GC_STATE_CONCURRENT)
        {
            moreWork = HelpImpl();

            if (moreWork)
            {
                // there is more work, we could use help
                if (m_backgroundGCGate && m_activeWorkers < 8)
                {
                    m_backgroundGCGate->Set();
                }
            }
            else
            {
                // we see no work, if workers are done, initiate blocking stage
                if ((m_activeWorkers == 0) &&
                    Interlocked::CompareExchange(&m_gcState, GC_STATE_BLOCKING, GC_STATE_CONCURRENT) == GC_STATE_CONCURRENT)
                {
                    BlockingCollect();
                }
            }
        }

        GCToEEInterface::GcPoll();
    }

    return moreWork;
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

bool SatoriRecycler::IsConcurrent()
{
    return m_gcState != GC_STATE_BLOCKED;
}

void SatoriRecycler::BlockingCollect()
{
    bool wasCoop = GCToEEInterface::EnablePreemptiveGC();
    _ASSERTE(wasCoop);

    // stop other threads.
    GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

#ifdef TIMED
    size_t time = GCToOSInterface::QueryPerformanceCounter();
#endif

    // become coop again (it will not block since VM is done suspending)
    GCToEEInterface::DisablePreemptiveGC();

    // tell EE that we are starting
    // this needs to be called on a "GC" thread while EE is stopped.
    GCToEEInterface::GcStartWork(m_condemnedGeneration, max_generation);

    if (m_isBarrierConcurrent)
    {
        ToggleWriteBarrier(false, /* eeSuspended */ true);
        m_isBarrierConcurrent = false;
    }

    // assume that we will compact. we will rethink later.
    m_isCompacting = COMPACTING;
    m_gcState = GC_STATE_BLOCKED;
    DrainDeferredSweepQueue();

    int gen2Count = Gen2RegionCount();
    if (m_prevCondemnedGeneration == 2)
    {
        // next gen2 is when gen2 grows by 20%
        m_gen2Budget = max(4, gen2Count + gen2Count / 4);
#ifdef TIMED
        printf("Gen2 budget:%d \n ", m_gen2Budget);
#endif
    }

    if (m_condemnedGeneration == 2)
    {
        m_isPromoting = true;
        m_gen1CountAtLastGen2 = (int)m_gen1Count;
    }
    else
    {
        int gen1CountAfterLastCollection = Gen1RegionCount() - m_regionsAddedSinceLastCollection;
        _ASSERTE(gen1CountAfterLastCollection >= 0);

        m_isPromoting = gen1CountAfterLastCollection > m_gen1Budget;
    }

#ifdef TIMED
    printf("Gen%i promoting:%d ", m_condemnedGeneration, m_isPromoting);
#endif

    DeactivateAllStacks();

    m_condemnedRegionsCount = m_condemnedGeneration == 2 ?
        RegionCount() :
        Gen1RegionCount();

    Mark();
    AfterMarkPass();
    Compact();
    Finish();

    m_gen1Count++;
    if (m_condemnedGeneration == 2)
    {
        m_gen2Count++;
    }

    // we may still have some deferred sweeping to do, but
    // that is unobservable to EE, so tell EE that we are done
    GCToEEInterface::GcDone(m_condemnedGeneration);

    m_prevCondemnedGeneration = m_condemnedGeneration;
    m_condemnedGeneration = 0;
    m_gcState = GC_STATE_NONE;
    m_deferredSweepCount = m_deferredSweepRegions->Count();
    m_regionsAddedSinceLastCollection = 0;

#ifdef TIMED
    time = (GCToOSInterface::QueryPerformanceCounter() - time) * 1000000 / GCToOSInterface::QueryPerformanceFrequency();
    printf("%zu\n", time);
#endif

    // TODO: VS callouts for before sweep, after sweep

    // restart VM
    GCToEEInterface::RestartEE(true);
}

void SatoriRecycler::Mark()
{
    // stack and handles do not track dirtying writes, therefore we must rescan
    IncrementStackScanCount();
    SatoriHandlePartitioner::StartNextScan();

    MarkOwnStackAndDrainQueues();
    MarkHandles();
    MarkFinalizationQueue();
    MarkOtherStacks();

    bool revisitCards = m_condemnedGeneration == 1 ?
        MarkThroughCards(/* isConcurrent */ false) :
        ENABLE_CONCURRENT;

    while (m_workList->Count() > 0 || revisitCards)
    {
        DrainMarkQueues();
        revisitCards = CleanCards();
    }

    // all strongly reachable objects are marked here
    //       sync
    AssertNoWork();

    DependentHandlesInitialScan();
    while (m_workList->Count() > 0)
    {
        do
        {
            DrainMarkQueues();
            revisitCards = CleanCards();
        } while (m_workList->Count() > 0 || revisitCards);

        DependentHandlesRescan();
    }

    //       sync
    AssertNoWork();
    WeakPtrScan(/*isShort*/ true);

    //       sync
    ScanFinalizables();

    // sync
    while (m_workList->Count() > 0)
    {
        do
        {
            DrainMarkQueues();
            revisitCards = CleanCards();
        } while (m_workList->Count() > 0 || revisitCards);

        DependentHandlesRescan();
    }

    //       sync 
    AssertNoWork();

    // tell EE we have scanned roots
    ScanContext sc;
    GCToEEInterface::AfterGcScanRoots(m_condemnedGeneration, max_generation, &sc);

    WeakPtrScan(/*isShort*/ false);
    WeakPtrScanBySingleThread();

    if (m_isPromoting)
    {
        PromoteSurvivedHandles();
    }
}

void SatoriRecycler::AssertNoWork()
{
    _ASSERTE(m_workList->Count() == 0);

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

    context->Deactivate(recycler, /*detach*/ recycler->m_isPromoting);
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
        if (o->IsPermanentlyPinned())
        {
            o->ContainingRegion()->SetHasPinnedObjects();
        }
    }
}

void SatoriRecycler::MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    size_t location = (size_t)*ppObject;
    if (location == 0)
    {
        return;
    }

    MarkContext* context = (MarkContext*)sc->_unused1;
    SatoriObject* o = SatoriObject::At(location);

    if (flags & GC_CALL_INTERIOR)
    {
        // byrefs may point to stack, use checked here
        o = context->m_heap->ObjectForAddressChecked(location);
        if (o == nullptr)
        {
            return;
        }

#ifdef FEATURE_CONSERVATIVE_GC
        if (GCConfig::GetConservativeGC() && o->IsFree())
        {
            return;
        }
#endif
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
            o->ContainingRegion()->SetHasPinnedObjects();
        }
    }
};

void SatoriRecycler::MarkFnConcurrent(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    size_t location = (size_t)*ppObject;
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
    SatoriObject* o = SatoriObject::At(location);

    if (flags & GC_CALL_INTERIOR)
    {
        // byrefs may point to stack, use checked here
        containingRegion = context->m_heap->RegionForAddressChecked(location);

        // concurrent FindObject is safe only in gen1/gen2 regions
        // gen0->gen1+ for a region can happen concurrently,
        // make sure we read generation before doing FindObject
        if (!containingRegion || containingRegion->GenerationAcquire() < 1)
        {
            return;
        }

        o = containingRegion->FindObject(location);
#ifdef FEATURE_CONSERVATIVE_GC
        if (GCConfig::GetConservativeGC() && o->IsFree())
        {
            return;
        }
#endif
    }
    else
    {
        containingRegion = o->ContainingRegion();
        // can't mark in regions which are tracking escapes, bitmap is in use
        if (containingRegion->IsThreadLocalAcquire())
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
            o->ContainingRegion()->SetHasPinnedObjects();
        }
    }
};

bool SatoriRecycler::MarkOwnStackAndDrainQueues(int64_t deadline)
{
    gc_alloc_context* aContext = GCToEEInterface::GetAllocContext();
    ScanContext sc;
    sc.promotion = TRUE;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    int threadScanCount = aContext->alloc_count;
    int currentScanCount = GetStackScanCount();
    if (threadScanCount != currentScanCount)
    {
        // claim our own stack for scanning
        if (Interlocked::CompareExchange(&aContext->alloc_count, currentScanCount, threadScanCount) == threadScanCount)
        {
            GCToEEInterface::GcScanCurrentStackRoots(IsConcurrent() ? MarkFnConcurrent : MarkFn, &sc);
        }
    }

    bool revisit = false;
    if (IsConcurrent())
    {
        revisit = DrainMarkQueuesConcurrent(c.m_markChunk, deadline);
    }
    else
    {
        DrainMarkQueues(c.m_markChunk);
    }

    return revisit;
}

void SatoriRecycler::MarkOtherStacks()
{
    // mark roots for all stacks
    ScanContext sc;
    sc.promotion = TRUE;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    //generations are meaningless here, so we pass -1
    GCToEEInterface::GcScanRoots(MarkFn, -1, -1, &sc);

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::MarkFinalizationQueue()
{
    if (!m_heap->FinalizationQueue()->HasItems())
    {
        return;
    }

    MarkContext c = MarkContext(this);
    m_heap->FinalizationQueue()->ForEachObjectRef(
        [&](SatoriObject** ppObject)
        {
            SatoriObject* o = *ppObject;
            if (!o->IsMarkedOrOlderThan(m_condemnedGeneration))
            {
                o->SetMarkedAtomic();
                c.PushToMarkQueues(o);
            }
        }
    );

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
            if (objectCount > 4096)
            {
                objectCount = 0;
                // printf(".");
                if (deadline && srcChunk && (GCToOSInterface::QueryPerformanceCounter() - deadline > 0))
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
            if (o->IsPermanentlyPinned())
            {
                o->ContainingRegion()->SetHasPinnedObjects();
            }

            o->ForEachObjectRef(
                [&](SatoriObject** ref)
                {
                    SatoriObject* child = *ref;
                    if (child)
                    {
                        objectCount++;
                        SatoriRegion* childRegion = child->ContainingRegion();
                        if (!childRegion->IsThreadLocalAcquire())
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
                        // if ref is outside of the object, it is a fake ref to collectible allocator.
                        // dirty the MT location as if it points to the allocator object
                        // technically it does reference the allocator, by indirection.
                        if ((size_t)ref - o->Start() > o->End())
                        {
                            ref = (SatoriObject**)o->Start();
                        }

                        o->ContainingRegion()->ContainingPage()->DirtyCardForAddress((size_t)ref);
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

    SatoriMarkChunk* dstChunk = nullptr;
    while (srcChunk)
    {
        // drain srcChunk to dst chunk
        while (srcChunk->Count() > 0)
        {
            SatoriObject* o = srcChunk->Pop();
            _ASSERTE(o->IsMarked());
            if (o->IsPermanentlyPinned())
            {
                o->ContainingRegion()->SetHasPinnedObjects();
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

bool SatoriRecycler::MarkThroughCards(bool isConcurrent, int64_t deadline)
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
                    if (!isConcurrent && pageState == Satori::CardState::DIRTY)
                    {
                        revisit = true;
                    }
                    // this is not a timeout, continue to next page
                    return false;
                }

                if (!isConcurrent)
                {
                    page->CardState() = Satori::CardState::PROCESSING;
                }

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
                            if (!isConcurrent && groupState == Satori::CardState::DIRTY)
                            {
                                page->CardState() = Satori::CardState::DIRTY;
                            }

                            continue;
                        }

                        // claim the group as complete, now we have to finish
                        page->CardGroupScanTicket(i) = currentScanTicket;

                        SatoriRegion* region = page->RegionForCardGroup(i);
                        // we should not be marking when there could be dead objects
                        _ASSERTE(!region->MayHaveDeadObjects());

                        // barrier sets cards in all generations, but REMEMBERED only has meaning in tenured
                        if (region->Generation() != 2)
                        {
                            // This is optimization. Not needed for correctness.
                            // If not dirty, we wipe the group, to not look at this again in the next scans.
                            // must use interlocked, even if not concurrent - in case it gets dirty by parallel mark (it can since it is gen1)
                            // wiping actual cards does not matter, we will look at them only if group is dirty,
                            // and then cleaner will reset them appropriately.
                            // there is no marking work in this region, so ticket is also irrelevant. it will be wiped if region gets promoted
                            if (groupState != Satori::CardState::DIRTY)
                            {
                                _ASSERTE(groupState == Satori::CardState::REMEMBERED);
                                page->TryEraseCardGroupState(i);
                            }
                            else
                            {
                                // This group needs cleaning, page should stay dirty.
                                page->CardState() = Satori::CardState::DIRTY;
                            }

                            continue;
                        }

                        const int8_t resetValue = Satori::CardState::REMEMBERED;
                        int8_t* cards = page->CardsForGroup(i);
                        if (!isConcurrent)
                        {
                            page->CardGroupState(i) = resetValue;
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
                                    if (!isConcurrent)
                                    {
                                        // this is to reduce cleaning if just one card gets dirty again.
                                        cards[j] = resetValue;
                                    }
                                    else
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
                                        SatoriObject* child = *ref;

                                        // cannot mark stuff in thread local regions. just mark as dirty to visit later.
                                        if (isConcurrent && child && child->ContainingRegion()->IsThreadLocalAcquire())
                                        {
                                            // if ref is outside of the object, it is a fake ref to collectible allocator.
                                            // dirty the MT location as if it points to the allocator object
                                            // technically it does reference the allocator, by indirection.
                                            if ((size_t)ref - o->Start() > o->End())
                                            {
                                                ref = (SatoriObject**)o->Start();
                                            }

                                            o->ContainingRegion()->ContainingPage()->DirtyCardForAddress((size_t)ref);
                                        }
                                        else if (child && !child->IsMarkedOrOlderThan(m_condemnedGeneration))
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

                        if (isConcurrent && deadline && (GCToOSInterface::QueryPerformanceCounter() - deadline > 0))
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

                // we went through the entire page, there is no more cleaning work to be found.
                // if it is still processing, move it to clean
                // if it is dirty, record a missed clean to revisit the page later.
                if (!isConcurrent && !page->UnsetProcessing())
                {
                    revisit = true;
                }
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

// cleaning is not concurrent, but could be parallel
bool SatoriRecycler::CleanCards()
{
    SatoriMarkChunk* dstChunk = nullptr;
    bool revisit = false;

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            // VolatileLoad to allow parallel card clearing.
            // Since we may concurrently make cards dirty due to overflow,
            // page must be checked first, then group, then cards.
            // Dirtying due to overflow will have to do writes in the opposite order.
            int8_t pageState = VolatileLoad(&page->CardState());
            if (pageState == Satori::CardState::DIRTY)
            {
                page->CardState() = Satori::CardState::PROCESSING;
                size_t groupCount = page->CardGroupCount();

                // add thread specific offset, to separate somewhat what threads read
                size_t offset = ThreadSpecificNumber();
                for (size_t ii = 0; ii < groupCount; ii++)
                {
                    size_t i = (offset + ii) % groupCount;
                    // VolatileLoad, see the comment above regading page/group/card read order
                    int8_t groupState = VolatileLoad(&page->CardGroupState(i));
                    if (groupState == Satori::CardState::DIRTY)
                    {
                        SatoriRegion* region = page->RegionForCardGroup(i);

                        const int8_t resetValue = region->Generation() == 2 ? Satori::CardState::REMEMBERED : Satori::CardState::BLANK;
                        bool considerAllMarked = region->Generation() > m_condemnedGeneration;

                        int8_t* cards = page->CardsForGroup(i);
                        page->CardGroupState(i) = resetValue;
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
                                cards[j++] = resetValue;
                            } while (j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j] == Satori::CardState::DIRTY);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);
                            do
                            {
                                if (considerAllMarked || o->IsMarked())
                                {
                                    if (o->IsPermanentlyPinned())
                                    {
                                        o->ContainingRegion()->SetHasPinnedObjects();
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

                // record a missed clean to revisit the whole deal. 
                revisit |= !page->UnsetProcessing();
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
            _ASSERTE(pageState != Satori::CardState::DIRTY);
            if (pageState == Satori::CardState::REMEMBERED)
            {
                size_t groupCount = page->CardGroupCount();

                // add thread specific offset, to separate somewhat what threads read
                size_t offset = ThreadSpecificNumber();
                for (size_t ii = 0; ii < groupCount; ii++)
                {
                    size_t i = (offset + ii) % groupCount;
                    int8_t groupState = page->CardGroupState(i);
                    _ASSERTE(groupState != Satori::CardState::DIRTY);
                    if (groupState == Satori::CardState::REMEMBERED)
                    {
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        _ASSERTE(region->Generation() == 2);

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
                                j++;
                            } while (j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j] == Satori::CardState::REMEMBERED);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* obj = region->FindObject(start);
                            do
                            {
                                obj->ForEachObjectRef(
                                    [&](SatoriObject** ppObject)
                                    {
                                        SatoriObject* o = *ppObject;
                                        if (o)
                                        {
                                            ptrdiff_t ptr = ((ptrdiff_t*)o)[-1];
                                            if (ptr < 0)
                                            {
                                                *ppObject = (SatoriObject*)-ptr;
                                            }
                                        }
                                    }, start, end);
                                obj = obj->Next();
                            } while (obj->Start() < objLimit);
                        }
                    }
                }
            }
            // do not stop
            return false;
        }
    );
}

bool SatoriRecycler::MarkHandles(int64_t deadline)
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc.concurrent = IsConcurrent();
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    bool revisit = SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            GCScan::GcScanHandles(IsConcurrent() ? MarkFnConcurrent : MarkFn, m_condemnedGeneration, 2, &sc);
        },
        deadline
    );

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }

    return revisit;
}

void SatoriRecycler::WeakPtrScan(bool isShort)
{
    ScanContext sc;
    sc.promotion = TRUE;

    SatoriHandlePartitioner::StartNextScan();
    SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            if (isShort)
            {
                GCScan::GcShortWeakPtrScan(nullptr, m_condemnedGeneration, 2, &sc);
            }
            else
            {
                GCScan::GcWeakPtrScan(nullptr, m_condemnedGeneration, 2, &sc);
            }
        }
    );
}

void SatoriRecycler::WeakPtrScanBySingleThread()
{
    // scan for deleted entries in the syncblk cache
    // sc is not really used, but must have "promotion == TRUE" if HeapVerify is on.
    ScanContext sc;
    sc.promotion = TRUE;

    GCScan::GcWeakPtrScanBySingleThread(m_condemnedGeneration, 2, &sc);
}

void SatoriRecycler::ScanFinalizableRegions(SatoriRegionQueue* regions, MarkContext* c)
{
    SatoriRegion* region;
    while (region = regions->TryPop())
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
                            if (m_heap->FinalizationQueue()->TryScheduleForFinalizationSTW(finalizable))
                            {
                                finalizable->SetMarkedAtomic();
                                c->PushToMarkQueues(finalizable);
                                // this tracker has served its purpose.
                                finalizable = nullptr;
                            }
                            else
                            {
                                _ASSERTE(!"handle overflow (just add to pending?)");
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

void SatoriRecycler::ScanFinalizables()
{
    MarkContext c = MarkContext(this);

    ScanFinalizableRegions(m_ephemeralFinalizationTrackingRegions, &c);
    if (m_condemnedGeneration == 2)
    {
        ScanFinalizableRegions(m_tenuredFinalizationTrackingRegions, &c);
    }

    SatoriRegion* region;
    while (region = m_finalizationPendingRegions->TryPop())
    {
        region->ForEachFinalizable(
            [&](SatoriObject* finalizable)
            {
                if ((size_t)finalizable & Satori::FINALIZATION_PENDING)
                {
                    (size_t&)finalizable &= ~Satori::FINALIZATION_PENDING;

                    if (m_heap->FinalizationQueue()->TryScheduleForFinalizationSTW(finalizable))
                    {
                        finalizable->SetMarkedAtomic();
                        c.PushToMarkQueues(finalizable);
                        // this tracker has served its purpose.
                        finalizable = nullptr;
                    }
                    else
                    {
                        // TODO: VS we need to ensure that WPF in progress does not exit and call Collect after a sleep.
                        //       this can be tested with a small unexpandable queue.
                        _ASSERTE(!"handle overflow (just mark all that did not schedule).");
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

void SatoriRecycler::DependentHandlesInitialScan()
{
    ScanContext sc;
    sc.promotion = TRUE;
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    SatoriHandlePartitioner::StartNextScan();
    SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            GCScan::GcDhInitialScan(MarkFn, m_condemnedGeneration, 2, &sc);
        }
    );

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::DependentHandlesRescan()
{
    ScanContext sc;
    sc.promotion = TRUE;
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    SatoriHandlePartitioner::StartNextScan();
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

void SatoriRecycler::PromoteSurvivedHandles()
{
    ScanContext sc;
    sc.promotion = TRUE;

    // no need for context. we do not create more work here.
    sc._unused1 = nullptr;

    SatoriHandlePartitioner::StartNextScan();
    SatoriHandlePartitioner::ForEachUnscannedPartition(
        [&](int p)
        {
            sc.thread_number = p;
            GCScan::GcPromotionsGranted(m_condemnedGeneration, 2, &sc);
        }
    );
}

void SatoriRecycler::AfterMarkPass()
{
    AfterMarkPassThroughRegions(m_finalizationScanCompleteRegions);
    AfterMarkPassThroughRegions(m_ephemeralRegions);

    if (m_isPromoting)
    {
        AfterMarkPassThroughRegions(m_tenuredFinalizationTrackingRegions);
        AfterMarkPassThroughRegions(m_tenuredRegions);
    }
}

void SatoriRecycler::AfterMarkPassThroughRegions(SatoriRegionQueue* regions)
{
    SatoriRegion* curRegion;
    while (curRegion = regions->TryPop())
    {
        if (curRegion->Generation() <= m_condemnedGeneration)
        {
            // we should not be marking when there could be dead objects
            _ASSERTE(!curRegion->MayHaveDeadObjects());
            // now, after marking, we may have garbage
            curRegion->SetMayHaveDeadObjects(true);
        }

        // nursery regions do not participate in evacuation
        // others also, if not compacting
        if (!m_isCompacting || curRegion->Generation() == 0)
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

        if (m_isPromoting && m_condemnedGeneration == 1)
        {
            if (curRegion->Occupancy() == 0)
            {
                curRegion->Sweep(true);
            }

            if (curRegion->Generation() == 2 ||
                curRegion->Occupancy() >= Satori::REGION_SIZE_GRANULARITY / 3)
            {
                AddRelocationTarget(curRegion);
            }
            else
            {
                m_relocatingRegions->Push(curRegion);
            }

            continue;
        }

        // this is a regular region and we have a regular gen1 GC
        // select evacuation candidates and relocation targets according to sizes.
        if (curRegion->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 3 &&
            curRegion->Occupancy() != 0)
        {
            m_relocatingRegions->Push(curRegion);
        }
        else
        {
            AddRelocationTarget(curRegion);
        }
    }
};

void SatoriRecycler::AddRelocationTarget(SatoriRegion* region)
{
    size_t maxFree = region->MaxAllocEstimate();
    // TODO: VS this  is to allow promoting > 1/2 occupancy regions
    // will we use this ? (it is expensive, the alternative is gen2 compaction)
    
    // scale down, so that top bucket is 75%
    size_t scaledMaxFree = maxFree;// -maxFree / 2;

    if (scaledMaxFree < Satori::MIN_FREELIST_SIZE)
    {
        m_stayingRegions->Push(region);
    }
    else
    {
        DWORD bucket;
        BitScanReverse64(&bucket, scaledMaxFree);
        bucket -= Satori::MIN_FREELIST_SIZE_BITS;
        _ASSERTE(bucket >= 0);
        _ASSERTE(bucket < Satori::FREELIST_COUNT);
        m_relocationTargets[bucket]->Push(region);
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

    // scale down, to match AddRelocationTarget
    size_t scaledMinFree = allocSize;// -allocSize / 2;

    DWORD bucket;
    BitScanReverse64(&bucket, scaledMinFree);

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

    SatoriRegion* newRegion = m_heap->Allocator()->GetRegion(ALIGN_UP(allocSize, Satori::REGION_SIZE_GRANULARITY));
    if (newRegion)
    {
        newRegion->SetGeneration(m_condemnedGeneration);
    }

    return newRegion;
}

void SatoriRecycler::Compact()
{
    int desiredCompacting = m_condemnedGeneration == 1 ?
        m_condemnedRegionsCount / 4 :
        m_condemnedRegionsCount * 3 / 4;

    if (m_relocatingRegions->Count() < desiredCompacting)
    {
        m_isCompacting = false;
    }

#ifdef TIMED
    printf("compacting:%d ", m_isCompacting);
#endif

    SatoriRegion* curRegion;
    while (curRegion = m_relocatingRegions->TryPop())
    {
        if (m_isCompacting)
        {
            RelocateRegion(curRegion);
        }
        else
        {
            m_stayingRegions->Push(curRegion);
        }
    }
}

void SatoriRecycler::RelocateRegion(SatoriRegion* relocationSource)
{
    // the region must be in after marking and before sweeping state
    _ASSERTE(relocationSource->MayHaveDeadObjects());
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

    // transfer finalization trackers if we have any any
    relocationTarget->TakeFinalizerInfoFrom(relocationSource);

    // allocate space for relocated objects
    size_t dstPtr = relocationTarget->Allocate(copySize, /*zeroInitialize*/ false);

    // actually relocate src objects into the allocated space.
    size_t dstPtrOrig = dstPtr;
    size_t objLimit = relocationSource->Start() + Satori::REGION_SIZE_GRANULARITY;
    SatoriObject* obj = relocationSource->FirstObject();
    // the target is typically marked, unless it is freshly allocated or in a higher generation
    // preserve marked state by copying marks
    bool needToCopyMarks = relocationTarget->MayHaveDeadObjects();
    do
    {
        size_t size = obj->Size();
        if (obj->IsMarked())
        {
            _ASSERTE(!obj->IsPermanentlyPinned());
            _ASSERTE(!obj->IsFree());

            memcpy((void*)(dstPtr - sizeof(size_t)), (void*)(obj->Start() - sizeof(size_t)), size);
            // record the new location of the object by storing it in the syncblock space.
            // make it negative so it is different from a normal syncblock.
            ((ptrdiff_t*)obj)[-1] = -(ptrdiff_t)dstPtr;

            if (needToCopyMarks)
            {
                ((SatoriObject*)dstPtr)->SetMarked();
            }

            dstPtr += size;
            obj = (SatoriObject*)(obj->Start() + size);
        }
        else
        {
            obj = relocationSource->SkipUnmarked(obj);
        }
    } while (obj->Start() < objLimit);

    size_t used = dstPtr - dstPtrOrig;
    relocationTarget->SetOccupancy(relocationTarget->Occupancy() + used);
    relocationTarget->StopAllocating(dstPtr);
    // the target may yet have more space and be a target for more relocations.
    AddRelocationTarget(relocationTarget);

    bool relocationIsPromotion = relocationTarget->Generation() > m_condemnedGeneration;
    if (relocationIsPromotion)
    {
        relocationTarget->SetAcceptedPromotedObjects(true);
        m_relocatedToHigherGenRegions->Push(relocationSource);
    }
    else
    {
        m_relocatedRegions->Push(relocationSource);
    }
}

void SatoriRecycler::UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
{
    size_t location = (size_t)*ppObject;
    if (location == 0)
    {
        return;
    }

    SatoriObject* o = SatoriObject::At(location);
    if (flags & GC_CALL_INTERIOR)
    {
        MarkContext* context = (MarkContext*)sc->_unused1;

        // byrefs may point to stack, use checked here
        o = context->m_heap->ObjectForAddressChecked(location);
        if (o == nullptr)
        {
            return;
        }

#ifdef FEATURE_CONSERVATIVE_GC
        if (GCConfig::GetConservativeGC() && o->IsFree())
        {
            return;
        }
#endif
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

void SatoriRecycler::Finish()
{
    if (m_isCompacting)
    {
        IncrementStackScanCount();
        SatoriHandlePartitioner::StartNextScan();

        ScanContext sc;
        sc.promotion = FALSE;
        MarkContext c = MarkContext(this);
        sc._unused1 = &c;

        //generations are meaningless here, so we pass -1
        GCToEEInterface::GcScanRoots(UpdateFn, -1, -1, &sc);

        SatoriHandlePartitioner::ForEachUnscannedPartition(
            [&](int p)
            {
                sc.thread_number = p;
                GCScan::GcScanHandles(UpdateFn, m_condemnedGeneration, 2, &sc);
            }
        );

        _ASSERTE(c.m_markChunk == nullptr);

        UpdatePointersInPromotedObjects();
        UpdateFinalizationQueue();

        if (m_condemnedGeneration != 2)
        {
            // IncrementCardScanTicket();
            UpdatePointersThroughCards();
        }
    }

    // finish and return target regions
    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        FinishRegions(m_relocationTargets[i]);
    }

    // finish and return staying regions
    FinishRegions(m_stayingRegions);

    if (m_isPromoting)
    {
        m_heap->ForEachPage(
            [](SatoriPage* page)
            {
                page->CardState() = Satori::CardState::BLANK;
            }
        );
    }

    // recycle relocated regions.
    SatoriRegion* curRegion;
    while (curRegion = m_relocatedRegions->TryPop())
    {
        curRegion->ClearMarks();
        curRegion->MakeBlank();
        m_heap->Allocator()->AddRegion(curRegion);
    }
}

void SatoriRecycler::UpdateFinalizationQueue()
{
    // update refs in finalization queue
    if (m_heap->FinalizationQueue()->HasItems())
    {
        // add finalization queue to mark list
        m_heap->FinalizationQueue()->ForEachObjectRef(
            [&](SatoriObject** ppObject)
            {
                SatoriObject* o = *ppObject;
                ptrdiff_t ptr = ((ptrdiff_t*)o)[-1];
                if (ptr < 0)
                {
                    *ppObject = (SatoriObject*)-ptr;
                }
            }
        );
    }
}

void SatoriRecycler::UpdatePointersInPromotedObjects()
{
    SatoriRegion* curRegion;
    while (curRegion = m_relocatedToHigherGenRegions->TryPop())
    {
        curRegion->UpdatePointersInPromotedObjects();
        m_relocatedRegions->Push(curRegion);
    }
}

void SatoriRecycler::FinishRegions(SatoriRegionQueue* queue)
{
    SatoriRegion* curRegion;
    while (curRegion = queue->TryPop())
    {
        // this can only happen when promoting
        if (curRegion->Generation() > m_condemnedGeneration)
        {
            if (curRegion->AcceptedPromotedObjects())
            {
                curRegion->ClearIndex();
                curRegion->UpdateFinalizibleTrackers();
                curRegion->SetAcceptedPromotedObjects(false);
            }
        }
        else if (m_isCompacting)
        {
            if (curRegion->MayHaveDeadObjects())
            {
                if (curRegion->SweepAndUpdatePointers())
                {
                    curRegion->MakeBlank();
                    m_heap->Allocator()->AddRegion(curRegion);
                    continue;
                }
            }
            else 
            {
                curRegion->UpdatePointers();
            }

            curRegion->UpdateFinalizibleTrackers();
        }

        // recycler owns nursery regions only temporarily, no need to return to queues.
        if (curRegion->Generation() == 0)
        {
            // when promoting, nursery regions should be detached and promoted to gen1
            _ASSERTE(!m_isPromoting);
            if (!m_isCompacting)
            {
                curRegion->Sweep();
            }

            continue;
        }

        if (m_isPromoting)
        {
            curRegion->SetGeneration(2);
            curRegion->WipeCards();
        }

        // make sure the region is swept - already, now, or later
        if (curRegion->MayHaveDeadObjects())
        {
            if (ENABLE_CONCURRENT)
            {
                m_deferredSweepRegions->Enqueue(curRegion);
                continue;
            }

            if (curRegion->Sweep())
            {
                curRegion->MakeBlank();
                m_heap->Allocator()->AddRegion(curRegion);
                continue;
            }
        }

        ReturnRegion(curRegion);
    }
}

void SatoriRecycler::ReturnRegion(SatoriRegion* curRegion)
{
    if (curRegion->Generation() == 2)
    {
        (curRegion->EverHadFinalizables() ? m_tenuredFinalizationTrackingRegions : m_tenuredRegions)->Push(curRegion);
    }
    else
    {
        (curRegion->EverHadFinalizables() ? m_ephemeralFinalizationTrackingRegions : m_ephemeralRegions)->Push(curRegion);
    }
}

bool SatoriRecycler::DrainDeferredSweepQueue(int64_t deadline)
{
    bool isGCThread = GCToEEInterface::IsGCThread();

    SatoriRegion* curRegion;
    while ((m_deferredSweepCount > 0) &&
        // decomitting may block, so leave this to GC threads, if possible.
        (isGCThread || !m_activeWorkers) &&
        (curRegion = m_deferredSweepRegions->TryPop()))
    {
        SweepAndReturnRegion(curRegion);
        Interlocked::Decrement(&m_deferredSweepCount);

        if (deadline && (GCToOSInterface::QueryPerformanceCounter() - deadline > 0))
        {
            break;
        }
    }

    return m_deferredSweepCount > 0;
}

void SatoriRecycler::SweepAndReturnRegion(SatoriRegion* curRegion)
{
    if (curRegion->Sweep())
    {
        curRegion->MakeBlank();
        m_heap->Allocator()->AddRegion(curRegion);
    }
    else
    {
        ReturnRegion(curRegion);
    }
}
