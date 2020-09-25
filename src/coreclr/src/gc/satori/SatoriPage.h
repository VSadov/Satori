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
    void RegionDestroyed(SatoriRegion* region);

    SatoriRegion* RegionForAddress(size_t address);

    size_t Start();
    size_t End();
    size_t RegionsStart();
    uint8_t* RegionMap();
    SatoriHeap* Heap();

    void SetCardForAddress(size_t address);

    bool IsClean()
    {
        return VolatileLoadWithoutBarrier(&m_cardState) == 0;
    }

    void SetProcessing()
    {
        VolatileStoreWithoutBarrier(&m_cardState, 2);
    }

    bool TrySetClean()
    {
        _ASSERTE((m_cardState >= 0) && (m_cardState <= 2));
        return Interlocked::CompareExchange(&m_cardState, 0, 2) != 1;
    }

    size_t CardGroupCount()
    {
        return (End() - Start()) >> Satori::REGION_BITS;
    }

    Satori::CardGroupState CardGroupState(size_t i)
    {
        return (Satori::CardGroupState)m_cardGroups[i];
    }

    void CardGroupSetProcessing(size_t i)
    {
        m_cardGroups[i] = Satori::CardGroupState::processing;
    }

    void CardGroupTrySetClean(size_t i)
    {
        Interlocked::CompareExchange<uint8_t>(&m_cardGroups[i], Satori::CardGroupState::clean, Satori::CardGroupState::processing);
    }

    size_t* CardsForGroup(size_t i)
    {
        return &m_cardTable[i * Satori::CARD_WORDS_IN_CARD_GROUP];
    }

    size_t LocationForCard(void* cardPtr)
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
        size_t m_cardTable[1];

        // header (can be up to 2Kb for 1Gb page)
        struct
        {
            int32_t m_cardState;
            size_t m_end;
            size_t m_initialCommit;
            size_t m_firstRegion;

            SatoriHeap* m_heap;

            // the following is useful when scanning/clearing cards
            // it can be computed from the page size, but we have space, so we will store.
            int m_cardTableSize;
            int m_cardTableStart;

            // -----  we can have a few more fields above as long as m_cardsStatus starts at offset 128.
            //        that can be adjusted if needed

            // computed size,
            // 1byte per region
            // 512 bytes per 1Gb
            uint8_t* m_regionMap;

            // computed size,
            // 1byte per region
            // 512 bytes per 1Gb
            DECLSPEC_ALIGN(128)
            uint8_t m_cardGroups[1];
        };
    };
};

#endif
