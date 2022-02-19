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
    _ASSERTE(cardTableSize % Satori::REGION_SIZE_GRANULARITY == 0);

    size_t commitSize = ALIGN_UP(cardTableSize, Satori::CommitGranularity());
    if (!GCToOSInterface::VirtualCommit((void*)address, commitSize))
    {
        GCToOSInterface::VirtualRelease((void*)address, pageSize);
        return nullptr;
    }

    result->m_end = address + pageSize;
    result->m_firstRegion = address + cardTableSize;
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

    VolatileStore(&RegionMap()[startIndex], (uint8_t)1);
    for (int i = 1; i < mapCount; i++)
    {
        DWORD log2;
        BitScanReverse64(&log2, i);
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
    while (RegionMap()[mapIndex] > 1)
    {
        mapIndex -= ((size_t)1 << (RegionMap()[mapIndex] - 2));
    }

    if (VolatileLoad(&RegionMap()[mapIndex]) != 1)
    {
        // there is no region here yet.
        return nullptr;
    }

    return (SatoriRegion*)((mapIndex << Satori::REGION_BITS) + Start());
}

SatoriRegion* SatoriPage::NextInPage(SatoriRegion* region)
{
    _ASSERTE(region->Start() > Start());

    size_t address = region->End();
    if (address >= End())
    {
        // this is the last region on this page, no next one.
        return nullptr;
    }

    size_t mapIndex = (address - Start()) >> Satori::REGION_BITS;
    if (VolatileLoad(&RegionMap()[mapIndex]) != 1)
    {
        // there is no region here yet.
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

void SatoriPage::SetCardForAddressOnly(size_t address)
{
    size_t offset = address - Start();
    size_t cardByteOffset = offset / Satori::BYTES_PER_CARD_BYTE;

    _ASSERTE(cardByteOffset >= m_cardTableStart);
    _ASSERTE(cardByteOffset < m_cardTableSize);

    m_cardTable[cardByteOffset] = Satori::CardState::REMEMBERED;
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

// dirtying in nonblocking phases could be unordered since we do not clean concurrently with mutator
void SatoriPage::DirtyCardForAddressUnordered(size_t address)
{
    size_t offset = address - Start();
    size_t cardByteOffset = offset / Satori::BYTES_PER_CARD_BYTE;

    _ASSERTE(cardByteOffset >= m_cardTableStart);
    _ASSERTE(cardByteOffset < m_cardTableSize);

    m_cardTable[cardByteOffset] = Satori::CardState::DIRTY;

    size_t cardGroup = offset / Satori::REGION_SIZE_GRANULARITY;
    this->m_cardGroups[cardGroup * 2] = Satori::CardState::DIRTY;
    this->m_cardState = Satori::CardState::DIRTY;
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

    memset((void*)(m_cardTable + firstCard), Satori::CardState::DIRTY, lastCard - firstCard + 1);

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

// dirtying in nonblocking phases could be unordered since we do not clean concurrently with mutator
void SatoriPage::DirtyCardsForRangeUnordered(size_t start, size_t end)
{
    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstCard = firstByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(firstCard >= m_cardTableStart);
    _ASSERTE(firstCard < m_cardTableSize);

    size_t lastCard = lastByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(lastCard >= m_cardTableStart);
    _ASSERTE(lastCard < m_cardTableSize);

    memset((void*)(m_cardTable + firstCard), Satori::CardState::DIRTY, lastCard - firstCard + 1);

    size_t firstGroup = firstByteOffset / Satori::REGION_SIZE_GRANULARITY;
    size_t lastGroup = lastByteOffset / Satori::REGION_SIZE_GRANULARITY;
    for (size_t i = firstGroup; i <= lastGroup; i++)
    {
        this->m_cardGroups[i * 2] = Satori::CardState::DIRTY;
    }

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
