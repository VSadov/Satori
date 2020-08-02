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
    friend class MarkContext;

public:
    void Initialize(SatoriHeap* heap);
    void AddRegion(SatoriRegion* region);

    int GetScanCount();

private:
    SatoriHeap* m_heap;

    // used to ensure each thread is scanned once per scan round.
    int m_scanCount;

    SatoriRegionQueue* m_allRegions;
    SatoriRegionQueue* m_stayingRegions;
    SatoriRegionQueue* m_relocatingRegions;

    SatoriMarkChunkQueue* m_workList;
    SatoriMarkChunkQueue* m_freeList;

    void Collect();
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);
    void PushToMarkQueuesSlow(SatoriMarkChunk*& currentMarkChunk, SatoriObject* o);
    void MarkOwnStack();
    void MarkOtherStacks();
    void IncrementScanCount();
    void DrainMarkQueues();
};

#endif
