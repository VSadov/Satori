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
    void AddEphemeralRegion(SatoriRegion* region);
    void TryStartGC(int generation);
    void HelpOnce();
    void MaybeTriggerGC();

    void Collect(int generation, bool force, bool blocking);

    int GetStackScanCount();
    int64_t GetCollectionCount(int gen);
    int CondemnedGeneration();

    int Gen1RegionCount();
    int Gen2RegionCount();

private:
    static const int GC_STATE_NONE = 0;
    static const int GC_STATE_CONCURRENT = 1;
    static const int GC_STATE_BLOCKING = 2;

    SatoriHeap* m_heap;

    int m_stackScanCount;
    uint8_t m_cardScanTicket;

    int m_condemnedGeneration;
    bool m_isCompacting;
    bool m_isConcurrent;
    int m_gcState;

    SatoriMarkChunkQueue* m_workList;

    // regions owned by recycler
    SatoriRegionQueue* m_ephemeralRegions;
    SatoriRegionQueue* m_ephemeralFinalizationTrackingRegions;
    SatoriRegionQueue* m_tenuredRegions;
    SatoriRegionQueue* m_tenuredFinalizationTrackingRegions;

    // temporary store for Gen0 regions
    SatoriRegionQueue* m_nurseryRegions;

    // temporary store while processing finalizables
    SatoriRegionQueue* m_finalizationScanCompleteRegions;
    SatoriRegionQueue* m_finalizationPendingRegions;

    // temporary store while planning
    SatoriRegionQueue* m_stayingRegions;
    SatoriRegionQueue* m_relocatingRegions;
    SatoriRegionQueue* m_relocationTargets[Satori::FREELIST_COUNT];
    SatoriRegionQueue* m_relocatedRegions;

    int64_t m_gen1Count;
    int64_t m_gen2Count;

    int m_gen1Threshold;
    int m_gen1Budget;
    int m_condemnedRegionsCount;

    static void DeactivateFn(gc_alloc_context* context, void* param);
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);
    static void MarkFnConcurrent(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);
    static void UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    void DeactivateAllStacks();
    void PushToMarkQueuesSlow(SatoriMarkChunk*& currentMarkChunk, SatoriObject* o);
    bool MarkOwnStackAndDrainQueues(int64_t deadline = 0);
    void MarkOtherStacks();
    void MarkFinalizationQueue();
    void IncrementStackScanCount();
    void IncrementCardScanTicket();
    uint8_t GetCardScanTicket();
    void DrainMarkQueues(SatoriMarkChunk* srcChunk = nullptr);
    bool DrainMarkQueuesConcurrent(SatoriMarkChunk* srcChunk = nullptr, int64_t deadline = 0);
    bool MarkThroughCards(bool isConcurrent, int64_t deadline = 0);
    bool CleanCards();
    bool MarkHandles(int64_t deadline = 0);
    void WeakPtrScan(bool isShort);
    void WeakPtrScanBySingleThread();
    void ScanFinalizables();
    void ScanFinalizableRegions(SatoriRegionQueue* regions, MarkContext* c);

    void DependentHandlesInitialScan();
    void DependentHandlesRescan();
    void PromoteSurvivedHandles();
    void SweepNurseryRegions();

    bool HelpImpl();

    void BlockingCollect();
    void Mark();
    void Sweep();
    void Compact();
    void RelocateRegion(SatoriRegion* region);
    void Finish();

    void UpdateFinalizationQueue();

    void FinishRegions(SatoriRegionQueue* queue);
    void UpdatePointersThroughCards();
    void SweepRegions(SatoriRegionQueue* regions);
    void AddRelocationTarget(SatoriRegion* region);

    SatoriRegion* TryGetRelocationTarget(size_t size, bool existingRegionOnly);

    int RegionCount();
    void AssertNoWork();
};

#endif
