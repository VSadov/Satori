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
// SatoriRecycler.h
//

#ifndef __SATORI_RECYCLER_H__
#define __SATORI_RECYCLER_H__

#include "common.h"
#include "../gc.h"
#include "SatoriRegionQueue.h"
#include "SatoriWorkList.h"
#include "SatoriGate.h"

class SatoriHeap;
class SatoriTrimmer;
class SatoriRegion;
class MarkContext;
struct SatoriIncrGcSnapshot;

struct LastRecordedGcInfo
{
    size_t m_index;
    size_t m_pauseDurations[2];
    uint32_t m_pausePercentage;
    uint8_t m_condemnedGeneration;
    bool m_compaction;
    bool m_concurrent;
};

class SatoriRecycler
{
    friend class MarkContext;

public:
    // Toggling the barrier to concurrent involves a process-wide fence, which is expensive.
    // Only the thread that claims BARRIER_STATE_SWITCHING does it, the rest just leave and
    // come back later, since they may not mark until the barrier is concurrent.
    //
    // NB: except for SWITCHING, which is internal to the GC, the values match what is
    //     published to the barriers in the g_write_watch_table slot.
    //     (see: ToggleWriteBarrier)
    static const int BARRIER_STATE_NOT_CONCURRENT = 0;
    static const int BARRIER_STATE_CONCURRENT = 1;
    // Not concurrent and the next GC is a full GC, thus cards are not needed.
    static const int BARRIER_STATE_SKIPPING_CARDS = 2;
    // Transient, while a thread is toggling the barrier. Has no counterpart in the
    // published state - the barrier is still in the previous state until the toggle is done.
    static const int BARRIER_STATE_SWITCHING = 3;

    void Initialize(SatoriHeap* heap);

    void AddEphemeralRegion(SatoriRegion* region);
    void AddTenuredRegion(SatoriRegion* region);

    size_t GetNowMillis();
    size_t GetNowUsecs();

    bool& IsLowLatencyMode();

    void Collect(int generation, bool force, bool blocking);
    int GetCondemnedGeneration();
    int GetRootScanTicket();
    size_t IncrementGen0Count();
    int64_t GetCollectionCount(int gen);

    void TryStartGC(int generation, gc_reason reason);
    void HelpOnce();
    void MaybeTriggerGC(gc_reason reason);
    bool IsBlockingPhase();

    bool ShouldDoConcurrent(int generation);
    void ConcurrentWorkerFn();
    void ShutDown();

    void BlockingMarkForConcurrentImpl();
    void BlockingMarkForConcurrent();
    void MaybeAskForHelp();

    SatoriRegion* TryGetReusable();
    SatoriRegion* TryGetReusableForLarge();

    void ReportThreadAllocBytes(int64_t bytes, bool isLive);
    int64_t GetTotalAllocatedBytes();

    void RecordOccupancy(int generation, size_t size);
    void RecordDemotedOccupancy(ptrdiff_t demotedOccupancy);
    void UpdateGenerationOccupancies();
    size_t GetTotalOccupancy();
    size_t GetOccupancy(int i);
    size_t GetGcStartMillis(int generation);
    size_t GetGcDurationMillis(int generation);
    size_t GetGcAccumulatingDurationMillis(int generation);

    int64_t GlobalGcIndex();

    SatoriWorkChunk* TakeObjectRange(SatoriWorkChunk* chunk, SatoriObject*& o, size_t& start, size_t& end, size_t scanSize);
    void ScheduleMarkAsChildRanges(SatoriObject* o);
    bool ScheduleUpdateAsChildRanges(SatoriObject* o);

    inline bool IsBarrierConcurrent()
    {
        // NB: while switching the barrier is not concurrent yet.
        return m_barrierState == BARRIER_STATE_CONCURRENT;
    }

    // Tells if the barrier needs to deal with cards.
    // Cards are not needed when the next GC is a full GC - it will not use the remembered set.
    // However, concurrent marking needs cards regardless, so that the writes that happen
    // while marking are not missed.
    // NB: both conditions are folded into the barrier state, which is updated atomically,
    //     so that they could not be observed in an inconsistent combination.
    inline bool CardsAreNeeded()
    {
        return m_barrierState != BARRIER_STATE_SKIPPING_CARDS;
    }

