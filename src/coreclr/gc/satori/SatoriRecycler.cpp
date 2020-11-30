// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRecycler.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

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

void SatoriRecycler::Initialize(SatoriHeap* heap)
{
    m_heap = heap;

    m_nurseryRegions = new SatoriRegionQueue(QueueKind::RecyclerNursery);

    m_ephemeralRegions = new SatoriRegionQueue(QueueKind::RecyclerEphemeral);
    m_ephemeralFinalizationTrackingRegions = new SatoriRegionQueue(QueueKind::RecyclerEphemeralFinalizationTracking);
    m_tenuredRegions = new SatoriRegionQueue(QueueKind::RecyclerTenured);
    m_tenuredFinalizationTrackingRegions = new SatoriRegionQueue(QueueKind::RecyclerTenuredFinalizationTracking);

    m_finalizationScanCompleteRegions = new SatoriRegionQueue(QueueKind::RecyclerFinalizationScanComplete);
    m_finalizationPendingRegions = new SatoriRegionQueue(QueueKind::RecyclerFinalizationPending);

    m_stayingRegions = new SatoriRegionQueue(QueueKind::RecyclerStaying);
    m_relocatingRegions = new SatoriRegionQueue(QueueKind::RecyclerRelocating);
    m_relocatedRegions = new SatoriRegionQueue(QueueKind::RecyclerRelocated);

    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        m_relocationTargets[i] = new SatoriRegionQueue(QueueKind::RecyclerRelocationTarget);
    }

    m_workList = new SatoriMarkChunkQueue();
    m_gcInProgress = 0;

    m_gen1Count = m_gen2Count = 0;
    m_condemnedGeneration = 0;
}

// not interlocked. this is not done concurrently. 
void SatoriRecycler::IncrementScanCount()
{
    m_scanCount++;
}

