// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGC.h
//

#ifndef __SATORI_HEAP_H__
#define __SATORI_HEAP_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"

#include "SatoriLock.h"
#include "SatoriAllocator.h"
#include "SatoriRecycler.h"
#include "SatoriFinalizationQueue.h"

class SatoriPage;
class SatoriRegion;
class SatoriObject;

class SatoriHeap
{
public:
    SatoriHeap() = delete;
    ~SatoriHeap() = delete;

    static SatoriHeap* Create();

    bool TryAddRegularPage(SatoriPage*& newPage);
    SatoriPage* AddLargePage(size_t size);

    SatoriAllocator* Allocator()
    {
        return &m_allocator;
    }

    SatoriRecycler* Recycler()
    {
        return &m_recycler;
    }

    SatoriFinalizationQueue* FinalizationQueue()
    {
        return &m_finalizationQueue;
    }

    size_t CommittedMapLength()
    {
        return m_committedMapSize / sizeof(SatoriPage*);
    }
    SatoriPage* PageForAddressChecked(size_t address)
    {
        size_t mapIndex = address >> Satori::PAGE_BITS;
        if (mapIndex < CommittedMapLength())
        {
            return m_pageMap[mapIndex];
        }

        return nullptr;
    }

    bool IsHeapAddress(size_t address)
    {
        return PageForAddressChecked(address) != nullptr;
    }

    SatoriRegion* RegionForAddressChecked(size_t address);

    template<typename F>
    void ForEachPage(F lambda)
    {
        size_t mapIndex = m_usedMapLength - 1;
        while (mapIndex > 0)
        {
            SatoriPage* page = m_pageMap[mapIndex--];
            if (page && page != m_pageMap[mapIndex])
            {
                lambda(page);
            }
        }
    }

    template<typename F>
    void ForEachPageUntil(F lambda)
    {
        size_t mapIndex = m_usedMapLength - 1;
        while (mapIndex > 0)
        {
            SatoriPage* page = m_pageMap[mapIndex--];
            if (page && page != m_pageMap[mapIndex])
            {
                if (lambda(page))
                {
                    return;
                }
            }
        }
    }

private:
    SatoriAllocator m_allocator;
    SatoriRecycler m_recycler;
    SatoriFinalizationQueue m_finalizationQueue;

    size_t m_reservedMapSize;
    size_t m_committedMapSize;
    size_t m_usedMapLength;
    size_t m_nextPageIndex;
    SatoriLock m_mapLock;
    SatoriPage* m_pageMap[1];

    bool CommitMoreMap(size_t currentlyCommitted);
};

#endif
