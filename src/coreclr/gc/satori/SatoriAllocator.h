// Copyright (c) 2025 Vladimir Sadov
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
class SatoriPage;
class SatoriRegion;
class SatoriObject;
class SatoriAllocationContext;
class SatoriWorkChunk;
class SatoriShardedWorkList;

enum AllocationTickKind
{
    Small = 0,
    Large = 1,
    Pinned = 2,
};

class SatoriAllocator
{
public:
    void Initialize(SatoriHeap* heap);
    Object* Alloc(SatoriAllocationContext* context, size_t size, uint32_t flags);

    // TEMP-TRACE: who asks is for the counters only
    SatoriRegion* GetRegion(size_t minSize, SatoriRegionRequester requester = SatoriRegionRequester::Unknown);
    SatoriRegion* GetRegionLow(size_t minSize, SatoriRegionRequester requester = SatoriRegionRequester::Unknown);
    void AddRegion(SatoriRegion* region);
    void ReturnRegion(SatoriRegion* region);
    // Gives the regions from the cache back to the free granules of their pages, where the trimmer can see them.
    void DrainRegionCache();
    void PrintTrace();

    size_t GetTotalAllocatedBytes();
    size_t GetTotalAllocatedBytesPrecise();
    void PublishAllocatedBytesOnThreadExit(SatoriAllocationContext* context);

    SatoriWorkChunk* TryGetWorkChunk();
    SatoriWorkChunk* GetWorkChunk();
    void ReturnWorkChunk(SatoriWorkChunk* chunk);
    void ReturnWorkChunks(SatoriWorkChunk* first, SatoriWorkChunk* last);
    void ReturnWorkChunks(SatoriWorkChunk* first);

    void DeactivateSharedRegion(SatoriRegion* region, bool promoteAllRegions);
    void DeactivateSharedRegions(bool promoteAllRegions);

private:
    // Bytes allocated by threads, as published by the threads. (see PublishAllocatedBytes)
    // Each counter is on its own cache line.
    struct DECLSPEC_ALIGN(Satori::WIDE_CACHE_GRANULARITY) AllocatedBytesCounter
    {
        size_t bytes;
        // the value of "bytes" at the last allocation tick. (see MaybeFireAllocationTick)
        size_t bytesAtLastTick;
    };

    SatoriHeap* m_heap;
    SatoriShardedWorkList* m_workChunks;

    SatoriRegion* m_regularRegion;
    SatoriRegion* m_largeRegion;
    SatoriRegion* m_pinnedRegion;
    SatoriRegion* m_immortalRegion;

    int m_smallAllocatedBytesCounterCount;

    // the allocation tick event was enabled when last checked. (see IsAllocationTickEnabled)
    volatile bool m_allocationTickWasEnabled;

    DECLSPEC_ALIGN(64)
    SatoriLock m_regularAllocLock;

    DECLSPEC_ALIGN(64)
    SatoriLock m_immortalAllocLock;

    DECLSPEC_ALIGN(64)
    SatoriLock m_largeAllocLock;

    DECLSPEC_ALIGN(64)
    SatoriLock m_pinnedAllocLock;

    // Work chunks are added by one thread at a time (see AddMoreWorkChunks), the others wait for it by polling.
    // Thus nobody may ask for a work chunk while holding what the refilling thread needs to get a region.
    // Lock order: the allocation locks, this lock, m_runClaimLock.
    DECLSPEC_ALIGN(64)
    SatoriLock m_workChunkRefillLock;

    // Serializes the threads that claim runs of granules for big regions. (see SatoriPage::TryClaimRun)
    // Claims of single granules do not take it. Nothing else is locked while holding it.
    DECLSPEC_ALIGN(64)
    SatoriLock m_runClaimLock;

    // the shard to publish the next chain of fresh work chunks to. Protected by m_workChunkRefillLock.
    size_t m_workChunkRefillShard;

    // Small objects are allocated most often, thus their counter is sharded,
    // one counter per ALLOCATED_BYTES_COUNTER_PROCESSORS processors.
    // Large and pinned objects are allocated less often and have a single counter each.
    // TODO: Ideally the sharding would consider NUMA nodes. We may also want to shard
    //       the large and pinned counters per NUMA node. Not yet.
    AllocatedBytesCounter m_smallAllocatedBytes[Satori::MAX_ALLOCATED_BYTES_COUNTERS];
    AllocatedBytesCounter m_largeAllocatedBytes;
    AllocatedBytesCounter m_pinnedAllocatedBytes;

    // What terminated threads have allocated, precisely. (see GetTotalAllocatedBytesPrecise)
    size_t m_deadThreadsAllocatedBytes;

    // A floor for approximate reads after a precise total has been observed.
    size_t m_lastPreciseAllocatedBytes;

private:
    // A small cache of free 2 MB regions in front of the free granules. Regular requests take from the cache first,
    // and the regions that are freed go there, while the cache is below its cap. That keeps the reuse of the regions
    // that were used recently, and the allocation of a region a pop from a queue.
    // The cached regions are formatted, have valid headers and are members of the queue, so a header may be read
    // only under the lock of the queue. (the queue is of the Allocator kind)
    SatoriShardedRegionQueue* m_regionCache;

    // How many regions the cache may have. The racing threads can get the count a bit over it. 0 - no cache.
    size_t m_regionCacheCap;

    DECLSPEC_ALIGN(64)
    volatile size_t m_regionCacheCount;

    DECLSPEC_ALIGN(64)
    volatile int32_t m_singePageAdders;

    int SmallAllocatedBytesCounterIndex();
    void PublishAllocatedBytes(SatoriAllocationContext* context, AllocationTickKind kind, SatoriObject* obj, size_t size);
    void PublishAllocatedBytesIfNeeded(SatoriAllocationContext* context, AllocationTickKind kind, SatoriObject* obj, size_t size);
    void OnLargeOrPinnedAllocated(SatoriAllocationContext* context, uint32_t flags, SatoriObject* obj, size_t size);
    bool IsAllocationTickEnabled();
    static void CatchUpAllocationTick(AllocatedBytesCounter* counter);
    static void MaybeFireAllocationTick(AllocatedBytesCounter* counter, size_t total, AllocationTickKind kind, SatoriObject* obj, size_t size);

    void UpdateAllocStatsAndHelpIfNeeded(SatoriAllocationContext * context);
    SatoriObject* AllocRegular(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocRegularShared(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocLarge(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocLargeShared(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocHuge(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocPinned(SatoriAllocationContext* context, size_t size, uint32_t flags);
    SatoriObject* AllocImmortal(SatoriAllocationContext* context, size_t size, uint32_t flags);

    void TryGetRegularRegion(SatoriRegion*& region, size_t size);
    SatoriRegion* GetRegionCore(size_t regionSize, bool low, SatoriRegionRequester requester);
    SatoriRegion* GetRegionImpl(size_t regionSize, bool low, SatoriRegionRequester requester);
    SatoriRegion* TryClaimRegion(size_t granules, bool low, bool& outOfMemory);
    SatoriRegion* TryClaimRegionInPage(SatoriPage* page, size_t granules, bool low, bool& outOfMemory);
    bool TryCacheRegion(SatoriRegion* region, bool recentlyUsed);
    void FreeToBitmap(SatoriRegion* region);
    void TraceClaim(SatoriRegion* region, size_t granules, size_t retries, size_t rollbacks, int64_t startTime);
    bool AddMoreWorkChunks();
};

#endif
