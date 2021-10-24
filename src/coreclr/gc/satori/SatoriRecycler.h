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
class MarkContext;

class SatoriRecycler
{
    friend class MarkContext;

public:
    void Initialize(SatoriHeap* heap);

    void AddEphemeralRegion(SatoriRegion* region);
    void AddTenuredRegion(SatoriRegion* region);

    void TryStartGC(int generation);
    bool HelpOnce();
    void ConcurrentHelp();
    void ShutDown();
    bool HelpOnceCore();
    void MarkAllStacksAndFinalizationQueueHelper();
    void StartMarkingAllStacksAndFinalizationQueue();
    void MaybeAskForHelp();
    void AskForHelp();
    void MaybeTriggerGC();

    void Collect(int generation, bool force, bool blocking);
    bool IsBlockingPhase();

    int GetRootScanTicket();
    int64_t GetCollectionCount(int gen);
    int CondemnedGeneration();

    size_t Gen1RegionCount();
    size_t Gen2RegionCount();
    size_t RegionCount();

    SatoriRegion* TryGetReusable();

private:
    SatoriHeap* m_heap;

    int m_rootScanTicket;
    uint8_t m_cardScanTicket;

    SatoriMarkChunkQueue* m_workList;

    // regions owned by recycler
    SatoriRegionQueue* m_ephemeralRegions;
    SatoriRegionQueue* m_ephemeralFinalizationTrackingRegions;
    SatoriRegionQueue* m_tenuredRegions;
    SatoriRegionQueue* m_tenuredFinalizationTrackingRegions;

    // temporary store while processing finalizables
    SatoriRegionQueue* m_finalizationScanCompleteRegions;
    SatoriRegionQueue* m_finalizationPendingRegions;

    // temporary store for planning and relocating
    SatoriRegionQueue* m_stayingRegions;
    SatoriRegionQueue* m_relocatingRegions;
    SatoriRegionQueue* m_relocationTargets[Satori::FREELIST_COUNT];
    SatoriRegionQueue* m_relocatedRegions;
    SatoriRegionQueue* m_relocatedToHigherGenRegions;

    // store regions for concurrent sweep
    SatoriRegionQueue* m_deferredSweepRegions;

    // regions that could be reused for Gen1
    SatoriRegionQueue* m_reusableRegions;

    SatoriRegionQueue* m_demotedRegions;

    static const int GC_STATE_NONE = 0;
    static const int GC_STATE_CONCURRENT = 1;
    static const int GC_STATE_BLOCKING = 2;
    static const int GC_STATE_BLOCKED = 3;

    volatile int m_gcState;

    static const int CC_MARK_STATE_NONE = 0;
    static const int CC_MARK_STATE_SUSPENDING_EE = 1;
    static const int CC_MARK_STATE_MARKING = 2;
    static const int CC_MARK_STATE_DONE = 3;

    volatile int m_ccStackMarkState;
    volatile int m_ccMarkingThreadsNum;

    int m_syncBlockCacheScanDone;

    void(SatoriRecycler::* m_activeHelperFn)();

    int m_prevCondemnedGeneration;
    int m_condemnedGeneration;
    bool m_isRelocating;   
    bool m_isPromotingAllRegions;  
    bool m_allowPromotingRelocations;
    bool m_isBarrierConcurrent;

    int64_t m_gen1Count;
    int64_t m_gen2Count;

    size_t m_gen1MinorBudget;
    size_t m_gen1Budget;
    size_t m_gen2Budget;
    size_t m_condemnedRegionsCount;
    size_t m_condemnedNurseryRegionsCount;
    size_t m_deferredSweepCount;
    // this one is signed because reusing regions "borrow" the count so it may go negative.
    ptrdiff_t m_gen1AddedSinceLastCollection;
    size_t m_gen2AddedSinceLastCollection;
    size_t m_gen1CountAtLastGen2;

    static void DeactivateFn(gc_alloc_context* context, void* param);

    template <bool isConservative>
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    template <bool isConservative>
    static void UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    template <bool isConservative>
    static void MarkFnConcurrent(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    static void HelperThreadFn(void* param);

    void PushToEphemeralQueues(SatoriRegion* region);

    void DeactivateAllStacks();
    void PushToMarkQueuesSlow(SatoriMarkChunk*& currentMarkChunk, SatoriObject* o);
    bool MarkOwnStackAndDrainQueues(int64_t deadline = 0);
    void MarkAllStacksFinalizationAndDemotedRoots();
    void IncrementRootScanTicket();
    void IncrementCardScanTicket();
    uint8_t GetCardScanTicket();
    void DrainMarkQueues(SatoriMarkChunk* srcChunk = nullptr);
    bool DrainMarkQueuesConcurrent(SatoriMarkChunk* srcChunk = nullptr, int64_t deadline = 0);
    void MarkThroughCards();
    bool MarkThroughCardsConcurrent(int64_t deadline);
    bool CleanCards();
    bool MarkHandles(int64_t deadline = 0);
    void ShortWeakPtrScan();
    void ShortWeakPtrScanWorker();
    void LongWeakPtrScan();
    void LongWeakPtrScanWorker();
    void ScanFinalizables();
    void ScanFinalizableRegions(SatoriRegionQueue* regions, MarkContext* c);

    void ScanAllFinalizableRegionsWorker();

    void QueueCriticalFinalizablesWorker();

    void DependentHandlesInitialScan();
    void DependentHandlesInitialScanWorker();
    void DependentHandlesRescan();
    void DependentHandlesRescanWorker();
    void PromoteHandlesAndFreeRelocatedRegions();

    void PromoteSurvivedHandlesAndFreeRelocatedRegionsWorker();

    void BlockingCollect();
    void RunWithHelp(void(SatoriRecycler::* method)());
    void BlockingMark();
    void MarkNewReachable();
    void DependentHandlesScan();
    void MarkStrongReferences();
    void MarkStrongReferencesWorker();
    void DrainAndCleanWorker();
    void Plan();
    void PlanWorker();
    void Relocate();
    void RelocateWorker();
    void RelocateRegion(SatoriRegion* region);
    void Update();

    void FreeRelocatedRegionsWorker();

    void UpdateRootsWorker();

    void UpdateRegionsWorker();

    void UpdatePointersInPromotedObjects();

    void UpdateRegions(SatoriRegionQueue* queue);
    void KeepRegion(SatoriRegion* curRegion);
    void DrainDeferredSweepQueue();
    bool DrainDeferredSweepQueueConcurrent(int64_t deadline = 0);
    void DrainDeferredSweepQueueHelp();
    void SweepAndReturnRegion(SatoriRegion* curRegion);
    void UpdatePointersThroughCards();
    void PlanRegions(SatoriRegionQueue* regions);
    void AddRelocationTarget(SatoriRegion* region);

    SatoriRegion* TryGetRelocationTarget(size_t size, bool existingRegionOnly);

    void ASSERT_NO_WORK();
};

#endif
