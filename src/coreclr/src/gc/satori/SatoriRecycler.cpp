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

void SatoriRecycler::Initialize(SatoriHeap* heap)
{
    m_heap = heap;

    m_regularRegions = new SatoriRegionQueue();
    m_finalizationTrackingRegions = new SatoriRegionQueue();

    m_finalizationScanCompleteRegions = new SatoriRegionQueue();
    m_finalizationPendingRegions = new SatoriRegionQueue();

    m_stayingRegions = new SatoriRegionQueue();
    m_relocatingRegions = new SatoriRegionQueue();

    m_workList = new SatoriMarkChunkQueue();
    m_gcInProgress = 0;

    m_gen1Count = m_gen2Count = 0;
    m_condemnedGeneration = 0;
}

// TODO: VS interlocked?
void SatoriRecycler::IncrementScanCount()
{
    m_scanCount++;
}

// TODO: VS volatile?
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

void SatoriRecycler::AddRegion(SatoriRegion* region)
{
    _ASSERTE(region->AllocStart() == 0);
    _ASSERTE(region->AllocRemaining() == 0);

    region->Verify();

    region->ResetOwningThread();
    region->CleanMarks();
    region->SetGeneration(1);

    if (region->HasFinalizables())
    {
        m_finalizationTrackingRegions->Push(region);
    }
    else
    {
        m_regularRegions->Push(region);
    }
}

// TODO: this should go to heuristics?
void SatoriRecycler::MaybeTriggerGC()
{
    int count = m_finalizationTrackingRegions->Count() + m_regularRegions->Count();
    if (count - m_prevRegionCount > 10)
    {
        if (m_gcInProgress)
        {
            //TODO: VS unhijack thread?
            GCToEEInterface::EnablePreemptiveGC();
            GCToEEInterface::DisablePreemptiveGC();
        }
        else if (Interlocked::CompareExchange(&m_gcInProgress, 1, 0) == 0)
        {
            // for now just do 1 , 2, 1, 2, ...
            int generation = m_scanCount % 2 == 0 ? 1 : 2;
            Collect(generation, /*force*/ false);
        }
    }
}

// TODO: VS gen1
//       clean all cards after gen2
//       Volatile for dirty (or put a comment)
//       pass generation to EE helpers
//       bariers
//       do not mark or trace into gen2 regions when in gen1
//       GcPromotionsGranted

void SatoriRecycler::Collect(int generation, bool force)
{
    bool wasCoop = GCToEEInterface::EnablePreemptiveGC();
    _ASSERTE(wasCoop);

    // stop other threads.
    GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

    // become coop again (it will not block since VM is done suspending)
    GCToEEInterface::DisablePreemptiveGC();

    int count = m_finalizationTrackingRegions->Count() + m_regularRegions->Count();
    if (count - m_prevRegionCount > 10 || force)
    {
        m_condemnedGeneration = generation;

        // deactivate all stacks
        DeactivateAllStacks();
        IncrementScanCount();

        MarkOwnStack();
        MarkOtherStacks();

        // mark handles
        MarkHandles();

        // mark through all cards that has interesting refs (remembered set).
        bool revisitCards = generation == 1 ?
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

        AssertNoWork();
        //       sync
        WeakPtrScan(/*isShort*/ true);
        //       sync
        ScanFinalizables();

        // TODO: VS why no sync before?
        while (m_workList->Count() > 0)
        {
            do
            {
                DrainMarkQueues();
                revisitCards = MarkThroughCards(/* minState */ Satori::CARD_DIRTY);
            } while (m_workList->Count() > 0 || revisitCards);

            DependentHandlesRescan();
        }

        AssertNoWork();
        //       sync 
        WeakPtrScan(/*isShort*/ false);
        WeakPtrScanBySingleThread();

        // TODO: VS can't know live size without scanning all live objects. We need to scan at least live ones.
        // Then we could as well coalesce gaps and thread to buckets.

        // plan regions:
        // 0% - return to recycler
        // > 80%   go to stayers
        // > 50% or with pins - targets, sweep and thread gaps, slice and release free tails, add to queues,   need buckets similar to allocator, should regs have buckets?
        //   targets go to stayers too.
        //
        // rest - add to move sources

        // once no more regs in queue
        // go through sources and relocate to destinations,
        // grab empties if no space, add to stayers and use as if gotten from free buckets.
        // if no space at all, put the region to stayers.

        // go through roots and update refs

        // go through stayers, update refs  (need to care about relocated in stayers, could happen if no space)


        // TODO: VS do trivial sweep for now
        auto SweepRegions = [&](SatoriRegionQueue* regions)
        {
            SatoriRegion* curRegion;
            while (curRegion = regions->TryPop())
            {
                // we must sweep in gen2, since unloadable types may invalidate method tables and make
                // unreachable objects unwalkable.
                // we do not sweep gen1 though. without compaction there is no benefit, just forcing index rebuilding.
                bool nothingMarked = generation == 2 ?
                    curRegion->Sweep() :
                    curRegion->NothingMarked();

                if (curRegion->Generation() <= generation && nothingMarked)
                {
                    // TODO: VS wipe cards should be a part of return and MakeBlank too, but make it minimal
                    curRegion->WipeCards();
                    curRegion->MakeBlank();
                    m_heap->Allocator()->AddRegion(curRegion);
                }
                else
                {
                    if (generation == 2)
                    {
                        // everything is Gen2 now.
                        curRegion->SetGeneration(2);
                        curRegion->WipeCards();
                    }

                    m_stayingRegions->Push(curRegion);
                }
            }

            while (curRegion = m_stayingRegions->TryPop())
            {
                curRegion->CleanMarks();
                regions->Push(curRegion);
            }
        };

        SweepRegions(m_regularRegions);
        SweepRegions(m_finalizationTrackingRegions);
        m_prevRegionCount = m_finalizationTrackingRegions->Count() + m_regularRegions->Count();

        m_gen1Count++;
        if (generation == 2)
        {
            m_gen2Count++;
        }
    }

    m_condemnedGeneration = 0;

    // restart VM
    GCToEEInterface::RestartEE(true);
    m_gcInProgress = false;
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
    context->Deactivate((SatoriHeap*)param);
}

