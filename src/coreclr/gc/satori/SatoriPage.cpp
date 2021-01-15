// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriPage.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

SatoriPage* SatoriPage::InitializeAt(size_t address, size_t pageSize, SatoriHeap* heap)
{
    _ASSERTE(pageSize % Satori::PAGE_SIZE_GRANULARITY == 0);

    SatoriPage* result = (SatoriPage*)GCToOSInterface::VirtualReserve((void*)address, pageSize);
    if (result == nullptr)
    {
        return result;
    }

    size_t cardTableSize = pageSize / Satori::BYTES_PER_CARD_BYTE;
    size_t commitSize = ALIGN_UP(cardTableSize, Satori::CommitGranularity());
    if (!GCToOSInterface::VirtualCommit((void*)address, commitSize))
    {
        GCToOSInterface::VirtualRelease((void*)address, pageSize);
        return nullptr;
    }

    result->m_end = address + pageSize;
    result->m_firstRegion = address + ALIGN_UP(cardTableSize, Satori::REGION_SIZE_GRANULARITY);
    result->m_initialCommit = address + commitSize;
    result->m_cardTableSize = cardTableSize;

    // conservatively assume the first useful card word to cover the start of the first region.
    size_t cardTableStart = (result->m_firstRegion - address) / Satori::BYTES_PER_CARD_BYTE;
    // this is also region map size
    size_t regionNumber = pageSize >> Satori::REGION_BITS;
    size_t cardGroupSize = regionNumber * 2;

    result->m_cardTableStart = cardTableStart;
    result->m_heap = heap;

    // make sure offset of m_cardGroups is 128.
    _ASSERTE(offsetof(SatoriPage, m_cardGroups) == 128);
    result->m_regionMap = (uint8_t*)(address + 128 + cardGroupSize);

    // make sure the first useful card word is beyond the header.
    _ASSERTE(result->Start() + cardTableStart > (size_t)(result->m_regionMap) + regionNumber);
    return result;
}

SatoriRegion* SatoriPage::MakeInitialRegion()
{
    // page memory should be considered dirtied up to the end of the card table, the rest is clear.
    size_t used = (size_t)&m_cardTable[m_cardTableSize];
    return SatoriRegion::InitializeAt(this, m_firstRegion, m_end - m_firstRegion, m_initialCommit, used);
}

void SatoriPage::RegionInitialized(SatoriRegion* region)
{
    _ASSERTE((size_t)region > Start() && (size_t)region < End());
    size_t startIndex = (region->Start() - Start()) >> Satori::REGION_BITS;
    size_t mapCount = region->Size() >> Satori::REGION_BITS;
    RegionMap()[startIndex] = 1;
    for (int i = 1; i < mapCount; i++)
    {
        DWORD log2;
        BitScanReverse(&log2, i);
        RegionMap()[startIndex + i] = (uint8_t)(log2 + 2);
    }
}

SatoriRegion* SatoriPage::RegionForAddress(size_t address)
{
    _ASSERTE(address >= Start() && address < End());
    size_t mapIndex = (address - Start()) >> Satori::REGION_BITS;
    while (RegionMap()[mapIndex] > 1)
    {
        mapIndex -= ((size_t)1 << (RegionMap()[mapIndex] - 2));
    }
    return (SatoriRegion*)((mapIndex << Satori::REGION_BITS) + Start());
}

SatoriRegion* SatoriPage::RegionForAddressChecked(size_t address)
{
    _ASSERTE(address >= Start() && address < End());
    size_t mapIndex = (address - Start()) >> Satori::REGION_BITS;
    if (RegionMap()[mapIndex])
    {
        while (RegionMap()[mapIndex] > 1)
        {
            mapIndex -= ((size_t)1 << (RegionMap()[mapIndex] - 2));
        }
        return (SatoriRegion*)((mapIndex << Satori::REGION_BITS) + Start());
    }

    return nullptr;
}

SatoriRegion* SatoriPage::NextInPage(SatoriRegion* region)
{
    _ASSERTE(region->Start() > Start());

    size_t address = region->End();
    if (address >= End())
    {
        return nullptr;
    }

    return (SatoriRegion*)address;
}

SatoriRegion* SatoriPage::RegionForCardGroup(size_t group)
{
    size_t mapIndex = group;
    while (RegionMap()[mapIndex] > 1)
    {
        mapIndex -= ((size_t)1 << (RegionMap()[mapIndex] - 2));
    }

    return (SatoriRegion*)((mapIndex << Satori::REGION_BITS) + Start());
}

