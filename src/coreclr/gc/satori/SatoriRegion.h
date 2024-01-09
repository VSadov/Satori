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
// SatoriRegion.h
//

#ifndef __SATORI_REGION_H__
#define __SATORI_REGION_H__

#include "common.h"
#include "../gc.h"
#include "SatoriHeap.h"
#include "SatoriUtil.h"
#include "SatoriObject.h"

class SatoriAllocator;
class SatoriRegionQueue;
class SatoriObject;
class SatoriAllocationContext;

// The Region contains objects and their metadata.
class SatoriRegion
{
    friend class SatoriObject;
    friend class SatoriRegionQueue;
    friend class SatoriQueue<SatoriRegion>;

public:
    SatoriRegion() = delete;
    ~SatoriRegion() = delete;

    static const int MAX_LARGE_OBJ_SIZE;

    static SatoriRegion* InitializeAt(SatoriPage* containingPage, size_t address, size_t regionSize, size_t committed, size_t used);
    void MakeBlank();
    bool ValidateBlank();

    void RearmCardsForTenured();
    void ResetCardsForEphemeral();

    SatoriRegion* TrySplit(size_t regionSize);
    bool CanDecommit();
    bool TryDecommit();
    void TryCommit();
    bool CanCoalesceWithNext();
    bool TryCoalesceWithNext();

    void ZeroInitAndLink(SatoriRegion* prev);

    static size_t RegionSizeForAlloc(size_t allocSize);

    size_t GetAllocStart();
    size_t GetAllocRemaining();
    size_t GetMaxAllocEstimate();
    size_t Allocate(size_t size, bool zeroInitialize);
    size_t AllocateHuge(size_t size, bool zeroInitialize);

    size_t StartAllocating(size_t minSize);
    void StopAllocating(size_t allocPtr);
    void StopAllocating();
    bool IsAllocating();

    void AddFreeSpace(SatoriObject* freeObj);

    bool HasFreeSpaceInTopBucket();
    bool HasFreeSpaceInTopNBuckets(int n);

    void StartEscapeTrackingRelease(size_t threadTag);
    void StopEscapeTracking();
    bool IsEscapeTracking();
    bool MaybeEscapeTrackingAcquire();
    bool IsEscapeTrackedByCurrentThread();

    void AttachToAllocatingOwner(SatoriRegion** attachementPoint);
    void DetachFromAlocatingOwnerRelease();
    bool IsAttachedToAllocatingOwner();
    bool MaybeAllocatingAcquire();
    void SetHasFinalizables();

    void ResetReusableForRelease();

    bool TryDemote();
    bool IsDemoted();
    SatoriWorkChunk* &DemotedObjects();
    void FreeDemotedTrackers();

    int Generation();
    int GenerationAcquire();
    void SetGeneration(int generation);
    void SetGenerationRelease(int generation);

    size_t Start();
    size_t End();
    size_t Size();

    SatoriObject* FirstObject();
    SatoriObject* FindObject(size_t location);
    size_t LocationToIndex(size_t location);
    void SetIndicesForObject(SatoriObject* o, size_t end);
    void SetIndicesForObjectCore(size_t start, size_t end);
    void ClearIndicesForAllocRange();

    int IncrementUnfinishedAlloc();
    void DecrementUnfinishedAlloc();

    SatoriObject* SkipUnmarked(SatoriObject* from);
    SatoriObject* SkipUnmarkedAndClear(SatoriObject* from);
    SatoriObject* SkipUnmarked(SatoriObject* from, size_t upTo);

    void TakeFinalizerInfoFrom(SatoriRegion* other);
    void IndividuallyPromote();
    void UpdateFinalizableTrackers();
    void UpdatePointers();
    void UpdatePointersInObject(SatoriObject* o);
    void SetCardsForObject(SatoriObject* o);

    template <bool promotingAllRegions>
    void UpdatePointersInPromotedObjects();

    template <bool updatePointers>
    bool Sweep();

