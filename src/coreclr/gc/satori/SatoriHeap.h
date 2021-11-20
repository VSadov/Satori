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
