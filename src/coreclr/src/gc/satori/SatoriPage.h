// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriPage.h
//

#ifndef __SATORI_PAGE_H__
#define __SATORI_PAGE_H__

#include "common.h"
#include "../gc.h"

class SatoriHeap;
class SatoriRegion;

class SatoriPage
{
public:
    SatoriPage() = delete;
    ~SatoriPage() = delete;

    static SatoriPage* InitializeAt(size_t address, size_t pageSize, SatoriHeap* heap);
    SatoriRegion* MakeInitialRegion();

    void RegionInitialized(SatoriRegion* region);
    void RegionDestroyed(SatoriRegion* region);

    SatoriRegion* RegionForAddress(size_t address);

    size_t Start()
    {
        return (size_t)this;
    }

    size_t End()
    {
        return m_end;
    }

    size_t RegionsStart()
    {
        return m_firstRegion;
    }

    uint8_t* RegionMap()
    {
        return m_regionMap;
    }

    SatoriHeap* Heap()
    {
        return m_heap;
    }

    void SetCardForAddress(size_t address)
    {
        size_t offset = address - Start();
        size_t cardByteOffset = offset / (128 * 8);

        _ASSERTE(cardByteOffset / 8 > m_cardTableStart);
        _ASSERTE(cardByteOffset / 8 < m_cardTableSize);

        ((uint8_t*)this)[cardByteOffset] = 0xFF;

        // TODO: VS dirty the region
    }

private:
    union
    {
        // 1bit  - 128 bytes
        // 1byte - 1k
        // 2K    - 2Mb (region granularity)
        // 1Mb   - 1Gb
        // We can start card table at the beginning of the page for simplicity
        // The first 2K+ cover the header, which includes the region map and the table itself
        // so that space will be unused.
        size_t m_cardTable[1];

        // header (can be up to 2Kb for 1Gb page)
        struct
        {
            size_t m_end;
            size_t m_initialCommit;
            size_t m_firstRegion;

            SatoriHeap* m_heap;

            // the following is useful when scanning/clearing cards
            // it can be computed from the page size, but we have space, so we will store.
            int m_cardTableSize;
            int m_cardTableStart;

            // 1byte per region
            // 512 bytes per 1Gb
            uint8_t m_regionMap[1];
        };
    };
};

#endif