    bool IsExposed(SatoriObject** location);
    bool AnyExposed(size_t from, size_t length);
    void EscapeRecursively(SatoriObject* obj);
    void EscsapeAll();
    void EscapeShallow(SatoriObject* o);

    template <typename F>
    void ForEachFinalizable(F lambda);

    template <typename F>
    void ForEachFinalizableThreadLocal(F lambda);

    // used for exclusive access to trackers when accessing concurrently with user threads
    void LockFinalizableTrackers();
    void UnlockFinalizableTrackers();

    bool RegisterForFinalization(SatoriObject* finalizable);
    bool HasFinalizables();
    bool& HasPendingFinalizables();

    void SetOccupancy(size_t occupancy, size_t objCount);
    void SetOccupancy(size_t occupancy);
    size_t Occupancy();
    size_t& OccupancyAtReuse();
    size_t ObjCount();

    bool& HasPinnedObjects();
    bool& DoNotSweep();
    bool& AcceptedPromotedObjects();
    bool& IndividuallyPromoted();

    size_t SweepsSinceLastAllocation();

    enum class ReuseLevel : uint32_t
    {
        None,
        Gen1,
        Gen0,
    };

    ReuseLevel& ReusableFor();
    bool IsReusable();

    SatoriQueue<SatoriRegion>* ContainingQueue();

#if _DEBUG
    bool& HasMarksSet();
#endif

    bool NothingMarked();
    void ClearMarks();
    void ClearIndex();
    void ClearFreeLists();

    // we tell where we are in terms of alloc bytes, so we do not collect too soon
    // returns true if it actually did a collection.
    bool ThreadLocalCollect(size_t allocBytes);

    SatoriPage* ContainingPage();
    SatoriRegion* NextInPage();

    void Verify(bool allowMarked = false);

private:
    static const int BITMAP_LENGTH = Satori::REGION_SIZE_GRANULARITY / sizeof(size_t) / sizeof(size_t) / 8;

    // The first actually useful index is offsetof(m_firstObject) / sizeof(size_t) / 8,
    static const int BITMAP_START = (BITMAP_LENGTH + (Satori::INDEX_LENGTH + 2) / 2 + 1) / sizeof(size_t) / 8;

    union
    {
        // object metadata - one bit per size_t
        // due to the minimum size of an object we can store 3 bits per object: {Marked, Escaped, Pinned}
        // it may be possible to repurpose the bits for other needs as we see fit.
        //
        // we will overlap the map and the header for simplicity of map operations.
        // it is ok because the first BITMAP_START elements of the map cover the header/map itself and thus will not be used.
        // +1 to include End(), it will always be 0, but it is conveninet to make it legal map index.
        size_t m_bitmap[BITMAP_LENGTH + 1];

        // Header.(can be up to 72 size_t)
        struct
        {
            // the tag is just some thread-specific value that is easy to get.
            // TEB address could be used on Windows, for example
            // ---- 0
            size_t m_ownerThreadTag0;
            int32_t m_generation0;
            ReuseLevel m_reusableFor0;

            SatoriRegion** m_allocatingOwnerAttachmentPoint;
            size_t m_unfinishedAllocationCount;

            size_t m_end;
            size_t m_committed;
            size_t m_used;
            SatoriPage* m_containingPage;

            // ---- 1
            size_t m_unused8;
            size_t m_unused9;

            SatoriRegion* m_prev;
            SatoriRegion* m_next;
            SatoriQueue<SatoriRegion>* m_containingQueue;

            // active allocation may happen in the following range.
            // the range may not be parseable as sequence of objects
            // NB: the range is in terms of objects,
            //     there is embedded off-by-one error for syncblocks
            size_t m_allocStart;
            size_t m_allocEnd;

            SatoriWorkChunk* m_finalizableTrackers;

            // ---- 2
            size_t m_unused16;
            size_t m_unused17;

            int32_t m_finalizableTrackersLock;
            int32_t m_markStack;