// CONSISTENCY: no synchronization needed
// there is only one writer (thread that initiates GC)
// and treads reading this are guarantee to see it
// since they need to know that there is GC in progress in the first place
int SatoriRecycler::GetScanCount()
{
    return m_scanCount;
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

int SatoriRecycler::RegionCount()
{
    return m_ephemeralFinalizationTrackingRegions->Count() + m_ephemeralRegions->Count() +
        m_tenuredFinalizationTrackingRegions->Count() + m_tenuredRegions->Count();
}

void SatoriRecycler::AddEphemeralRegion(SatoriRegion* region)
{
    _ASSERTE(region->AllocStart() == 0);
    _ASSERTE(region->AllocRemaining() == 0);
    _ASSERTE(region->Generation() < 2);

    region->Verify();

    if (region->EverHadFinalizables())
    {
        m_ephemeralFinalizationTrackingRegions->Push(region);
    }
    else
    {
        (region->Generation() == 0 ? m_nurseryRegions : m_ephemeralRegions)->Push(region);
    }
}

// TODO: VS this should be moved to heuristics.
void SatoriRecycler::MaybeTriggerGC()
{
    int count = RegionCount();

    if (count - m_prevRegionCount > 10)
    {
        if (m_gcInProgress)
        {
            GCToEEInterface::GcPoll();
        }
        else if (Interlocked::CompareExchange(&m_gcInProgress, 1, 0) == 0)
        {
            //TODO: VS start with gen1?  (though first gen2 is probably cheap)
            // for now just do every 16th scan (every 8 global GCs)
            int generation = m_scanCount % 16 == 0 ? 2 : 1;
            Collect(generation, /*force*/ false);
        }
    }
}

void SatoriRecycler::Collect(int generation, bool force)
{
    bool wasCoop = GCToEEInterface::EnablePreemptiveGC();
    _ASSERTE(wasCoop);

    // stop other threads.
    GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

    // become coop again (it will not block since VM is done suspending)
    GCToEEInterface::DisablePreemptiveGC();

    int count = RegionCount();
    if (count - m_prevRegionCount > 10 || force)
    {
        m_condemnedGeneration = generation;
        DeactivateAllStacks();

        Mark();
        Sweep();
        Compact();

        // TODO: VS we might be able to do this right after marking.
        //       since we are not demoting, this only updates the age of handles
        //       which does not look at the age or location of the actual objects.
        if (m_condemnedGeneration == 2)
        {
            PromoteSurvivedHandles();
        }

        // TODO: VS looks like this needs to hapen after PromoteSurvivedHandles. is that true?
        UpdatePointers();

        m_gen1Count++;
        if (m_condemnedGeneration == 2)
        {
            m_gen2Count++;
        }

        // TODO: update stats and heuristics.
        m_prevRegionCount = RegionCount();
    }

    m_condemnedGeneration = 0;

    // restart VM
    GCToEEInterface::RestartEE(true);
    m_gcInProgress = false;
}

void SatoriRecycler::Mark()
{
    IncrementScanCount();
    MarkOwnStack();
    MarkOtherStacks();

    // mark handles
    MarkHandles();

    // mark through all cards that have interesting refs (remembered set).
    bool revisitCards = m_condemnedGeneration == 1 ?
        MarkThroughCards(/* minState */ Satori::CARD_HAS_REFERENCES) :
        false;

    while (m_workList->Count() > 0 || revisitCards)
    {
        DrainMarkQueues();
        revisitCards = MarkThroughCards(/* minState */ Satori::CARD_DIRTY);
    }

    // all strongly reachable objects are marked here
    AssertNoWork();

    DependentHandlesInitialScan();
    while (m_workList->Count() > 0)
    {
        do
        {
            DrainMarkQueues();
            revisitCards = MarkThroughCards(/* minState */ Satori::CARD_DIRTY);
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
            revisitCards = MarkThroughCards(/* minState */ Satori::CARD_DIRTY);
        } while (m_workList->Count() > 0 || revisitCards);

        DependentHandlesRescan();
    }

    //       sync 
    AssertNoWork();

    WeakPtrScan(/*isShort*/ false);
    WeakPtrScanBySingleThread();
}

void SatoriRecycler::AssertNoWork()
{
    _ASSERTE(m_workList->Count() == 0);

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            _ASSERTE(page->CardState() < Satori::CARD_PROCESSING);
        }
    );
}

void SatoriRecycler::DeactivateFn(gc_alloc_context* gcContext, void* param)
{
    SatoriAllocationContext* context = (SatoriAllocationContext*)gcContext;
    SatoriRecycler* recycler = (SatoriRecycler*)param;

    context->Deactivate(recycler, /*detach*/ recycler->m_condemnedGeneration == 2);
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
    SatoriRecycler* m_recycler;
    SatoriMarkChunk* m_markChunk;
    SatoriHeap* m_heap;
    int m_condemnedGeneration;
};

void SatoriRecycler::PushToMarkQueuesSlow(SatoriMarkChunk*& currentMarkChunk, SatoriObject* o)
{
    if (currentMarkChunk)
    {
        m_workList->Push(currentMarkChunk);
    }

#ifdef _DEBUG
    // Limit worklist in debug/chk.
    // This is just to force more overflows. Otherwise they are rather rare.
    currentMarkChunk = nullptr;
    if (m_workList->Count() < 3)
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
        o->DirtyCardsForContent();
    }
}

void SatoriRecycler::MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
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
    }

    // TODO: VS when concurrent should not go into gen 0
    //if (o->ContainingRegion()->Generation() == 0)
    //{
    //    // do not mark thread local regions.
    //    _ASSERTE(!"thread local region is unexpected");
    //    return;
    //}

    MarkContext* context = (MarkContext*)sc->_unused1;
    if (o->ContainingRegion()->Generation() <= context->m_condemnedGeneration)
    {
        if (!o->IsMarked())
        {
            // TODO: VS should use threadsafe variant
            o->SetMarked();
            context->PushToMarkQueues(o);
        }

        if (flags & GC_CALL_PINNED)
        {
            // TODO: VS should use threadsafe variant
            o->SetPinned();
        }
    }
};

