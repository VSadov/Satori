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
// SatoriAllocator.cpp
//

#include "common.h"
#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../gceventstatus.h"

#include "SatoriUtil.h"
#include "SatoriHeap.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"
#include "SatoriPage.h"
#include "SatoriAllocator.h"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"
#include "SatoriRegionQueue.h"
#include "SatoriAllocationContext.h"
#include "SatoriWorkChunk.h"
#include "SatoriWorkList.h"

void SatoriAllocator::Initialize(SatoriHeap* heap)
{
    m_heap = heap;

    for (int i = 0; i < Satori::ALLOCATOR_BUCKET_COUNT; i++)
    {
        m_queues[i] = new (nothrow) SatoriRegionQueue(QueueKind::Allocator);
    }

    m_workChunks = new (nothrow) SatoriWorkList();

    m_immortalRegion = nullptr;
    m_immortalAlocLock.Initialize();

    m_pinnedRegion = nullptr;
    m_pinnedAlocLock.Initialize();

    m_largeRegion = nullptr;
    m_largeAlocLock.Initialize();

    m_regularRegion = nullptr;
    m_regularAlocLock.Initialize();
}

SatoriRegion* SatoriAllocator::GetRegion(size_t regionSize)
{
    _ASSERTE(regionSize % Satori::REGION_SIZE_GRANULARITY == 0);

tryAgain:
    SatoriRegion* putBack = nullptr;

    int bucket = SizeToBucket(regionSize);

    SatoriRegion* region = (bucket == 0) ?
                            m_queues[bucket]->TryPop():
                            m_queues[bucket]->TryRemoveWithSize(regionSize, putBack);

    if (region)
    {
        if (putBack)
        {
            AddRegion(putBack);
        }

        return region;
    }

    while (++bucket < Satori::ALLOCATOR_BUCKET_COUNT)
    {
        region = m_queues[bucket]->TryPopWithSize(regionSize, putBack);
        if (region)
        {
            if (putBack)
            {
                AddRegion(putBack);
            }

            return region;
        }
    }

    // for regions < 1/2 page granularity, just allocate a regular page
    // this is the most common case.
    // for bigger ones, use more complex helper that may allocate a larger
    // page, if needed.
    if (regionSize < Satori::PAGE_SIZE_GRANULARITY / 2)
    {
        SatoriPage* page = nullptr;
        if (m_heap->TryAddRegularPage(page))
        {
            if (page)
            {
                putBack = page->MakeInitialRegion();
                region = putBack->TrySplit(regionSize);
                AddRegion(putBack);
                return region;
            }

            // Someone added a page, but not us. There could be more regions.
            goto tryAgain;
        }
    }
    else
    {
        SatoriPage* page = m_heap->AddLargePage(regionSize);
        if (page)
        {
            region = page->MakeInitialRegion();
            _ASSERTE(region->Size() >= regionSize);
            if (region->Size() > regionSize)
            {
                putBack = region->TrySplit(region->Size() - regionSize);
                if (putBack)
                {
                    AddRegion(putBack);
                }
                else
                {
                    // OOM
                    AddRegion(region);
                    region = nullptr;
                }
            }

            return region;
        }
    }

    // most likely OOM
    return nullptr;
}

void SatoriAllocator::AddRegion(SatoriRegion* region)
{
    _ASSERTE(region->Generation() == -1);
    m_queues[SizeToBucket(region->Size())]->Enqueue(region);
}

// same as AddRegion, just puts it in front of the queue
// this is for recently used regions
void SatoriAllocator::ReturnRegion(SatoriRegion* region)
{
    _ASSERTE(region->IsAttachedToAllocatingOwner() == false);
    _ASSERTE(region->Generation() == -1);
    m_queues[SizeToBucket(region->Size())]->Push(region);
}

