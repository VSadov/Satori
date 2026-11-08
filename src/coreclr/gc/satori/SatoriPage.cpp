// Copyright (c) 2025 Vladimir Sadov
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

#include "SatoriUtil.h"
#include "SatoriPage.h"
#include "SatoriPage.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"

SatoriPage* SatoriPage::InitializeAt(size_t address, size_t pageSize, SatoriHeap* heap, InitStatus& status)
{
    _ASSERTE(pageSize % Satori::PAGE_SIZE_GRANULARITY == 0);

    VirtualReserveStatus reserveStatus = VirtualReserveStatus::Refused;
    SatoriPage* result = (SatoriPage*)GCToOSInterface::VirtualReserve((void*)address, pageSize, SatoriUtil::UseTHP(), &reserveStatus);
    if (result == nullptr)
    {
        status = reserveStatus == VirtualReserveStatus::Occupied ?
            InitStatus::Occupied :
            InitStatus::ReserveRefused;

        return nullptr;
    }

    size_t cardTableSize = pageSize / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(cardTableSize % Satori::REGION_SIZE_GRANULARITY == 0);

    // when commit granularity is larger than region granularity (1Gb), commit the entire page.
    // we can't have sparse commit coarser than regions.
    // besides we would end up comitting the entire page anyways once the obj size
    // is aligned up and committed.
    size_t commitSize = SatoriUtil::CommitGranularity() > Satori::REGION_SIZE_GRANULARITY ?
        pageSize :
        ALIGN_UP(cardTableSize, SatoriUtil::CommitGranularity());

    if (!GCToOSInterface::VirtualCommit((void*)address, commitSize))
    {
        GCToOSInterface::VirtualRelease(result, pageSize);
        status = InitStatus::CommitFailed;
        return nullptr;
    }

    heap->IncBytesCommitted(commitSize);
    result->m_end = address + pageSize;
    result->m_firstRegion = address + cardTableSize;
    result->m_initialCommit = address + commitSize;
    result->m_cardTableSize = cardTableSize;

    // conservatively assume the first useful card word to cover the start of the first region.
    size_t cardTableStart = (result->m_firstRegion - address) / Satori::BYTES_PER_CARD_BYTE;
    // this is also region map size
    size_t regionCount = pageSize >> Satori::REGION_BITS;
    size_t cardGroupSize = pageSize / Satori::BYTES_PER_CARD_GROUP * 2;

    // initializing cards to EPHEMERAL is optional. 0 is ok too.
    // for huge pages it is not as useful and may get expensive.
    // also if the huge obj contains references, its region will go to gen2 anyways
    if (pageSize == Satori::PAGE_SIZE_GRANULARITY)
    {
#if _DEBUG
        // in debug we initialize only half the cards, since it is optional...
        memset(&result->m_cardTable[cardTableStart], Satori::CardState::EPHEMERAL, (cardTableSize - cardTableStart) / 2);
#else
        memset(&result->m_cardTable[cardTableStart], Satori::CardState::EPHEMERAL, cardTableSize - cardTableStart);
#endif
        // We leave card groups blank as we do not want to look at them when they may not even be covered by regions.
        // We maintain the invariant that groups are not set for allocator regions.
        // We do not use EPHEMERAL for groups.
    }

    result->m_cardTableStart = cardTableStart;
    result->m_heap = heap;

    // make sure offset of m_cardGroups is 128.
    _ASSERTE(offsetof(SatoriPage, m_cardGroups) == 128);
    static_assert(offsetof(SatoriPage, m_dirtyWatermarks) + sizeof(uint8_t*) <= 128,
        "the header fields must end before the card groups.");

    result->m_regionMap = (uint8_t*)(address + 128 + cardGroupSize);

    // The free bitmap is one cache line per 1Gb. The bitmaps and the watermarks need 2.25 bytes per granule
    // on top of the region map, and the card table that covers itself has room for that at any page size.
    _ASSERTE(regionCount % 64 == 0);
    result->m_freeBitmap = (size_t*)ALIGN_UP((size_t)result->m_regionMap + regionCount, 64);
    result->m_warmBitmap = result->m_freeBitmap + regionCount / 64;
    result->m_commitWatermarks = (uint8_t*)(result->m_warmBitmap + regionCount / 64);
    result->m_dirtyWatermarks = result->m_commitWatermarks + regionCount;

    // make sure the first useful card word is beyond the header.
    _ASSERTE(result->Start() + cardTableStart >= (size_t)(result->m_dirtyWatermarks) + regionCount);

    // Initialize the first region before publishing the page in the heap map.
    // Until then a failed commit can safely release the entire reservation.
    size_t used = (size_t)&result->m_cardTable[cardTableSize];
    if (SatoriRegion::InitializeAt(result, result->m_firstRegion, result->m_end - result->m_firstRegion,
        result->m_initialCommit, used) == nullptr)
    {
        heap->DecBytesCommitted(commitSize);
        GCToOSInterface::VirtualRelease(result, pageSize);
        status = InitStatus::CommitFailed;
        return nullptr;
    }

    status = InitStatus::Success;
    return result;
}

