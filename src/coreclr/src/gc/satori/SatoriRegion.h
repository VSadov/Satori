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

enum class SatoriRegionState : int8_t
{
    allocating   = 0,
    shared       = 1,
};

class SatoriRegion
{
    friend class SatoriRegionQueue;


public:
    SatoriRegion() = delete;
    ~SatoriRegion() = delete;

    static SatoriRegion* InitializeAt(SatoriPage* containingPage, size_t address, size_t regionSize, size_t committed, size_t zeroInitedAfter);
    void MakeBlank();
    bool ValidateBlank();
    void StopAllocating();

    SatoriRegion* Split(size_t regionSize);
    bool CanCoalesce(SatoriRegion* other);
    void Coalesce(SatoriRegion* next);

    void Deactivate(SatoriHeap* heap);

    size_t Allocate(size_t size, bool ensureZeroInited);
    size_t AllocateHuge(size_t size, bool ensureZeroInited);

    bool IsAllocating();
    void Publish();

    size_t Start();
    size_t End();
    size_t Size();
    size_t AllocStart();
    size_t AllocEnd();
    size_t AllocSize();
    SatoriObject* FirstObject();

    SatoriObject* FindObject(size_t location);

    void ThreadLocalMark();
    size_t ThreadLocalPlan();
    void ThreadLocalUpdatePointers();
    bool ThreadLocalCompact(size_t desiredFreeSpace);

    void Verify();

private:
    SatoriRegionState m_state;
    int32_t m_markStack;
    // end is edge exclusive
    size_t m_end;
    size_t m_committed;
    size_t m_zeroInitedAfter;
    SatoriPage* m_containingPage;

    SatoriRegion* m_prev;
    SatoriRegion* m_next;
    SatoriRegionQueue* m_containingQueue;

    // active allocation may happen in the following range.
    // the range may not be parseable as sequence of objects
    // NB: the range is in terms of objects,
    //     there is embedded off-by-one error for syncblocks
    size_t m_allocStart;
    size_t m_allocEnd;

    SatoriObject* m_index[Satori::INDEX_ITEMS];

    size_t m_syncBlock;
    SatoriObject m_firstObject;

private:
    void SplitCore(size_t regionSize, size_t& newStart, size_t& newCommitted, size_t& newZeroInitedAfter);
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);
    static void UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);
    bool IsEmpty();

    void PushToMarkStack(SatoriObject* obj);
    SatoriObject* PopFromMarkStack();
};

#endif