Object* SatoriAllocator::Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size = ALIGN_UP(size, Satori::OBJECT_ALIGNMENT);

    if ((flags & (GC_ALLOC_IMMORTAL | GC_ALLOC_PINNED_OBJECT_HEAP | GC_ALLOC_LARGE_OBJECT_HEAP)) == 0)
    {
        if (context->alloc_ptr + size <= context->alloc_limit)
        {
            SatoriObject* result = (SatoriObject*)context->alloc_ptr;
            if ((flags & GC_ALLOC_FINALIZE) && !result->ContainingRegion()->RegisterForFinalization(result))
            {
                result = nullptr;
            }

            context->alloc_ptr += size;
            return result;
        }
        else
        {
            return AllocRegular(context, size, flags);
        }
    }

    if (flags & GC_ALLOC_PINNED_OBJECT_HEAP)
    {
        SatoriObject* result = AllocPinned(context, size, flags);
        if (result != nullptr)
        {
            result->SetUnmovable();
        }

        return result;
    }

    if (flags & GC_ALLOC_LARGE_OBJECT_HEAP)
    {
        return AllocLarge(context, size, flags);
    }

    if (flags & GC_ALLOC_IMMORTAL)
    {
        return AllocImmortal(context, size, flags);
    }

    return nullptr;
}

SatoriObject* SatoriAllocator::AllocRegular(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    m_heap->Recycler()->HelpOnce();
    if (!context->RegularRegion())
    {
        if ((context->alloc_bytes ^ (context->alloc_bytes + SatoriUtil::MinZeroInitSize())) >> Satori::REGION_BITS)
        {
            m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_soh);
        }

        SatoriObject* freeObj = context->alloc_ptr != 0 ? context->FinishAllocFromShared() : nullptr;

        m_regularAlocLock.Enter();
        //if (m_regularAlocLock.TryEnter())
        {
            if (freeObj && freeObj->ContainingRegion() == m_regularRegion)
            {
                m_regularRegion->SetOccupancy(m_regularRegion->Occupancy() - freeObj->Size());
                m_regularRegion->AddFreeSpace(freeObj);
            }

            return AllocRegularShared(context, size, flags);
        }
    }

    SatoriRegion* region = context->RegularRegion();
    _ASSERTE(region == nullptr || region->IsAttachedToAllocatingOwner());

    while (true)
    {
        if (region != nullptr)
        {
            _ASSERTE((size_t)context->alloc_limit == region->GetAllocStart());
            size_t moreSpace = context->alloc_ptr + size - context->alloc_limit;

            // try allocate more to form a contiguous chunk to fit size 
            size_t allocRemaining = region->GetAllocRemaining();
            if (moreSpace <= allocRemaining)
            {
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                if (zeroInitialize && moreSpace < SatoriUtil::MinZeroInitSize())
                {
                    moreSpace = min(allocRemaining, SatoriUtil::MinZeroInitSize());
                }

                if (region->Allocate(moreSpace, zeroInitialize))
                {
                    context->alloc_bytes += moreSpace;
                    context->alloc_limit += moreSpace;

                    SatoriObject* result = (SatoriObject*)(size_t)context->alloc_ptr;
                    if ((flags & GC_ALLOC_FINALIZE) &&
                        !region->RegisterForFinalization(result))
                    {
                        return nullptr;
                    }

                    context->alloc_ptr += size;
                    result->CleanSyncBlock();
                    region->SetIndicesForObject(result, result->Start() + size);

                    FIRE_EVENT(GCAllocationTick_V4,
                        size,
                        /*gen_number*/ 1,
                        /*heap_number*/ 0,
                        (void*)result,
                        0);

                    return result;
                }
                else
                {
                    //OOM
                    return nullptr;
                }
            }

            // unclaim unused.
            context->alloc_bytes -= context->alloc_limit - context->alloc_ptr;
            if (region->IsAllocating())
            {
                region->StopAllocating((size_t)context->alloc_ptr);
            }
            else
            {
                _ASSERTE(context->alloc_ptr == nullptr);
            }

            // try get from the free list
            if (region->StartAllocating(size))
            {
                // we have enough free space in the region to continue
                context->alloc_ptr = context->alloc_limit = (uint8_t*)region->GetAllocStart();
                continue;
            }

            if (region->IsEscapeTracking())
            {
                // try performing thread local collection and see if we have enough space after that.
                if (region->ThreadLocalCollect(context->alloc_bytes))
                {
                    if (region->StartAllocating(size))
                    {
                        // we have enough free space in the region to continue
                        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->GetAllocStart();
                        continue;
                    }
                }
            }

            context->alloc_ptr = context->alloc_limit = nullptr;
            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);

            // if we got this far with region not detached, get another one
            // TUNING: we could force trying to allocate from shared based on some heuristic
        }

        m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_soh);
        TryGetRegularRegion(region);

        if (region == nullptr)
        {
            //OOM
            return nullptr;
        }

        // the order of assignments:
        // 1) Attach  (optional: set escape tag)
        // 2) Set Generation to 0 or 1
        // 3) Reset ReusableFor
        // <objects allocated>
        // 4) (optional: clear escape tag) Detach       

        region->AttachToAllocatingOwner(&context->RegularRegion());
        if (SatoriUtil::IsThreadLocalGCEnabled())
        {
            switch (region->ReusableFor())
            {
            case SatoriRegion::ReuseLevel::Gen0:
                region->EscsapeAll();
                goto fallthrough;
            case SatoriRegion::ReuseLevel::None:
            fallthrough:
                //NB: sets Generation to 0
                region->StartEscapeTrackingRelease(SatoriUtil::GetCurrentThreadTag());
                break;
            case SatoriRegion::ReuseLevel::Gen1:
                region->SetGenerationRelease(1);
                break;
            }
        }
        else
        {
            region->SetGenerationRelease(1);
        }

        region->ResetReusableForRelease();
        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->GetAllocStart();
    }
}

