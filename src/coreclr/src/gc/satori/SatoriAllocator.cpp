// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriAllocator.cpp
//

#include "common.h"
#include "gcenv.h"
#include "../env/gcenv.os.h"
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
#include "SatoriMarkChunk.h"
#include "SatoriMarkChunkQueue.h"

void SatoriAllocator::Initialize(SatoriHeap* heap)
{
    m_heap = heap;

    for (int i = 0; i < Satori::BUCKET_COUNT; i++)
    {
        m_queues[i] = new SatoriRegionQueue();
    }

    m_markChunks = new SatoriMarkChunkQueue();
}

SatoriRegion* SatoriAllocator::GetRegion(size_t regionSize)
{
    m_heap->Recycler()->MaybeTriggerGC();

tryAgain:
    SatoriRegion* putBack = nullptr;

    int bucket = SizeToBucket(regionSize);
    SatoriRegion* region = m_queues[bucket]->TryRemoveWithSize(regionSize, putBack);
    if (region)
    {
        if (putBack)
        {
            ReturnRegion(putBack);
        }

        return region;
    }

    while (++bucket < Satori::BUCKET_COUNT)
    {
        region = m_queues[bucket]->TryPopWithSize(regionSize, putBack);
        if (region)
        {
            if (putBack)
            {
                ReturnRegion(putBack);
            }

            return region;
        }
    }

    if (regionSize < Satori::PAGE_SIZE_GRANULARITY / 2)
    {
        SatoriPage* page = nullptr;
        if (m_heap->TryAddRegularPage(page))
        {
            if (page)
            {
                region = page->MakeInitialRegion();
                putBack = region->Split(region->Size() - regionSize);
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
            putBack = page->MakeInitialRegion();
            region = putBack->Split(regionSize);
            AddRegion(putBack);
            return region;
        }
    }

    // most likely OOM
    return nullptr;
}

void SatoriAllocator::ReturnRegion(SatoriRegion* region)
{
    // TODO: VS select by current core
    m_queues[SizeToBucket(region->Size())]->Push(region);
}

void SatoriAllocator::AddRegion(SatoriRegion* region)
{
    // TODO: VS select by current core
    m_queues[SizeToBucket(region->Size())]->Enqueue(region);
}

Object* SatoriAllocator::Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size = ALIGN_UP(size, Satori::OBJECT_ALIGNMENT);

    SatoriObject* result;

    if (context->alloc_ptr + size <= context->alloc_limit)
    {
        result = (SatoriObject*)context->alloc_ptr;
        context->alloc_ptr += size;
    }
    else if (size < Satori::LARGE_OBJECT_THRESHOLD)
    {
        result = AllocRegular(context, size, flags);
    }
    else
    {
        result = AllocLarge(context, size, flags);
    }

    if (flags & GC_ALLOC_FINALIZE)
    {
        if (!result->ContainingRegion()->RegisterForFinalization(result))
        {
            return nullptr;
        }
    }

    return result;
}