void SatoriRecycler::MarkOwnStack()
{
    gc_alloc_context* aContext = GCToEEInterface::GetAllocContext();

    // TODO: VS can this be more robust in case the thread gets stuck?
    // claim our own stack for scanning
    while (true)
    {
        int threadScanCount = aContext->alloc_count;
        int currentScanCount = GetScanCount();
        if (threadScanCount >= currentScanCount)
        {
            return;
        }

        if (Interlocked::CompareExchange(&aContext->alloc_count, currentScanCount, threadScanCount) == threadScanCount)
        {
            break;
        }
    }

    // mark roots for the current stack
    ScanContext sc;
    sc.promotion = TRUE;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;
    GCToEEInterface::GcScanCurrentStackRoots(MarkFn, &sc);

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::MarkOtherStacks()
{
    //TODO: VS can we reuse a context?
    // mark roots for all stacks
    ScanContext sc;
    sc.promotion = TRUE;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    //TODO: VS there should be only one thread with "thread_number == 0"
    //TODO: VS implement two-pass scheme with preferred vs. any stacks

    //generations are meaningless here, so we pass -1
    GCToEEInterface::GcScanRoots(MarkFn, -1, -1, &sc);

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::DrainMarkQueues()
{
    SatoriMarkChunk* srcChunk = m_workList->TryPop();
    SatoriMarkChunk* dstChunk = nullptr;
    while (srcChunk)
    {
        // drain srcChunk to dst chunk
        while (srcChunk->Count() > 0)
        {
            SatoriObject* o = srcChunk->Pop();
            _ASSERTE(o->IsMarked());
            o->ForEachObjectRef(
                [&](SatoriObject** ref)
                {
                    SatoriObject* child = *ref;
                    if (child && !child->IsMarkedOrOlderThan(m_condemnedGeneration))
                    {
                        child->SetMarked();
                        child->Validate();
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

//TODO: VS Re: concurrency
//      Card Marking will be done with EE suspended, so IU barriers do not need
//      to order card writes.
//      However marking/clearing itself may cause overflows and that could happen concurrently, thus:
//      - IU barriers can use regular writes to dirty cards/groups/pages
//      - Ovf dirtying must use write fences, but those should be very rare
//      - card maeking/clearing must use read fences, not a lot though - per page and per group.

bool SatoriRecycler::MarkThroughCards(int8_t minState)
{
    SatoriMarkChunk* dstChunk = nullptr;
    bool revisit = false;

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            // VolatileLoad to allow concurent card clearing.
            // Since we may concurrently make cards dirty due to overflow,
            // page must be checked first, then group, then cards.
            // Dirtying due to overflow will have to do writes in the opposite order.
            int8_t pageState = VolatileLoad(&page->CardState());
            if (pageState >= minState)
            {
                page->CardState() = Satori::CARD_PROCESSING;
                size_t groupCount = page->CardGroupCount();
                // TODO: VS when stealing is implemented we should start from a random location
                for (size_t i = 0; i < groupCount; i++)
                {
                    // VolatileLoad, see the comment above regading page/group/card read order
                    int8_t groupState = VolatileLoad(&page->CardGroup(i));
                    if (groupState >= minState)
                    {
                        SatoriRegion* region = page->RegionForCardGroup(i);

                        //ephemeral regions are not interesting here unless they are dirty.
                        if (groupState < Satori::CARD_DIRTY && region->Generation() < 2)
                        {
                            continue;
                        }

                        int8_t resetValue = region->Generation() == 2 ? Satori::CARD_HAS_REFERENCES : Satori::CARD_BLANK;
                        bool considerAllMarked = region->Generation() > m_condemnedGeneration;

                        int8_t* cards = page->CardsForGroup(i);
                        page->CardGroup(i) = resetValue;
                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for 0
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == 0)
                            {
                                j += sizeof(size_t) - 1;
                                continue;
                            }

                            if (cards[j] < minState)
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                cards[j++] = resetValue;
                            } while (j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j] >= minState);

                            size_t end = page->LocationForCard(&cards[j]);
                            size_t objLimit = min(end, region->Start() + Satori::REGION_SIZE_GRANULARITY);
                            SatoriObject* o = region->FindObject(start);
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
                                                child->SetMarked();
                                                child->Validate();
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
                revisit = !page->TrySetClean();
            }
        }
    );

    if (dstChunk)
    {
        m_workList->Push(dstChunk);
    }

    return revisit;
}

void SatoriRecycler::MarkHandles()
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc.thread_number = 0;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    // concurrent, per thread/heap
    // relies on thread_number to select handle buckets and specialcases #0
    GCScan::GcScanHandles(MarkFn, m_condemnedGeneration, 2, &sc);

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::WeakPtrScan(bool isShort)
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc.thread_number = 0;

    // concurrent, per thread/heap
    // relies on thread_number to select handle buckets and specialcases #0
    // null out the target of short weakref that were not promoted.
    if (isShort)
    {
        GCScan::GcShortWeakPtrScan(nullptr, m_condemnedGeneration, 2, &sc);
    }
    else
    {
        GCScan::GcWeakPtrScan(nullptr, m_condemnedGeneration, 2, &sc);
    }
}

void SatoriRecycler::WeakPtrScanBySingleThread()
{
    // scan for deleted entries in the syncblk cache
    // does not use a context, so we pass nullptr
    GCScan::GcWeakPtrScanBySingleThread(m_condemnedGeneration, 2, nullptr);
}

void SatoriRecycler::ScanFinalizableRegions(SatoriRegionQueue* regions)
{
    SatoriRegion* region;
    while (region = regions->TryPop())
    {
        bool hasPendingCF = false;

        region->ForEachFinalizable(
            [&](SatoriObject* finalizable)
            {
                _ASSERTE(((size_t)finalizable & Satori::FINALIZATION_PENDING) == 0);

                // reachable finalizables are not iteresting in any state.
                // finalizer can be suppressed and re-registered again without creating new trackers.
                // (this is preexisting behavior)

                if (!finalizable->IsMarkedOrOlderThan(m_condemnedGeneration))
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
                        finalizable->GetHeader()->ClrBit(BIT_SBLK_FINALIZER_RUN);
                        finalizable = nullptr;
                    }
                    else
                    {
                        // finalizable has just become unreachable
                        if (finalizable->RawGetMethodTable()->HasCriticalFinalizer())
                        {
                            // can't schedule just yet, because CriticalFinalizables must go
                            // after _all_  regular Finalizables scheduled in this GC
                            hasPendingCF = true;
                            (size_t&)finalizable |= Satori::FINALIZATION_PENDING;
                        }
                        else
                        {
                            if (m_heap->FinalizationQueue()->TryScheduleForFinalizationExclusive(finalizable))
                            {
                                // this tracker has served its purpose.
                                finalizable = nullptr;
                            }
                            else
                            {
                                _ASSERTE(!"handle overflow (just add the rest to pending?)");
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
            (region->Generation() == 0 ? m_nurseryRegions : m_finalizationScanCompleteRegions)->Push(region);
        }
    }
}

// TODO: VS can run concurrently, but not with mutator (since it may reregister for finalization) 
void SatoriRecycler::ScanFinalizables()
{
    ScanFinalizableRegions(m_ephemeralFinalizationTrackingRegions);
    if (m_condemnedGeneration == 2)
    {
        ScanFinalizableRegions(m_tenuredFinalizationTrackingRegions);
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

                    if (m_heap->FinalizationQueue()->TryScheduleForFinalizationExclusive(finalizable))
                    {
                        // this tracker has served its purpose.
                        finalizable = nullptr;
                    }
                    else
                    {
                        _ASSERTE(!"handle overflow (short circuit the rest) NOTE: must mark all that did not schedule and unpend.");
                    }
                }

                return finalizable;
            }
        );

        (region->Generation() == 0 ? m_nurseryRegions : m_finalizationScanCompleteRegions)->Push(region);
    }

    MarkContext c = MarkContext(this);
    if (m_heap->FinalizationQueue()->HasItems())
    {
        // add finalization queue to mark list
        m_heap->FinalizationQueue()->ForEachObjectRef(
            [&](SatoriObject** ppObject)
            {
                SatoriObject* o = *ppObject;
                if (!o->IsMarkedOrOlderThan(m_condemnedGeneration))
                {
                    o->SetMarked();
                    c.PushToMarkQueues(o);
                }
            }
        );

        GCToEEInterface::EnableFinalization(true);
    }

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::DependentHandlesInitialScan()
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc.thread_number = 0;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    // concurrent, per thread/heap
    // relies on thread_number to select handle buckets and specialcases #0
    GCScan::GcDhInitialScan(MarkFn, m_condemnedGeneration, 2, &sc);

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::DependentHandlesRescan()
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc.thread_number = 0;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    // concurrent, per thread/heap
    // relies on thread_number to select handle buckets and specialcases #0
    if (GCScan::GcDhUnpromotedHandlesExist(&sc))
    {
        GCScan::GcDhReScan(&sc);
    }

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::PromoteSurvivedHandles()
{
    ScanContext sc;
    sc.promotion = TRUE;
    sc.thread_number = 0;

    // no need for context. we do not create more work here.
    sc._unused1 = nullptr;

    GCScan::GcPromotionsGranted(m_condemnedGeneration, 2, &sc);
}

void SatoriRecycler::SweepNurseryRegions()
{
    _ASSERTE(m_condemnedGeneration != 2 || m_nurseryRegions->Count() == 0);

    // Sweep Gen0 regions.
    SatoriRegion* curRegion;
    while (curRegion = m_nurseryRegions->TryPop())
    {
        bool turnMarkedIntoEscaped = curRegion->IsThreadLocal();
        curRegion->Sweep(turnMarkedIntoEscaped);
        if (!turnMarkedIntoEscaped)
        {
            curRegion->ClearMarks();
        }

        curRegion->Verify();
        m_stayingRegions->Push(curRegion);
    }
}

void SatoriRecycler::Sweep()
{
    SweepNurseryRegions();
    SweepRegions(m_ephemeralRegions);

    if (m_condemnedGeneration == 2)
    {
        SweepRegions(m_tenuredRegions);
    }

    SweepRegions(m_finalizationScanCompleteRegions);
}

void SatoriRecycler::SweepRegions(SatoriRegionQueue* regions)
{
    bool compacting = true;

    SatoriRegion* curRegion;
    while (curRegion = regions->TryPop())
    {
        _ASSERTE(curRegion->Generation() != 0);

        bool canRecycle = false;
        if (m_condemnedGeneration == 2)
        {
            // we must sweep everything in gen2 GC, since after gen2 GC unloadable types
            // may invalidate method tables and make unreachable objects unwalkable.
            canRecycle = curRegion->Sweep(/*turnMarkedIntoEscaped*/ false);
        }
        else
        {
            _ASSERTE(curRegion->Generation() != 2);
            // when not compacting, gen1 GC does not need to sweep.
            canRecycle = compacting ?
                curRegion->Sweep(/*turnMarkedIntoEscaped*/ false) :
                curRegion->NothingMarked();
        }

        if (canRecycle)
        {
            curRegion->MakeBlank();
            m_heap->Allocator()->AddRegion(curRegion);
        }
        else
        {
            // if not compacting, we are done here
            if (!compacting)
            {
                m_stayingRegions->Push(curRegion);
                continue;
            }

            if ((curRegion->Occupancy() < Satori::REGION_SIZE_GRANULARITY / 4) &&
                !curRegion->HasPinnedObjects())
            {
                m_relocatingRegions->Push(curRegion);
            }
            else
            {
                AddRelocationTarget(curRegion);
            }
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
        m_relocationTargets[bucket]->Push(region);
    }
}

SatoriRegion* SatoriRecycler::TryGetRelocationTarget(size_t minSize)
{
    //make this occasionally fail in debug to be sure we can handle low memory case.
#if _DEBUG
    if (minSize % 1024 == 0)
    {
        return nullptr;
    }
#endif

    DWORD bucket;
    BitScanReverse64(&bucket, minSize);

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
                region->StartAllocating(minSize);
                return region;
            }
        }
    }

    SatoriRegion* newRegion = m_heap->Allocator()->GetRegion(ALIGN_UP(minSize, Satori::REGION_SIZE_GRANULARITY));
    if (newRegion)
    {
        newRegion->SetGeneration(m_condemnedGeneration);
    }

    return newRegion;
}

void SatoriRecycler::Compact()
{
    SatoriRegion* curRegion;
    while (curRegion = m_relocatingRegions->TryPop())
    {
        RelocateRegion(curRegion);
    }
}

void SatoriRecycler::RelocateRegion(SatoriRegion* relocationSource)
{
    relocationSource->Verify(true);

    size_t copySize = relocationSource->Occupancy();
    SatoriRegion* relocationTarget = TryGetRelocationTarget(copySize);

    // could not get a region. we must be low on available memory.
    // we can try using the source as a target for others.
    if (!relocationTarget)
    {
        AddRelocationTarget(relocationSource);
        return;
    }

    size_t dstPtr = relocationTarget->Allocate(copySize, /*zeroInitialize*/ false);
    size_t dstPtrOrig = dstPtr;

    size_t objLimit = relocationSource->Start() + Satori::REGION_SIZE_GRANULARITY;
    SatoriObject* obj = relocationSource->FirstObject();
    do
    {
        size_t size = obj->Size();
        if (obj->IsMarked())
        {
            // TODO: VS optimize by copying adjacent objects at once.
            //       Is there actually enough gain vs. touching src twice?   (can force copy N times to amplify cost and measure)
            //       Then update reloc data. Which could be tricky. Can use mark bits.
            //
            memcpy((void*)(dstPtr - sizeof(size_t)), (void*)(obj->Start() - sizeof(size_t)), size);
            ((ptrdiff_t*)obj)[-1] = -(ptrdiff_t)dstPtr;
            dstPtr += size;
        }

        obj = (SatoriObject*)(obj->Start() + size);
    } while (obj->Start() < objLimit);

    _ASSERTE(dstPtr - dstPtrOrig == copySize);
    relocationTarget->StopAllocating(/*allocPtr*/ 0);

    // transfer finalization trackers if any
    relocationTarget->TakeFinalizerInfoFrom(relocationSource);

    // the target may yet have more space. put it back.
    relocationTarget->Verify(true);
    AddRelocationTarget(relocationTarget);

    // the region is now relocated.
    m_relocatedRegions->Push(relocationSource);
}

void SatoriRecycler::UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags)
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
    }

    // TODO: VS not sure we need to check the region.
    //       - stack is more likely to reference younger stuff.
    //       - reading generation may thrash the cache by associating to same slot
    //       - scaning handles will filter to the generation (IS THIS TRUE?)

    // MarkContext* context = (MarkContext*)sc->_unused1;
    // if (o->ContainingRegion()->Generation() <= context->m_condemnedGeneration)
    {
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
    }
};

