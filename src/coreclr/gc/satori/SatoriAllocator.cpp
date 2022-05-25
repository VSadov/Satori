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
#include "SatoriWorkChunkQueue.h"

void SatoriAllocator::Initialize(SatoriHeap* heap)
{
    m_heap = heap;

    for (int i = 0; i < Satori::ALLOCATOR_BUCKET_COUNT; i++)
    {
        m_queues[i] = new SatoriRegionQueue(QueueKind::Allocator);
    }

    m_WorkChunks = new SatoriWorkChunkQueue();
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
                    context->alloc_ptr += size;
                    result->CleanSyncBlock();
                    region->SetIndicesForObject(result, result->Start() + size);
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
            region->DetachFromContextRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

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

        region->AttachToContext(&context->RegularRegion());
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

void SatoriAllocator::TryGetRegularRegion(SatoriRegion*& region)
{
    m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_soh);

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
    m_heap->Recycler()->HelpOnce();
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
                if (result)
                {
                    result->CleanSyncBlock();
                    context->alloc_bytes_uoh += size;
                    region->SetIndicesForObject(result, result->Start() + size);
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

            region->DetachFromContextRelease();
            m_heap->Recycler()->AddEphemeralRegion(region);
        }

        // get a new regular region.
        m_heap->Recycler()->MaybeTriggerGC(gc_reason::reason_alloc_loh);
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
                return nullptr;
            }

            _ASSERTE(region->NothingMarked());
        }

        region->AttachToContext(&context->LargeRegion());
        region->SetGenerationRelease(1);
        region->ResetReusableForRelease();
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
    context->alloc_bytes_uoh += size;

    // huge regions are not attached to contexts and in gen0+ would appear parseable,
    // but this one is not parseable yet since the new object has no MethodTable
    // we will keep the region in gen -1 for now and make it gen1 or gen2 in PublishObject.
    hugeRegion->StopAllocating(/* allocPtr */ 0);
    return result;
}

SatoriWorkChunk* SatoriAllocator::TryGetWorkChunk()
{
    SatoriWorkChunk* chunk = m_WorkChunks->TryPop();

#if _DEBUG
    // simulate low memory case once in a while
    if (!chunk && GCToOSInterface::GetCurrentProcessorNumber() == 2)
    {
        return nullptr;
    }
#endif

    while (!chunk && AddMoreWorkChunks())
    {
        chunk = m_WorkChunks->TryPop();
    }

    _ASSERTE(chunk->Count() == 0);
    return chunk;
}

// returns NULL only in OOM case
SatoriWorkChunk* SatoriAllocator::GetWorkChunk()
{
    SatoriWorkChunk* chunk = m_WorkChunks->TryPop();
    while (!chunk && AddMoreWorkChunks())
    {
        chunk = m_WorkChunks->TryPop();
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
        m_WorkChunks->Push(chunk);
    }

    return true;
}

void SatoriAllocator::ReturnWorkChunk(SatoriWorkChunk* chunk)
{
    _ASSERTE(chunk->Count() == 0);
    m_WorkChunks->Push(chunk);
}