SatoriObject* SatoriAllocator::AllocRegular(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    SatoriRegion* region = context->RegularRegion();

    while (true)
    {
        if (region != nullptr)
        {
            _ASSERTE((size_t)context->alloc_limit == region->AllocStart());
            size_t moreSpace = context->alloc_ptr + size - context->alloc_limit;

            // try allocate more to form a contiguous chunk to fit size 
            size_t allocRemaining = region->AllocRemaining();
            if (moreSpace <= allocRemaining)
            {
                bool isZeroing = true;
                if (isZeroing && moreSpace < Satori::MIN_REGULAR_ALLOC)
                {
                    moreSpace = min(allocRemaining, Satori::MIN_REGULAR_ALLOC);
                }

                if (region->Allocate(moreSpace, isZeroing))
                {
                    context->alloc_bytes += moreSpace;
                    context->alloc_limit += moreSpace;

                    SatoriObject* result = SatoriObject::At((size_t)context->alloc_ptr);
                    context->alloc_ptr += size;
                    result->CleanSyncBlock();
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
            region->StopAllocating((size_t)context->alloc_ptr);

            // TODO: VS heuristic needed
            //       when there is 10% "sediment" we want to release this to recycler
            //       the rate may be different and consider fragmentation, escaped, and marked values
            //       may also try smoothing, although unlikely.
            //       All this can be tuned once full GC works.
            if (region->Occupancy() < Satori::REGION_SIZE_GRANULARITY * 1 / 10)
            {
                // try compact current
                region->ThreadLocalMark();
                //TODO: VS check if can split and split (here, after marking)
                region->ThreadLocalPlan();
                region->ThreadLocalUpdatePointers();

                size_t desiredFreeSpace = max(size, Satori::MIN_REGULAR_ALLOC);
                if (region->ThreadLocalCompact(desiredFreeSpace))
                {
                    // we have enough free space in the region to continue
                    context->alloc_ptr = context->alloc_limit = (uint8_t*)region->AllocStart();
                    continue;
                }

                //TODO: VS we should not start allocating if we have no space. This will change when we do free lists.
                if (region->IsAllocating())
                {
                    region->StopAllocating(0);
                }
            }

            context->RegularRegion() = nullptr;
            context->alloc_ptr = context->alloc_limit = nullptr;
            m_heap->Recycler()->AddRegion(region);
        }

        region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (region == nullptr)
        {
            //OOM
            return nullptr;
        }

        _ASSERTE(region->NothingMarked());
        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->AllocStart();
        region->m_ownerThreadTag = SatoriUtil::GetCurrentThreadTag();
        context->RegularRegion() = region;
    }
}

SatoriObject* SatoriAllocator::AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    SatoriRegion* region = context->LargeRegion();

    while (true)
    {
        if (region)
        {
            size_t allocRemaining = region->AllocRemaining();
            if (allocRemaining >= size)
            {
                SatoriObject* result = SatoriObject::At(region->Allocate(size, true));
                if (result)
                {
                    result->CleanSyncBlock();
                    context->alloc_bytes_uoh += size;
                }
                else
                {
                    // OOM
                }

                return result;
            }
        }

        size_t regionSize = ALIGN_UP(size + sizeof(SatoriRegion) + Satori::MIN_FREE_SIZE, Satori::REGION_SIZE_GRANULARITY);
        if (regionSize > Satori::REGION_SIZE_GRANULARITY)
        {
            return AllocHuge(context, size, flags);
        }

        // drop existing region into recycler
        if (region)
        {
            region->StopAllocating(/* allocPtr */ 0);
            context->LargeRegion() = nullptr;
            m_heap->Recycler()->AddRegion(region);
        }

        // get a new region.
        region = GetRegion(regionSize);
        if (!region)
        {
            //OOM
            return nullptr;
        }

        _ASSERTE(region->NothingMarked());
        region->m_ownerThreadTag = SatoriUtil::GetCurrentThreadTag();
        context->LargeRegion() = region;
    }
}

SatoriObject* SatoriAllocator::AllocHuge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size_t regionSize = ALIGN_UP(size + sizeof(SatoriRegion) + Satori::MIN_FREE_SIZE, Satori::REGION_SIZE_GRANULARITY);
    SatoriRegion* region = GetRegion(regionSize);
    if (!region)
    {
        //OOM
        return nullptr;
    }

    SatoriObject* result = SatoriObject::At(region->AllocateHuge(size, true));
    if (result)
    {
        result->CleanSyncBlock();
        context->alloc_bytes_uoh += size;
    }
    else
    {
        ReturnRegion(region);
        // OOM
    }

    // TODO: VS maybe one day we can keep them if there is enough space remaining

    // we do not want to keep huge region in allocator for simplicity,
    // but can't drop it to recycler yet since the object has no MethodTable.
    // we will leave the region unowned and send it to recycler later in PublishObject.
    region->StopAllocating(/* allocPtr */ 0);
    return result;
}

SatoriMarkChunk* SatoriAllocator::TryGetMarkChunk()
{
    SatoriMarkChunk* chunk = m_markChunks->TryPop();
    while (!chunk && AddMoreMarkChunks())
    {
        chunk = m_markChunks->TryPop();
    }

    _ASSERTE(chunk->Count() == 0);
    return chunk;
}

bool SatoriAllocator::AddMoreMarkChunks()
{
    SatoriRegion* region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
    if (!region)
    {
        return false;
    }

    while (true)
    {
        size_t mem = region->Allocate(Satori::MARK_CHUNK_SIZE, /*ensureZeroInited*/ false);
        if (!mem)
        {
            break;
        }

        SatoriMarkChunk* chunk = SatoriMarkChunk::InitializeAt(mem);
        m_markChunks->Push(chunk);
    }

    return true;
}

void SatoriAllocator::ReturnMarkChunk(SatoriMarkChunk* chunk)
{
    _ASSERTE(chunk->Count() == 0);
    m_markChunks->Push(chunk);
}
