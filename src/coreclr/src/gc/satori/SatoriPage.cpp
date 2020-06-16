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

SatoriPage* SatoriPage::InitializeAt(size_t address, size_t pageSize)
{
    _ASSERTE(pageSize % Satori::PAGE_SIZE_GRANULARITY == 0);

    SatoriPage* result = (SatoriPage*)GCToOSInterface::VirtualReserve((void*)address, pageSize);
    if (result == nullptr)
    {
        return result;
    }

    size_t headerSize = ALIGN_UP(sizeof(SatoriPage), Satori::REGION_SIZE_GRANULARITY);
    size_t commitSize = headerSize;
    if (!GCToOSInterface::VirtualCommit((void*)address, commitSize))
    {
        GCToOSInterface::VirtualRelease((void*)address, pageSize);
        return nullptr;
    }

    result->m_end = address + pageSize;
    result->m_firstRegion = address + headerSize;
    result->m_initialCommit = address + commitSize;
    return result;
}

SatoriRegion* SatoriPage::MakeInitialRegion()
{
    return SatoriRegion::InitializeAt(this, m_firstRegion, m_end - m_firstRegion, m_initialCommit, m_firstRegion);
}

void SatoriPage::RegionAdded(size_t address)
{
}

void SatoriPage::RegionDestroyed(size_t address)
{
}
