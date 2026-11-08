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
// SatoriPage.inl
//

#ifndef __SATORI_PAGE_INL__
#define __SATORI_PAGE_INL__

#include "SatoriUtil.h"
#include "SatoriPage.h"
#include "SatoriRegion.h"

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

inline size_t SatoriPage::GranuleCount()
{
    return (End() - Start()) >> Satori::REGION_BITS;
}

inline size_t SatoriPage::FirstGranule()
{
    return (m_firstRegion - Start()) >> Satori::REGION_BITS;
}

inline size_t SatoriPage::GranuleStart(size_t granule)
{
    return Start() + (granule << Satori::REGION_BITS);
}

inline uint8_t* SatoriPage::RegionMap()
{
    return m_regionMap;
}

inline SatoriHeap* SatoriPage::Heap()
{
    return m_heap;
}

// order is unimportant, but we want to read it only once when we read it, thus volatile.
inline volatile int8_t& SatoriPage::CardState()
{
    return m_cardState;
}

// order is unimportant, but we want to read it only once when we read it, thus volatile.
inline volatile uint8_t& SatoriPage::ScanTicket()
{
    return m_scanTicket;
}

// order is unimportant, but we want to read it once when we read it, thus volatile.
inline volatile int8_t& SatoriPage::CardGroupState(size_t i)
{
    return m_cardGroups[i * 2];
}

// order is unimportant, but we want to read it once when we read it, thus volatile.
inline volatile uint8_t& SatoriPage::CardGroupScanTicket(size_t i)
{
    return (uint8_t&)m_cardGroups[i * 2 + 1];
}

inline size_t SatoriPage::CardGroupCount()
{
    return (End() - Start()) / Satori::BYTES_PER_CARD_GROUP;
}

inline int8_t* SatoriPage::CardsForGroup(size_t i)
{
    return &m_cardTable[i * Satori::CARD_BYTES_IN_CARD_GROUP];
}

inline size_t SatoriPage::LocationForCard(int8_t* cardPtr)
{
    return Start() + ((size_t)cardPtr - Start()) * Satori::BYTES_PER_CARD_BYTE;
}

// Visits the regions of the page from the end of the page down. Free granules are skipped.
template <typename F>
    void SatoriPage::ForEachRegion(F lambda)
    {
        size_t first = FirstGranule();
        size_t granule = GranuleCount();

        while (granule > first)
        {
            granule--;
            if (VolatileLoad(&RegionMap()[granule]) == 0)
            {
                // a free granule.
                continue;
            }

            SatoriRegion* region = RegionForAddressChecked(GranuleStart(granule));
            if (!region)
            {
                // the region is gone.
                continue;
            }

            // continue below the region. The lambda may change it.
            granule = (region->Start() - Start()) >> Satori::REGION_BITS;
            lambda(region);
        }
    }

#endif