SatoriObject* SatoriAllocator::AllocRegularShared(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    SatoriRegion* region = m_regularRegion;
    _ASSERTE(region == nullptr || region->IsAttachedToAllocatingOwner());

    while (true)
    {
        if (region != nullptr)
        {
            _ASSERTE(!region->IsAllocating());
            // try get from the free list
            size_t moreSpace = size + Satori::MIN_FREE_SIZE;
            if (region->StartAllocating(moreSpace))
            {
                // we have enough free space in the region to continue
                context->alloc_ptr = context->alloc_limit = (uint8_t*)region->GetAllocStart();
                size_t allocRemaining = region->GetAllocRemaining();
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                if (zeroInitialize && moreSpace < SatoriUtil::MinZeroInitSize())
                {
                    moreSpace = min(allocRemaining, SatoriUtil::MinZeroInitSize());
                }

                // do not zero-initialize just yet, we will do that after leaving the lock.
                if (region->Allocate(moreSpace, /*zeroInitialize*/ false))
                {
                    // TODO: VS pass 0
                    region->StopAllocating((size_t)context->alloc_ptr + moreSpace);

                    // region stays unparsable until "moreSpace" is consumed.
                    region->IncrementUnfinishedAlloc();
                    // conservatively assume the region may get finalizable objects.
                    // this is for initial placement into queues, which may happen before
                    // all objects in the buffer are allocated.
                    region->SetHasFinalizables();

                    // leave some space at the end for the free object
                    moreSpace -= Satori::MIN_FREE_SIZE;
                    context->alloc_bytes += moreSpace;
                    context->alloc_limit += moreSpace;

                    SatoriObject* result = (SatoriObject*)(size_t)context->alloc_ptr;
                    if ((flags & GC_ALLOC_FINALIZE) &&
                        !region->RegisterForFinalization(result))
                    {
                        // TODO: do this before StopAllocating, and call StopAllocating with context->alloc_ptr
                        m_regularAlocLock.Leave();
                        return nullptr;
                    }

                    region->SetIndicesForObject(result, result->Start() + size);

                    // done with region modifications.
                    m_regularAlocLock.Leave();

                    context->alloc_ptr += size;
                    result->CleanSyncBlock();
                    if (zeroInitialize)
                    {
                        memset(result, 0, moreSpace);
                    }

                    FIRE_EVENT(GCAllocationTick_V4,
                        size,
                        /*gen_number*/ 1,
                        /*heap_number*/ 0,
                        (void*)result,
                        0);

                    return result;
                }
                else
                {
                    //OOM
                    region->StopAllocating(0);
                    m_regularAlocLock.Leave();
                    return nullptr;
                }
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        TryGetRegularRegion(region);
        if (region == nullptr)
        {
            //OOM
            return nullptr;
        }

        region->TryCommit();

        region->AttachToAllocatingOwner(&m_regularRegion);
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
        if (region->IsAllocating())
        {
            region->StopAllocating(0);
        }
    }
}

void SatoriAllocator::TryGetRegularRegion(SatoriRegion*& region)
{
    // opportunistically try get the next region, if available
    SatoriRegion* next = nullptr;
    SatoriQueue<SatoriRegion>* nextContainingQueue = nullptr;

    if (region)
    {
        next = region->NextInPage();
        if (next)
        {
            nextContainingQueue = next->ContainingQueue();
            if (nextContainingQueue &&
                nextContainingQueue->Kind() == QueueKind::RecyclerReusable &&
                nextContainingQueue->TryRemove(next))
            {
                region = next;
                return;
            }
        }
    }

    region = m_heap->Recycler()->TryGetReusable();

    if (region == nullptr)
    {
        if (nextContainingQueue &&
            nextContainingQueue->Kind() == QueueKind::Allocator &&
            next->Size() == Satori::REGION_SIZE_GRANULARITY &&
            nextContainingQueue->TryRemove(next))
        {
            if (next->Size() != Satori::REGION_SIZE_GRANULARITY)
            {
                ReturnRegion(next);
            }
            else
            {
                region = next;
                return;
            }
        }

        region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
        _ASSERTE(region == nullptr || region->NothingMarked());
    }
}

SatoriObject* SatoriAllocator::AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    // handle huge allocation. It can't be from the current region.
    if (size > SatoriRegion::MAX_LARGE_OBJ_SIZE)
    {
        return AllocHuge(context, size, flags);
    }

    m_heap->Recycler()->HelpOnce();

tryAgain:
    // TODO: VS too large size do not use shared?
    if (!context->LargeRegion())
    {
        if ((context->alloc_bytes_uoh ^ (context->alloc_bytes_uoh + size)) >> Satori::REGION_BITS)
        {
            m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_loh);
        }

        //m_largeAlocLock.Enter();
        if (m_largeAlocLock.TryEnter())
        {
            return AllocLargeShared(context, size, flags);
        }
    }

    SatoriRegion* region = context->LargeRegion();
    while (true)
    {
        if (region)
        {
            size_t allocRemaining = region->GetAllocRemaining();
            if (allocRemaining >= size)
            {
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                SatoriObject* result = (SatoriObject*)region->Allocate(size, zeroInitialize);
                if (!result)
                {
                    // OOM
                    return nullptr;
                }

                if (flags & GC_ALLOC_FINALIZE)
                {
                    if (!region->RegisterForFinalization(result))
                    {
                        // OOM
                        region->StopAllocating((size_t)result - size);
                        return nullptr;
                    }

                    region->SetHasFinalizables();
                }

                // success
                context->alloc_bytes_uoh += size;
                result->CleanSyncBlock();
                region->SetIndicesForObject(result, result->Start() + size);

                FIRE_EVENT(GCAllocationTick_V4,
                    size,
                    /*gen_number*/ 1,
                    /*heap_number*/ 0,
                    (void*)result,
                    0);

                return result;
            }
        }

        // check if existing region has space or drop it into recycler
        if (region)
        {
            if (region->IsAllocating())
            {
                region->StopAllocating(/* allocPtr */ 0);
            }

            // try get from the free list
            if (region->StartAllocating(size))
            {
                // we have enough free space in the region to continue
                continue;
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);

            // retry
            goto tryAgain;
        }

        // get a new regular region.
        _ASSERTE(SatoriRegion::RegionSizeForAlloc(size) == Satori::REGION_SIZE_GRANULARITY);

        region = nullptr;
        if (size < Satori::REGION_SIZE_GRANULARITY / 2)
        {
            region = m_heap->Recycler()->TryGetReusableForLarge();
        }

        if (!region)
        {
            region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
            if (!region)
            {
                //OOM
                return (SatoriObject*)nullptr;
            }

            _ASSERTE(region->NothingMarked());
        }

        region->AttachToAllocatingOwner(&context->LargeRegion());
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
    }
}

