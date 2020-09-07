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
    m_suspensionLock.Initialize();

    m_regularRegions = new SatoriRegionQueue();
    m_finalizationTrackingRegions = new SatoriRegionQueue();

    m_finalizationScanCompleteRegions = new SatoriRegionQueue();
    m_finalizationPendingRegions = new SatoriRegionQueue();

    m_stayingRegions = new SatoriRegionQueue();
    m_relocatingRegions = new SatoriRegionQueue();

    m_workList = new SatoriMarkChunkQueue();
}

void SatoriRecycler::AddRegion(SatoriRegion* region, bool forGc)
{
    // TODO: VS make end parsable?

    // TODO: VS verify

    // TODO: VS volatile?
    region->Publish();

    if (region->HasFinalizables())
    {
        m_finalizationTrackingRegions->Push(region);
    }
    else
    {
        m_regularRegions->Push(region);
    }

    int count = m_finalizationTrackingRegions->Count() + m_regularRegions->Count();
    if (!forGc && count - m_prevRegionCount > 10)
    {
        Collect();
    }
}

void SatoriRecycler::Collect()
{
    bool wasCoop = GCToEEInterface::EnablePreemptiveGC();
    _ASSERTE(wasCoop);

    {
        SatoriLockHolder<SatoriLock> holder(&m_suspensionLock);
        int count = m_finalizationTrackingRegions->Count() + m_regularRegions->Count();
        if (count - m_prevRegionCount <= 10)
        {
            goto exit;
        }

        // stop other threads.
        GCToEEInterface::SuspendEE(SUSPEND_FOR_GC);

        // deactivate all stacks
        DeactivateAllStacks();

        // mark own stack into work queues
        IncrementScanCount();
        MarkOwnStack();

        // TODO: VS perhaps drain queues as a part of MarkOwnStack? - to give other threads chance to self-mark?
        //       thread marking is fast though, so it may not help a lot.

        // TODO: VS this also scans statics. Do we want this?
        MarkOtherStacks();

        // drain queues
        DrainMarkQueues();

        // mark handles
        MarkHandles();

        while (m_workList->Count() > 0)
        {
            DrainMarkQueues();
            //TODO: VS clean cards   (could be due to overflow)
        }

        // all strongly reachable objects are marked here 

        DependentHandlesInitialScan();
        while (m_workList->Count() > 0)
        {
            DrainMarkQueues();
            //TODO: VS clean cards   (could be due to overflow)
            DependentHandlesRescan();
        }

        //       sync
        WeakPtrScan(/*isShort*/ true);
        //       sync
        ScanFinalizables();

        // TODO: VS why no sync before?
        while (m_workList->Count() > 0)
        {
            DrainMarkQueues();
            //TODO: VS clean cards   (could be due to overflow)
            DependentHandlesRescan();
        }

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
        // if no space at all, put the reg to stayers.

        // go through roots and update refs

        // go through stayers, update refs  (need to care about relocated in stayers, could happen if no space)


        // TODO: VS do trivial sweep for now
        auto SweepRegions = [&](SatoriRegionQueue* regions)
        {
            SatoriRegion* curReg;
            while (curReg = regions->TryPop())
            {
                if (curReg->NothingMarked())
                {
                    curReg->MakeBlank();
                    m_heap->Allocator()->AddRegion(curReg);
                }
                else
                {
                    m_stayingRegions->Push(curReg);
                }
            }

            while (curReg = m_stayingRegions->TryPop())
            {
                curReg->CleanMarks();
                regions->Push(curReg);
            }
        };

        SweepRegions(m_regularRegions);
        SweepRegions(m_finalizationTrackingRegions);
    }

    m_prevRegionCount = m_finalizationTrackingRegions->Count() + m_regularRegions->Count();

    // restart VM
    GCToEEInterface::RestartEE(true);

 exit:
    // become coop again (note - could block here, it is ok)
    GCToEEInterface::DisablePreemptiveGC();
}

void SatoriRecycler::DeactivateFn(gc_alloc_context* gcContext, void* param)
{
    SatoriAllocationContext* context = (SatoriAllocationContext*)gcContext;
    context->Deactivate((SatoriHeap*)param, /* forGc */ true);
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

    currentMarkChunk = m_heap->Allocator()->TryGetMarkChunk();
    if (currentMarkChunk)
    {
        currentMarkChunk->Push(o);
    }
    else
    {
        // TODO: VS handle mark overflow.
        _ASSERTE(!"overflow");
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
        //TODO: VS need ObjectForAddressChecked
        o = context->m_recycler->m_heap->ObjectForAddress(location);
        if (o == nullptr)
        {
            //TODO: VS when this could happen? matching orig GC?
            return;
        }
    }

    if (o->ContainingRegion()->IsThreadLocal())
    {
        // do not mark thread local regions.
        return;
    }

    if (!o->IsMarked())
    {
        // TODO: VS should use threadsafe variant
        o->SetMarked();

        MarkContext* context = (MarkContext*)sc->_unused1;
        // TODO: VS we do not need to push if card is marked, we will have to revisit anyways.

        // TODO: VS test card setting. for now this is unused.
        context->m_recycler->m_heap->SetCardForAddress(location);

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

// TODO: VS interlocked?
void SatoriRecycler::IncrementScanCount()
{
    m_scanCount++;
}

// TODO: VS volatile?
inline int SatoriRecycler::GetScanCount()
{
    return m_scanCount;
}

void SatoriRecycler::WaitOnSuspension()
{
    bool wasCoop = GCToEEInterface::EnablePreemptiveGC();

    {
        SatoriLockHolder<SatoriLock> holder(&m_suspensionLock);
    }

    if (wasCoop)
    {
        GCToEEInterface::DisablePreemptiveGC();
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
                        if (!dstChunk || !dstChunk->TryPush(child))
                        {
                            this->PushToMarkQueuesSlow(dstChunk, child);
                        }
                    }
                }
            );
        }

        // done with srcChunk
        // if we have nonempty dstChunk (i.e. produced more work),
        // swap src and dst and continue
        if (dstChunk && dstChunk->Count() > 0)
        {
            SatoriMarkChunk* tmp = srcChunk;
            _ASSERTE(tmp->Count() == 0);
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
