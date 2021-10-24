// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriHeap.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"
#include "../env/gcenv.ee.h"

#include "SatoriUtil.h"
#include "SatoriHeap.h"
#include "SatoriRegion.h"
#include "SatoriObject.h"
#include "SatoriPage.h"
#include "SatoriPage.inl"

void InitWriteBarrier(uint8_t* pageMap, size_t highest_address)
{
    WriteBarrierParameters args = {};
    args.operation = WriteBarrierOp::Initialize;
    args.is_runtime_suspended = true;
    args.card_table = (uint32_t*)pageMap;
    args.highest_address = (uint8_t*)highest_address;

    // dummy values to make asserts happy, we will not use this
    args.lowest_address = (uint8_t*)1;
    args.ephemeral_low = (uint8_t*)-1;
    args.ephemeral_high = (uint8_t*)-1;

    GCToEEInterface::StompWriteBarrier(&args);
}

void UpdateWriteBarrier(uint8_t* pageMap, size_t highest_address)
{
    WriteBarrierParameters args = {};
    args.operation = WriteBarrierOp::StompResize;
    args.is_runtime_suspended = false;
    args.card_table = (uint32_t*)pageMap;
    args.highest_address = (uint8_t*)highest_address;

    // dummy values to make asserts happy, we will not use this
    args.lowest_address = (uint8_t*)1;
    args.ephemeral_low = (uint8_t*)-1;
    args.ephemeral_high = (uint8_t*)-1;
    args.card_bundle_table = (uint32_t*)-1;

    GCToEEInterface::StompWriteBarrier(&args);
}

SatoriHeap* SatoriHeap::Create()
{
    // we need to cover the whole possible address space (48bit, 52 on rare ARM64).
    // Half-byte per 4Gb
    const int availableAddressSpaceBits = 48;
    const int pageCountBits = availableAddressSpaceBits - Satori::PAGE_BITS;
    const int mapSize = 1 << pageCountBits;
    size_t rezerveSize = mapSize + sizeof(SatoriHeap);

    void* reserved = GCToOSInterface::VirtualReserve(rezerveSize, 0, VirtualReserveFlags::None);

    size_t commitSize = min(Satori::CommitGranularity(), rezerveSize);
    if (!GCToOSInterface::VirtualCommit(reserved, commitSize))
    {
        // failure
        GCToOSInterface::VirtualRelease(reserved, rezerveSize);
        return nullptr;
    }

    // TODO: VS under very low memory, some of the following Initialize calls may fail
    //       do we care to handle that?
    SatoriHeap* heap = (SatoriHeap*)reserved;
    heap->m_reservedMapSize = mapSize;
    heap->m_committedMapSize = (int)(commitSize - ((size_t)&heap->m_pageMap - (size_t)heap));
    InitWriteBarrier(heap->m_pageMap, heap->m_committedMapSize * Satori::PAGE_SIZE_GRANULARITY - 1);
    heap->m_mapLock.Initialize();
    heap->m_nextPageIndex = 1;
    heap->m_usedMapSize = 1;

    heap->m_allocator.Initialize(heap);
    heap->m_recycler.Initialize(heap);
    heap->m_finalizationQueue.Initialize(heap);

    return heap;
}

bool SatoriHeap::CommitMoreMap(int currentlyCommitted)
{
    void* commitFrom = &m_pageMap[currentlyCommitted];
    size_t commitSize = Satori::CommitGranularity();

    SatoriLockHolder<SatoriLock> holder(&m_mapLock);
    if (currentlyCommitted < m_committedMapSize)
    {
        if (GCToOSInterface::VirtualCommit(commitFrom, commitSize))
        {
            // we did the commit
            m_committedMapSize = min(currentlyCommitted + (int)commitSize, m_reservedMapSize);
            UpdateWriteBarrier(m_pageMap, m_committedMapSize * Satori::PAGE_SIZE_GRANULARITY - 1);
        }
    }

    // either we did commit or someone else did, otherwise this is a failure.
    return m_committedMapSize > currentlyCommitted;
}