SatoriObject* SatoriAllocator::AllocLargeShared(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    SatoriRegion* region = m_largeRegion;
    _ASSERTE(region == nullptr || region->IsAttachedToAllocatingOwner());

    while (true)
    {
        if (region != nullptr)
        {
            _ASSERTE(!region->IsAllocating() || region->GetAllocRemaining() >= size);
            // try get from the free list
            if (region->IsAllocating() || region->StartAllocating(size))
            {
                // we have enough free space in the region to continue
                // do not zero-initialize just yet, we will do that after leaving the lock.
                SatoriObject* result = (SatoriObject*)region->Allocate(size, /*zeroInitialize*/ false);
                if (!result)
                {
                    //OOM
                    region->StopAllocating(0);
                    m_largeAlocLock.Leave();
                    return nullptr;
                }

                if (flags & GC_ALLOC_FINALIZE)
                {
                    if (!region->RegisterForFinalization(result))
                    {
                        // OOM
                        region->StopAllocating((size_t)result - size);
                        m_largeAlocLock.Leave();
                        return nullptr;
                    }

                    region->SetHasFinalizables();
                }

                region->StopAllocating(0);
                // region stays unparsable until allocation is complete.
                region->IncrementUnfinishedAlloc();
                // done with region modifications.
                m_largeAlocLock.Leave();

                context->alloc_bytes_uoh += size;
                result->CleanSyncBlock();
                if (!(flags & GC_ALLOC_ZEROING_OPTIONAL))
                {
                    memset(result, 0, size);
                }

                FIRE_EVENT(GCAllocationTick_V4,
                    size,
                    /*gen_number*/ 1,
                    /*heap_number*/ 0,
                    (void*)result,
                    0);

                return result;
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        // get a new regular region.
        _ASSERTE(SatoriRegion::RegionSizeForAlloc(size) == Satori::REGION_SIZE_GRANULARITY);

        region = nullptr;
        if (size < Satori::REGION_SIZE_GRANULARITY / 2)
        {
            region = m_heap->Recycler()->TryGetReusableForLarge();
            _ASSERTE(region == nullptr || !region->IsAllocating());
        }

        if (!region)
        {
            region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
            if (!region)
            {
                //OOM
                m_largeAlocLock.Leave();
                return nullptr;
            }

            _ASSERTE(region->NothingMarked());
        }

        region->AttachToAllocatingOwner(&m_largeRegion);
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
        region->TryCommit();
    }
}

void SatoriAllocator::UnlockRegionIfShared(SatoriRegion* region)
{
    if (region == m_largeRegion)
    {
        region->DecrementUnfinishedAlloc();
    }
    else if (region == m_pinnedRegion)
    {
        m_pinnedAlocLock.Leave();
    }
}

SatoriObject* SatoriAllocator::AllocHuge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size_t regionSize = SatoriRegion::RegionSizeForAlloc(size);
    _ASSERTE(regionSize > Satori::REGION_SIZE_GRANULARITY);

    m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_loh);
    SatoriRegion* hugeRegion = GetRegion(regionSize);
    if (!hugeRegion)
    {
        //OOM
        return nullptr;
    }

    _ASSERTE(hugeRegion->NothingMarked());
    bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
    SatoriObject* result = (SatoriObject*)hugeRegion->AllocateHuge(size, zeroInitialize);
    if (!result)
    {
        ReturnRegion(hugeRegion);
        // OOM
        return nullptr;
    }

    result->CleanSyncBlock();

    // huge regions are not attached to contexts and in gen0+ would appear parseable,
    // but this one is not parseable yet since the new object has no MethodTable
    // we will keep the region in gen -1 for now and make it gen1 or gen2 in PublishObject.
    hugeRegion->StopAllocating(/* allocPtr */ 0);

    if ((flags & GC_ALLOC_FINALIZE) &&
        !hugeRegion->RegisterForFinalization(result))
    {
        hugeRegion->MakeBlank();
        ReturnRegion(hugeRegion);
        return nullptr;
    }

    FIRE_EVENT(GCAllocationTick_V4,
        size,
        /*gen_number*/ 2,
        /*heap_number*/ 0,
        (void*)result,
        0);

    context->alloc_bytes_uoh += size;
    return result;
}

