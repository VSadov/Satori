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

// A candidate for incremental relocation, with what selection needs to know about it.
struct SatoriIncrEntry
{
    SatoriRegion* m_region;
    size_t m_key;
    uint32_t m_objCount;
    uint32_t m_occupancy;
};

// What selection of incremental relocation candidates takes from a Gen2 region.
struct SatoriIncrRegionInfo
{
    size_t m_occupancy;
    // not large, counts toward Gen2 space
    bool m_inSpace;
    bool m_isSource;
    bool m_isTarget;
    int m_bucket;
    size_t m_gain;
    SatoriIncrEntry m_source;
};

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
    //
    // While EE is suspended to mark stacks for the concurrent marking and in the blocking collection,
    // the EE may also switch the barrier code to the code specialized for the state.
    // Without concurrent GC the state changes only then, so the barriers between GCs do not check it either.
    static const int BARRIER_STATE_NOT_CONCURRENT = 0;
    static const int BARRIER_STATE_CONCURRENT = 1;
    // Not concurrent and the next GC is a full GC, thus cards are not needed.
    static const int BARRIER_STATE_SKIPPING_CARDS = 2;
    // Transient, while a thread is toggling the barrier. The published barrier may already
    // be concurrent before the toggle's process-wide fence completes.
    static const int BARRIER_STATE_SWITCHING = 3;

#if _DEBUG
    // a conservative marker runs only while the trimmer cannot free regions.
    void AssertTrimmerInactiveForMarker();
#endif

    void Initialize(SatoriHeap* heap);

    void AddEphemeralRegion(SatoriRegion* region);
    void AddTenuredRegion(SatoriRegion* region);

    size_t GetNowMillis();
    size_t GetNowUsecs();

    bool& IsLowLatencyMode();

    void Collect(int generation, bool force, bool blocking, bool compact = false, bool aggressive = false);
    int GetCondemnedGeneration();
    int GetRootScanTicket();
    size_t IncrementGen0Count();
    int64_t GetCollectionCount(int gen);

    void TryStartGC(int generation, gc_reason reason);
    bool HelpOnce(bool waitForGC = false);
    void MaybeTriggerGC(gc_reason reason);
    bool IsBlockingPhase();
    void WaitForTrimmerToStop();

    bool ShouldDoConcurrent(int generation);
    void ConcurrentWorkerFn();
    void ShutDown();

    void BlockingMarkForConcurrentImpl();
    void BlockingMarkForConcurrent();
    void MaybeAskForHelp();

    SatoriRegion* TryGetReusable(size_t minFree);
    SatoriRegion* TryGetReusableForLarge(size_t minFree);
    // Whether `queue` is a queue of reusable regions. Compares addresses only, so it can be given any pointer. (see TryTakeReusableNext)
    bool IsReusableQueue(SatoriQueue<SatoriRegion>* queue);

    // Whether a thread is in a scan of stacks that looks candidate pointers up in the region map. Checked builds only.
    // No region may be freed into the free granules meanwhile. (see SatoriAllocator::FreeToBitmap)
#if _DEBUG
    bool IsConservativeScanInProgress()
    {
        return m_conservativeScans != 0;
    }
