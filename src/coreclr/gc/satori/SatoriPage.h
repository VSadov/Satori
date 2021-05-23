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
#include "SatoriUtil.h"

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

    SatoriRegion* RegionForAddress(size_t address);
    SatoriRegion* RegionForAddressChecked(size_t address);
    SatoriRegion* RegionForCardGroup(size_t group);

    SatoriRegion* NextInPage(SatoriRegion* region);

    size_t Start();
    size_t End();
    size_t RegionsStart();
    uint8_t* RegionMap();
    SatoriHeap* Heap();

    void SetCardForAddress(size_t address);
    void SetCardsForRange(size_t start, size_t end);
    void DirtyCardForAddress(size_t address);
    void DirtyCardsForRange(size_t start, size_t length);
    void WipeCardsForRange(size_t start, size_t end);

    // order is unimportant, but we want to read it only once when we read it, thus volatile.
    volatile int8_t& CardState()
    {
        return m_cardState;
    }

    // order is unimportant, but we want to read it only once when we read it, thus volatile.
    volatile int8_t& ScanTicket()
    {
        return m_scanTicket;
    }

    size_t CardGroupCount()
    {
        return (End() - Start()) >> Satori::REGION_BITS;
    }

    // order is unimportant, but we want to read it once when we read it, thus volatile.
    volatile int8_t& CardGroupState(size_t i)
    {
        return m_cardGroups[i * 2];
    }

    // order is unimportant, but we want to read it once when we read it, thus volatile.
    volatile int8_t& CardGroupScanTicket(size_t i)
    {
        return m_cardGroups[i * 2 + 1];
    }

    int8_t* CardsForGroup(size_t i)
    {
        return &m_cardTable[i * Satori::CARD_BYTES_IN_CARD_GROUP];
    }

    size_t LocationForCard(int8_t* cardPtr)
    {
        return Start() + ((size_t)cardPtr - Start()) * Satori::BYTES_PER_CARD_BYTE;
    }

private:
    union
    {
        // 1bit  - 64  bytes
        // 1byte - 512 bytes   i.e cards add ~ 0.002 overhead
        // 8byte - 4k
        // 4K    - 2Mb (region granularity)
        // 2Mb   - 1Gb (region granule can store cards for 1Gb page)
        // We can start card table at the beginning of the page for simplicity
        // The first 4K cover the card itself, so that space will be unused and we can use it for other metadata.
        int8_t m_cardTable[1];

        // header (can be up to 2Kb for 1Gb page)
        struct
        {
            int8_t m_cardState;
            int8_t m_scanTicket;
            size_t m_end;
            size_t m_initialCommit;
            size_t m_firstRegion;

            SatoriHeap* m_heap;

            // the following is useful when scanning/clearing cards
            // it can be computed from the page size, but we have space, so we will store.
            size_t m_cardTableSize;
            size_t m_cardTableStart;

            // -----  we can have a few more fields above as long as m_cardGroups starts at offset 128.
            //        that can be adjusted if needed

            // computed size,
            // 1byte per region
            // 512 bytes per 1Gb
            uint8_t* m_regionMap;

            // computed size,
            // 2byte per region
            // 1024 bytes per 1Gb
            DECLSPEC_ALIGN(128)
            int8_t m_cardGroups[1];
        };
    };
};

#endif