SatoriObject* SatoriAllocator::AllocPinned(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    if (size > SatoriRegion::MAX_LARGE_OBJ_SIZE)
    {
        return AllocHuge(context, size, flags);
    }

    if ((context->alloc_bytes_uoh ^ (context->alloc_bytes_uoh + size)) >> Satori::REGION_BITS)
    {
        m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_soh);
    }

    // if can't get a lock, let AllocLarge handle this.
    if (!m_pinnedAlocLock.TryEnter())
    {
        return AllocLarge(context, size, flags);
    }

    SatoriRegion* region = m_pinnedRegion;
    while (true)
    {
        if (region)
        {
            size_t allocRemaining = region->GetAllocRemaining();
            if (allocRemaining >= size)
            {
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                SatoriObject* result = (SatoriObject*)region->Allocate(size, zeroInitialize);
                if (result &&
                    ((flags & GC_ALLOC_FINALIZE) == 0 || (region->RegisterForFinalization(result))))
                {
                    // success
                    result->CleanSyncBlock();
                    region->SetIndicesForObject(result, result->Start() + size);
                    context->alloc_bytes_uoh += size;

                    FIRE_EVENT(GCAllocationTick_V4,
                        size,
                        /*gen_number*/ 1,
                        /*heap_number*/ 0,
                        (void*)result,
                        0);

                    // NOTE: we are not leaving the lock here since the allocation is not complete.
                    //       we will leave in PublishObject
                    return result;
                }

                // OOM
                region->StopAllocating((size_t)result - size);
                m_pinnedAlocLock.Leave();
                return nullptr;
            }
        }

        // check if existing region has space or drop it into recycler
        if (region)
        {
            if (region->IsAllocating())
            {
                region->StopAllocating(/* allocPtr */ 0);
            }

            // try get from the free list
            size_t desiredFreeSpace = size + Satori::MIN_FREE_SIZE;
            if (region->StartAllocating(desiredFreeSpace))
            {
                // we have enough free space in the region to continue
                continue;
            }

            region->DetachFromAlocatingOwnerRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        // get a new regular region.
        _ASSERTE(SatoriRegion::RegionSizeForAlloc(size) == Satori::REGION_SIZE_GRANULARITY);

        region = nullptr;
        if (size < Satori::REGION_SIZE_GRANULARITY / 2)
        {
            region = m_heap->Recycler()->TryGetReusableForLarge();
        }

        if (!region)
        {
            region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
            if (!region)
            {
                //OOM
                m_pinnedAlocLock.Leave();
                return nullptr;
            }

            _ASSERTE(region->NothingMarked());
        }

        region->AttachToAllocatingOwner(&m_pinnedRegion);
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
    }
}

