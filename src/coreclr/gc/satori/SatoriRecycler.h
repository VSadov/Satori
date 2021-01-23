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
    void Help();
    void TryStartGC(int generation);
    void HelpOnce();
    void MaybeTriggerGC();

    void Collect(int generation, bool force);
    void BlockingCollect();

    int GetStackScanCount();
    int64_t GetCollectionCount(int gen);
    int CondemnedGeneration();

    int Gen1RegionCount();
    int Gen2RegionCount();

private:
    SatoriHeap* m_heap;

    int m_stackScanCount;
    uint8_t m_cardScanTicket;

    int m_condemnedGeneration;
    bool m_isCompacting;
    bool m_isConcurrent;
    int m_gcInProgress;

    SatoriMarkChunkQueue* m_workList;

    // temporary store for Gen0 regions
    SatoriRegionQueue* m_nurseryRegions;
    SatoriRegionQueue* m_ephemeralRegions;
    SatoriRegionQueue* m_ephemeralFinalizationTrackingRegions;

    SatoriRegionQueue* m_tenuredRegions;
    SatoriRegionQueue* m_tenuredFinalizationTrackingRegions;

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
    void MarkOwnStack();
    void MarkOtherStacks();
    void MarkFinalizationQueue();
    void IncrementStackScanCount();
    void IncrementCardScanTicket();
    uint8_t GetCardScanTicket();
    void DrainMarkQueues(SatoriMarkChunk* srcChunk = nullptr);
    void DrainMarkQueuesConcurrent(SatoriMarkChunk* srcChunk = nullptr);
    bool MarkThroughCards(bool isConcurrent);
    bool CleanCards();
    void MarkHandles();
    void WeakPtrScan(bool isShort);
    void WeakPtrScanBySingleThread();
    void ScanFinalizables();
    void ScanFinalizableRegions(SatoriRegionQueue* regions, MarkContext* c);

    void DependentHandlesInitialScan();
    void DependentHandlesRescan();
    void PromoteSurvivedHandles();
    void SweepNurseryRegions();

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
