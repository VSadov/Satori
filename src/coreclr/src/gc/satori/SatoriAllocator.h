// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.h
//

#ifndef __SATORI_ALLOCATOR_H__
#define __SATORI_ALLOCATOR_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"

#include "SatoriRegionQueue.h"

class SatoriHeap;
class SatoriRegion;
class SatoriAllocationContext;

class SatoriAllocator
{
public:
    void Initialize(SatoriHeap* heap);
    SatoriRegion* GetRegion(size_t minSize);

    void ReturnRegion(SatoriRegion* region);
    void AddRegion(SatoriRegion* region);
    Object* Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags);

private:
    SatoriHeap* m_heap;
    //TODO: VS embed
    SatoriRegionQueue* m_queues[Satori::BUCKET_COUNT];

    Object* AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags);
    Object* AllocSmall(SatoriAllocationContext* context, size_t size, uint32_t flags);

    static int SizeToBucket(size_t size)
    {
        _ASSERTE(size >= Satori::REGION_SIZE_GRANULARITY);

        DWORD highestBit;
#ifdef HOST_64BIT
        BitScanReverse64(&highestBit, size);
#else
        BitScanReverse(&highestBit, value);
#endif
        return min(highestBit - Satori::REGION_BITS, Satori::BUCKET_COUNT - 1);
    }
};

#endif