SatoriObject* SatoriAllocator::AllocImmortal(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    // immortal allocs should be way less than region size.
    _ASSERTE(size < Satori::REGION_SIZE_GRANULARITY / 2);

    SatoriLockHolder<SatoriLock> holder(&m_immortalAlocLock);
    SatoriRegion* region = m_immortalRegion;

    while (true)
    {
        if (region)
        {
            size_t allocRemaining = region->GetAllocRemaining();
            if (allocRemaining >= size)
            {
                // we ensure that region is zero-inited
                // so we do not need to clear here for typically small objects
                SatoriObject* result = (SatoriObject*)region->Allocate(size, /*zeroInitialize*/ false);
                if (result)
                {
                    context->alloc_bytes_uoh += size;
                    region->SetIndicesForObject(result, result->Start() + size);
                }
                else
                {
                    // OOM
                }

                return result;
            }

            if (region->IsAllocating())
            {
                region->StopAllocating(/* allocPtr */ 0);
            }
            else
            {
                size_t desiredFreeSpace = size + Satori::MIN_FREE_SIZE;
                if (region->StartAllocating(desiredFreeSpace))
                {
                    // we have enough free space in the region to continue
                    continue;
                }
            }
        }

        // get a new regular region.
        region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (!region)
        {
            //OOM
            return nullptr;
        }

        _ASSERTE(region->NothingMarked());
        // Ensure the region is zeroed, to not clear for each allocated (and typically small) object.
        // And, while at that, link the previous one in case we have a reason to iterate old regions.
        region->ZeroInitAndLink(m_immortalRegion);
        if (m_immortalRegion)
        {
            m_immortalRegion->DetachFromAlocatingOwnerRelease();
        }

        region->AttachToAllocatingOwner(&m_immortalRegion);
        region->SetGenerationRelease(3);
    }

    return nullptr;
}

