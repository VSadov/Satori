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
class SatoriWorkChunk;
class SatoriWorkChunkQueue;

class SatoriAllocator
{
public:
    void Initialize(SatoriHeap* heap);
    SatoriRegion* GetRegion(size_t minSize);

    void ReturnRegion(SatoriRegion* region);
    void AddRegion(SatoriRegion* region);
    Object* Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags);

    SatoriWorkChunk* TryGetWorkChunk();
    SatoriWorkChunk* GetWorkChunk();
    void ReturnWorkChunk(SatoriWorkChunk* chunk);

private:
    SatoriHeap* m_heap;
    SatoriRegionQueue* m_queues[Satori::ALLOCATOR_BUCKET_COUNT];

    SatoriWorkChunkQueue* m_WorkChunks;

    SatoriObject* AllocRegular(SatoriAllocationContext* context, size_t size, uint32_t flags);
    void TryGetRegularRegion(SatoriRegion*& region);
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

    bool AddMoreWorkChunks();
};

#endif
