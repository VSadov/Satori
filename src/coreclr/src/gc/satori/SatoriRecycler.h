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

    void MaybeTriggerGC();

    int GetScanCount();

    void Collect(bool force);

private:
    SatoriHeap* m_heap;

    // this is to ensure that only one thread suspends VM and to block all others on it.
    SatoriLock m_suspensionLock;

    // used to ensure each thread is scanned once per scan round.
    int m_scanCount;

    // region count at the end of last GC, used in a crude GC triggering heuristic.
    int m_prevRegionCount;
    int m_gcInProgress;

    SatoriRegionQueue* m_regularRegions;
    SatoriRegionQueue* m_finalizationTrackingRegions;

    // temporary store while processing finalizables
    SatoriRegionQueue* m_finalizationScanCompleteRegions;
    SatoriRegionQueue* m_finalizationPendingRegions;

    // temporary store while planning
    SatoriRegionQueue* m_stayingRegions;
    SatoriRegionQueue* m_relocatingRegions;

    SatoriMarkChunkQueue* m_workList;

    static void DeactivateFn(gc_alloc_context* context, void* param);
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    void DeactivateAllStacks();
    void PushToMarkQueuesSlow(SatoriMarkChunk*& currentMarkChunk, SatoriObject* o);
    void MarkOwnStack();
    void MarkOtherStacks();
    void IncrementScanCount();
    void DrainMarkQueues();
    void MarkHandles();
    void WeakPtrScan(bool isShort);
    void WeakPtrScanBySingleThread();
    void ScanFinalizables();

    void DependentHandlesInitialScan();
    void DependentHandlesRescan();
};

#endif
