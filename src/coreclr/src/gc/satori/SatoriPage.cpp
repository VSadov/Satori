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
#include "SatoriRegion.h"

SatoriPage* SatoriPage::InitializeAt(size_t address, size_t pageSize, SatoriHeap* heap)
{
    _ASSERTE(pageSize % Satori::PAGE_SIZE_GRANULARITY == 0);

    SatoriPage* result = (SatoriPage*)GCToOSInterface::VirtualReserve((void*)address, pageSize);
    if (result == nullptr)
    {
        return result;
    }

    // 128 bit/byte
    size_t cardTableBytes = pageSize / (128 * 8);

    // commit size is the same as header size. We could commit more in the future.
    size_t commitSize = ALIGN_UP(cardTableBytes, GCToOSInterface::GetPageSize());
    if (!GCToOSInterface::VirtualCommit((void*)address, commitSize))
    {
        GCToOSInterface::VirtualRelease((void*)address, pageSize);
        return nullptr;
    }

    result->m_end = address + pageSize;
    result->m_firstRegion = address + ALIGN_UP(cardTableBytes, Satori::REGION_SIZE_GRANULARITY);
    result->m_initialCommit = address + commitSize;
    result->m_cardTableSize = (int)cardTableBytes / sizeof(size_t);

    // conservatively assume the first useful card word to cover the start of the first region.
    size_t cardTableStart = (result->m_firstRegion - address) / (128 * 8 * sizeof(size_t));

    // make sure the first useful card word is beyond the header.
    size_t regionMapSize = pageSize >> Satori::REGION_BITS;
    _ASSERTE(cardTableStart * sizeof(size_t) > offsetof(SatoriPage, m_regionMap) + regionMapSize);

    result->m_cardTableStart = (int)cardTableStart;
    result->m_heap = heap;

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
    size_t startIndex = (region->Start() - Start()) >> Satori::REGION_BITS;
    size_t mapCount = region->Size() >> Satori::REGION_BITS;
    RegionMap()[startIndex] = 1;
    for (int i = 1; i < mapCount; i++)
    {
        // TODO: VS skip-marks
        RegionMap()[startIndex + i] = 2;
    }
}

void SatoriPage::RegionDestroyed(SatoriRegion* region)
{
    size_t startIndex = (region->Start() - Start()) >> Satori::REGION_BITS;
    size_t mapCount = region->Size() >> Satori::REGION_BITS;
    for (int i = 0; i < mapCount; i++)
    {
        // TODO: VS skip-marks
        RegionMap()[startIndex + i] = 0;
    }
}

SatoriRegion* SatoriPage::RegionForAddress(size_t address)
{
    size_t mapIndex = address >> Satori::REGION_BITS;
    while (RegionMap()[mapIndex] > 1)
    {
        mapIndex -= ((size_t)1 << (RegionMap()[mapIndex] - 2));
    }

    return (SatoriRegion*)(mapIndex << Satori::REGION_BITS);
}
