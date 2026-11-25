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
// SatoriIncrementalRelocation.cpp
//

#include "common.h"

#include "gcenv.h"
#include "../env/gcenv.os.h"

#include "SatoriHeap.h"
#include "SatoriRecycler.h"
#include "SatoriObject.h"
#include "SatoriObject.inl"
#include "SatoriRegion.h"
#include "SatoriRegion.inl"
#include "SatoriWorkChunk.h"

//#pragma optimize("", off)

// why recording of references to incremental relocation candidates was given up
static const int INCR_ABANDON_REFS = 1;
static const int INCR_ABANDON_OOM = 2;

// The cost model of incremental relocation.
// A candidate costs units for its objects (copying, updating the copy), its bytes and for the recorded references
// to it (updating). A unit is roughly a nanosecond of single thread time, but that does not need to be precise,
// since the pause per unit and the fixed part of the pause (updating roots, overhead) are calibrated as we go.
static const size_t INCR_UNITS_PER_OBJ = 40;
static const size_t INCR_UNITS_PER_REF = 40;
static const size_t INCR_BYTES_PER_UNIT = 8;
// A recorded reference to a candidate that is not relocated is only looked at, not updated.
static const size_t INCR_UNITS_PER_SKIPPED_REF = 2;
// the model before anything is measured. conservative, as if we had 4 threads.
static const double INCR_INITIAL_ROOTS_US = 200;
static const double INCR_INITIAL_OVERHEAD_US = 100;
static const double INCR_INITIAL_US_PER_UNIT = 0.001 / 4;
static const double INCR_INITIAL_REFS_PER_OBJ = 2;
// weight of a new sample in the calibrated values
static const double INCR_CALIBRATION_WEIGHT_UP = 1.0 / 2;
static const double INCR_CALIBRATION_WEIGHT_DOWN = 1.0 / 8;
// The fit of the overhead and the pause per unit. Older samples decay, so it follows changes within ~20 relocations.
// The initial model weighs as one sample, which keeps the fit stable when relocations are all about the same size.
// Pauses longer than predicted weigh more, since that could make pauses longer than the budget.
static const double INCR_FIT_DECAY = 0.95;
static const double INCR_FIT_PRIOR_WEIGHT = 1;
static const double INCR_FIT_WEIGHT_UP = 2;
static const double INCR_FIT_UNITS_SCALE = 1000000;

void SatoriRecycler::InitializeIncrementalRelocation()
{
    m_recordedRefs = SatoriWorkList::AllocAligned();
    m_incrRecording = false;
    m_isIncrementalRelocation = false;
    m_incrSourceCount = 0;
    m_incrTargetCount = 0;
    m_incrRecordedRefs = 0;
    m_incrAbandonReason = 0;
    m_incrMaxRecordedRefs = 0;
    memset(m_incrKeep, 0, sizeof(m_incrKeep));
    m_incrSpillLock.Initialize();
    m_incrSpill = nullptr;
    m_incrRecordedLast = nullptr;
    m_incrCopyRangeCount = 0;
    m_incrCopyRangeClaim = 0;
    m_incrSelectionDone = false;
    m_incrSelectedObjs = 0;
    m_incrUnitsBudget = 0;
    m_incrPlannedUnits = 0;
    m_incrRootsUs = INCR_INITIAL_ROOTS_US;
    m_incrOverheadUs = INCR_INITIAL_OVERHEAD_US;
    m_incrUsPerUnit = INCR_INITIAL_US_PER_UNIT;
    m_incrFitW = 0;
    m_incrFitX = 0;
    m_incrFitY = 0;
    m_incrFitXX = 0;
    m_incrFitXY = 0;
    m_incrRefsPerObj = INCR_INITIAL_REFS_PER_OBJ;
    m_incrRootsSamples = 0;
    m_incrRefsSamples = 0;
    m_incrNoRoomCount = 0;
    m_incrMeasuredTicks = 0;
    m_incrRootsStartTicks = 0;
    m_incrRootsDoneTicks = 0;
    m_incrSummary = nullptr;
    m_incrGen1Summary = nullptr;
    m_incrSummaryShardCount = 0;
    // The summary is allocated when low latency mode is first seen. (see IncrStartSummarizing)
    if (SatoriUtil::IsIncrementalRelocation() && SatoriUtil::IsLowLatencyMode())
    {
        IncrStartSummarizing();
    }
}

typedef SatoriIncrEntry IncrEntry;

// Keeps the entries with the smallest keys, no more than capacity, in a max-heap. The root is the largest kept.
static void IncrHeapInsertSmallest(IncrEntry* heap, int& count, int capacity, const IncrEntry& entry)
{
    int i;
    if (count < capacity)
    {
        // sift up
        i = count++;
        while (i > 0)
        {
            int parent = (i - 1) / 2;
            if (heap[parent].m_key >= entry.m_key)
            {
                break;
            }

            heap[i] = heap[parent];
            i = parent;
        }

        heap[i] = entry;
        return;
    }

    if (entry.m_key >= heap[0].m_key)
    {
        return;
    }

    // replace the root and sift down
    i = 0;
    for (;;)
    {
        int child = i * 2 + 1;
        if (child >= count)
        {
            break;
        }

        if (child + 1 < count && heap[child + 1].m_key > heap[child].m_key)
        {
            child++;
        }

        if (heap[child].m_key <= entry.m_key)
        {
            break;
        }

        heap[i] = heap[child];
        i = child;
    }

    heap[i] = entry;
}

