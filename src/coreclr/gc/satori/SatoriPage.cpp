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
#include "SatoriRegionQueue.h"
#include "SatoriPageAddTrace.h" // TEMP-TRACE
#include <minipal/log.h>

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

    if (!TraceVirtualCommit((void*)address, commitSize)) // TEMP-TRACE
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
#if _DEBUG
    static_assert(offsetof(SatoriPage, m_snapshotStamp) + sizeof(size_t) <= 128,
        "the header fields must end before the card groups.");
#else
    static_assert(offsetof(SatoriPage, m_seenFreeBitmap) + sizeof(size_t*) <= 128,
        "the header fields must end before the card groups.");
#endif

    result->m_regionMap = (uint8_t*)(address + 128 + cardGroupSize);

    // The free bitmap is one cache line per 1Gb. The bitmaps and the watermarks need 2.375 bytes per granule
    // on top of the region map, and the card table that covers itself has room for that at any page size.
    _ASSERTE(regionCount % 64 == 0);
    result->m_freeBitmap = (size_t*)ALIGN_UP((size_t)result->m_regionMap + regionCount, 64);
    result->m_warmBitmap = result->m_freeBitmap + regionCount / 64;
    result->m_commitWatermarks = (uint8_t*)(result->m_warmBitmap + regionCount / 64);
    result->m_dirtyWatermarks = result->m_commitWatermarks + regionCount;
    result->m_seenFreeBitmap = (size_t*)(result->m_dirtyWatermarks + regionCount);

    // make sure the first useful card word is beyond the header.
    _ASSERTE(result->Start() + cardTableStart >= (size_t)(result->m_seenFreeBitmap + regionCount / 64));

#if _DEBUG
    // The stamps do not fit into the page, they are for Checked builds only.
    result->m_freeStamps = new (nothrow) size_t[regionCount]();
    if (result->m_freeStamps == nullptr)
    {
        heap->DecBytesCommitted(commitSize);
        GCToOSInterface::VirtualRelease(result, pageSize);
        status = InitStatus::CommitFailed;
        return nullptr;
    }
#endif

    // Everything after the card table is free. If the commit granularity made us commit more than the card table,
    // that part is committed and clean.
    // The page is published in the heap only after this, so nobody can claim anything before it is ready.
    for (size_t granule = result->FirstGranule(); granule < regionCount; granule++)
    {
        size_t bit = (size_t)1 << (granule % 64);
        result->m_freeBitmap[granule / 64] |= bit;

        size_t granuleStart = result->GranuleStart(granule);
        size_t committedBytes = result->m_initialCommit > granuleStart ?
            min(result->m_initialCommit - granuleStart, Satori::REGION_SIZE_GRANULARITY) :
            0;

        size_t committedUnits = committedBytes >> Satori::WATERMARK_UNIT_BITS;
        result->m_commitWatermarks[granule] = (uint8_t)committedUnits;
        if (committedUnits == Satori::WATERMARK_UNITS_PER_GRANULE)
        {
            result->m_warmBitmap[granule / 64] |= bit;
        }
    }

    status = InitStatus::Success;
    return result;
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

#if _DEBUG
// The counter that the stamps of the trimmer's check come from, see m_freeStamps.
static volatile size_t s_freeClock;
#endif

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

#if _DEBUG
    // before the seen bits are cleared and the free bits set, see TryTrimGranule
    size_t freeStamp = Interlocked::Increment(&s_freeClock);
#endif

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
#if _DEBUG
        m_freeStamps[granule] = freeStamp;
#endif

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

    // These granules are not the ones that the trimmer saw free. The trimmer reads a free bit before it reads the seen bit,
    // and the interlocked operation that sets the free bits below is a release for this one.
    ForgetSeenFree(first, count);

    OnRegionFreed(region);

    // The interlocked operation is a release for everything above. The next owner reads
    // the watermarks and the region map only after it clears its free bit.
    PublishFreeGranules(first, count);
}

