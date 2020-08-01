// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
// SatoriRecycler.h
//

#ifndef __SATORI_RECYCLER_H__
#define __SATORI_RECYCLER_H__

#include "common.h"
#include "../gc.h"
#include "SatoriRegionQueue.h"
#include "SatoriMarkChunkQueue.h"

class SatoriHeap;
class SatoriRegion;

class SatoriRecycler
{
public:
    void Initialize(SatoriHeap* heap);
    void AddRegion(SatoriRegion* region);

private:
    SatoriHeap* m_heap;

    SatoriRegionQueue* m_regions;
    SatoriMarkChunkQueue* m_work_list;
    SatoriMarkChunkQueue* m_free_list;
};

#endif