// Sorts a max-heap in place, the smallest key first.
static void IncrHeapSort(IncrEntry* heap, int count)
{
    for (int end = count - 1; end > 0; end--)
    {
        IncrEntry last = heap[end];
        heap[end] = heap[0];

        // sift the former last element down from the root, within [0, end)
        int i = 0;
        for (;;)
        {
            int child = i * 2 + 1;
            if (child >= end)
            {
                break;
            }

            if (child + 1 < end && heap[child + 1].m_key > heap[child].m_key)
            {
                child++;
            }

            if (heap[child].m_key <= last.m_key)
            {
                break;
            }

            heap[i] = heap[child];
            i = child;
        }

        heap[i] = last;
    }
}

// Recording is given up when the candidates are referenced from this many times more places than the budget allows
// updating. That bounds the memory used for recording, while the rest is dealt with by dropping costly candidates in planning.
static const size_t INCR_HARD_REFS_FACTOR = 4;
static const size_t INCR_MIN_HARD_REFS = 64 * 1024;

// When nothing fits the budget, we relocate only this often, to see if that changed.
static const int INCR_NO_ROOM_PROBE_INTERVAL = 16;

static size_t IncrRelocationCost(size_t objCount, size_t bytes, size_t refs)
{
    return objCount * INCR_UNITS_PER_OBJ + refs * INCR_UNITS_PER_REF + bytes / INCR_BYTES_PER_UNIT;
}

static size_t IncrRelocationGain(size_t bytes)
{
    return Satori::REGION_SIZE_GRANULARITY - bytes - sizeof(SatoriRegion);
}

// Moves a calibrated value towards a new sample. The first sample replaces the initial guess.
// Costlier than thought is followed quickly, since that could make pauses longer than the budget, cheaper - slowly.
static void IncrCalibrate(double& value, int& samples, double sample)
{
    // outliers, like a helper that was not scheduled for a while, move the value only so much.
    // that includes the first sample, which is clamped against the initial value.
    sample = min(max(sample, value / 4), value * 4);
    if (samples == 0)
    {
        value = sample;
    }
    else
    {
        double weight = sample > value ? INCR_CALIBRATION_WEIGHT_UP : INCR_CALIBRATION_WEIGHT_DOWN;
        value += (sample - value) * weight;
    }

    samples++;
}

// Evaluates a region for selection, as it enters the tenured queues, or the ephemeral queues for Gen1 regions.
// (see IncrSummarizeRegion) Gen1 regions are only sources, since they are rebuilt by every GC.
// They may be demoted - a Gen2 GC marks the demoted objects and frees the trackers before planning.
void SatoriRecycler::IncrEvaluateRegion(SatoriRegion* region, double refsPerObj, bool gen1, SatoriIncrRegionInfo& info)
{
    info.m_occupancy = region->Occupancy();
    info.m_inSpace = false;
    info.m_isSource = false;
    info.m_isTarget = false;
    info.m_bucket = -1;
    info.m_gain = 0;

    if (region->IsLarge())
    {
        return;
    }

    info.m_inSpace = true;
    if ((!gen1 && (region->IsDemoted() || region->HasUnmarkedDemotedObjects())) ||
        region->IsAttachedToAllocatingOwner() ||
        region->IsPinnedByMark() ||
        region->SweepsSinceLastAllocation() == 0)
    {
        return;
    }

    size_t occupancy = info.m_occupancy;
    size_t objCount = (size_t)region->ObjCount();
    if (objCount == 0)
    {
        return;
    }

    int bucket = region->GetMaxFreeBucket();
    info.m_bucket = bucket;

    // As in ReclaimSizeIfRelocated, we do not want to move much.
    // A region that could take its own objects in one of its free spans is a better target than a source.
    // Pinned object heap objects keep a region in place. A region that was pinned by marking is likely to be pinned again.
    if (occupancy <= Satori::REGION_SIZE_GRANULARITY / 2 &&
        bucket < SatoriUtil::BucketForAlloc(occupancy) &&
        !region->HasPinnedObjects() &&
        !region->WasPinnedByMark())
    {
        size_t gain = IncrRelocationGain(occupancy);
        size_t cost = IncrRelocationCost(objCount, occupancy, (size_t)(objCount * refsPerObj));
        info.m_isSource = true;
        info.m_gain = gain;
        info.m_source.m_region = region;
        info.m_source.m_key = cost * 65536 / gain;
        info.m_source.m_objCount = (uint32_t)objCount;
        info.m_source.m_occupancy = (uint32_t)occupancy;
    }

    info.m_isTarget = !gen1 && bucket >= INCR_MIN_TARGET_BUCKET;
}

static void IncrSpinLockEnter(volatile int* lock)
{
    while (Interlocked::CompareExchange(lock, 1, 0) != 0)
    {
        int spins = 0;
        while (VolatileLoadWithoutBarrier(lock) != 0)
        {
            // the holder is almost done, unless it was preempted
            if (++spins < 1024)
            {
                YieldProcessor();
            }
            else
            {
                GCToOSInterface::YieldThread(0);
                spins = 0;
            }
        }
    }
}

static void IncrSpinLockLeave(volatile int* lock)
{
    VolatileStore((int*)lock, 0);
}

// The shard of the summary for the current thread - by the processor it runs on, when we can know that.
static int IncrSummaryShardIndex(int shardCount)
{
    uint32_t value;
    if (GCToOSInterface::CanGetCurrentProcessorNumber())
    {
        value = GCToOSInterface::GetCurrentProcessorNumber();
    }
    else
    {
        size_t thread = SatoriUtil::GetCurrentThreadTag();
        value = (uint32_t)((thread * 11400714819323198485llu) >> 32);
    }

    return (int)(value % (uint32_t)shardCount);
}