// Sets the free bits of the granules.
// The granules must be ready for the next owner: the watermarks written and the region map bytes zero.
// (A claim that did not format anything gives the granules back with the watermarks that it found.)
void SatoriPage::PublishFreeGranules(size_t first, size_t count)
{
    _ASSERTE(first >= FirstGranule() && first + count <= GranuleCount());
    for (size_t word = first / 64; word <= (first + count - 1) / 64; word++)
    {
        size_t lowGranule = max(first, word * 64);
        size_t highGranule = min(first + count, word * 64 + 64);
        size_t bits = BitRange(lowGranule - word * 64, highGranule - lowGranule);
        _ASSERTE((VolatileLoad(&m_freeBitmap[word]) & bits) == 0);
        Interlocked::Or(&m_freeBitmap[word], bits);
    }
}

// Claims the granule that has the lowest (or the highest) address of the free ones. When asked, only among the warm ones.
// Returns SIZE_MAX if there is none. Every failed attempt to take a bit is counted as a retry.
size_t SatoriPage::TryClaimGranule(bool warmOnly, bool fromTop, size_t& retries)
{
    size_t wordCount = GranuleCount() / 64;
    for (size_t i = 0; i < wordCount; i++)
    {
        size_t word = fromTop ? wordCount - 1 - i : i;
        size_t freeBits = VolatileLoad(&m_freeBitmap[word]);
        while (true)
        {
            size_t candidates = warmOnly ? (freeBits & VolatileLoad(&m_warmBitmap[word])) : freeBits;
            if (candidates == 0)
            {
                break;
            }

            DWORD bit;
            if (fromTop)
            {
                BitScanReverse64(&bit, candidates);
            }
            else
            {
                BitScanForward64(&bit, candidates);
            }

            size_t seen = Interlocked::CompareExchange(&m_freeBitmap[word], freeBits & ~((size_t)1 << bit), freeBits);
            if (seen == freeBits)
            {
                return word * 64 + bit;
            }

            // the word has changed. Whether our bit is still there is decided by the next round.
            freeBits = seen;
            retries++;
        }
    }

    return SIZE_MAX;
}

// Claims the granule, if it is free.
bool SatoriPage::TryClaimGranuleAt(size_t granule)
{
    _ASSERTE(granule >= FirstGranule() && granule < GranuleCount());

    size_t bit = (size_t)1 << (granule % 64);
    size_t freeBits = VolatileLoad(&m_freeBitmap[granule / 64]);
    while (freeBits & bit)
    {
        size_t seen = Interlocked::CompareExchange(&m_freeBitmap[granule / 64], freeBits & ~bit, freeBits);
        if (seen == freeBits)
        {
            return true;
        }

        freeBits = seen;
    }

    return false;
}

// Finds the first granule of a run of free granules that is the lowest (or the highest) among the runs that fit.
// The free bits are read as they are, claimers of single granules could change them meanwhile.
size_t SatoriPage::FindRun(size_t count, bool fromTop)
{
    _ASSERTE(count > 1);

    size_t first = FirstGranule();
    size_t end = GranuleCount();
    size_t runLength = 0;

    if (!fromTop)
    {
        size_t granule = first;
        while (granule < end)
        {
            size_t word = VolatileLoad(&m_freeBitmap[granule / 64]);
            size_t bit = granule % 64;
            if (bit == 0 && word == 0)
            {
                // nothing is free in the word
                runLength = 0;
                granule += 64;
                continue;
            }

            if ((word >> bit) & 1)
            {
                // this is the end of the run if it is long enough.
                if (++runLength == count)
                {
                    return granule + 1 - count;
                }
            }
            else
            {
                runLength = 0;
            }

            granule++;
        }
    }
    else
    {
        // the granule before this one is the next to look at
        size_t granule = end;
        while (granule > first)
        {
            size_t current = granule - 1;
            size_t word = VolatileLoad(&m_freeBitmap[current / 64]);
            size_t bit = current % 64;
            if (bit == 63 && word == 0)
            {
                runLength = 0;
                granule -= 64;
                continue;
            }

            if ((word >> bit) & 1)
            {
                // this is the start of the run if it is long enough.
                if (++runLength == count)
                {
                    return current;
                }
            }
            else
            {
                runLength = 0;
            }

            granule--;
        }
    }

    return SIZE_MAX;
}