void SatoriPage::SetCardForAddress(size_t address)
{
    size_t offset = address - Start();
    size_t cardByteOffset = offset / Satori::BYTES_PER_CARD_BYTE;

    _ASSERTE(cardByteOffset >= m_cardTableStart);
    _ASSERTE(cardByteOffset < m_cardTableSize);

    if (!m_cardTable[cardByteOffset])
    {
        m_cardTable[cardByteOffset] = Satori::CardState::REMEMBERED;

        size_t cardGroup = offset / Satori::REGION_SIZE_GRANULARITY;
        if (!m_cardGroups[cardGroup * 2])
        {
            m_cardGroups[cardGroup * 2] = Satori::CardState::REMEMBERED;

            if (!m_cardState)
            {
                m_cardState = Satori::CardState::REMEMBERED;
            }
        }
    }
}

void SatoriPage::SetCardsForRange(size_t start, size_t end)
{
    _ASSERTE(end > start);

    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstCard = firstByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(firstCard >= m_cardTableStart);
    _ASSERTE(firstCard < m_cardTableSize);

    size_t lastCard = lastByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(lastCard >= m_cardTableStart);
    _ASSERTE(lastCard < m_cardTableSize);

    memset((void*)(m_cardTable + firstCard), Satori::CardState::REMEMBERED, lastCard - firstCard + 1);
   
    size_t firstGroup = firstByteOffset / Satori::REGION_SIZE_GRANULARITY;
    size_t lastGroup = lastByteOffset / Satori::REGION_SIZE_GRANULARITY;
    for (size_t i = firstGroup; i <= lastGroup; i++)
    {
        if (!m_cardGroups[i * 2])
        {
            m_cardGroups[i * 2] = Satori::CardState::REMEMBERED;
        }
    }

    if (!m_cardState)
    {
        m_cardState = Satori::CardState::REMEMBERED;
    }
}

void SatoriPage::DirtyCardForAddress(size_t address)
{
    size_t offset = address - Start();
    size_t cardByteOffset = offset / Satori::BYTES_PER_CARD_BYTE;

    _ASSERTE(cardByteOffset >= m_cardTableStart);
    _ASSERTE(cardByteOffset < m_cardTableSize);

    m_cardTable[cardByteOffset] = Satori::CardState::DIRTY;

    size_t cardGroup = offset / Satori::REGION_SIZE_GRANULARITY;
    VolatileStore(&this->m_cardGroups[cardGroup * 2], Satori::CardState::DIRTY);
    VolatileStore(&this->m_cardState, Satori::CardState::DIRTY);
}

// TODO: VS barrier dirtying could be unordered?
void SatoriPage::DirtyCardsForRange(size_t start, size_t end)
{
    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstCard = firstByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(firstCard >= m_cardTableStart);
    _ASSERTE(firstCard < m_cardTableSize);

    size_t lastCard = lastByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(lastCard >= m_cardTableStart);
    _ASSERTE(lastCard < m_cardTableSize);

    for (size_t i = firstCard; i <= lastCard; i++)
    {
        m_cardTable[i] = Satori::CardState::DIRTY;
    }

    // dirtying can be concurrent with cleaning, so we must ensure order
    // of writes - cards, then groups, then page
    // cleaning will read in the opposite order
    VolatileStoreBarrier();

    size_t firstGroup = firstByteOffset / Satori::REGION_SIZE_GRANULARITY;
    size_t lastGroup = lastByteOffset / Satori::REGION_SIZE_GRANULARITY;
    for (size_t i = firstGroup; i <= lastGroup; i++)
    {
        this->m_cardGroups[i * 2] = Satori::CardState::DIRTY;
    }

    VolatileStoreBarrier();

    this->m_cardState = Satori::CardState::DIRTY;
}

void SatoriPage::WipeCardsForRange(size_t start, size_t end)
{
    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstCard = firstByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(firstCard >= m_cardTableStart);
    _ASSERTE(firstCard < m_cardTableSize);

    size_t lastCard = lastByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(lastCard >= m_cardTableStart);
    _ASSERTE(lastCard < m_cardTableSize);
    memset((void*)(m_cardTable + firstCard), 0, lastCard - firstCard + 1);

    size_t firstGroup = firstByteOffset / Satori::REGION_SIZE_GRANULARITY;
    size_t lastGroup = lastByteOffset / Satori::REGION_SIZE_GRANULARITY;
    memset((void*)&m_cardGroups[firstGroup * 2], 0, (lastGroup - firstGroup + 1) * 2);
}
