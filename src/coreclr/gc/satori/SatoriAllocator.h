// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriAllocator.h
//

#ifndef __SATORI_ALLOCATOR_H__
#define __SATORI_ALLOCATOR_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"

#include "SatoriRegionQueue.h"

class SatoriHeap;
class SatoriRegion;
class SatoriObject;
class SatoriAllocationContext;
class SatoriMarkChunk;
class SatoriMarkChunkQueue;

class SatoriAllocator
{
public:
    void Initialize(SatoriHeap* heap);
    SatoriRegion* GetRegion(size_t minSize);

    void ReturnRegion(SatoriRegion* region);
    void AddRegion(SatoriRegion* region);
    Object* Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags);

    //TODO: VS when understand constraints we need a not-allocating version for recycler (recycler is able to handle failures)
    SatoriMarkChunk* TryGetMarkChunk();
    void ReturnMarkChunk(SatoriMarkChunk* chunk);

private:
    SatoriHeap* m_heap;
    SatoriRegionQueue* m_queues[Satori::ALLOCATOR_BUCKET_COUNT];

    SatoriMarkChunkQueue* m_markChunks;

    SatoriObject* AllocRegular(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocHuge(SatoriAllocationContext* context, size_t size, uint32_t flags);

    static int SizeToBucket(size_t size)
    {
        _ASSERTE(size >= Satori::REGION_SIZE_GRANULARITY);

        DWORD highestBit;
#ifdef HOST_64BIT
        BitScanReverse64(&highestBit, size);
#else
        BitScanReverse(&highestBit, value);
#endif
        return min(highestBit - Satori::REGION_BITS, Satori::ALLOCATOR_BUCKET_COUNT - 1);
    }

    bool AddMoreMarkChunks();
};

#endif