SatoriRegion* SatoriPage::InitialRegion()
{
    return (SatoriRegion*)m_firstRegion;
}

void SatoriPage::OnRegionInitialized(SatoriRegion* region)
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

// The reverse of OnRegionInitialized. The start goes first, so that a lookup that does not find the region
// as it was finds nothing, and not a continuation of a region that is no more.
void SatoriPage::OnRegionFreed(SatoriRegion* region)
{
    size_t startIndex = (region->Start() - Start()) >> Satori::REGION_BITS;
    size_t mapCount = region->Size() >> Satori::REGION_BITS;
    _ASSERTE(RegionMap()[startIndex] == 1);

    VolatileStore(&RegionMap()[startIndex], (uint8_t)0);
    memset(&RegionMap()[startIndex + 1], 0, mapCount - 1);
}

// The bits [lowBit, lowBit + width) of a bitmap word.
static size_t BitRange(size_t lowBit, size_t width)
{
    _ASSERTE(width > 0 && lowBit + width <= 64);
    size_t ones = width == 64 ? ~(size_t)0 : (((size_t)1 << width) - 1);
    return ones << lowBit;
}

// Turns the granules of a region that nobody uses any more into free granules.
// Nothing is decommitted or written into the freed memory. What the next owner needs to know about it,
// how much is committed and how much of that may be dirty, is recorded in the watermarks.
void SatoriPage::FreeRegion(SatoriRegion* region)
{
    _ASSERTE(region->ContainingPage() == this);
    _ASSERTE(region->Generation() == -1);
    _ASSERTE(region->ContainingQueue() == nullptr);

    // read everything that is needed from the header while the region is ours.
    size_t start = region->Start();
    size_t end = region->End();
    size_t committed = region->CommittedEnd();
    size_t used = region->UsedEnd();

    _ASSERTE(start % Satori::REGION_SIZE_GRANULARITY == 0 && end % Satori::REGION_SIZE_GRANULARITY == 0);
    _ASSERTE(start >= m_firstRegion && end <= End());
    _ASSERTE(committed >= start && committed <= end);
    _ASSERTE(used >= start && used <= committed);
    // every commit is a whole number of watermark units
    _ASSERTE((committed - start) % Satori::WATERMARK_UNIT == 0);

    size_t first = (start - Start()) >> Satori::REGION_BITS;
    size_t count = (end - start) >> Satori::REGION_BITS;

    for (size_t granule = first; granule < first + count; granule++)
    {
        size_t granuleStart = GranuleStart(granule);
        size_t committedBytes = committed > granuleStart ?
            min(committed - granuleStart, Satori::REGION_SIZE_GRANULARITY) :
            0;
        size_t usedBytes = used > granuleStart ?
            min(used - granuleStart, Satori::REGION_SIZE_GRANULARITY) :
            0;

        // rounding up the used part is the safe direction. It cannot get above the committed part.
        size_t committedUnits = committedBytes >> Satori::WATERMARK_UNIT_BITS;
        size_t dirtyUnits = (usedBytes + Satori::WATERMARK_UNIT - 1) >> Satori::WATERMARK_UNIT_BITS;
        _ASSERTE(dirtyUnits <= committedUnits);

        m_commitWatermarks[granule] = (uint8_t)committedUnits;
        m_dirtyWatermarks[granule] = (uint8_t)dirtyUnits;

        // the other granules in the word may be changing, thus interlocked.
        size_t bit = BitRange(granule % 64, 1);
        if (committedUnits == Satori::WATERMARK_UNITS_PER_GRANULE)
        {
            Interlocked::Or(&m_warmBitmap[granule / 64], bit);
        }
        else
        {
            Interlocked::And(&m_warmBitmap[granule / 64], ~bit);
        }
    }

    OnRegionFreed(region);

    // The interlocked operation is a release for everything above. The next owner reads
    // the watermarks and the region map only after it clears its free bit.
    for (size_t word = first / 64; word <= (first + count - 1) / 64; word++)
    {
        size_t lowGranule = max(first, word * 64);
        size_t highGranule = min(first + count, word * 64 + 64);
        Interlocked::Or(&m_freeBitmap[word], BitRange(lowGranule - word * 64, highGranule - lowGranule));
    }
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
    size_t mapIndex = group * Satori::BYTES_PER_CARD_GROUP / Satori::REGION_SIZE_GRANULARITY;
    while (RegionMap()[mapIndex] > 1)
    {
        mapIndex -= ((size_t)1 << (RegionMap()[mapIndex] - 2));
    }

    // card groups are never set for free granules.
    _ASSERTE(RegionMap()[mapIndex] == 1);
    return (SatoriRegion*)((mapIndex << Satori::REGION_BITS) + Start());
}