    inline bool IsNextGcFullGc()
    {
        return m_nextGcIsFullGc;
    }

    inline int GetPercentTimeInGcSinceLastGc()
    {
        return m_percentTimeInGcSinceLastGc;
    }

    LastRecordedGcInfo* GetLastGcInfo(gc_kind kind)
    {
        if (kind == gc_kind_ephemeral)
            return &m_lastEphemeralGcInfo;

        if (kind == gc_kind_full_blocking)
            return &m_lastTenuredGcInfo; // no concept of background GC, every GC has blocking part.

        if (kind == gc_kind_background)
            return GetLastGcInfo(gc_kind_any); // no concept of background GC, cant have 2 GCs at a time.

        // if (kind == gc_kind_any)
        return m_lastTenuredGcInfo.m_index > m_lastEphemeralGcInfo.m_index ?
            &m_lastTenuredGcInfo :
            &m_lastEphemeralGcInfo;
    };

    // Two scenarios when worker threads should rather suspend thancontinue helping/pacing
    bool AppThreadsShouldSuspend()
    {
        return m_gcState == GC_STATE_BLOCKING ||
            m_ccStackMarkState == CC_MARK_STATE_SUSPENDING_EE;
    }

private:
    SatoriHeap* m_heap;

    int m_rootScanTicket;
    uint8_t m_cardScanTicket;

    SatoriWorkList* m_workList;
    SatoriTrimmer* m_trimmer;

    // regions owned by recycler
    SatoriRegionQueue* m_ephemeralRegions;
    SatoriRegionQueue* m_ephemeralFinalizationTrackingRegions;
    SatoriRegionQueue* m_ephemeralWithUnmarkedDemoted;

    SatoriRegionQueue* m_tenuredRegions;
    SatoriRegionQueue* m_tenuredFinalizationTrackingRegions;

    // temporary store while processing finalizables
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
    SatoriRegionQueue* m_reusableRegionsAlternate;

    static const int GC_STATE_NONE = 0;
    static const int GC_STATE_CONCURRENT = 1;
    static const int GC_STATE_BLOCKING = 2;
    static const int GC_STATE_BLOCKED = 3;

    volatile int m_gcState;

    static const int CC_MARK_STATE_NONE = 0;
    static const int CC_MARK_STATE_SUSPENDING_EE = 1;
    static const int CC_MARK_STATE_MARKING = 2;
    static const int CC_MARK_STATE_DONE = 3;

    static const int CC_CLEAN_STATE_NOT_READY = 0;
    static const int CC_CLEAN_STATE_WAIT_FOR_HELPERS = 1;
    static const int CC_CLEAN_STATE_SETTING_UP = 2;
    static const int CC_CLEAN_STATE_CLEANING = 3;
    static const int CC_CLEAN_STATE_DONE = 4;

    volatile int m_ccStackMarkState;

    int m_syncBlockCacheScanDone;

    int m_condemnedGeneration;

    bool m_concurrentCardsDone;
    bool m_concurrentHandlesDone;
    volatile int m_concurrentCleaningState;

    bool m_isRelocating;
    bool m_isLowLatencyMode;
    bool m_promoteAllRegions;
    volatile int m_barrierState;

    int m_prevCondemnedGeneration;

    int64_t m_gcCount[3];
    int64_t m_compactingGcCount[3];

    int64_t m_gcStartMillis[3];
    int64_t m_gcDurationUsecs[3];
    int64_t m_gcAccmulatingDurationUsecs[3];
    // when the current blocking GC finished its work. The reported pause ends there (see BlockingCollectImpl).
    int64_t m_blockingWorkEndTicks;

    int64_t m_totalTimeAtLastGcEnd;
    int m_percentTimeInGcSinceLastGc;

    size_t m_gen1Budget;
    size_t m_totalLimit;

    // percent of the gen1 budget that may be parked in reusable regions, see m_reusableLimit.
    int m_reusableTargetPercent;
    bool m_nextGcIsFullGc;

    size_t m_condemnedRegionsCount;
    size_t m_gen1CountAtLastGen2;
    size_t m_gcNextTimeTarget;

    size_t m_occupancy[3];
    bool m_occupancyReportingEnabled;

