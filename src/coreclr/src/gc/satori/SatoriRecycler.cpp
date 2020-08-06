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
#include "../gcscan.h"

void SatoriRecycler::Initialize(SatoriHeap* heap)
{
    m_heap = heap;
    m_suspensionLock.Initialize();

    m_allRegions = new SatoriRegionQueue();
    m_stayingRegions = new SatoriRegionQueue();
    m_relocatingRegions = new SatoriRegionQueue();

    m_workList = new SatoriMarkChunkQueue();
    m_freeList = new SatoriMarkChunkQueue();

    SatoriRegion* region = m_heap->Allocator()->GetRegion(Satori::REGION_SIZE_GRANULARITY);

    while (true)
    {
        size_t mem = region->Allocate(Satori::MARK_CHUNK_SIZE, /*ensureZeroInited*/ false);
        if (!mem)
        {
            break;
        }

        SatoriMarkChunk* chunk = SatoriMarkChunk::InitializeAt(mem);
        m_freeList->Push(chunk);
    }
}

void SatoriRecycler::AddRegion(SatoriRegion* region, bool forGc)
{
    // TODO: VS make end parsable?

    // TODO: VS verify

    // TODO: VS volatile?
    region->Publish();

    int count = m_allRegions->Push(region);
    if (!forGc && count - m_baseCount > 10)
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
        if (m_allRegions->Count() - m_baseCount <= 10)
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

        // mark handles to queues
        MarkHandles();

        while (m_workList->Count() > 0)
        {
            DrainMarkQueues();
            //mark through SATB cards   (could be due to overflow)
        }

        // all marked here 

        // TODO: VS
        //       DH initial scan (could be a part of loop above?)
        //       sync
        WeakPtrScan(true);
        //       sync
        //       scan finalize queue
        //       scan DH again     (why no sync before?)
        //       sync 
        WeakPtrScan(false);
        WeakPtrScanBySingleThread();

        // TODO: VS can't know live size without scanning all live objects. We need to scan at least live ones.
        // Then we could as well coalesce gaps and thread to buckets.
        // What to do with finalizables?

        // plan regions:
        // 0% - return to recycler
        // > 80%   go to stayers   (scan for finalizable one day, if occupancy reduced and has finalizable, this can be done after releasing VM.)
        // > 50% or with pins - targets, sweep and thread gaps, slice and release free tails, add to queues,   need buckets similar to allocator, should regs have buckets?
        //   targets go to stayers too.
        //
        // rest - add to move sources
        SatoriRegion* curReg;
        while (curReg = m_allRegions->TryPop())
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

        // once no more regs in queue
        // go through sources and relocate to destinations,
        // grab empties if no space, add to stayers and use as if gotten from free buckets.
        // if no space at all, put the reg to stayers. (scan for finalizable one day)

        // go through roots and update refs

        // go through stayers, update refs  (need to care about relocated in stayers, could happen if no space)
        while (curReg = m_stayingRegions->TryPop())
        {
            curReg->CleanMarks();
            m_allRegions->Push(curReg);
        }
    }

    m_baseCount = m_allRegions->Count();

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

    currentMarkChunk = m_freeList->TryPop();
    if (currentMarkChunk)
    {
        bool pushed = currentMarkChunk->TryPush(o);
        _ASSERTE(pushed);
    }
    else
    {
        // TODO: VS mark card table
        o->SetEscaped();
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
        SatoriObject* o;
        while (o = srcChunk->TryPop())
        {
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
            m_freeList->Push(srcChunk);
            srcChunk = m_workList->TryPop();
        }
    }

    if (dstChunk)
    {
        _ASSERTE(dstChunk->Count() == 0);
        m_freeList->Push(dstChunk);
    }
}

void SatoriRecycler::MarkHandles()
{
    // mark roots for the current stack
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
    // mark roots for the current stack
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