void SatoriRecycler::DeactivateAllStacks()
{
    GCToEEInterface::GcEnumAllocContexts(DeactivateFn, this->m_heap);
}

class MarkContext
{
    friend class SatoriRecycler;

public:
    MarkContext(SatoriRecycler* recycler)
        : m_markChunk()
    {
        m_recycler = recycler;
    }

    void PushToMarkQueues(SatoriObject* o)
    {
        o->Validate();
        if (m_markChunk && m_markChunk->TryPush(o))
        {
            return;
        }

        m_recycler->PushToMarkQueuesSlow(m_markChunk, o);
    }

private:
    SatoriRecycler* m_recycler;
    SatoriMarkChunk* m_markChunk;
};

void SatoriRecycler::PushToMarkQueuesSlow(SatoriMarkChunk* &currentMarkChunk, SatoriObject* o)
{
    if (currentMarkChunk)
    {
        m_workList->Push(currentMarkChunk);
    }

#ifdef _DEBUG
    // Limit worklist to one item in debug/chk.
    // This is just to force more overflows. Otherwise they are rather rare.
    currentMarkChunk = nullptr;
    if (m_workList->Count() == 0)
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

        //TODO: VS put heap directly on context.
        o = context->m_recycler->m_heap->ObjectForAddressChecked(location);
        if (o == nullptr)
        {
            return;
        }
    }

    if (o->ContainingRegion()->Generation() == 0)
    {
        // do not mark thread local regions.
        _ASSERTE(!"thread local region is unexpected");
        return;
    }

    if (!o->IsMarked())
    {
        // TODO: VS should use threadsafe variant
        o->SetMarked();
        MarkContext* context = (MarkContext*)sc->_unused1;
        context->PushToMarkQueues(o);
    }

    if (flags & GC_CALL_PINNED)
    {
        o->SetPinned();
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
    GCToEEInterface::GcScanCurrentStackRoots((promote_func*)MarkFn, &sc);

    if (c.m_markChunk != nullptr)
    {
        m_workList->Push(c.m_markChunk);
    }
}