// setting cards is never concurent with unsetting (unlike dirtying)
// so there are no ordering implications.
void SatoriPage::SetCardsForRange(size_t start, size_t end)
{
    _ASSERTE(end > start);

    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstCard = firstByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(firstCard >= m_cardTableStart);
    _ASSERTE(firstCard < m_cardTableSize);

    if (m_cardTable[firstCard] == Satori::CardState::EPHEMERAL)
    {
        // this is optimization
        // marking cards in an ephemeral region is ok, but pointless
        return;
    }

    size_t lastCard = lastByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(lastCard >= m_cardTableStart);
    _ASSERTE(lastCard < m_cardTableSize);

    memset((void*)(m_cardTable + firstCard), Satori::CardState::REMEMBERED, lastCard - firstCard + 1);

    size_t firstGroup = firstByteOffset / Satori::BYTES_PER_CARD_GROUP;
    size_t lastGroup = lastByteOffset / Satori::BYTES_PER_CARD_GROUP;
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

// 
void SatoriPage::DirtyCardForAddress(size_t address)
{
    size_t offset = address - Start();
    size_t cardByteOffset = offset / Satori::BYTES_PER_CARD_BYTE;

    _ASSERTE(cardByteOffset >= m_cardTableStart);
    _ASSERTE(cardByteOffset < m_cardTableSize);

    // we dirty the card unconditionally, since it can be concurrently cleaned
    // however this does not get called after field writes (unlike in barriers),
    // so the card dirtying write can be unordered.
    m_cardTable[cardByteOffset] = Satori::CardState::DIRTY;

    size_t cardGroup = offset / Satori::BYTES_PER_CARD_GROUP;
    VolatileStore(&this->m_cardGroups[cardGroup * 2], Satori::CardState::DIRTY);
    VolatileStore(&this->m_cardState, Satori::CardState::DIRTY);
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

    // we dirty these cards unconditionally, since they can be concurrently cleaned
    // see similar note above
    memset((void*)(m_cardTable + firstCard), Satori::CardState::DIRTY, lastCard - firstCard + 1);

    // if dirtying can be concurrent with cleaning, we must ensure order
    // of writes: cards, then groups, then page.
    // cleaning will read in the opposite order
    VolatileStoreBarrier();

    size_t firstGroup = firstByteOffset / Satori::BYTES_PER_CARD_GROUP;
    size_t lastGroup = lastByteOffset / Satori::BYTES_PER_CARD_GROUP;
    for (size_t i = firstGroup; i <= lastGroup; i++)
    {
        this->m_cardGroups[i * 2] = Satori::CardState::DIRTY;
    }

    VolatileStore(&this->m_cardState, Satori::CardState::DIRTY);
}

// this is called concurently with mutator after bulk writes
// we may be concurrently cleaning cards, but not groups and pages
void SatoriPage::DirtyCardsForRangeConcurrent(size_t start, size_t end)
{
    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstCard = firstByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(firstCard >= m_cardTableStart);
    _ASSERTE(firstCard < m_cardTableSize);

    size_t lastCard = lastByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(lastCard >= m_cardTableStart);
    _ASSERTE(lastCard < m_cardTableSize);

    // this must be ordered after bulk writes and cannot be conditional
    // since we may be cleaning concurrently
    VolatileStoreBarrier();
    memset((void*)(m_cardTable + firstCard), Satori::CardState::DIRTY, lastCard - firstCard + 1);

    // we do not clean groups concurrently, so these can be conditional and unordered
    // only the eventual final state matters
    size_t firstGroup = firstByteOffset / Satori::BYTES_PER_CARD_GROUP;
    size_t lastGroup = lastByteOffset / Satori::BYTES_PER_CARD_GROUP;
    for (size_t i = firstGroup; i <= lastGroup; i++)
    {
        if (m_cardGroups[i * 2] != Satori::CardState::DIRTY)
        {
            m_cardGroups[i * 2] = Satori::CardState::DIRTY;
        }
    }

    if (m_cardState != Satori::CardState::DIRTY)
    {
        m_cardState = Satori::CardState::DIRTY;
    }
}

void SatoriPage::WipeGroupsForRange(size_t start, size_t end)
{
    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstGroup = firstByteOffset / Satori::BYTES_PER_CARD_GROUP;
    size_t lastGroup = lastByteOffset / Satori::BYTES_PER_CARD_GROUP;
    // this resets both states and tickets
    memset((void*)&m_cardGroups[firstGroup * 2], Satori::CardState::BLANK, (lastGroup - firstGroup + 1) * 2);
}

void SatoriPage::ResetCardsForRange(size_t start, size_t end, bool isTenured)
{
    size_t firstByteOffset = start - Start();
    size_t lastByteOffset = end - Start() - 1;

    size_t firstCard = firstByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(firstCard >= m_cardTableStart);
    _ASSERTE(firstCard < m_cardTableSize);

    size_t lastCard = lastByteOffset / Satori::BYTES_PER_CARD_BYTE;
    _ASSERTE(lastCard >= m_cardTableStart);
    _ASSERTE(lastCard < m_cardTableSize);
    int8_t resetValue = isTenured ? Satori::CardState::BLANK : Satori::CardState::EPHEMERAL;
    memset((void*)(m_cardTable + firstCard), resetValue, lastCard - firstCard + 1);

    WipeGroupsForRange(start, end);
}

// Tells if the card groups for the range are as WipeGroupsForRange leaves them - blank and with no scan ticket.
bool SatoriPage::CardGroupsAreWipedForRange(size_t start, size_t end)
{
    size_t firstGroup = (start - Start()) / Satori::BYTES_PER_CARD_GROUP;
    size_t lastGroup = (end - Start() - 1) / Satori::BYTES_PER_CARD_GROUP;
    for (size_t i = firstGroup; i <= lastGroup; i++)
    {
        if (m_cardGroups[i * 2] != Satori::CardState::BLANK || m_cardGroups[i * 2 + 1] != 0)
        {
            return false;
        }
    }

    return true;
}

#if _DEBUG
bool SatoriPage::CardsAreBlankForRange(size_t start, size_t end)
{
    size_t firstCard = (start - Start()) / Satori::BYTES_PER_CARD_BYTE;
    size_t lastCard = (end - Start() - 1) / Satori::BYTES_PER_CARD_BYTE;
    for (size_t i = firstCard; i <= lastCard; i++)
    {
        if (m_cardTable[i] != Satori::CardState::BLANK)
        {
            return false;
        }
    }

    return true;
}
#endif