    // The counters below are updated with interlocked operations, about once per region.
    // Allocating threads and sweepers update different ones, so each group gets its own cache line
    // and none shares a line with the state above, which is read much more often.

    // updated by allocating threads when they hand over a region
    DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY)
    size_t m_gen1AddedSinceLastCollection;
    size_t m_gen2AddedSinceLastCollection;

    // updated by both allocating threads and sweepers
    DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY)
    size_t m_estimatedEphemeralReclaim;
    size_t m_estimatedTenuredReclaim;
    size_t m_promotionEstimate;

    // updated by sweepers
    DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY)
    size_t m_deferredSweepCount;
    size_t m_occupancyAcc[3];
    size_t m_demotedOccupancyAcc;

    // how much more free space may be parked in reusable regions. set from the gen1 budget
    // once it is known, and spent as regions are parked. 0 stops parking any more.
    int64_t m_reusableLimit;

    DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY)
    int64_t m_currentAllocBytesLiveThreads;
    int64_t m_currentAllocBytesDeadThreads;
    int64_t m_totalAllocBytes;

    int64_t m_osTicksPerMilli;
    int64_t m_osTicksPerMicro;
    int64_t m_timeStampTicksPerMilli;

    SatoriGate* m_workerGate;

    // Updated by helper threads on every help quantum. They have their own cache line, so these
    // updates do not invalidate the GC state that markers, helpers and mutators keep reading.
    DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY)
    volatile int m_ccHelpersNum;
    volatile int m_ccStackMarkingThreadsNum;

    // threads filtering m_reusableRegions concurrently. prep waits for these to leave
    // before it may swap the queues.
    volatile int m_reusableFilterThreadsNum;

    volatile int m_gateSignaled;
    volatile int m_workerWoken;
    volatile int m_activeWorkers;
    volatile int m_totalWorkers;

    void(SatoriRecycler::* volatile m_activeWorkerFn)();

    int64_t m_noWorkSince;

    DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY)
    LastRecordedGcInfo m_lastEphemeralGcInfo;
    LastRecordedGcInfo m_lastTenuredGcInfo;
    LastRecordedGcInfo* m_CurrentGcInfo;

    size_t m_startMillis;

    // ---- incremental relocation ----
    //
    // It is used in low latency mode, or when regular relocation is disabled, where it is the only relocation.
    // Otherwise it is preferred to regular relocation, which is used when incremental does not keep up,
    // that is when too much reclaimable space remains in sparse Gen2 regions.
    //
    // Before the concurrent marking of a gen2 GC starts, a few sparse regions are selected as relocation
    // sources (candidates), and a few regions with large free spans as targets.
    // While marking, the locations of all references to candidates are recorded.
    // If the candidates are still relocatable when marking is done, the blocking phase relocates only these
    // and updates only roots, recorded locations and the copies. Everything else stays and is swept later,
    // as if the GC did not relocate. Thus the cost is proportional to what we relocate, not to the heap size.
    // candidates are identified by indices 1..INCR_MAX_REGIONS, which must fit in a byte
    static const int INCR_MAX_REGIONS = 255;

    struct IncrCopyRange
    {
        SatoriRegion* m_region;
        size_t m_start;
        size_t m_end;
    };

    class RefRecorder;

    // true from selection until planning. The marker records references to candidates while it is true.
    volatile bool m_incrRecording;
    bool m_isIncrementalRelocation;
    int m_incrSourceCount;
    int m_incrTargetCount;
    SatoriRegion* m_incrSources[INCR_MAX_REGIONS];
    SatoriRegion* m_incrTargets[INCR_MAX_REGIONS];
    SatoriWorkList* m_recordedRefs;
    volatile int64_t m_incrRecordedRefs;
    // which candidates (by index) are relocated, the recorded locations of the others are skipped
    bool m_incrKeep[INCR_MAX_REGIONS + 1];
    // how many references to each candidate were recorded (with duplicates)
    volatile int64_t m_incrSourceRefs[INCR_MAX_REGIONS];
    // why recording was given up, 0 if not (see INCR_ABANDON_*)
    volatile int m_incrAbandonReason;
    size_t m_incrMaxRecordedRefs;
    // Recorders publish what they have when they are done, and that is often just a few references.
    // Such leftovers are merged into this chunk, so that we do not use a chunk for a few references.
    SatoriLock m_incrSpillLock;
    SatoriWorkChunk* m_incrSpill;
    // the last chunk in the chain of recorded chunks
    SatoriWorkChunk* m_incrRecordedLast;
    IncrCopyRange m_incrCopyRanges[INCR_MAX_REGIONS];
    volatile int m_incrCopyRangeCount;
    volatile int m_incrCopyRangeClaim;

    // Selection was done for this GC, even if nothing was selected. Planning then decides how to relocate.
    bool m_incrSelectionDone;
    // what the candidates were when selected
    size_t m_incrSourceObjs[INCR_MAX_REGIONS];
    size_t m_incrSourceBytes[INCR_MAX_REGIONS];
    size_t m_incrSelectedObjs;
    // Sparse Gen2 regions are the backlog. This is what they could free if relocated,
    // and the Gen2 space (without large regions) that they are a part of.
    size_t m_incrEligibleGain;
    size_t m_incrGen2Space;
    // the budget of the current GC in cost units, and what the kept candidates are estimated to cost.
    size_t m_incrUnitsBudget;
    size_t m_incrPlannedUnits;

    // The cost model, calibrated as we go: pause = fixed + perUnit * units.
    // The fixed part is mostly updating roots. Units are estimated from objects, bytes and references.
    double m_incrFixedUs;
    double m_incrUsPerUnit;
    // references to candidates that are recorded, per object in candidates
    double m_incrRefsPerObj;
    int m_incrFixedSamples;
    int m_incrUnitSamples;
    int m_incrRefsSamples;
    // when the fixed part alone would not fit the budget, we relocate only occasionally, to see if that changed.
    int m_incrNoRoomCount;
    // measured parts of the current incremental relocation
    int64_t m_incrMeasuredTicks;
    size_t m_incrRootsStartTicks;
    volatile size_t m_incrRootsDoneTicks;