void SatoriRecycler::MarkOtherStacks()
{
    // mark roots for all stacks
    ScanContext sc;
    sc.promotion = TRUE;

    MarkContext c = MarkContext(this);
    sc._unused1 = &c;

    //TODO: VS there should be only one thread with "thread_number == 0"
    //TODO: VS implement two-pass scheme with preferred vs. any stacks

    GCToEEInterface::GcScanRoots((promote_func*)MarkFn, 2, 2, &sc);

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
                    if (child && !child->IsMarked())
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

bool SatoriRecycler::MarkThroughCards(int8_t minState)
{
    SatoriMarkChunk* dstChunk = nullptr;
    bool revisit = false;

    m_heap->ForEachPage(
        [&](SatoriPage* page)
        {
            if (page->CardState() >= minState)
            {
                page->SetProcessing();

                size_t groupCount = page->CardGroupCount();
                for (size_t i = 0; i < groupCount; i++)
                {
                    // TODO: VS when stealing is implemented we should start from a random location
                    if (page->CardGroup(i) >= minState)
                    {
                        int8_t* cards = page->CardsForGroup(i);
                        SatoriRegion* region = page->RegionForCardGroup(i);
                        int8_t resetValue = region->Generation() == 2 ? Satori::CARD_HAS_REFERENCES : Satori::CARD_BLANK;

                        //TODO: VS enable when truly generational.
                        bool considerAllMarked = false; // region->Generation() > m_condemnedGeneration;

                        page->CardGroup(i) = resetValue;
                        for (int j = 0; j < Satori::CARD_BYTES_IN_CARD_GROUP; j++)
                        {
                            //TODO: VS size_t steps, maybe, at least when skipping?
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

                            SatoriObject* o = region->FindObject(start);
                            do
                            {
                                // we trace only through marked objects.
                                // things to consider:
                                // 1) tracing into dead objects is dangerous. marked objects will not have that.
                                // 2) tracing from unmarked retains too much, in particular blocking gen2 must be precise.
                                // 3) overflow will mark before dirtying, so always ok.
                                // 4) for concurrent dirtying - asignment happening before marking will be traced (careful with HW order!!)
                                // 5) gen2 objects are all considered marked in partial GC, but full GC is precise.
                                // 6) gen2 should not have dead objects and must sweep (the other reason is unloadable types)
                                //
                                if (considerAllMarked || o->IsMarked())
                                {
                                    o->ForEachObjectRef(
                                        [&](SatoriObject** ref)
                                        {
                                            SatoriObject* child = *ref;
                                            if (child && !child->IsMarked())
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
                            } while (o->Start() < end);
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
    GCScan::GcScanHandles(MarkFn, 2, 2, &sc);

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
        GCScan::GcShortWeakPtrScan(nullptr, 2, 2, &sc);
    }
    else
    {
        GCScan::GcWeakPtrScan(nullptr, 2, 2, &sc);
    }
}

void SatoriRecycler::WeakPtrScanBySingleThread()
{
    // scan for deleted entries in the syncblk cache
    // does not use a context, so we pass nullptr
    GCScan::GcWeakPtrScanBySingleThread(2, 2, nullptr);
}

// can run concurrently, but not with mutator (since it may reregister for finalization) 
void SatoriRecycler::ScanFinalizables()
{
    MarkContext c = MarkContext(this);

    SatoriRegion* region;
    while (region = m_finalizationTrackingRegions->TryPop())
    {
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
                            if (m_heap->FinalizationQueue()->TryScheduleForFinalizationSingleThreaded(finalizable))
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
            m_finalizationScanCompleteRegions->Push(region);
        }
    }

    while (region = m_finalizationPendingRegions->TryPop())
    {
        region->ForEachFinalizable(
            [&](SatoriObject* finalizable)
            {
                if ((size_t)finalizable & Satori::FINALIZATION_PENDING)
                {
                    (size_t&)finalizable &= ~Satori::FINALIZATION_PENDING;

                    if (m_heap->FinalizationQueue()->TryScheduleForFinalizationSingleThreaded(finalizable))
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

        m_finalizationScanCompleteRegions->Push(region);
    }

    _ASSERTE(m_finalizationTrackingRegions->Count() == 0);

    // swap m_finalizationScanCompleteRegions and m_finalizationTrackingRegions
    SatoriRegionQueue* tmp = m_finalizationTrackingRegions;
    m_finalizationTrackingRegions = m_finalizationScanCompleteRegions;
    m_finalizationScanCompleteRegions = tmp;

    if (m_heap->FinalizationQueue()->HasItems())
    {
        // add finalization queue to mark list
        m_heap->FinalizationQueue()->ForEachObjectRef(
            [&](SatoriObject** ppObject)
            {
                SatoriObject* o = *ppObject;
                if (!o->IsMarked())
                {
                    o->SetMarked();
                    c.PushToMarkQueues(*ppObject);
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
    GCScan::GcDhInitialScan(MarkFn, 2, 2, &sc);

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
