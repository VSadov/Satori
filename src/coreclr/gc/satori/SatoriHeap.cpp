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

void InitWriteBarrier(uint8_t* segmentTable, size_t highest_address)
{
    WriteBarrierParameters args = {};
    args.operation = WriteBarrierOp::Initialize;
    args.is_runtime_suspended = true;
    args.requires_upper_bounds_check = false;
    args.card_table = (uint32_t*)segmentTable;

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    args.card_bundle_table = nullptr;
#endif

    args.lowest_address = (uint8_t*)1;
    args.highest_address = (uint8_t*)highest_address;
    args.ephemeral_low = (uint8_t*)-1;
    args.ephemeral_high = (uint8_t*)-1;
    GCToEEInterface::StompWriteBarrier(&args);
}

void UpdateWriteBarrier(uint8_t* segmentTable, size_t highest_address)
{
    WriteBarrierParameters args = {};
    args.operation = WriteBarrierOp::StompResize;
    args.is_runtime_suspended = false;
    args.requires_upper_bounds_check = false;
    args.card_table = (uint32_t*)segmentTable;

#ifdef FEATURE_MANUALLY_MANAGED_CARD_BUNDLES
    args.card_bundle_table = nullptr;
#endif

    args.lowest_address = (uint8_t*)1;
    args.highest_address = (uint8_t*)highest_address;
    args.ephemeral_low = (uint8_t*)-1;
    args.ephemeral_high = (uint8_t*)-1;
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

    SatoriHeap* result = (SatoriHeap*)reserved;
    result->m_reservedMapSize = mapSize;
    result->m_committedMapSize = (int)(commitSize - ((size_t)&result->m_pageMap - (size_t)result));
    InitWriteBarrier(result->m_pageMap, result->m_committedMapSize * Satori::PAGE_SIZE_GRANULARITY);
    result->m_mapLock.Initialize();
    result->m_nextPageIndex = 1;

    result->m_allocator.Initialize(result);
    result->m_recycler.Initialize(result);
    result->m_finalizationQueue.Initialize();

    return result;
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
            UpdateWriteBarrier(m_pageMap, m_committedMapSize * Satori::PAGE_SIZE_GRANULARITY);
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

        size_t pageAddress = (size_t)i << Satori::PAGE_BITS;
        newPage = SatoriPage::InitializeAt(pageAddress, Satori::PAGE_SIZE_GRANULARITY, this);
        if (newPage)
        {
            // SYNCRONIZATION:
            // A page map update must be seen by all threads befeore seeing objects allocated
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

            return true;
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
    // TUNING: extra address space is not necessarily a waste. It can be used by other regions.
    //         Considering that a region can never cross pages, maybe reserving slightly larger page
    //         might be beneficial.
    //         For now we will ensure just slightly more than the minimum.
    //
    //         The overhead of card table is 1/512, but there is some space needed for region headers
    //         and allocation padding. It is nearly 0, compared to page sizes, but not 0.
    //         Since we do not need to be precise in our estimates, we will just reserve 1/256 extra.
    //         The extra reserved space can be used so it is not a waste.
    minSize += minSize / 256;

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
                BitScanReverse(&log2, j);
                m_pageMap[i + j] = (uint8_t)(log2 + 2);
            }

            // we also need to ensure that the other thread doing the barrier,
            // reads the object before reading the updated map.
            // on ARM this would require load fence in the barrier. We will do a processwide here instead.
#if defined(HOST_ARM64) || defined(HOST_ARM)
            GCToOSInterface::FlushProcessWriteBuffers();
#endif


            // advance next if contiguous
            // Note: it may become contiguous after we check, but it should be rare.
            //       advancing is not needed for correctness, so we do not care.
            if (i == m_nextPageIndex)
            {
                Interlocked::CompareExchange(&m_nextPageIndex, i + mapMarkCount, i);
            }

            return newPage;
        }
    }

    return nullptr;
}

SatoriObject* SatoriHeap::ObjectForAddress(size_t address)
{
    return PageForAddress(address)->RegionForAddress(address)->FindObject(address);
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

SatoriObject* SatoriHeap::ObjectForAddressChecked(size_t address)
{
    SatoriRegion* region = RegionForAddressChecked(address);
    if (region)
    {
        return region->FindObject(address);
    }

    return nullptr;
}
