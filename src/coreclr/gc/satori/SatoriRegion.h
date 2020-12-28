// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
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

class SatoriRegion
{
    friend class SatoriRegionQueue;
    friend class SatoriObject;
    friend class SatoriAllocationContext;

public:
    SatoriRegion() = delete;
    ~SatoriRegion() = delete;

    static SatoriRegion* InitializeAt(SatoriPage* containingPage, size_t address, size_t regionSize, size_t committed, size_t used);
    void MakeBlank();
    bool ValidateBlank();
    bool ValidateIndexEmpty();
    void WipeCards();

    SatoriRegion* Split(size_t regionSize);
    void TryCoalesceWithNext();
    void Coalesce(SatoriRegion* next);

    void TryDecommit();

    size_t AllocStart();
    size_t AllocRemaining();
    size_t MaxAllocEstimate();
    size_t Allocate(size_t size, bool zeroInitialize);
    size_t AllocateHuge(size_t size, bool zeroInitialize);
    void StopAllocating(size_t allocPtr);

    void AddFreeSpace(SatoriObject* freeObj);

    size_t StartAllocating(size_t minSize);

    bool HasFreeSpaceInTopBucket();

    bool IsAllocating();
    void StopEscapeTracking();

    bool IsThreadLocal();
    bool OwnedByCurrentThread();

    int Generation();
    void SetGeneration(int generation);

    size_t Start();
    size_t End();
    size_t Size();

    SatoriObject* FirstObject();
    SatoriObject* FindObject(size_t location);

    SatoriObject* SkipUnmarked(SatoriObject* from);
    SatoriObject* SkipUnmarked(SatoriObject* from, size_t upTo);
    bool Sweep(bool turnMarkedIntoEscaped);
    void TakeFinalizerInfoFrom(SatoriRegion* other);
    bool NothingMarked();
    void UpdatePointers();

    bool IsExposed(SatoriObject** location);
    bool AnyExposed(size_t from, size_t length);
    void EscapeRecursively(SatoriObject* obj);

    void EscapeShallow(SatoriObject* o);
    void ReportOccupancy(size_t occupancy);

    template<typename F>
    void ForEachFinalizable(F& lambda);
    bool RegisterForFinalization(SatoriObject* finalizable);
    bool EverHadFinalizables();
    bool& HasPendingFinalizables();

    size_t Occupancy();

    bool HasPinnedObjects();

    void ClearMarks();
    void PromoteToGen1();

    void ThreadLocalCollect();

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
        // it is ok because the first BITMAP_START elements of the map cover the header/map and thus will not be used.
        // +1 to include End(), it will always be 0, but it is conveninet to make it legal map index.
        size_t m_bitmap[BITMAP_LENGTH + 1];

        // Header.(can be up to 72 size_t)
        struct
        {
            // just some thread-specific value that is easy to get.
            // TEB address could be used on Windows, for example
            size_t m_ownerThreadTag;
            void (*m_escapeFunc)(SatoriObject**, SatoriObject*, SatoriRegion*);
            int m_generation;

            size_t m_end;
            size_t m_committed;
            size_t m_used;
            SatoriPage* m_containingPage;

            SatoriRegion* m_prev;
            SatoriRegion* m_next;
            SatoriQueue<SatoriRegion>* m_containingQueue;
            SatoriMarkChunk* m_finalizableTrackers;

            bool m_everHadFinalizables;
            bool m_hasPendingFinalizables;

            // active allocation may happen in the following range.
            // the range may not be parseable as sequence of objects
            // NB: the range is in terms of objects,
            //     there is embedded off-by-one error for syncblocks
            size_t m_allocStart;
            size_t m_allocEnd;

            int32_t m_markStack;

            // counting escaped objects
            // when number goes too high, we stop escaping and do not do local GC.
            int32_t m_escapeCounter;

            size_t m_occupancy;
            size_t m_hasPinnedObjects;

            SatoriObject* m_freeLists[Satori::FREELIST_COUNT];
        };
    };

    int m_index[Satori::INDEX_LENGTH + 2];

    size_t m_syncBlock;
    SatoriObject m_firstObject;

private:
    void SplitCore(size_t regionSize, size_t& newStart, size_t& newCommitted, size_t& newZeroInitedAfter);
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);
    static void UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    static void EscapeFn(SatoriObject** dst, SatoriObject* src, SatoriRegion* region);

    void ThreadLocalMark();
    size_t ThreadLocalPlan();
    void ThreadLocalUpdatePointers();
    void ThreadLocalCompact();
    void ThreadLocalPendFinalizables();

    SatoriAllocator* Allocator();

    void PushToMarkStack(SatoriObject* obj);
    SatoriObject* PopFromMarkStack();
    SatoriObject* ObjectForMarkBit(size_t bitmapIndex, int offset);
    void CompactFinalizableTrackers();

    void SetExposed(SatoriObject** location);
};

#endif