// Called for every region that enters the tenured queues, when incremental relocation is enabled.
// The region has just been swept or handed over, so its header is likely still in cache.
// NB: the cost model (refs per object) changes only when a Gen2 GC plans, which starts the summary over.
void SatoriRecycler::IncrSummarizeRegion(SatoriRegion* region)
{
    _ASSERTE(region->Generation() == 2);

    SatoriIncrRegionInfo info;
    IncrEvaluateRegion(region, m_incrRefsPerObj, /* gen1 */ false, info);
    bool isLarge = !info.m_inSpace;
    if (isLarge || info.m_isSource || info.m_isTarget)
    {
        IncrSummaryShard& shard = m_incrSummary[IncrSummaryShardIndex(m_incrSummaryShardCount)];
        IncrSpinLockEnter(&shard.m_lock);
        if (isLarge)
        {
            shard.m_largeRegions++;
        }

        if (info.m_isSource)
        {
            shard.m_eligibleGain += info.m_gain;
            IncrHeapInsertSmallest(shard.m_sources, shard.m_sourceCount, INCR_MAX_REGIONS, info.m_source);
        }

        if (info.m_isTarget)
        {
            // a shard keeps fewer targets than selection may take, but together they have more.
            int b = info.m_bucket - INCR_MIN_TARGET_BUCKET;
            if (shard.m_targetCounts[b] < INCR_SHARD_TARGETS)
            {
                shard.m_targets[b][shard.m_targetCounts[b]++] = region;
            }
        }

        IncrSpinLockLeave(&shard.m_lock);
    }
}

// Called for every Gen1 region that a GC puts into the ephemeral queues, when incremental relocation is enabled.
// (not the reusable ones, which allocators may take at any time)
// The ephemeral queues are rebuilt by every GC, and so is this summary. Until then, the regions stay in the queues.
// Thus a Gen2 GC that starts after a GC finds them there and may select them as sources. Being Gen1, they are promoted by the Gen2 GC.
void SatoriRecycler::IncrSummarizeGen1Region(SatoriRegion* region)
{
    _ASSERTE(region->Generation() == 1);

    SatoriIncrRegionInfo info;
    IncrEvaluateRegion(region, m_incrRefsPerObj, /* gen1 */ true, info);
    if (info.m_isSource)
    {
        IncrSourceShard& shard = m_incrGen1Summary[IncrSummaryShardIndex(m_incrSummaryShardCount)];
        IncrSpinLockEnter(&shard.m_lock);
        shard.m_eligibleGain += info.m_gain;
        IncrHeapInsertSmallest(shard.m_sources, shard.m_sourceCount, INCR_MAX_REGIONS, info.m_source);
        IncrSpinLockLeave(&shard.m_lock);
    }
}

// The summary is allocated only when low latency mode is first seen, at startup or by a GC, while EE is stopped.
// From then on it is kept up in all modes, since the latency mode can change back.
// Regions that entered the queues before that are not in the summary until Gen2 is rebuilt. That only makes fewer
// sources and targets known, and counts less backlog, thus relocation does less, not more.
// If there is no memory for it, the next GC tries again.
void SatoriRecycler::IncrStartSummarizing()
{
    _ASSERTE(m_incrSummary == nullptr);

    // The Gen1 summary follows the Gen2 one.
    int shardCount = min(INCR_MAX_SUMMARY_SHARDS, max(1, (int)GCToEEInterface::GetCurrentProcessCpuCount()));
    size_t summarySize = (sizeof(IncrSummaryShard) + sizeof(IncrSourceShard)) * shardCount;
    // The shards are aligned to cache lines, and an allocation in a region is not: it starts after the sync block of an object.
    size_t allocSize = summarySize + Satori::CACHE_LINE_GRANULARITY;

    // The summary lives as long as the process, so, like the finalization queue, it is in a region at the highest free granules of a page.
    // The claim commits what we need before it publishes the region, and gives the granules back if it cannot.
    SatoriRegion* region = m_heap->Allocator()->GetRegionLongLived(
        SatoriRegion::RegionSizeForAlloc(allocSize), SatoriRegion::CommitSizeForAlloc(allocSize));
    if (region == nullptr)
    {
        return;
    }

    // Nothing is left to commit, so this cannot fail. Zeroed is the initial state.
    size_t chunk = region->Allocate(allocSize, /*zeroInitialize*/ true);
    _ASSERTE(chunk != 0);
    size_t summary = ALIGN_UP(chunk, Satori::CACHE_LINE_GRANULARITY);

    // The region could come committed beyond what we need.
    region->TryDecommitAbove(summary + summarySize);

    m_incrSummaryShardCount = shardCount;
    m_incrGen1Summary = (IncrSourceShard*)(summary + sizeof(IncrSummaryShard) * shardCount);
    m_incrSummary = (IncrSummaryShard*)summary;
}

// Starts the Gen1 summary over. Called by every GC, while EE is stopped and before any Gen1 region is put back into the queues.
void SatoriRecycler::IncrGen1SummaryReset()
{
    for (int s = 0; s < m_incrSummaryShardCount; s++)
    {
        m_incrGen1Summary[s].m_sourceCount = 0;
        m_incrGen1Summary[s].m_eligibleGain = 0;
    }
}

// Starts the summary over. Called when Gen2 is rebuilt, while EE is stopped.
void SatoriRecycler::IncrSummaryReset()
{
    for (int s = 0; s < m_incrSummaryShardCount; s++)
    {
        IncrSummaryShard& shard = m_incrSummary[s];
        shard.m_sourceCount = 0;
        shard.m_eligibleGain = 0;
        shard.m_largeRegions = 0;
        for (int b = 0; b < INCR_TARGET_BUCKETS; b++)
        {
            shard.m_targetCounts[b] = 0;
        }
    }
}