// The bits of the run [first, first + count) that are in the word.
static size_t RunBitsInWord(size_t first, size_t count, size_t word)
{
    size_t lowGranule = max(first, word * 64);
    size_t highGranule = min(first + count, word * 64 + 64);
    return BitRange(lowGranule - word * 64, highGranule - lowGranule);
}

// Clears the free bits of the run, if all of them are set. Otherwise leaves everything as it was.
bool SatoriPage::TryClearRun(size_t first, size_t count)
{
    size_t firstWord = first / 64;
    size_t lastWord = (first + count - 1) / 64;
    for (size_t word = firstWord; word <= lastWord; word++)
    {
        size_t runBits = RunBitsInWord(first, count, word);
        size_t freeBits = VolatileLoad(&m_freeBitmap[word]);
        while (true)
        {
            if ((freeBits & runBits) != runBits)
            {
                // Someone has taken a granule of the run. Put back what we have cleared.
                for (size_t cleared = firstWord; cleared < word; cleared++)
                {
                    Interlocked::Or(&m_freeBitmap[cleared], RunBitsInWord(first, count, cleared));
                }

                return false;
            }

            size_t seen = Interlocked::CompareExchange(&m_freeBitmap[word], freeBits & ~runBits, freeBits);
            if (seen == freeBits)
            {
                break;
            }

            freeBits = seen;
        }
    }

    return true;
}

size_t SatoriPage::TryClaimRun(size_t count, bool fromTop, size_t& rollbacks)
{
    // Every failed attempt means that somebody claimed a granule of the run that we found. That is rare,
    // and it is not worth looking for long on this page if it keeps happening.
    for (int attempt = 0; attempt < 4; attempt++)
    {
        size_t first = FindRun(count, fromTop);
        if (first == SIZE_MAX || TryClearRun(first, count))
        {
            return first;
        }

        rollbacks++;
    }

    return SIZE_MAX;
}

// Gives back a page that was not published. The only thing committed in it is the card table.
void SatoriPage::Release()
{
#if _DEBUG
    delete[] m_freeStamps;
#endif
    m_heap->DecBytesCommitted(m_initialCommit - Start());
    GCToOSInterface::VirtualRelease(this, m_end - Start());
}

// Clears the seen bits of the granules, see FreeRegion.
void SatoriPage::ForgetSeenFree(size_t first, size_t count)
{
    for (size_t word = first / 64; word <= (first + count - 1) / 64; word++)
    {
        Interlocked::And(&m_seenFreeBitmap[word], ~RunBitsInWord(first, count, word));
    }
}

// The head of a trimmed granule that stays committed, a whole number of watermark units.
// TryGetRegularRegion reads the header of the region that follows its own, a region that it does not own. That region
// can be freed and its granule trimmed meanwhile, and the read must not fault. What the head holds is what the last region
// left there, and FreeRegion asserts that its queue is null, so the read finds no queue and moves on.
// This also covers any other reader that gets to a header through the region map.
static size_t TrimmedHeadBytes()
{
    return ALIGN_UP(SatoriUtil::CommitGranularity(), Satori::WATERMARK_UNIT);
}

size_t SatoriPage::NextGranuleToTrim(size_t from, bool onlyIfSeenFree)
{
    size_t end = GranuleCount();
    size_t granule = max(from, FirstGranule());
    while (granule < end)
    {
        size_t word = granule / 64;
        size_t candidates = VolatileLoad(&m_freeBitmap[word]);
        if (onlyIfSeenFree)
        {
            // after the free bits: see FreeRegion
            candidates &= VolatileLoad(&m_seenFreeBitmap[word]);
        }

        // only the granules that are not before the one we were asked for
        candidates &= ~(size_t)0 << (granule % 64);
        while (candidates)
        {
            DWORD bit;
            BitScanForward64(&bit, candidates);
            size_t candidate = word * 64 + bit;

            // The watermark of a free granule is stable, but the granule may be claimed any moment. This is only a hint,
            // TryTrimGranule looks at the watermark again when the granule is ours.
            if (((size_t)m_commitWatermarks[candidate] << Satori::WATERMARK_UNIT_BITS) > TrimmedHeadBytes())
            {
                return candidate;
            }

            candidates &= candidates - 1;
        }

        granule = (word + 1) * 64;
    }

    return SIZE_MAX;
}