void SatoriRecycler::UpdatePointers()
{
    ScanContext sc;
    sc.promotion = FALSE;
    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    //TODO: VS there should be only one thread with "thread_number == 0"
    //TODO: VS implement two-pass scheme with preferred vs. any stacks
    IncrementScanCount();

    //generations are meaningless here, so we pass -1
    GCToEEInterface::GcScanRoots(UpdateFn, -1, -1, &sc);

    // concurrent, per thread/heap
    // relies on thread_number to select handle buckets and specialcases #0
    GCScan::GcScanHandles(UpdateFn, m_condemnedGeneration, 2, &sc);
    _ASSERTE(c.m_markChunk == nullptr);

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

    if (m_condemnedGeneration != 2)
    {
        UpdatePointersThroughCards();
    }

    // return target regions
    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        UpdatePointersInRegions(m_relocationTargets[i]);
    }

    // return staying regions
    UpdatePointersInRegions(m_stayingRegions);

    // recycle relocated regions.
    SatoriRegion* curRegion;
    while (curRegion = m_relocatedRegions->TryPop())
    {
        curRegion->ClearMarks();
        curRegion->MakeBlank();
        m_heap->Allocator()->AddRegion(curRegion);
    }
}

void SatoriRecycler::UpdatePointersInRegions(SatoriRegionQueue* queue)
{
    SatoriRegion* curRegion;
    while (curRegion = queue->TryPop())
    {
        curRegion->UpdateReferences();
        if (curRegion->Generation() == 0)
        {
            continue;
        }

        curRegion->ClearMarks();
        if (m_condemnedGeneration == 2)
        {
            curRegion->SetGeneration(2);
            curRegion->WipeCards();
            (curRegion->EverHadFinalizables() ? m_tenuredFinalizationTrackingRegions : m_tenuredRegions)->Push(curRegion);
        }
        else
        {
            (curRegion->EverHadFinalizables() ? m_ephemeralFinalizationTrackingRegions : m_ephemeralRegions)->Push(curRegion);
        }
    }
}