// Called when the barrier is about to become concurrent for a gen2 GC, before any marking.
// All deferred sweeping is done by now, so the summary of gen2 regions, made as they entered the tenured queues, is complete.
// Selects sparse gen2 regions as relocation candidates, and gen2 regions with large free spans as targets.
// Only in low latency mode, where only as many candidates are selected as needed to bring the backlog down to what is tolerated.
// Only the summary is looked at, not the regions, thus the cost does not grow with the heap. It is done concurrently.
void SatoriRecycler::SelectIncrementalRelocationCandidates()
{
    _ASSERTE(!m_incrRecording);
    m_incrSourceCount = 0;
    m_incrTargetCount = 0;
    m_incrSelectedObjs = 0;
    m_incrSelectionDone = false;

    // the summary exists only when incremental relocation is enabled, from the first time low latency mode is seen.
    // Only low latency mode relocates incrementally, since it does not relocate regularly.
    // (NB: the latency mode can change, so once started, the summary is kept up in all modes)
    // Forced compaction relocates regularly, even in low latency mode.
    if (m_condemnedGeneration != 2 ||
        m_incrSummary == nullptr ||
        !IsLowLatencyMode() ||
        m_forceCompact)
    {
        return;
    }

    _ASSERTE(m_recordedRefs->IsEmpty());
    m_incrRecordedLast = nullptr;
    m_incrRecordedRefs = 0;
    m_incrAbandonReason = 0;
    m_incrCopyRangeCount = 0;
    m_incrCopyRangeClaim = 0;

    IncrEntry sources[INCR_MAX_REGIONS];
    SatoriRegion* targets[INCR_MAX_REGIONS * 2];
    int sourceCount = 0;
    int targetCount = 0;
    size_t eligibleGain = 0;
    size_t largeRegions = 0;
    double refsPerObj = m_incrRefsPerObj;

    // Merge the shards. Pushes may still happen (direct Gen2 allocations), so the locks.
    // Most shards of a small heap are empty, which is seen without the lock. A region pushed right now could be missed.
    int shardTargets[INCR_MAX_SUMMARY_SHARDS][INCR_TARGET_BUCKETS];
    for (int s = 0; s < m_incrSummaryShardCount; s++)
    {
        IncrSummaryShard& shard = m_incrSummary[s];
        bool hasTargets = false;
        for (int b = 0; b < INCR_TARGET_BUCKETS; b++)
        {
            shardTargets[s][b] = VolatileLoadWithoutBarrier(&shard.m_targetCounts[b]);
            hasTargets |= shardTargets[s][b] != 0;
        }

        if (!hasTargets &&
            VolatileLoadWithoutBarrier(&shard.m_sourceCount) == 0 &&
            VolatileLoadWithoutBarrier(&shard.m_largeRegions) == 0)
        {
            continue;
        }

        IncrSpinLockEnter(&shard.m_lock);
        for (int i = 0; i < shard.m_sourceCount; i++)
        {
            IncrHeapInsertSmallest(sources, sourceCount, INCR_MAX_REGIONS, shard.m_sources[i]);
        }

        eligibleGain += shard.m_eligibleGain;
        largeRegions += shard.m_largeRegions;
        IncrSpinLockLeave(&shard.m_lock);
    }

    // Gen1 regions are only sources. (see IncrSummarizeGen1Region)
    for (int s = 0; s < m_incrSummaryShardCount; s++)
    {
        IncrSourceShard& shard = m_incrGen1Summary[s];
        if (VolatileLoadWithoutBarrier(&shard.m_sourceCount) == 0)
        {
            continue;
        }

        IncrSpinLockEnter(&shard.m_lock);
        for (int i = 0; i < shard.m_sourceCount; i++)
        {
            IncrHeapInsertSmallest(sources, sourceCount, INCR_MAX_REGIONS, shard.m_sources[i]);
        }

        eligibleGain += shard.m_eligibleGain;
        IncrSpinLockLeave(&shard.m_lock);
    }

    // the best sources first
    IncrHeapSort(sources, sourceCount);

    // the largest spans first
    for (int b = INCR_TARGET_BUCKETS - 1; b >= 0; b--)
    {
        for (int s = 0; s < m_incrSummaryShardCount && targetCount < INCR_MAX_REGIONS * 2; s++)
        {
            if (shardTargets[s][b] == 0)
            {
                continue;
            }

            IncrSummaryShard& shard = m_incrSummary[s];
            IncrSpinLockEnter(&shard.m_lock);
            int n = min(shard.m_targetCounts[b], INCR_MAX_REGIONS * 2 - targetCount);
            for (int i = 0; i < n; i++)
            {
                targets[targetCount++] = shard.m_targets[b][i];
            }

            IncrSpinLockLeave(&shard.m_lock);
        }
    }

    // Gen2 regions are known from the queues, see IncrSummaryShard.
    // The counts are read separately from the summary, while large regions may be added. When they disagree, Gen2 space is taken as 0.
    // Gen1 regions count as well, since Gen1 sources are in the backlog and the Gen2 GC promotes them.
    size_t gen2Regions = Gen2RegionCount();
    size_t gen2Space = ((gen2Regions > largeRegions ? gen2Regions - largeRegions : 0) + Gen1RegionCount()) * Satori::REGION_SIZE_GRANULARITY;

    // We relocate only as much as brings the backlog down to what is tolerated, and nothing when the backlog is already there.
    // The budget, which is the pause that incremental relocation may add, is only a limit.
    size_t toleratedBacklog = gen2Space * SatoriUtil::IncrRelocBacklogPercent() / 100;
    size_t neededGain = eligibleGain > toleratedBacklog ? eligibleGain - toleratedBacklog : 0;
    double budgetUs = (double)SatoriUtil::IncrRelocBudgetUs();

    // What the budget leaves after the fixed part of the pause, in cost units.
    double fixedUs = m_incrRootsUs + m_incrOverheadUs;
    size_t unitsBudget = budgetUs > fixedUs ?
        (size_t)((budgetUs - fixedUs) / m_incrUsPerUnit) :
        0;

    auto select = [&](int i)
    {
        size_t objCount = sources[i].m_objCount;
        size_t occupancy = sources[i].m_occupancy;
        m_incrSelectedObjs += objCount;
        m_incrSourceObjs[m_incrSourceCount] = objCount;
        m_incrSourceBytes[m_incrSourceCount] = occupancy;
        m_incrSources[m_incrSourceCount++] = sources[i].m_region;
        return IncrRelocationGain(occupancy);
    };

    // sources, best first, as long as the budget allows and more is needed
    size_t unitsLeft = unitsBudget;
    size_t selectedGain = 0;
    for (int i = 0; i < sourceCount && selectedGain < neededGain; i++)
    {
        size_t objCount = sources[i].m_objCount;
        size_t cost = IncrRelocationCost(objCount, sources[i].m_occupancy, (size_t)(objCount * refsPerObj));
        if (cost > unitsLeft)
        {
            // maybe a cheaper one will fit
            continue;
        }

        unitsLeft -= cost;
        selectedGain += select(i);
    }

    // Nothing fits the budget, which could be the model being off, since it learns only from relocations.
    // Once in a while relocate the cheapest candidate anyway, to see if that is still so.
    // A budget of 0 means no added pause, thus no probes either.
    if (m_incrSourceCount == 0 &&
        sourceCount > 0 &&
        neededGain > 0 &&
        budgetUs > 0 &&
        ++m_incrNoRoomCount % INCR_NO_ROOM_PROBE_INTERVAL == 0)
    {
        int cheapest = 0;
        size_t cheapestCost = SIZE_MAX;
        for (int i = 0; i < sourceCount; i++)
        {
            size_t objCount = sources[i].m_objCount;
            size_t cost = IncrRelocationCost(objCount, sources[i].m_occupancy, (size_t)(objCount * refsPerObj));
            if (cost < cheapestCost)
            {
                cheapest = i;
                cheapestCost = cost;
            }
        }

        select(cheapest);
        // a probe may use a bit more than expected, but not much more.
        unitsBudget = cheapestCost * 2;
    }

    // Recording is abandoned when there are far more references than the budget allows updating.
    size_t maxRecordedRefs = SatoriUtil::IncrRelocRefs();
    if (maxRecordedRefs == 0)
    {
        maxRecordedRefs = max(INCR_MIN_HARD_REFS, INCR_HARD_REFS_FACTOR * unitsBudget / INCR_UNITS_PER_REF);
    }

    m_incrMaxRecordedRefs = maxRecordedRefs;
    m_incrUnitsBudget = unitsBudget;

    for (int i = 0; i < m_incrSourceCount; i++)
    {
        _ASSERTE(m_incrSources[i]->Generation() >= 1);
        _ASSERTE((size_t)m_incrSources[i]->ObjCount() == m_incrSourceObjs[i]);
        m_incrSourceRefs[i] = 0;
        m_incrSources[i]->RelocationCandidateIndex() = (uint8_t)(i + 1);
    }

    // Targets are the ones with the largest spans, but not the sources.
    // The sources are few. To not look at the headers of the targets, we look them up in a sorted copy.
    SatoriRegion* sortedSources[INCR_MAX_REGIONS];
    for (int i = 0; i < m_incrSourceCount; i++)
    {
        SatoriRegion* region = m_incrSources[i];
        int j = i;
        while (j > 0 && sortedSources[j - 1] > region)
        {
            sortedSources[j] = sortedSources[j - 1];
            j--;
        }

        sortedSources[j] = region;
    }

    auto isSource = [&](SatoriRegion* region)
    {
        int lo = 0;
        int hi = m_incrSourceCount;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (sortedSources[mid] < region)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }

        return lo < m_incrSourceCount && sortedSources[lo] == region;
    };

    for (int i = 0; i < targetCount && m_incrTargetCount < INCR_MAX_REGIONS && m_incrSourceCount > 0; i++)
    {
        SatoriRegion* region = targets[i];
        if (!isSource(region))
        {
            _ASSERTE(region->RelocationCandidateIndex() == 0);
            m_incrTargets[m_incrTargetCount++] = region;
        }
    }

    // planning will decide how to relocate, even if there are no candidates.
    m_incrSelectionDone = true;

    // publish the tags, before the barrier is toggled to concurrent (which is a process-wide fence).
    // and thus before anyone can start marking.
    m_incrRecording = m_incrSourceCount > 0;
}

