// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.cpp
//

#include "common.h"
#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "SatoriUtil.h"

#include "SatoriHeap.h"
#include "SatoriPage.h"
#include "SatoriAllocator.h"
#include "SatoriRegion.h"
#include "SatoriRegionQueue.h"
#include "SatoriAllocationContext.h"

void SatoriAllocator::Initialize(SatoriHeap* heap)
{
    m_heap = heap;

    for (int i = 0; i < Satori::BUCKET_COUNT; i++)
    {
        m_queues[i] = new SatoriRegionQueue();
    }
}

SatoriRegion* SatoriAllocator::GetRegion(size_t regionSize)
{
tryAgain:

    SatoriRegion* putBack = nullptr;

    int bucket = SizeToBucket(regionSize);
    SatoriRegion* region = m_queues[bucket]->TryRemove(regionSize, putBack);
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
        region = m_queues[bucket]->TryPop(regionSize, putBack);
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
    // TODO: Push or enqueue?
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
    size = ALIGN_UP(size, sizeof(size_t));

    if (context->alloc_ptr + size <= context->alloc_limit)
    {
        Object* result = (Object*)context->alloc_ptr;
        context->alloc_ptr += size;
        return result;
    }

    if (size < Satori::LARGE_OBJECT_THRESHOLD)
    {
        return AllocSmall(context, size, flags);
    }

    return AllocLarge(context, size, flags);
}

Object* SatoriAllocator::AllocSmall(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    SatoriRegion* region = context->RegularRegion();

    while (true)
    {
        if (region != nullptr &&
            region->AllocEnd() - (size_t)context->alloc_ptr > size + Satori::MIN_FREE_SIZE)
        {
            size_t alloc = context->alloc_ptr + size - context->alloc_limit;
            bool isZeroing = true;
            if (isZeroing && alloc < Satori::MIN_REGULAR_ALLOC)
            {
                alloc = min(region->AllocEnd() - Satori::MIN_FREE_SIZE - (size_t)context->alloc_limit, alloc + Satori::MIN_REGULAR_ALLOC);
            }

            if (region->Allocate(alloc, isZeroing))
            {
                context->alloc_bytes += alloc;
                context->alloc_limit += alloc;

                Object* result = (Object*)context->alloc_ptr;
                context->alloc_ptr += size;
                // clean syncblock.
                ((size_t*)result)[-1] = 0;
                return result;
            }
        }

        if (region != nullptr)
        {
            // unclaim unused.
            context->alloc_bytes -= context->alloc_limit - context->alloc_ptr;

            // make parsable?

            // TODO: VS try compact current

            m_heap->Recycler()->AddRegion(region);
            context->alloc_ptr = context->alloc_limit = nullptr;
        }

        region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (region == nullptr)
        {
            return nullptr;
        }

        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->AllocStart();
        context->RegularRegion() = region;
    }
}

Object* SatoriAllocator::AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size_t result =  0;

    // try large region first, if present
    SatoriRegion* region = context->LargeRegion();
    if (region)
    {
        result = region->Allocate(size, true);
        if (result)
        {
            goto done;
        }
    }

    size_t regionSize = ALIGN_UP(size + sizeof(SatoriRegion), Satori::REGION_SIZE_GRANULARITY);
    region = GetRegion(regionSize);

    if (regionSize == Satori::REGION_SIZE_GRANULARITY)
    {
        result = region->Allocate(size, true);
    }
    else
    {
        result = region->AllocateHuge(size, true);
    }

    if (context->LargeRegion() == nullptr)
    {
        context->LargeRegion() = region;
    }
    else
    {
        if (context->LargeRegion()->AllocSize() < region->AllocSize())
        {
            SatoriRegion* tmp = context->LargeRegion();
            context->LargeRegion() = region;
            region = tmp;
        }

        m_heap->Recycler()->AddRegion(region);
    }

done:
    // clean syncblock.
    ((size_t*)result)[-1] = 0;
    context->alloc_bytes_uoh += size;
    return (Object*)result;
}