// TODO: VS how to steal? same for marking need a way to claim card groups, even/odd counter?
//              perhaps leave for later - need to handle numa/cores too,
//              maybe push pages/groups to WL)
void SatoriRecycler::UpdatePointersThroughCards()
{
    SatoriMarkChunk* dstChunk = nullptr;
    bool revisit = false;

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            int8_t pageState = page->CardState();
            if (pageState >= Satori::CARD_HAS_REFERENCES)
            {
                //TODO: VS claim the page
                page->CardState() = Satori::CARD_PROCESSING;

                size_t groupCount = page->CardGroupCount();
                // TODO: VS when stealing is implemented we should start from a random location
                for (size_t i = 0; i < groupCount; i++)
                {
                    int8_t groupState = page->CardGroup(i);
                    if (groupState >= Satori::CARD_HAS_REFERENCES)
                    {
                        SatoriRegion* region = page->RegionForCardGroup(i);

                        //ephemeral regions are not interesting here.
                        if (region->Generation() < 2)
                        {
                            continue;
                        }

                        // TODO: VS claim the group
                        int8_t resetValue = Satori::CARD_HAS_REFERENCES;

                        int8_t* cards = page->CardsForGroup(i);
                        page->CardGroup(i) = resetValue;
                        for (size_t j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            // cards are often sparsely set, if j is aligned, check the entire size_t for 0
                            if (((j & (sizeof(size_t) - 1)) == 0) && *((size_t*)&cards[j]) == 0)
                            {
                                j += sizeof(size_t) - 1;
                                continue;
                            }

                            if (cards[j] < Satori::CARD_HAS_REFERENCES)
                            {
                                continue;
                            }

                            size_t start = page->LocationForCard(&cards[j]);
                            do
                            {
                                _ASSERTE(cards[j] <= Satori::CARD_HAS_REFERENCES);
                                j++;
                            } while (j < Satori::CARD_BYTES_IN_CARD_GROUP && cards[j] >= Satori::CARD_HAS_REFERENCES);

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

                // record a missed clean to revisit the whole deal. 
                revisit = !page->TrySetClean();
                _ASSERTE(revisit == false);
            }
        }
    );
}