void SatoriRecycler::AbandonIncrementalRelocation()
{
    // Just stop recording. Planning will see the reason and will not relocate.
    // The recorded chunks are freed at that point.
    m_incrRecording = false;
}

// A chunk with fewer entries than this is not worth publishing on its own.
static const size_t INCR_SPILL_THRESHOLD = 64;

void SatoriRecycler::PublishRecordedRefs(SatoriWorkChunk*& chunk)
{
    size_t count = chunk->Count();
    SatoriWorkChunk* toPush = chunk;
    chunk = nullptr;

    // Once recording was given up, what is recorded is of no use.
    if (count == 0 || m_incrAbandonReason != 0)
    {
        m_heap->Allocator()->ReturnWorkChunks(toPush);
        return;
    }

    int64_t total = Interlocked::ExchangeAdd64(&m_incrRecordedRefs, (int64_t)count) + (int64_t)count;
    if (count < INCR_SPILL_THRESHOLD)
    {
        SatoriLockHolder holder(&m_incrSpillLock);
        if (m_incrSpill == nullptr)
        {
            m_incrSpill = toPush;
            toPush = nullptr;
        }
        else
        {
            while (toPush->Count() > 0 && m_incrSpill->HasSpace())
            {
                m_incrSpill->Push(toPush->Pop());
            }

            if (toPush->Count() == 0)
            {
                m_heap->Allocator()->ReturnWorkChunk(toPush);
                toPush = nullptr;
            }
            else
            {
                // the spill is full. it goes out and the leftover becomes the new spill.
                SatoriWorkChunk* full = m_incrSpill;
                m_incrSpill = toPush;
                toPush = full;
            }
        }
    }

    if (toPush)
    {
        PushRecordedChunk(toPush);
    }

    if ((size_t)total > m_incrMaxRecordedRefs)
    {
        // the candidates are referenced from too many places, it would be too expensive to update these.
        if (Interlocked::CompareExchange(&m_incrAbandonReason, INCR_ABANDON_REFS, 0) == 0)
        {
            AbandonIncrementalRelocation();
        }
    }
}