bool SatoriPage::TryTrimGranule(size_t granule, bool onlyIfSeenFree)
{
    if (!TryClaimGranuleAt(granule))
    {
        return false;
    }

    TraceTrimInc(); // TEMP-TRACE

    // The granule could have been claimed and freed again after the caller found it, and then our claim took the new free bit.
    // FreeRegion clears the seen bit before it sets the free bit, so a granule that is seen now was not freed since it was seen.
    if (onlyIfSeenFree && ((VolatileLoad(&m_seenFreeBitmap[granule / 64]) >> (granule % 64)) & 1) == 0)
    {
        PublishFreeGranules(granule, 1);
        TraceTrimDec(); // TEMP-TRACE
        return false;
    }

#if _DEBUG
    if (onlyIfSeenFree && m_freeStamps[granule] >= m_snapshotStamp)
    {
        minipal_log_print_error("Satori: page %p, granule %zu: the trimmer took a granule that was freed (stamp %zu) after the last look at its page (stamp %zu)\n",
            (void*)this, granule, m_freeStamps[granule], m_snapshotStamp);
        _ASSERTE(!"the trimmer took a granule that was freed after the last look at its page");
    }
#endif

    // The granule is ours. What is committed is what the watermarks say, and no region is looking at the memory.
    // Everything but the head is decommitted.
    bool trimmed = false;
    size_t headBytes = TrimmedHeadBytes();
    size_t committedBytes = (size_t)m_commitWatermarks[granule] << Satori::WATERMARK_UNIT_BITS;
    if (committedBytes > headBytes &&
        TraceVirtualDecommit((void*)(GranuleStart(granule) + headBytes), committedBytes - headBytes)) // TEMP-TRACE
    {
        m_heap->DecBytesCommitted(committedBytes - headBytes);
        TraceNoteDecommit(GranuleStart(granule) + headBytes, committedBytes - headBytes); // TEMP-TRACE

        // what is committed next time is the head, and it may hold what the last region left there. No part of it is warm.
        size_t headUnits = headBytes >> Satori::WATERMARK_UNIT_BITS;
        m_commitWatermarks[granule] = (uint8_t)headUnits;
        m_dirtyWatermarks[granule] = (uint8_t)min((size_t)m_dirtyWatermarks[granule], headUnits);
        Interlocked::And(&m_warmBitmap[granule / 64], ~((size_t)1 << (granule % 64)));
        trimmed = true;
    }

    PublishFreeGranules(granule, 1);
    TraceTrimDec(); // TEMP-TRACE
    return trimmed;
}

// TEMP-TRACE
void SatoriPage::TraceCollect(TraceSnap& s, size_t request)
{
    size_t runStart = SIZE_MAX;
    bool runCommitted = true;
    auto closeRun = [&](size_t endGranule)
    {
        size_t runSize = (endGranule - runStart) << Satori::REGION_BITS;
        s.runs++;
        s.maxRun = max(s.maxRun, runSize);
        s.maxRegion = max(s.maxRegion, runSize);
        if (runSize >= request)
        {
            s.runsGE++;
            if (runCommitted)
            {
                s.runsGECommitted++;
            }
        }

        runStart = SIZE_MAX;
    };

    for (size_t granule = FirstGranule(); granule < GranuleCount(); granule++)
    {
        if ((VolatileLoad(&m_freeBitmap[granule / 64]) >> (granule % 64)) & 1)
        {
            size_t committedBytes = (size_t)m_commitWatermarks[granule] << Satori::WATERMARK_UNIT_BITS;
            s.regions++;
            s.n2++;
            s.freeBytes += Satori::REGION_SIZE_GRANULARITY;
            s.freeCommitted += committedBytes;
            if (request <= Satori::REGION_SIZE_GRANULARITY)
            {
                s.singleGE++;
            }

            if (runStart == SIZE_MAX)
            {
                runStart = granule;
                runCommitted = true;
            }

            runCommitted = runCommitted && committedBytes == Satori::REGION_SIZE_GRANULARITY;
            if (committedBytes == Satori::REGION_SIZE_GRANULARITY)
            {
                s.warmFree++;
            }
        }
        else if (runStart != SIZE_MAX)
        {
            closeRun(granule);
        }
    }

    if (runStart != SIZE_MAX)
    {
        closeRun(GranuleCount());
    }
}