private:
    size_t Gen1RegionCount();
    size_t Gen2RegionCount();
    size_t RegionCount();

    static void DeactivateFn(gc_alloc_context* context, void* param);
    static void ConcurrentPhasePrepFn(gc_alloc_context* gcContext, void* param);

    template <bool isConservative>
    static void MarkFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    template <bool isConservative>
    static void UpdateFn(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    template <bool isConservative>
    static void MarkFnConcurrent(PTR_PTR_Object ppObject, ScanContext* sc, uint32_t flags);

    static void WorkerThreadMainLoop(void* param);
    int MaxWorkers();
    int64_t HelpQuantumTimeStampTicks();
    int64_t HelpQuantumOsTicks();
    void AskForHelp();
    void RunWithHelp(void(SatoriRecycler::* method)());
    bool HelpOnceCore(bool minQuantum);
    bool HelpOnceCoreInner(bool minQuantum);

    void PushToEphemeralQueues(SatoriRegion* region);
    void PushToTenuredQueues(SatoriRegion* region);

    void AdjustHeuristics();
    void DeactivateAllocatingRegions();

    void IncrementRootScanTicket();
    void IncrementCardScanTicket();
    uint8_t GetCardScanTicket();

    void MarkOwnStack(gc_alloc_context* aContext, MarkContext* markContext);
    void MarkThroughCards();
    bool MarkThroughCardsConcurrent(int64_t deadline);
    void MarkDemoted(SatoriRegion* curRegion, MarkContext* markContext);
    void MarkAllStacksFinalizationAndDemotedRoots();

    void PushToMarkQueuesSlow(SatoriWorkChunk*& currentWorkChunk, SatoriObject* o);
    void DrainMarkQueues(SatoriWorkChunk* srcChunk = nullptr);
    void MarkOwnStackAndDrainQueues();
    void MarkOwnStackOrDrainQueuesConcurrent(int64_t deadline);
    bool MarkDemotedAndDrainQueuesConcurrent(int64_t deadline);
    bool MarkDemotedInReusableConcurrent(int64_t deadline);
    void PushOrReturnWorkChunk(SatoriWorkChunk * srcChunk);
    bool DrainMarkQueuesConcurrent(SatoriWorkChunk* srcChunk = nullptr, int64_t deadline = 0);

    bool HasDirtyCards();
    bool CleanCardsConcurrent(int64_t deadline);
    void CleanCards();
    bool MarkHandles(int64_t deadline = 0);
    void ShortWeakPtrScan();
    void ShortWeakPtrScanWorker();
    void LongWeakPtrScan();
    void LongWeakPtrScanWorker();

    void ScanFinalizables();
    void ScanFinalizableRegions(SatoriRegionQueue* regions, MarkContext* markContext, SatoriRegionQueue::Batch* pending, SatoriRegionQueue::Batch* eph, SatoriRegionQueue::Batch* ten);
    void ScanAllFinalizableRegionsWorker();
    void QueueCriticalFinalizablesWorker();

    void DependentHandlesScan();
    void DependentHandlesInitialScan();
    void DependentHandlesInitialScanWorker();
    void DependentHandlesRescan();
    void DependentHandlesRescanWorker();

    void BlockingCollect();
    // for profiling purposes Gen1 and Gen2 GC have distinct entrypoints, but the same implementation
    void BlockingCollect1();
    void BlockingCollect2();
    void BlockingCollectImpl();

    void BlockingMark();
    void MarkNewReachable();
    void DrainAndCleanWorker();
    void MarkStrongReferences();
    void MarkStrongReferencesWorker();
#ifdef FEATURE_JAVAMARSHAL
    void MarkBridgeObjects();
#endif

    void Plan();
    void PlanWorker();
    void PlanRegions(SatoriRegionQueue* regions);
    void DenyRelocation();

    void AddTenuredRegionsToPlan(SatoriRegionQueue* regions);
    void AddRelocationTarget(SatoriRegion* region);
    SatoriRegion* TryGetRelocationTarget(size_t size, bool existingRegionOnly);
    SatoriRegion* GetOrAddRelocationTarget(SatoriRegion * region, size_t allocSize);

    void Relocate();
    void RelocateWorker();
    void RelocateRegion(SatoriRegion* region);
    void FreeLogicallyEmptyRegion(SatoriRegion* curRegion, bool hasMarks, bool noLock);
    void FreeRelocatedRegionsWorker();

    void PromoteHandlesAndFreeRelocatedRegions();
    void PromoteSurvivedHandlesAndFreeRelocatedRegionsWorker();

    void Update();
    void UpdateRootsWorker();
    void UpdateRegionsWorker();
    void UpdatePointersThroughCards();
    void UpdatePointersInObjectRanges();
    void UpdatePointersInPromotedObjects();
    void UpdateRegions(SatoriRegionQueue* queue, SatoriRegionQueue::Batch* deferredFirst, SatoriRegionQueue::Batch* deferredRest);

    void KeepRegion(SatoriRegion* curRegion);
    bool ShouldReuse(SatoriRegion* curRegion);
    void DrainDeferredSweepQueue();
    void DrainReusableQueue();
    bool DrainDeferredSweepQueueConcurrent(int64_t deadline = 0);
    void DrainDeferredSweepQueueWorkerFn();
    void SweepAndReturnRegion(SatoriRegion* curRegion);

    void UpdateGcCounters(int64_t blockingStart);

    void SelectIncrementalRelocationCandidates();
    bool PlanIncrementalRelocation(bool canRelocateRegularly);
    void CalibrateIncrementalRelocation();
    void PublishRecordedRefs(SatoriWorkChunk*& chunk);
    void PushRecordedChunk(SatoriWorkChunk* chunk);
    bool RecordRelocationRefSlow(SatoriWorkChunk*& chunk, SatoriObject* entry);
    void AddRecordedRefCounts(const int32_t* counts);
    void AbandonIncrementalRelocation();
    void FreeRecordedRefs();
    void UpdateRecordedRefsWorker();
    void VerifyIncrementalRelocation();
    void IncrStatsOnBlockingGcStart();
    void IncrStatsOnBlockingGcEnd(int generation, int64_t pauseTicks, SatoriIncrGcSnapshot& snapshot);
    void IncrStatsWriteLog(const SatoriIncrGcSnapshot& snapshot);

    void ASSERT_NO_WORK();
};

#endif