void SatoriRecycler::PushRecordedChunk(SatoriWorkChunk* chunk)
{
    m_recordedRefs->Push(chunk);

    // The first chunk pushed ends the chain. We remember it, so that the chain could be freed at once.
    // (nothing is popped until recording is done, thus only one chunk can see an empty list)
    if (chunk->Next() == nullptr)
    {
        m_incrRecordedLast = chunk;
    }
}

// Returns false if we are no longer recording.
bool SatoriRecycler::RecordRelocationRefSlow(SatoriWorkChunk*& chunk, SatoriObject* entry)
{
    if (chunk)
    {
        PublishRecordedRefs(chunk);
    }

    if (!m_incrRecording)
    {
        // gave up, nothing to record.
        return false;
    }

    // NB: not TryGetWorkChunk, which is allowed to fail in debug builds for no reason.
    chunk = m_heap->Allocator()->GetWorkChunk();
    if (chunk == nullptr)
    {
        if (Interlocked::CompareExchange(&m_incrAbandonReason, INCR_ABANDON_OOM, 0) == 0)
        {
            AbandonIncrementalRelocation();
        }

        return false;
    }

    chunk->Push(entry);
    return true;
}

void SatoriRecycler::AddRecordedRefCounts(const int32_t* counts)
{
    for (int i = 0; i < m_incrSourceCount; i++)
    {
        if (counts[i])
        {
            Interlocked::ExchangeAdd64(&m_incrSourceRefs[i], (int64_t)counts[i]);
        }
    }
}

void SatoriRecycler::FreeRecordedRefs()
{
    if (m_incrSpill)
    {
        PushRecordedChunk(m_incrSpill);
        m_incrSpill = nullptr;
    }

    // Returning chunks one by one would be a cache miss per chunk, and there could be thousands of chunks.
    // We return the whole chain at once instead.
    SatoriWorkChunk* first = m_recordedRefs->TakeAll();
    if (first)
    {
        _ASSERTE(m_incrRecordedLast != nullptr && m_incrRecordedLast->Next() == nullptr);
        m_heap->Allocator()->ReturnWorkChunks(first, m_incrRecordedLast);
    }

    m_incrRecordedLast = nullptr;
    m_incrRecordedRefs = 0;
}

// Candidates were selected in low latency mode, but the mode has changed since and this GC relocates regularly,
// which does not know about them.
void SatoriRecycler::CancelIncrementalRelocation()
{
    _ASSERTE(m_incrSelectionDone);

    m_incrRecording = false;
    for (int i = 0; i < m_incrSourceCount; i++)
    {
        m_incrSources[i]->RelocationCandidateIndex() = 0;
    }

    FreeRecordedRefs();
    m_incrSourceCount = 0;
    m_incrTargetCount = 0;
    m_incrSelectionDone = false;
}