            // counting escaped objects
            // when size goes too high, we stop escaping and do not do local GC.
            size_t m_escapedSize;
            size_t m_allocBytesAtCollect;
            size_t m_objCount;
            size_t m_occupancy;
            size_t m_occupancyAtReuse;

            // ---- 3
            size_t m_unused24;
            size_t m_unused25;

            size_t m_sweepsSinceLastAllocation;

            bool m_hasPinnedObjects;
            bool m_hasMarksSet;
            bool m_doNotSweep;
            bool m_hasFinalizables;

            bool m_hasPendingFinalizables;
            bool m_acceptedPromotedObjects;
            bool m_individuallyPromoted;
            // bool unused;

            size_t m_unused28_31[4];

            // ---- 4
            size_t m_unused32;
            size_t m_unused33;

            size_t m_unuseds34_39[6];

            // ---- 5
            size_t m_unused40;
            size_t m_unused41;

            size_t m_unuseds42_47[6];

            // ---- 6
            size_t m_unused48;
            size_t m_unused49;

            size_t m_unuseds50_55[6];

            // ---- 7
            size_t m_unused56;
            size_t m_unused57;

            // when demoted, we remember our gen2 objects here
            SatoriWorkChunk* m_gen2Objects;

            SatoriObject* m_freeLists[Satori::FREELIST_COUNT];

            size_t m_unused68_70[3];

            // word71
            void (*m_escapeFunc)(SatoriObject**, SatoriObject*, SatoriRegion*);
        };
    };

    volatile int m_index[Satori::INDEX_LENGTH + 2];

    size_t m_syncBlock;
    SatoriObject m_firstObject;

private:
    inline size_t Offset() { return ((size_t)this >> (Satori::REGION_BITS - 6)) & (7 << 6); };

    inline size_t& OwnerThreadTagRef() { return *(size_t*)((size_t)&m_ownerThreadTag0 + Offset()); };
    inline int32_t& GenerationRef() { return *(int32_t*)((size_t)&m_generation0 + Offset()); };
    inline ReuseLevel& ReusableForRef() { return *(ReuseLevel*)((size_t)&m_reusableFor0 + Offset()); };

    bool CanSplitWithoutCommit(size_t size);
    void SplitCore(size_t regionSize, size_t& newStart, size_t& newCommitted, size_t& newZeroInitedAfter);
    void UndoSplitCore(size_t regionSize, size_t nextStart, size_t nextCommitted, size_t nextUsed);

    template <bool isConservative>
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    template <bool isConservative>
    static void UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    static void EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region);

    void ThreadLocalMark();
    void ThreadLocalPlan();
    void ThreadLocalUpdatePointers();
    void ThreadLocalCompact();
    NOINLINE void ClearPinned(SatoriObject* o);
    void ThreadLocalPendFinalizables();

    SatoriAllocator* Allocator();
    SatoriRecycler* Recycler();

    void PushToMarkStackIfHasPointers(SatoriObject* obj);
    SatoriObject* PopFromMarkStack();
    SatoriObject* ObjectForMarkBit(size_t bitmapIndex, int offset);
    void CompactFinalizableTrackers();

    enum MarkOffset: int
    {
        Marked,
        Escaped,
        Pinned,
    };

    bool IsMarked(SatoriObject* o);
    void SetMarked(SatoriObject* o);
    void SetMarkedAtomic(SatoriObject* o);
    void ClearMarked(SatoriObject* o);
    bool CheckAndClearMarked(SatoriObject* o);

    bool IsPinned(SatoriObject* o);
    void SetPinned(SatoriObject* o);
    void ClearPinnedAndMarked(SatoriObject* o);
    bool IsEscaped(SatoriObject* o);
    void SetEscaped(SatoriObject* o);
    bool IsEscapedOrPinned(SatoriObject* o);

    void SetExposed(SatoriObject** location);

    bool ValidateIndexEmpty();
    bool Coalesce(SatoriRegion* next);

    template <bool updatePointers, bool individuallyPromoted, bool isEscapeTracking>
    bool Sweep();
};

#endif
