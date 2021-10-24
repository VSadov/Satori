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

#define ENABLE_ESCAPE_TRACKING

void SatoriAllocator::Initialize(SatoriHeap* heap)
{
    m_heap = heap;

    for (int i = 0; i < Satori::ALLOCATOR_BUCKET_COUNT; i++)
    {
        m_queues[i] = new SatoriRegionQueue(QueueKind::Allocator);
    }

    m_markChunks = new SatoriMarkChunkQueue();
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
                region = putBack->Split(regionSize);
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
                putBack = region->Split(region->Size() - regionSize);
                AddRegion(putBack);
            }

            return region;
        }
    }

    // most likely OOM
    return nullptr;
}

void SatoriAllocator::ReturnRegion(SatoriRegion* region)
{
    _ASSERTE(region->Generation() == -1);

    //TUNING: this is too aggressive!!
    //while (region->TryCoalesceWithNext()) {};
    //region->TryDecommit();

    m_queues[SizeToBucket(region->Size())]->Push(region);
}

void SatoriAllocator::AddRegion(SatoriRegion* region)
{
    _ASSERTE(region->Generation() == -1);
    m_queues[SizeToBucket(region->Size())]->Push(region);
}

Object* SatoriAllocator::Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size = ALIGN_UP(size, Satori::OBJECT_ALIGNMENT);

    SatoriObject* result;

    if (flags & GC_ALLOC_PINNED_OBJECT_HEAP)
    {
        result = AllocLarge(context, size, flags);
    }
    else if (context->alloc_ptr + size <= context->alloc_limit)
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

    if (flags & GC_ALLOC_PINNED_OBJECT_HEAP)
    {
        result->SetUnmovable();
    }

    return result;
}

SatoriObject* SatoriAllocator::AllocRegular(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    m_heap->Recycler()->HelpOnce();
    SatoriRegion* region = context->RegularRegion();

    _ASSERTE(region == nullptr || region->IsAttachedToContext());

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
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                if (zeroInitialize && moreSpace < Satori::MIN_REGULAR_ALLOC)
                {
                    moreSpace = min(allocRemaining, Satori::MIN_REGULAR_ALLOC);
                }

                if (region->Allocate(moreSpace, zeroInitialize))
                {
                    context->alloc_bytes += moreSpace;
                    context->alloc_limit += moreSpace;

                    SatoriObject* result = (SatoriObject*)(size_t)context->alloc_ptr;
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
                context->alloc_ptr = context->alloc_limit = (uint8_t*)region->AllocStart();
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
                        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->AllocStart();
                        continue;
                    }
                }
            }

            context->alloc_ptr = context->alloc_limit = nullptr;
            region->DetachFromContext();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        m_heap->Recycler()->MaybeTriggerGC();
        region = m_heap->Recycler()->TryGetReusable();
        if (region == nullptr)
        {
            region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
            _ASSERTE(region == nullptr || region->NothingMarked());
        }

        if (region == nullptr)
        {
            //OOM
            return nullptr;
        }

        region->Attach(&context->RegularRegion());
#ifdef ENABLE_ESCAPE_TRACKING
        switch (region->ReusableFor())
        {
        case SatoriRegion::ReuseLevel::Gen0:
            region->EscsapeAll();
            goto fallthrough;
        case SatoriRegion::ReuseLevel::None:
            fallthrough:
            region->StartEscapeTracking(SatoriUtil::GetCurrentThreadTag());
            break;
        case SatoriRegion::ReuseLevel::Gen1:
            region->SetGenerationRelease(1);
            break;
        }
#else
        region->SetGenerationRelease(1);
#endif

        region->ReusableFor() = SatoriRegion::ReuseLevel::None;
        context->alloc_ptr = context->alloc_limit = (uint8_t*)region->AllocStart();
    }
}

SatoriObject* SatoriAllocator::AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    m_heap->Recycler()->HelpOnce();
    SatoriRegion* region = context->LargeRegion();

    while (true)
    {
        if (region)
        {
            size_t allocRemaining = region->AllocRemaining();
            if (allocRemaining >= size)
            {
                bool zeroInitialize = !(flags & GC_ALLOC_ZEROING_OPTIONAL);
                SatoriObject* result = (SatoriObject*)region->Allocate(size, zeroInitialize);
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

        // handle huge allocation. It can't be from the current region.
        if (size > SatoriRegion::MAX_LARGE_OBJ_SIZE)
        {
            return AllocHuge(context, size, flags);
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

            region->DetachFromContext();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        // get a new regular region.
        m_heap->Recycler()->MaybeTriggerGC();
        _ASSERTE(SatoriRegion::RegionSizeForAlloc(size) == Satori::REGION_SIZE_GRANULARITY);
        region = GetRegion(Satori::REGION_SIZE_GRANULARITY);
        if (!region)
        {
            //OOM
            return nullptr;
        }

        _ASSERTE(region->NothingMarked());
        region->Attach(&context->LargeRegion());
        region->SetGenerationRelease(1);
    }
}

SatoriObject* SatoriAllocator::AllocHuge(SatoriAllocationContext* context, size_t size, uint32_t flags)
{
    size_t regionSize = SatoriRegion::RegionSizeForAlloc(size);
    _ASSERTE(regionSize > Satori::REGION_SIZE_GRANULARITY);

    m_heap->Recycler()->MaybeTriggerGC();
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
    context->alloc_bytes_uoh += size;

    // huge regions are not attached to contexts and in gen0+ would appear parseable,
    // but this one is not parseable yet since the new object has no MethodTable
    // we will keep the region in gen -1 for now and make it gen1 or gen2 in PublishObject.
    hugeRegion->StopAllocating(/* allocPtr */ 0);
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
    //TODO: VS if committing by OS page get one region at the beginning and dispense by a piece (take a lock),
    //      then just do whole regions.
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
