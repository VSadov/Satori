// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriGCHeap.h
//

#ifndef __SATORI_PAGE_H__
#define __SATORI_PAGE_H__

#include "common.h"
#include "../gc.h"

class SatoriRegion;

class SatoriPage
{
public:
    SatoriPage() = delete;
    ~SatoriPage() = delete;

    static SatoriPage* InitializeAt(size_t address, size_t pageSize);
    SatoriRegion* MakeInitialRegion();

    void RegionAdded(size_t address);
    void RegionDestroyed(size_t address);

    size_t End()
    {
        return m_end;
    }

    size_t RegionsStart()
    {
        return m_firstRegion;
    }

private:
    size_t m_end;
    size_t m_initialCommit;
    size_t m_firstRegion;
};

#endif