void SatoriAllocator::DeactivateSharedRegion(SatoriRegion* region, bool promoteAllRegions)
{
    if (region)
    {
        if (region->IsAllocating())
        {
            region->StopAllocating(/* allocPtr */ 0);
        }

        if (promoteAllRegions)
        {
            region->DetachFromAlocatingOwnerRelease();
        }

        m_heap->Recycler()->AddEphemeralRegion(region);
    }
}

void SatoriAllocator::DeactivateSharedRegions(bool promoteAllRegions)
{
    SatoriRegion* immortalRegion = m_immortalRegion;
    if (immortalRegion && immortalRegion->IsAllocating())
    {
        immortalRegion->StopAllocating(/* allocPtr */ 0);
    }

    DeactivateSharedRegion(m_pinnedRegion, promoteAllRegions);
    DeactivateSharedRegion(m_largeRegion, promoteAllRegions);
    DeactivateSharedRegion(m_regularRegion, promoteAllRegions);
}

SatoriWorkChunk* SatoriAllocator::TryGetWorkChunk()
{
#if _DEBUG
    static int i = 0;
    // simulate low memory case once in a while
    // This is just to force more overflows. Otherwise they are very rare.
    if (i++ % 2 == 0)
    {
        return nullptr;
    }
#endif
    SatoriWorkChunk* chunk = m_workChunks->TryPop();

    while (!chunk && AddMoreWorkChunks())
    {
        chunk = m_workChunks->TryPop();
    }

    _ASSERTE(chunk->Count() == 0);
    return chunk;
}

// returns NULL only in OOM case
SatoriWorkChunk* SatoriAllocator::GetWorkChunk()
{
    SatoriWorkChunk* chunk = m_workChunks->TryPop();
    while (!chunk && AddMoreWorkChunks())
    {
        chunk = m_workChunks->TryPop();
    }

    _ASSERTE(chunk->Count() == 0);
    return chunk;
}

bool SatoriAllocator::AddMoreWorkChunks()
{
    SatoriRegion* region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
    if (!region)
    {
        return false;
    }

    while (true)
    {
        size_t mem = region->Allocate(Satori::MARK_CHUNK_SIZE, /*zeroInitialize*/ false);
        if (!mem)
        {
            break;
        }

        SatoriWorkChunk* chunk = SatoriWorkChunk::InitializeAt(mem);
        m_workChunks->Push(chunk);
    }

    return true;
}

void SatoriAllocator::ReturnWorkChunk(SatoriWorkChunk* chunk)
{
    _ASSERTE(chunk->Count() == 0);
    m_workChunks->Push(chunk);
}