// This is in place of PlanRegions, and is called when marking is done.
// Keeps the candidates that are still relocatable and, now that we know how many references to them need updating,
// fit the budget. These become the only relocation sources.
void SatoriRecycler::PlanIncrementalRelocation()
{
    _ASSERTE(m_incrSelectionDone);
    _ASSERTE(m_condemnedGeneration == 2);

    int64_t startTicks = minipal_hires_ticks();
    int reason = m_incrAbandonReason;
    int64_t recorded = m_incrRecordedRefs;

    // marking is done, so no more recording.
    m_incrRecording = false;
    if (m_incrSpill)
    {
        PushRecordedChunk(m_incrSpill);
        m_incrSpill = nullptr;
    }

    // learn how many references to expect per object, for the next selection.
    // when recording was abandoned because of references, we saw only a part of them.
    if (m_incrSelectedObjs > 0 && reason != INCR_ABANDON_OOM)
    {
        double refsPerObj = (double)recorded / (double)m_incrSelectedObjs;
        if (reason == INCR_ABANDON_REFS)
        {
            refsPerObj = max(refsPerObj, m_incrRefsPerObj * 2);
        }

        IncrCalibrate(m_incrRefsPerObj, m_incrRefsSamples, min(max(refsPerObj, 0.25), 64.0));
    }

    SatoriRegion* keep[INCR_MAX_REGIONS];
    int keepIndex[INCR_MAX_REGIONS];
    size_t keepCost[INCR_MAX_REGIONS];
    size_t keepGain[INCR_MAX_REGIONS];
    size_t keepRefs[INCR_MAX_REGIONS];
    size_t keepTotalCost = 0;
    int keepCount = 0;
    // recorded references to the candidates that are not relocated. Updating still goes through these.
    size_t skippedRefs = 0;

    for (int i = 0; i < m_incrSourceCount; i++)
    {
        SatoriRegion* region = m_incrSources[i];
        region->RelocationCandidateIndex() = 0;
        if (reason != 0)
        {
            continue;
        }

        size_t refs = (size_t)m_incrSourceRefs[i];

        // pinned candidates are not movable. we found that while marking stacks, which could not be known earlier.
        if (region->IsUnmovable())
        {
            skippedRefs += refs;
            continue;
        }

        keep[keepCount] = region;
        keepIndex[keepCount] = i;
        keepCost[keepCount] = IncrRelocationCost(m_incrSourceObjs[i], m_incrSourceBytes[i], refs);
        keepGain[keepCount] = IncrRelocationGain(m_incrSourceBytes[i]);
        keepRefs[keepCount] = refs;
        keepTotalCost += keepCost[keepCount];
        keepCount++;
    }

    // Keep what fits the budget. The least efficient candidates (by cost per gain) are dropped first,
    // these are often the ones referenced from too many places.
    // If going through the references to the dropped ones alone does not fit, nothing is relocated.
    while (keepTotalCost + skippedRefs * INCR_UNITS_PER_SKIPPED_REF > m_incrUnitsBudget && keepCount > 0)
    {
        int worst = 0;
        for (int i = 1; i < keepCount; i++)
        {
            if ((double)keepCost[i] * keepGain[worst] > (double)keepCost[worst] * keepGain[i])
            {
                worst = i;
            }
        }

        keepTotalCost -= keepCost[worst];
        skippedRefs += keepRefs[worst];
        keepCount--;
        keep[worst] = keep[keepCount];
        keepIndex[worst] = keepIndex[keepCount];
        keepCost[worst] = keepCost[keepCount];
        keepGain[worst] = keepGain[keepCount];
        keepRefs[worst] = keepRefs[keepCount];
    }

    // Largest first, since these are harder to fit. PlanRegions does that too.
    // Push puts at the head, so we push the smallest first.
    for (int i = 1; i < keepCount; i++)
    {
        SatoriRegion* region = keep[i];
        int index = keepIndex[i];
        int j = i;
        while (j > 0 && keep[j - 1]->Occupancy() > region->Occupancy())
        {
            keep[j] = keep[j - 1];
            keepIndex[j] = keepIndex[j - 1];
            j--;
        }

        keep[j] = region;
        keepIndex[j] = index;
    }

    // Recorded locations of references to the candidates that we keep are the ones to update.
    // Recorded locations of references to the rest are skipped.
    memset(m_incrKeep, 0, sizeof(m_incrKeep));
    int relocatingCount = 0;
    for (int i = 0; i < keepCount; i++)
    {
        // The candidate is in the tenured queue, or in the ephemeral one if it is Gen1. It must be taken out, to not be treated as staying.
        // (finalization tracking queues are empty when planning a Gen2 GC, see Plan)
        SatoriRegion* region = keep[i];
        int index = keepIndex[i];
        if (m_tenuredRegions->TryRemove(region) ||
            m_ephemeralRegions->TryRemove(region))
        {
            m_relocatingRegions->Push(region);
            m_incrKeep[index + 1] = true;
            relocatingCount++;
        }
        else
        {
            keepTotalCost -= IncrRelocationCost(m_incrSourceObjs[index], m_incrSourceBytes[index], (size_t)m_incrSourceRefs[index]);
            skippedRefs += (size_t)m_incrSourceRefs[index];
        }
    }

    if (relocatingCount > 0)
    {
        for (int i = 0; i < m_incrTargetCount; i++)
        {
            if (m_tenuredRegions->TryRemove(m_incrTargets[i]))
            {
                AddRelocationTarget(m_incrTargets[i]);
            }
        }
    }

    // everything else stays
    DenyRelocation();

    m_isRelocating = relocatingCount > 0;
    m_isIncrementalRelocation = m_isRelocating;
    if (m_isRelocating)
    {
        // What we expect this to cost, and the start of measuring what it actually costs.
        // The estimate is from the same quantities as in selection (occupancy and object count as of the last sweep),
        // so that the calibration learns what selection needs to predict.
        m_incrPlannedUnits = keepTotalCost + skippedRefs * INCR_UNITS_PER_SKIPPED_REF;
        m_incrMeasuredTicks = minipal_hires_ticks() - startTicks;
    }
    else
    {
        FreeRecordedRefs();
    }

    m_incrSourceCount = 0;
    m_incrTargetCount = 0;
    m_incrSelectionDone = false;
}

// Learns from what incremental relocation actually cost. Called when it is done.
void SatoriRecycler::CalibrateIncrementalRelocation()
{
    double usPerTick = 1000000.0 / (double)minipal_hires_tick_frequency();
    double measuredUs = (double)m_incrMeasuredTicks * usPerTick;
    double rootsUs = (double)(m_incrRootsDoneTicks - m_incrRootsStartTicks) * usPerTick;

    // The roots part is measured. The rest is the overhead of a relocation plus what is per unit, fitted as a line.
    IncrCalibrate(m_incrRootsUs, m_incrRootsSamples, max(rootsUs, 1.0));

    double x = (double)m_incrPlannedUnits / INCR_FIT_UNITS_SCALE;
    double slope = m_incrUsPerUnit * INCR_FIT_UNITS_SCALE;
    double predicted = m_incrOverheadUs + slope * x;
    // outliers, like a helper that was not scheduled for a while, move the fit only so much.
    double y = min(max(max(measuredUs - rootsUs, 0.0), predicted / 4), predicted * 4);
    double w = y > predicted ? INCR_FIT_WEIGHT_UP : 1;

    m_incrFitW = m_incrFitW * INCR_FIT_DECAY + w;
    m_incrFitX = m_incrFitX * INCR_FIT_DECAY + w * x;
    m_incrFitY = m_incrFitY * INCR_FIT_DECAY + w * y;
    m_incrFitXX = m_incrFitXX * INCR_FIT_DECAY + w * x * x;
    m_incrFitXY = m_incrFitXY * INCR_FIT_DECAY + w * x * y;

    // least squares, pulled towards the initial model
    double initialSlope = INCR_INITIAL_US_PER_UNIT * INCR_FIT_UNITS_SCALE;
    double a11 = m_incrFitW + INCR_FIT_PRIOR_WEIGHT;
    double a12 = m_incrFitX;
    double a22 = m_incrFitXX + INCR_FIT_PRIOR_WEIGHT;
    double b1 = m_incrFitY + INCR_FIT_PRIOR_WEIGHT * INCR_INITIAL_OVERHEAD_US;
    double b2 = m_incrFitXY + INCR_FIT_PRIOR_WEIGHT * initialSlope;
    double det = a11 * a22 - a12 * a12;
    double overhead = (b1 * a22 - a12 * b2) / det;
    slope = (a11 * b2 - a12 * b1) / det;
    if (overhead < 0)
    {
        overhead = 0;
        slope = b2 / a22;
    }

    m_incrOverheadUs = overhead;
    m_incrUsPerUnit = max(slope, initialSlope / 1024) / INCR_FIT_UNITS_SCALE;
}

