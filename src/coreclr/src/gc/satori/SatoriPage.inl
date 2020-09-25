// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriPage.inl
//

#ifndef __SATORI_PAGE_INL__
#define __SATORI_PAGE_INL__

#include "SatoriUtil.h"
#include "SatoriPage.h"

inline size_t SatoriPage::Start()
{
    return (size_t)this;
}

inline size_t SatoriPage::End()
{
    return m_end;
}

inline size_t SatoriPage::RegionsStart()
{
    return m_firstRegion;
}

inline uint8_t* SatoriPage::RegionMap()
{
    return m_regionMap;
}

inline SatoriHeap* SatoriPage::Heap()
{
    return m_heap;
}

inline void SatoriPage::SetCardForAddress(size_t address)
{
    size_t offset = address - Start();
    size_t cardByteOffset = offset / Satori::BYTES_PER_CARD_BYTE;

    _ASSERTE(cardByteOffset / 8 > m_cardTableStart);
    _ASSERTE(cardByteOffset / 8 < m_cardTableSize);

    ((uint8_t*)this)[cardByteOffset] = 0xFF;

    size_t cardGroupOffset = offset / Satori::REGION_SIZE_GRANULARITY;
    this->m_cardGroups[cardGroupOffset] = Satori::CardGroupState::dirty;

    this->m_cardState = 1;
}

#endif
