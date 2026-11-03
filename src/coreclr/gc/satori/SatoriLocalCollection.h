// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __SATORI_LOCAL_COLLECTION_H__
#define __SATORI_LOCAL_COLLECTION_H__

#include <stddef.h>
#include <stdint.h>

enum class SatoriLocalCollectionOutcome : uint32_t
{
    EscapedLimit = 0,
    InsufficientProgress = 1,
    SurvivalAbort = 2,
    Completed = 3,
};

enum class SatoriLocalCollectionTrigger : uint32_t
{
    DyadicLimit = 0,
    ExhaustedSpan = 1,
};

enum class SatoriLocalCollectionRetry : uint32_t
{
    NotAttempted = 0,
    Succeeded = 1,
    Failed = 2,
};

enum class SatoriLocalCollectionProgressStatus : uint32_t
{
    Available = 0,
    CounterRollback = 1,
    AccountingRebased = 2,
    CounterOverflow = 3,
};

inline SatoriLocalCollectionProgressStatus SatoriProgressStatus(size_t current, size_t baseline,
    SatoriLocalCollectionProgressStatus status)
{
    if (current > static_cast<uint64_t>(INT64_MAX) || baseline > static_cast<uint64_t>(INT64_MAX))
    {
        return SatoriLocalCollectionProgressStatus::CounterOverflow;
    }

    return status == SatoriLocalCollectionProgressStatus::Available && current < baseline ?
        SatoriLocalCollectionProgressStatus::CounterRollback : status;
}

inline SatoriLocalCollectionProgressStatus SatoriProgressAfterClaim(size_t current, size_t claimed,
    SatoriLocalCollectionProgressStatus status)
{
    // gc_alloc_context::alloc_bytes is signed, although the region's baselines use size_t.
    return current > static_cast<uint64_t>(INT64_MAX) || claimed > static_cast<uint64_t>(INT64_MAX) - current ?
        SatoriLocalCollectionProgressStatus::CounterOverflow : status;
}

struct SatoriLocalCollectionResult
{
    SatoriLocalCollectionOutcome outcome;
    uint32_t collectionIndex;
    // Sampled before the marking baseline is updated, after unused claims are returned.
    uint64_t allocationBytes = 0;
    uint64_t allocationBytesBaseline = 0;
    SatoriLocalCollectionProgressStatus progressStatus = SatoriLocalCollectionProgressStatus::Available;
    // Both samples precede the allocation retry. Outcome qualifies completed-compaction recovery.
    uint64_t usableBefore = 0;
    uint64_t usableAfter = 0;

    bool HasCollectionIndex() const
    {
        return outcome == SatoriLocalCollectionOutcome::SurvivalAbort ||
            outcome == SatoriLocalCollectionOutcome::Completed;
    }

    bool Collected() const
    {
        return outcome == SatoriLocalCollectionOutcome::Completed;
    }
};

// Keep the allocation decision shared with the deterministic native test. In particular,
// a limit-triggered rejection still retries, whereas an exhausted-span rejection does not.
// Inline in the runtime so the adapter does not add a frame to stack-depth-based windows.
template <typename TRegion>
FORCEINLINE bool SatoriCollectAndRetry(TRegion* region, size_t allocBytes, size_t requestedBytes, SatoriLocalCollectionTrigger trigger)
{
    bool atTlgcLimit = trigger == SatoriLocalCollectionTrigger::DyadicLimit;
    SatoriLocalCollectionResult result = region->ThreadLocalCollect(allocBytes, atTlgcLimit);
    if (!result.Collected())
    {
        TRegion::OnEscapeTrackingEnded(/* outOfSpace */ !atTlgcLimit);
        region->StopEscapeTracking();
    }

    SatoriLocalCollectionRetry retry = SatoriLocalCollectionRetry::NotAttempted;
    bool allocated = false;
    if (atTlgcLimit || result.Collected())
    {
        allocated = region->StartAllocating(requestedBytes, allocBytes);
        retry = allocated ? SatoriLocalCollectionRetry::Succeeded : SatoriLocalCollectionRetry::Failed;
    }

    region->ReportThreadLocalCollection(result, requestedBytes, trigger, retry);
    return allocated;
}

#endif // __SATORI_LOCAL_COLLECTION_H__