// Updates what was recorded (or is otherwise known) to need updating after an incremental relocation.
// Roots are updated separately.
void SatoriRecycler::UpdateRecordedRefsWorker()
{
    _ASSERTE(m_isIncrementalRelocation);

    // The copies have references to what was in the relocated regions. There are not a lot of copies.
    SatoriRelocatedRefQueue<8> updateQueue;
    int rangeCount = m_incrCopyRangeCount;
    for (;;)
    {
        int i = Interlocked::Increment(&m_incrCopyRangeClaim) - 1;
        if (i >= rangeCount)
        {
            break;
        }

        IncrCopyRange& range = m_incrCopyRanges[i];
        SatoriObject* o = (SatoriObject*)range.m_start;
        while (o->Start() < range.m_end)
        {
            size_t size = o->Size();
            range.m_region->UpdatePointersInObject(o, size, updateQueue);
            o = (SatoriObject*)(o->Start() + size);
        }
    }

    updateQueue.Flush();

    // recorded locations. Some may have been overwritten since, that is fine, we check what is there now.
    // (the locations in the relocated objects are also there, that is fine too - just a redundant update of the old copy)
    SatoriWorkChunk* chunk = m_recordedRefs->TryPop();
    if (chunk)
    {
        MaybeAskForHelp();

        // processed chunks are returned at the end, at once.
        SatoriWorkChunk* doneFirst = nullptr;
        SatoriWorkChunk* doneLast = chunk;
        do
        {
            size_t count = chunk->Count();
            for (size_t i = 0; i < count; i++)
            {
                size_t ahead = (size_t)chunk->Item(min(i + 8, count - 1));
                if (m_incrKeep[ahead >> INCR_INDEX_SHIFT])
                {
                    SatoriUtil::Prefetch((void*)(ahead & INCR_LOCATION_MASK));
                }

                // references to candidates that we did not relocate need no update
                // (and we do not even want to read these locations)
                size_t entry = (size_t)chunk->Item(i);
                size_t candidateIndex = entry >> INCR_INDEX_SHIFT;
                _ASSERTE(candidateIndex >= 1 && candidateIndex <= INCR_MAX_REGIONS);
                if (!m_incrKeep[candidateIndex])
                {
                    continue;
                }

                SatoriObject** ref = (SatoriObject**)(entry & INCR_LOCATION_MASK);

                // someone else could be updating the same location
                SatoriObject* child = VolatileLoadWithoutBarrier(ref);
                SatoriObject* newLocation;
                if (child && child->IsRelocatedTo(&newLocation))
                {
                    VolatileStoreWithoutBarrier(ref, newLocation);
                }
            }

            chunk->SetNext(doneFirst);
            doneFirst = chunk;
        } while ((chunk = m_recordedRefs->TryPop()));

        m_heap->Allocator()->ReturnWorkChunks(doneFirst, doneLast);
    }
}

#if _DEBUG
// Checks that nothing alive refers to relocated regions. Runs before the relocated regions are freed,
// so stale references are found here and not as heap corruption later.
void SatoriRecycler::VerifyIncrementalRelocation()
{
    int64_t failed = 0;

    auto verifyObject = [&](SatoriObject* o, size_t size)
    {
        o->ForEachObjectRef(
            [&](SatoriObject** ref)
            {
                SatoriObject* child = *ref;
                if (child && !child->IsExternal() && child->ContainingRegion()->IsRelocated())
                {
                    failed++;
                }
            },
            size);
    };

    auto verifyRegion = [&](SatoriRegion* region)
    {
        _ASSERTE(!region->IsRelocated());
        size_t objLimit = region->Start() + Satori::REGION_SIZE_GRANULARITY;
        SatoriObject* o = region->FirstObject();

        if (region->IsLarge())
        {
            if (o->IsFree())
            {
                o = o->Next();
            }

            if (o->Start() < region->End() && o->IsMarked())
            {
                verifyObject(o, o->Size());
            }

            return;
        }

        // regions that are swept do not have marks, the rest have marks and dead objects.
        bool hasMarks = !region->DoNotSweep();
        while (o->Start() < objLimit)
        {
            if (hasMarks)
            {
                o = region->SkipUnmarked(o);
                if (o->Start() >= objLimit)
                {
                    break;
                }
            }

            size_t size = o->Size();
            verifyObject(o, size);
            o = (SatoriObject*)(o->Start() + size);
        }
    };

    m_stayingRegions->ForEachRegion(verifyRegion);
    for (int i = 0; i < Satori::FREELIST_COUNT; i++)
    {
        m_relocationTargets[i]->ForEachRegion(verifyRegion);
    }

    if (failed)
    {
        _ASSERTE(!"Stale references after incremental relocation");
    }
}
#endif