#endif

    void RecordOccupancy(int generation, size_t size);
    void RecordDemotedOccupancy(ptrdiff_t demotedOccupancy);
    void UpdateGenerationOccupancies();
    size_t GetTotalOccupancy();
    // How much free committed memory the trimmer keeps: what the coming GC cycles are expected to take, see SatoriTrimmer::Loop,
    // and at least heldFree, what brings the heap back to a peak that it had in use recently.
    // underPressure is set when the memory is too low for the expected cycles, and the budget is then what is available.
    size_t GetTrimBudget(size_t heldFree, bool& underPressure);
    // The most memory that the heap had in use at the GCs since the last call. (see NoteInUse)
    size_t TakeInUsePeak();
    size_t GetOccupancy(int i);
    size_t GetGcStartMillis(int generation);
    size_t GetGcDurationMillis(int generation);
    size_t GetGcAccumulatingDurationMillis(int generation);

    int64_t GlobalGcIndex();

    SatoriWorkChunk* TakeObjectRange(SatoriWorkChunk* chunk, SatoriObject*& o, size_t& start, size_t& end, size_t scanSize);
    void ScheduleMarkAsChildRanges(SatoriObject* o);
    bool ScheduleUpdateAsChildRanges(SatoriObject* o);

    inline bool IsBarrierConcurrent(bool includeSwitching = false)
    {
        // Markers must wait for the toggle; writers must conservatively dirty cards during it.
        int barrierState = m_barrierState;
        return barrierState == BARRIER_STATE_CONCURRENT ||
            (includeSwitching && barrierState == BARRIER_STATE_SWITCHING);
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

    SatoriShardedWorkList* m_workList;
    SatoriTrimmer* m_trimmer;

    // regions owned by recycler
    SatoriShardedRegionQueue* m_ephemeralRegions;
    SatoriShardedRegionQueue* m_ephemeralFinalizationTrackingRegions;
    SatoriShardedRegionQueue* m_ephemeralWithUnmarkedDemoted;

    SatoriShardedRegionQueue* m_tenuredRegions;
    SatoriShardedRegionQueue* m_tenuredFinalizationTrackingRegions;

    // temporary store while processing finalizables
    SatoriShardedRegionQueue* m_finalizationPendingRegions;

    // temporary store for planning and relocating
    SatoriShardedRegionQueue* m_stayingRegions;
    SatoriShardedRegionQueue* m_relocatingRegions;
    SatoriShardedRegionQueue* m_relocationTargets[Satori::FREELIST_COUNT];
    SatoriShardedRegionQueue* m_relocatedRegions;
    SatoriRegionQueue* m_relocatedToHigherGenRegions;

    // store regions for concurrent sweep
    SatoriShardedRegionQueue* m_deferredSweepRegions;

    // regions that could be reused for Gen1
    SatoriShardedRegionQueue* m_reusableRegions;
    SatoriShardedRegionQueue* m_reusableRegionsAlternate;

    static const int GC_STATE_NONE = 0;
    static const int GC_STATE_CONCURRENT = 1;
    static const int GC_STATE_BLOCKING = 2;
    static const int GC_STATE_BLOCKED = 3;

    volatile int m_gcState;

    // Requests name the earliest GC-start sequence that may satisfy them.
    struct CollectionRequest
    {
        size_t m_requested;
        size_t m_started;
        size_t m_completed;
    };

    CollectionRequest m_compactionRequests[2];
    CollectionRequest m_trimRequest;
    CollectionRequest m_blockingRequests[2];
    size_t m_gcStartSequence;

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
    volatile int m_ccStackMarkingThreadsNum;

#if _DEBUG
    // The threads that scan stacks with a conservative marker now. (see IsConservativeScanInProgress)
    volatile int32_t m_conservativeScans;
#endif

    // Counts a thread that scans stacks with a conservative marker while it is in scope, in Checked builds only.
    class ConservativeScanScope
    {
    public:
#if _DEBUG
        ConservativeScanScope(SatoriRecycler* recycler) : m_recycler(recycler)
        {
            Interlocked::Increment(&recycler->m_conservativeScans);
        }

        ~ConservativeScanScope()
        {
            Interlocked::Decrement(&m_recycler->m_conservativeScans);
        }

    private:
        SatoriRecycler* m_recycler;
#else
        ConservativeScanScope(SatoriRecycler*)
        {
        }
#endif
    };

    // threads filtering m_reusableRegions concurrently. prep waits for these to leave
    // before it may swap the queues.
    volatile int m_reusableFilterThreadsNum;

    volatile int m_ccHelpersNum;

    int m_syncBlockCacheScanDone;

    int m_condemnedGeneration;

    bool m_concurrentCardsDone;
    bool m_concurrentHandlesDone;
    volatile int m_concurrentCleaningState;

    bool m_isRelocating;
    // the current GC accepted a compaction request, see TryStartGC
    bool m_forceCompact;
    // the current GC accepted a trim request, see TryStartGC
    bool m_forceTrim;
    bool m_forceBlocking;
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
    size_t m_prevGen1Budget;
    size_t m_gen1OverrunPerCycle;
    size_t m_availableMemory;
    size_t m_totalLimit;

    // how much more free space may be parked in reusable regions. set in Plan once the gen1
    // budget and cycle allocations are known, and spent as regions are parked. 0 stops parking any more.
    int m_reusableTargetPercent;
    int64_t m_reusableLimit;
    // what Plan set m_reusableLimit to, and the allocation that it expects until the next GC. (see GetTrimBudget)
    int64_t m_reusableLimitAtPlan;
    size_t m_expectedGen1;
    bool m_nextGcIsFullGc;
    // the most memory that the heap had in use at the GCs since the trimmer took it. (see NoteInUse)
    size_t m_inUsePeak;

    size_t m_condemnedRegionsCount;
    size_t m_deferredSweepCount;
    size_t m_gen1AddedSinceLastCollection;
    size_t m_gen2AddedSinceLastCollection;
    size_t m_gen1CountAtLastGen2;
    size_t m_gcNextTimeTarget;

    size_t m_occupancy[3];
    size_t m_occupancyAcc[3];
    size_t m_demotedOccupancyAcc;
    bool m_occupancyReportingEnabled;

    size_t m_estimatedEphemeralReclaim;
    size_t m_estimatedTenuredReclaim;
    size_t m_promotionEstimate;

    int64_t m_osTicksPerMilli;
    int64_t m_osTicksPerMicro;
    int64_t m_timeStampTicksPerMilli;

    SatoriGate* m_workerGate;

    // Idle workers poll this while spinning. Keep it on a cache line of its own, away from the counters
    // below, which are updated with interlocked operations. (the next field starts a new line)
    DECLSPEC_ALIGN(Satori::WIDE_CACHE_GRANULARITY)
    volatile int m_gateSignaled;

    DECLSPEC_ALIGN(Satori::WIDE_CACHE_GRANULARITY)
    volatile int m_workerWoken;
    volatile int m_activeWorkers;
    volatile int m_totalWorkers;

    void(SatoriRecycler::* volatile m_activeWorkerFn)();

    int64_t m_noWorkSince;

    LastRecordedGcInfo m_lastEphemeralGcInfo;
    LastRecordedGcInfo m_lastTenuredGcInfo;
    LastRecordedGcInfo* m_CurrentGcInfo;

    size_t m_startMillis;

    // ---- incremental relocation ----
    //
    // It is used only in low latency mode, which does not relocate regularly, since that costs in proportion to the heap.
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

    template <bool recording>
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
    // the budget of the current GC in cost units, and what the kept candidates are estimated to cost.
    size_t m_incrUnitsBudget;
    size_t m_incrPlannedUnits;

    // The cost model, calibrated as we go: pause = fixed + perUnit * units.
    // The fixed part is updating roots, which is measured, and the overhead of a relocation, which is not
    // separable from the part per unit, so the two are fitted together. Units are estimated from objects, bytes and references.
    double m_incrRootsUs;
    double m_incrOverheadUs;
    double m_incrUsPerUnit;
    // decaying sums of the fit of the pause without roots (y, in microseconds) to units (x, in millions)
    double m_incrFitW;
    double m_incrFitX;
    double m_incrFitY;
    double m_incrFitXX;
    double m_incrFitXY;
    // references to candidates that are recorded, per object in candidates
    double m_incrRefsPerObj;
    int m_incrRootsSamples;
    int m_incrRefsSamples;
    // when nothing fits the budget, we relocate only occasionally, to see if that changed.
    int m_incrNoRoomCount;
    // measured parts of the current incremental relocation
    int64_t m_incrMeasuredTicks;
    size_t m_incrRootsStartTicks;
    volatile size_t m_incrRootsDoneTicks;

    // A target needs a free span that could take some of the relocated objects. (128K+, the top 4 buckets)
    static const int INCR_MIN_TARGET_BUCKET = Satori::FREELIST_COUNT - 4;
    static const int INCR_TARGET_BUCKETS = Satori::FREELIST_COUNT - INCR_MIN_TARGET_BUCKET;

    // A summary of what selection needs to know about Gen2 regions, made as they enter the tenured queues,
    // so that selection does not need to walk the queues.
    // Regions leave the tenured queues only in GCs that rebuild Gen2 (m_promoteAllRegions), and these start the summary over.
    // Otherwise the queues only grow, and so does the summary. Thus it is what a walk of the queues would find.
    //
    // It is updated in bursts - as the Gen2 regions are swept after a GC, by several threads at once.
    // A shared structure makes that a point of contention, so every processor updates a shard of its own.
    // Selection then merges the shards. Gen2 occupancy and the number of Gen2 regions are known from the recycler's
    // accumulators and the queues, which are reset and filled at the same points, so these are not in the summary.
    static const int INCR_MAX_SUMMARY_SHARDS = 64;
    static const int INCR_SHARD_TARGETS = 64;

    struct DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY) IncrSummaryShard
    {
        // a spin lock - the critical section is a few moves in a small heap
        volatile int m_lock;
        // the best relocation sources, as a max-heap by key - the root is the worst one kept
        int m_sourceCount;
        // what the eligible regions would free (the backlog), and large regions, which do not count toward Gen2 space
        size_t m_eligibleGain;
        size_t m_largeRegions;
        // relocation targets, by their largest free bucket
        int m_targetCounts[INCR_TARGET_BUCKETS];
        SatoriIncrEntry m_sources[INCR_MAX_REGIONS];
        SatoriRegion* m_targets[INCR_TARGET_BUCKETS][INCR_SHARD_TARGETS];
    };

    // one shard per processor (up to INCR_MAX_SUMMARY_SHARDS), or null until low latency mode is first seen
    // or when incremental relocation is not enabled. (see IncrStartSummarizing)
    IncrSummaryShard* m_incrSummary;
    int m_incrSummaryShardCount;
    // reserved at startup for the summaries, if incremental relocation is enabled
    void* m_incrSummaryReservation;
    int m_incrSummaryReservedShards;

    // Gen1 regions that could be sources, by shard as above. Rebuilt by every GC. (see IncrSummarizeGen1Region)
    struct DECLSPEC_ALIGN(Satori::CACHE_LINE_GRANULARITY) IncrSourceShard
    {
        volatile int m_lock;
        int m_sourceCount;
        // what the eligible regions would free, as in IncrSummaryShard
        size_t m_eligibleGain;
        SatoriIncrEntry m_sources[INCR_MAX_REGIONS];
    };

    IncrSourceShard* m_incrGen1Summary;

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
    void MaybeAskForHelpOnce(bool& askedForHelp);
    void RunWithHelp(void(SatoriRecycler::* method)());
    bool HelpOnceCore(bool minQuantum);
    bool HelpOnceCoreInner(bool minQuantum);

    void PushToEphemeralQueues(SatoriRegion* region);
    void PushToTenuredQueues(SatoriRegion* region);

    void AdjustHeuristics();
    void DeactivateAllocatingRegions();
    void RequestCollection(CollectionRequest* request, size_t requestNumber);

    void IncrementRootScanTicket();
    void IncrementCardScanTicket();
    uint8_t GetCardScanTicket();

    void MarkOwnStack(gc_alloc_context* aContext, MarkContext* markContext);
    void MarkThroughCards();
    bool MarkThroughCardsConcurrent(int64_t deadline);
    void MarkDemoted(SatoriRegion* curRegion, MarkContext* markContext);
    void MarkDemotedInReusable(SatoriRegion* curRegion, MarkContext* markContext);
    void MarkAllStacksFinalizationAndDemotedRoots();

    void PushToMarkQueuesSlow(SatoriWorkChunk*& currentWorkChunk, SatoriObject* o);
    void DrainMarkQueues(SatoriWorkChunk* srcChunk = nullptr);
    template <bool recording>
    void DrainMarkQueuesCore(SatoriWorkChunk* srcChunk);
    void MarkOwnStackAndDrainQueues();
    void MarkOwnStackOrDrainQueuesConcurrent(int64_t deadline);
    bool MarkDemotedAndDrainQueuesConcurrent(int64_t deadline);
    bool MarkDemotedInReusableConcurrent(int64_t deadline);
    void PushOrReturnWorkChunk(SatoriWorkChunk * srcChunk);
    bool DrainMarkQueuesConcurrent(SatoriWorkChunk* srcChunk = nullptr, int64_t deadline = 0);
    template <bool recording>
    bool DrainMarkQueuesConcurrentCore(SatoriWorkChunk* srcChunk, int64_t deadline);

    bool HasDirtyCards();
    bool CleanCardsConcurrent(int64_t deadline);
    template <bool recording>
    bool CleanCardsConcurrentCore(int64_t deadline);
    void CleanCards();
    template <bool recording>
    void CleanCardsCore();
    bool MarkHandles(int64_t deadline = 0);
    void ShortWeakPtrScan();
    void ShortWeakPtrScanWorker();
    void LongWeakPtrScan();
    void LongWeakPtrScanWorker();

    void ScanFinalizables();
    void ScanFinalizableRegions(SatoriShardedRegionQueue* regions, MarkContext* markContext, SatoriShardedRegionQueue::Batch* pending, SatoriShardedRegionQueue::Batch* eph, SatoriShardedRegionQueue::Batch* ten);
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
    void NoteInUse();
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
    void PlanRegions(SatoriShardedRegionQueue* regions);
    void DenyRelocation();

    void AddTenuredRegionsToPlan(SatoriShardedRegionQueue* regions);
    void AddRelocationTarget(SatoriRegion* region);
    SatoriRegion* TryGetRelocationTarget(size_t size, bool existingRegionOnly);
    SatoriRegion* GetOrAddRelocationTarget(SatoriRegion * region, size_t allocSize);

    void Relocate();
    void RelocateWorker();
    void RelocateRegion(SatoriRegion* region);
    bool RelocateRegionSplit(SatoriRegion* relocationSource, size_t maxBytesToCopy);
    void FreeLogicallyEmptyRegion(SatoriRegion* curRegion, bool hasMarks);
    void FreeRelocatedRegionsWorker();

    void PromoteHandlesAndFreeRelocatedRegions();
    void PromoteSurvivedHandlesAndFreeRelocatedRegionsWorker();

    void Update();
    void UpdateRootsWorker();
    void UpdateRegionsWorker();
    void UpdatePointersThroughCards();
    void UpdatePointersInObjectRanges();
    void UpdatePointersInPromotedObjects();
    void UpdateRegions(SatoriShardedRegionQueue* queue, SatoriShardedRegionQueue::Batch* deferredFirst, SatoriShardedRegionQueue::Batch* deferredRest);

    void KeepRegion(SatoriRegion* curRegion);
    bool ShouldReuse(SatoriRegion* curRegion);
    void DrainDeferredSweepQueue();
    void DrainReusableQueue();
    bool DrainDeferredSweepQueueConcurrent(int64_t deadline = 0);
    void DrainDeferredSweepQueueWorkerFn();
    void SweepAndReturnRegion(SatoriRegion* curRegion);

    void UpdateGcCounters(int64_t blockingStart);

    void SelectIncrementalRelocationCandidates();
    static void IncrEvaluateRegion(SatoriRegion* region, double refsPerObj, bool gen1, SatoriIncrRegionInfo& info);
    void IncrSummarizeRegion(SatoriRegion* region);
    void IncrSummarizeGen1Region(SatoriRegion* region);
    static size_t IncrSummarySize(int shardCount);
    void IncrStartSummarizing();
    void IncrSummaryReset();
    void IncrGen1SummaryReset();
    void PlanIncrementalRelocation();
    void CancelIncrementalRelocation();
    void CalibrateIncrementalRelocation();
    void PublishRecordedRefs(SatoriWorkChunk*& chunk);
    void PushRecordedChunk(SatoriWorkChunk* chunk);
    bool RecordRelocationRefSlow(SatoriWorkChunk*& chunk, SatoriObject* entry);
    void AddRecordedRefCounts(const int32_t* counts);
    void AbandonIncrementalRelocation();
    void FreeRecordedRefs();
    void UpdateRecordedRefsWorker();
#if _DEBUG
    void VerifyIncrementalRelocation();
#endif

    void ASSERT_NO_WORK();
};

#endif