bool SatoriHeap::TryAddRegularPage(SatoriPage*& newPage)
{
    int nextPageIndex = m_nextPageIndex;
    for (int i = nextPageIndex; i < m_reservedMapSize; i++)
    {
        int currentMapSize = m_committedMapSize;
        if (i >= m_committedMapSize && !CommitMoreMap(currentMapSize))
        {
            break;
        }

        if (m_pageMap[i] == 0)
        {
            size_t pageAddress = (size_t)i << Satori::PAGE_BITS;
            newPage = SatoriPage::InitializeAt(pageAddress, Satori::PAGE_SIZE_GRANULARITY, this);
            if (newPage)
            {
                // SYNCRONIZATION:
                // A page map update must be seen by all threads before seeing objects allocated
                // in the new page or checked barriers may consider the objects not in the heap.
                //
                // If another thread checks if object is in heap, its read of the map element is dependent on object,
                // therefore the read will happen after the object is obtained.
                // Also the object must be published before other thread could see it, and publishing is a release.
                // Thus an ordinary write is ok even for weak memory cases.
                m_pageMap[i] = 1;

                // ensure the next is advanced to at least i + 1
                while ((nextPageIndex = m_nextPageIndex) < i + 1 &&
                    Interlocked::CompareExchange(&m_nextPageIndex, i + 1, nextPageIndex) != nextPageIndex);

                int usedMapSize;
                // ensure the m_usedMapSize is advanced to at least i + 1
                while ((usedMapSize = m_usedMapSize) < i + 1 &&
                    Interlocked::CompareExchange(&m_usedMapSize, i + 1, usedMapSize) != usedMapSize);

                return true;
            }
        }

        // check if someone else added a page
        if (m_nextPageIndex != nextPageIndex)
        {
            return true;
        }
    }

    // most likely OOM
    return false;
}

// it is harder to find a contiguous space for a large page
// also unlikely that we cross-use one if just comitted
// we scan ahead and claim, but do not move m_nextPageIndex
// unless we claimed contiguously.
SatoriPage* SatoriHeap::AddLargePage(size_t minSize)
{
    // Add the overhead of card table (1/512).
    minSize += minSize / Satori::BYTES_PER_CARD_BYTE;
    minSize = ALIGN_UP(minSize, Satori::PAGE_SIZE_GRANULARITY);
    int mapMarkCount = (int)(minSize >> Satori::PAGE_BITS);

    for (int i = m_nextPageIndex; i < m_reservedMapSize; i++)
    {
        int currentMapSize = m_committedMapSize;
        while (i + mapMarkCount >= m_committedMapSize)
        {
            if (!CommitMoreMap(currentMapSize))
            {
                return nullptr;
            }
        }

        if (m_pageMap[i] == 0)
        {
            size_t pageAddress = (size_t)i << Satori::PAGE_BITS;
            SatoriPage* newPage = SatoriPage::InitializeAt(pageAddress, minSize, this);
            if (newPage)
            {
                // mark the map, before an object can be allocated in the new page and
                // may be seen in a GC barrier
                m_pageMap[i] = 1;
                for (int j = 1; j < mapMarkCount; j++)
                {
                    DWORD log2;
                    BitScanReverse64(&log2, j);
                    m_pageMap[i + j] = (uint8_t)(log2 + 2);
                }

                int usedMapSize;
                // ensure the m_usedMapSize is advanced to at least i + mapMarkCount
                while ((usedMapSize = m_usedMapSize) < i + mapMarkCount &&
                    Interlocked::CompareExchange(&m_usedMapSize, i + mapMarkCount, usedMapSize) != usedMapSize);

                // advance next if contiguous
                // Note: it may become contiguous after we check, but it should be rare.
                //       advancing m_nextPageIndex is not needed for correctness, so we do not retry.
                if (i == m_nextPageIndex)
                {
                    Interlocked::CompareExchange(&m_nextPageIndex, i + mapMarkCount, i);
                }

                return newPage;
            }
        }
    }

    return nullptr;
}

SatoriRegion* SatoriHeap::RegionForAddressChecked(size_t address)
{
    SatoriPage* page = PageForAddressChecked(address);
    if (page)
    {
        return page->RegionForAddressChecked(address);
    }

    return nullptr;
}
