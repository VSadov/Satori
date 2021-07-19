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

inline uint8_t* SatoriPage::RegionMap()
{
    return m_regionMap;
}

inline SatoriHeap* SatoriPage::Heap()
{
    return m_heap;
}

template<typename F>
    void SatoriPage::ForEachRegion(F& lambda)
    {
        SatoriRegion* region = (SatoriRegion*)this->End();

        do
        {
            region = RegionForAddress(region->Start() - 1);
            lambda(region);
        }
        while (region->Start() != m_firstRegion);
    }

#endif
