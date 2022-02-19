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

    bool IsHeapAddress(size_t address)
    {
        size_t mapIndex = address >> Satori::PAGE_BITS;
        return (unsigned int)mapIndex < (unsigned int)m_committedMapSize&&
            m_pageMap[mapIndex] != 0;
    }

    SatoriRegion* RegionForAddressChecked(size_t address);

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

    template<typename F>
    void ForEachPage(F lambda)
    {
        size_t mapIndex = m_usedMapSize - 1;
        while (mapIndex > 0)
        {
            switch (m_pageMap[mapIndex])
            {
            case 1:
                lambda((SatoriPage*)(mapIndex << Satori::PAGE_BITS));
                goto fallthrough;
            case 0:
                fallthrough:
                mapIndex--;
                continue;
            default:
                mapIndex -= ((size_t)1 << (m_pageMap[mapIndex] - 2));
            }
        }
    }

    template<typename F>
    void ForEachPageUntil(F lambda)
    {
        size_t mapIndex = m_usedMapSize - 1;
        while (mapIndex > 0)
        {
            switch (m_pageMap[mapIndex])
            {
            case 1:
                if (lambda((SatoriPage*)(mapIndex << Satori::PAGE_BITS)))
                {
                    return;
                }
                goto fallthrough;
            case 0:
                fallthrough:
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
    int m_usedMapSize;
    int m_nextPageIndex;
    SatoriLock m_mapLock;
    uint8_t m_pageMap[1];

    bool CommitMoreMap(int currentlyCommitted);
};

#endif