void SatoriPage::RememberFreeGranules()
{
    for (size_t word = 0; word < GranuleCount() / 64; word++)
    {
        VolatileStore(&m_seenFreeBitmap[word], VolatileLoad(&m_freeBitmap[word]));
    }

#if _DEBUG
    // After the copy: a granule that the copy saw as free was freed before this stamp.
    m_snapshotStamp = Interlocked::Increment(&s_freeClock);
#endif
}

// TEMP-TRACE
void SatoriPage::PrintLayout()
{
    size_t count = GranuleCount();
    size_t first = FirstGranule();
    minipal_log_print_error("  page %p, %zu granules\n", (void*)this, count);

    char row[65];
    for (size_t rowStart = 0; rowStart < count; rowStart += 64)
    {
        for (size_t i = 0; i < 64; i++)
        {
            size_t granule = rowStart + i;
            char c;
            if (granule < first)
            {
                c = '#';
            }
            else if ((VolatileLoad(&m_freeBitmap[granule / 64]) >> (granule % 64)) & 1)
            {
                c = ((m_warmBitmap[granule / 64] >> (granule % 64)) & 1) ? ':' : '.';
            }
            else if (RegionMap()[granule] == 1)
            {
                SatoriRegion* region = (SatoriRegion*)GranuleStart(granule);
                int generation = region->Generation();
                SatoriQueue<SatoriRegion>* queue = region->ContainingQueue();
                c = (queue && queue->Kind() == QueueKind::Allocator) ? 'c' :
                    generation == 3 ? 'I' :
                    generation >= 0 ? (char)('0' + generation) :
                    'L';
            }
            else
            {
                c = '=';
            }

            row[i] = c;
        }

        row[64] = 0;
        minipal_log_print_error("  %5zu %s\n", rowStart, row);
    }
}

#if _DEBUG
static size_t GranuleProblem(SatoriPage* page, size_t granule, const char* what, bool report)
{
    if (report)
    {
        minipal_log_print_error("Satori: page %p, granule %zu: %s\n", (void*)page, granule, what);
    }

    return 1;
}

size_t SatoriPage::VerifyGranules(size_t& committedBytes, bool report)
{
    size_t problems = 0;
    size_t first = FirstGranule();
    size_t end = GranuleCount();

    // The card table is neither free nor covered by regions.
    for (size_t granule = 0; granule < first; granule++)
    {
        if ((m_freeBitmap[granule / 64] >> (granule % 64)) & 1)
        {
            problems += GranuleProblem(this, granule, "a granule of the card table is free", report);
        }

        if (RegionMap()[granule] != 0)
        {
            problems += GranuleProblem(this, granule, "a granule of the card table is in the region map", report);
        }
    }

    committedBytes += min(m_initialCommit, m_firstRegion) - Start();

    // Every other granule is either free or covered by exactly one region.
    size_t granule = first;
    while (granule < end)
    {
        bool isFree = (VolatileLoad(&m_freeBitmap[granule / 64]) >> (granule % 64)) & 1;
        uint8_t mapValue = RegionMap()[granule];

        if (isFree)
        {
            if (mapValue != 0)
            {
                problems += GranuleProblem(this, granule, "a free granule is in the region map", report);
            }

            size_t committedUnits = m_commitWatermarks[granule];
            size_t dirtyUnits = m_dirtyWatermarks[granule];
            bool isWarm = (m_warmBitmap[granule / 64] >> (granule % 64)) & 1;
            if (dirtyUnits > committedUnits || committedUnits > Satori::WATERMARK_UNITS_PER_GRANULE)
            {
                problems += GranuleProblem(this, granule, "the watermarks of a free granule are not in order", report);
            }

            if (isWarm != (committedUnits == Satori::WATERMARK_UNITS_PER_GRANULE))
            {
                problems += GranuleProblem(this, granule, "the warm bit of a free granule does not match its commit", report);
            }

            committedBytes += committedUnits << Satori::WATERMARK_UNIT_BITS;
            granule++;
            continue;
        }

        if (mapValue != 1)
        {
            problems += GranuleProblem(this, granule, "a granule is not free and is not the start of a region", report);
            granule++;
            continue;
        }

        SatoriRegion* region = (SatoriRegion*)GranuleStart(granule);
        size_t size = region->Size();
        size_t count = size >> Satori::REGION_BITS;
        if (region->Start() != GranuleStart(granule) ||
            region->ContainingPage() != this ||
            size == 0 ||
            size % Satori::REGION_SIZE_GRANULARITY != 0 ||
            granule + count > end)
        {
            problems += GranuleProblem(this, granule, "a region does not fit where the region map has it", report);
            granule++;
            continue;
        }

        if (region->CommittedEnd() < region->Start() ||
            region->CommittedEnd() > region->End() ||
            region->UsedEnd() > region->CommittedEnd())
        {
            problems += GranuleProblem(this, granule, "a region has its commit or used part out of order", report);
        }
        else
        {
            committedBytes += region->CommittedEnd() - region->Start();
        }

        for (size_t i = 1; i < count; i++)
        {
            DWORD log2;
            BitScanReverse64(&log2, i);
            if (RegionMap()[granule + i] != (uint8_t)(log2 + 2))
            {
                problems += GranuleProblem(this, granule + i, "the region map does not continue the region", report);
            }

            if ((m_freeBitmap[(granule + i) / 64] >> ((granule + i) % 64)) & 1)
            {
                problems += GranuleProblem(this, granule + i, "a granule of a region is free", report);
            }
        }

        granule += count;
    }

    return problems;
}
#endif

