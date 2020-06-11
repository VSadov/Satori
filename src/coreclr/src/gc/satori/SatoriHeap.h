// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.h
//

#ifndef __SATORI_HEAP_H__
#define __SATORI_HEAP_H__

#include "common.h"
#include "../gc.h"
#include "SatoriUtil.h"

#include "SatoriLock.h"
#include "SatoriPage.h"
#include "SatoriAllocator.h"
#include "SatoriRecycler.h"

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

    SatoriPage* PageForAddress(uint8_t* address);

    bool IsHeapAddress(uint8_t* address)
    {
        size_t mapIndex = (size_t)address >> Satori::PAGE_BITS;
        return (unsigned int)mapIndex < (unsigned int)m_committedMapSize &&
            m_pageMap[mapIndex] != 0;
    }

private:
    SatoriAllocator m_allocator;
    SatoriRecycler m_recycler;

    int m_reservedMapSize;
    int m_committedMapSize;
    int m_nextPageIndex;
    SatoriLock m_mapLock;
    uint8_t m_pageMap[1];

    bool CommitMoreMap(int currentlyCommitted);
};

#endif
