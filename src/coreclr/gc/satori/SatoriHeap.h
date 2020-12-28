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

    bool IsHeapAddress(size_t address)
    {
        size_t mapIndex = address >> Satori::PAGE_BITS;
        return (unsigned int)mapIndex < (unsigned int)m_committedMapSize&&
            m_pageMap[mapIndex] != 0;
    }

    SatoriPage* PageForAddressChecked(size_t address)
    {
        size_t mapIndex = address >> Satori::PAGE_BITS;
        if (mapIndex < m_committedMapSize)
        {
        tryAgain:
            switch (m_pageMap[mapIndex])
            {
            case 0:
                break;
            case 1:
                return (SatoriPage*)(mapIndex << Satori::PAGE_BITS);
            default:
                mapIndex -= ((size_t)1 << (m_pageMap[mapIndex] - 2));
                goto tryAgain;
            }
        }

        return nullptr;
    }

    SatoriPage* PageForAddress(size_t address)
    {
        size_t mapIndex = address >> Satori::PAGE_BITS;
        while (m_pageMap[mapIndex] != 1)
        {
            mapIndex -= ((size_t)1 << (m_pageMap[mapIndex] - 2));
        }

        return (SatoriPage*)(mapIndex << Satori::PAGE_BITS);
    }

    SatoriObject* ObjectForAddress(size_t address);
    SatoriRegion* RegionForAddressChecked(size_t address);
    SatoriObject* ObjectForAddressChecked(size_t address);

    template<typename F>
    void ForEachPage(F& lambda)
    {
        size_t mapIndex = m_nextPageIndex - 1;
        while (mapIndex > 0)
        {
            switch (m_pageMap[mapIndex])
            {
            case 1:
                lambda((SatoriPage*)(mapIndex << Satori::PAGE_BITS));
            case 0:
                mapIndex--;
                continue;
            default:
                mapIndex -= ((size_t)1 << (m_pageMap[mapIndex] - 2));
            }
        }
    }

private:
    SatoriAllocator m_allocator;
    SatoriRecycler m_recycler;
    SatoriFinalizationQueue m_finalizationQueue;

    int m_reservedMapSize;
    int m_committedMapSize;
    int m_nextPageIndex;
    SatoriLock m_mapLock;
    uint8_t m_pageMap[1];

    bool CommitMoreMap(int currentlyCommitted);
};

#endif