// Calls the function for the ranges of a claimed run that must be committed to make its commit a prefix.
// The commit of the granules is whatever the last owners left. The function returns false to stop.
template <typename F>
static bool ForEachCommitGap(SatoriPage* page, size_t first, size_t count, size_t committed, uint8_t* commitWatermarks, F fn)
{
    size_t gapStart = 0;
    size_t gapEnd = 0;
    for (size_t granule = first; granule < first + count; granule++)
    {
        size_t granuleStart = page->GranuleStart(granule);
        if (granuleStart >= committed)
        {
            break;
        }

        // what is committed in this granule and what must be
        size_t have = granuleStart + ((size_t)commitWatermarks[granule] << Satori::WATERMARK_UNIT_BITS);
        size_t need = min(granuleStart + Satori::REGION_SIZE_GRANULARITY, committed);
        if (have < need)
        {
            if (gapEnd == have)
            {
                gapEnd = need;
            }
            else
            {
                if (gapStart != gapEnd && !fn(gapStart, gapEnd))
                {
                    return false;
                }

                gapStart = have;
                gapEnd = need;
            }
        }
    }

    return gapStart == gapEnd || fn(gapStart, gapEnd);
}

// Formats claimed granules as a region.
// The granules are ours, so what the last owners left in the watermarks is stable.
// If that can't be done, gives the granules back as they were and returns nullptr.
SatoriRegion* SatoriPage::FormatClaimed(size_t first, size_t count)
{
    _ASSERTE(count > 0);
    size_t start = GranuleStart(first);

    // The region is committed as a prefix, so it is the highest committed byte that matters.
    // Same for the dirty part, the rest of the committed part is zeroed.
    size_t committed = start;
    size_t used = start;
    for (size_t granule = first; granule < first + count; granule++)
    {
        size_t committedUnits = m_commitWatermarks[granule];
        size_t dirtyUnits = m_dirtyWatermarks[granule];
        _ASSERTE(dirtyUnits <= committedUnits && committedUnits <= Satori::WATERMARK_UNITS_PER_GRANULE);

        if (committedUnits)
        {
            committed = GranuleStart(granule) + (committedUnits << Satori::WATERMARK_UNIT_BITS);
        }

        if (dirtyUnits)
        {
            used = GranuleStart(granule) + (dirtyUnits << Satori::WATERMARK_UNIT_BITS);
        }
    }

#if _DEBUG
    // The region does not zero what is above its used part when it allocates, so it had better be zero.
    for (size_t granule = first; granule < first + count; granule++)
    {
        size_t granuleStart = GranuleStart(granule);
        size_t* p = (size_t*)(granuleStart + ((size_t)m_dirtyWatermarks[granule] << Satori::WATERMARK_UNIT_BITS));
        size_t* end = (size_t*)(granuleStart + ((size_t)m_commitWatermarks[granule] << Satori::WATERMARK_UNIT_BITS));
        for (; p < end; p++)
        {
            if (*p != 0)
            {
                minipal_log_print_error("Satori: page %p, granule %zu: nonzero memory at %p, above the dirty watermark of %u units\n",
                    (void*)this, granule, (void*)p, (unsigned)m_dirtyWatermarks[granule]);
                _ASSERTE(!"nonzero memory above the dirty watermark");
                break;
            }
        }
    }
#endif

    // Fill the gaps in the commit, the object that needed the run would commit them anyway.
    // This is not a thing that a single granule needs.
    size_t failedAt = SIZE_MAX;
    bool committedGaps = ForEachCommitGap(this, first, count, committed, m_commitWatermarks,
        [&](size_t gapStart, size_t gapEnd)
        {
            if (!TraceVirtualCommit((void*)gapStart, gapEnd - gapStart)) // TEMP-TRACE
            {
                failedAt = gapStart;
                return false;
            }

            m_heap->IncBytesCommitted(gapEnd - gapStart);
            TraceNoteCommit(gapStart, gapEnd - gapStart); // TEMP-TRACE
            SatoriTrace::Count(SatoriTrace::s_counters.gapCommitBytes, gapEnd - gapStart);
            return true;
        }
    );

    SatoriRegion* region = nullptr;
    if (committedGaps)
    {
        region = SatoriRegion::InitializeAt(this, start, count << Satori::REGION_BITS, committed, used);
    }

    if (region == nullptr)
    {
        // out of memory. Decommit what we have committed and give the granules back.
        ForEachCommitGap(this, first, count, committed, m_commitWatermarks,
            [&](size_t gapStart, size_t gapEnd)
            {
                if (gapStart >= failedAt)
                {
                    return false;
                }

                if (TraceVirtualDecommit((void*)gapStart, gapEnd - gapStart)) // TEMP-TRACE
                {
                    m_heap->DecBytesCommitted(gapEnd - gapStart);
                    TraceNoteDecommit(gapStart, gapEnd - gapStart); // TEMP-TRACE
                    return true;
                }

                // It stays committed, and it is still zero, nothing has written into it. The granules go back as they are,
                // except that the watermarks must tell the next owner about this commit, and the heap keeps counting it.
                for (size_t granule = (gapStart - Start()) >> Satori::REGION_BITS; GranuleStart(granule) < gapEnd; granule++)
                {
                    size_t granuleStart = GranuleStart(granule);
                    size_t units = (min(gapEnd, granuleStart + Satori::REGION_SIZE_GRANULARITY) - granuleStart) >> Satori::WATERMARK_UNIT_BITS;
                    if (units > m_commitWatermarks[granule])
                    {
                        m_commitWatermarks[granule] = (uint8_t)units;
                        if (units == Satori::WATERMARK_UNITS_PER_GRANULE)
                        {
                            Interlocked::Or(&m_warmBitmap[granule / 64], (size_t)1 << (granule % 64));
                        }
                    }
                }

                return true;
            }
        );

        PublishFreeGranules(first, count);
    }

    return region;
}

SatoriRegion* SatoriPage::RegionForAddressChecked(size_t address)
{
    _ASSERTE(address >= Start() && address < End());
    size_t mapIndex = (address - Start()) >> Satori::REGION_BITS;

    // Regions are freed meanwhile, and that zeroes the map. A byte is read once, and the byte that ends the search
    // is the one that decides. Reading it again could find zero where there was a continuation, and shift by a negative count.
    uint8_t mapValue;
    while ((mapValue = VolatileLoad(&RegionMap()[mapIndex])) > 1)
    {
        mapIndex -= ((size_t)1 << (mapValue - 2));
    }

    if (mapValue != 1)
    {
        // there is no region here.
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

    // a byte is read once, see RegionForAddressChecked
    uint8_t mapValue;
    while ((mapValue = VolatileLoad(&RegionMap()[mapIndex])) > 1)
    {
        mapIndex -= ((size_t)1 << (mapValue - 2));
    }

    // card groups are never set for free granules.
    _ASSERTE(mapValue == 1);
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
