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

template <typename F>
    void SatoriPage::ForEachRegion(F lambda)
    {
        SatoriRegion* region = (SatoriRegion*)this->End();

        do
        {
            region = RegionForAddressChecked(region->Start() - 1);
            if (!region)
            {
                // page is not done formatting the region map.
                break;
            }

            lambda(region);
        }
        while (region->Start() != m_firstRegion);
    }

#endif
