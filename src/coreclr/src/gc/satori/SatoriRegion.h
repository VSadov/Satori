// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.h
//

#ifndef __SATORI_REGION_H__
#define __SATORI_REGION_H__

#include "common.h"
#include "../gc.h"
#include "SatoriHeap.h"
#include "SatoriUtil.h"

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

    bool IsAllocating()
    {
        return m_allocEnd != 0;
    }

    size_t Start()
    {
        return (size_t)&m_end;
    }

    size_t End()
    {
        return m_end;
    }

    size_t Size()
    {
        return End() - Start();
    }

    size_t AllocStart()
    {
        return m_allocStart;
    }

    size_t AllocEnd()
    {
        return m_allocEnd;
    }

    size_t AllocSize()
    {
        return m_allocEnd - m_allocStart;
    }

    static SatoriRegion* RegionForObject(Object* obj)
    {
        return (SatoriRegion*)(((size_t)obj) >> Satori::REGION_BITS);
    }

private:
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
    // NB: it is in terms of objects, if converting to size_t beware of sync blocks
    size_t m_allocStart;
    size_t m_allocEnd;

    Object* m_index[Satori::INDEX_ITEMS];

    size_t m_syncBlock;
    Object m_firstObject;

private:
    void SplitCore(size_t regionSize, size_t& newStart, size_t& newCommitted, size_t& newZeroInitedAfter);
    bool IsEmpty();
};

#endif
