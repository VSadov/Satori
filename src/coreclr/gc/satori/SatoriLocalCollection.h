// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __SATORI_LOCAL_COLLECTION_H__
#define __SATORI_LOCAL_COLLECTION_H__

#include <stddef.h>
#include <stdint.h>
#include <minipal/utils.h>

enum class SatoriLocalCollectionOutcome : uint32_t
{
    EscapedLimit = 0,
    InsufficientProgress = 1,
    SurvivalAbort = 2,
    Completed = 3,
};

enum class SatoriLocalCollectionTrigger : uint32_t
{
    WindowLimit = 0,
    ExhaustedSpan = 1,
};

enum class SatoriLocalCollectionRetry : uint32_t
{
    NotAttempted = 0,
    Succeeded = 1,
    Failed = 2,
};

struct SatoriLocalCollectionData
{
    uint64_t allocationProgress;
    uint64_t usableBefore;
    uint64_t usableAfter;
    uint64_t occupancyBefore;
    uint64_t occupancyAfter;
    uint64_t escapedBytesBefore;
    uint64_t escapedBytesAfter;
    double durationNs;
    uint32_t collectionIndex;
};

// Limit-triggered rejections still retry allocation; exhausted-span rejections do not.
// Keep inline to avoid adding a frame to stack-depth-based collection windows.
template <typename TRegion>
FORCEINLINE bool SatoriCollectAndRetry(TRegion* region, size_t allocBytes, size_t requestedBytes, SatoriLocalCollectionTrigger trigger)
{
    bool atTlgcLimit = trigger == SatoriLocalCollectionTrigger::WindowLimit;
    bool eventEnabled = TRegion::ThreadLocalCollectionEventEnabled();
    SatoriLocalCollectionData data;
    SatoriLocalCollectionOutcome outcome = region->ThreadLocalCollect(allocBytes, atTlgcLimit,
        eventEnabled ? &data : nullptr);
    bool collected = outcome == SatoriLocalCollectionOutcome::Completed;
    if (!collected)
    {
        TRegion::OnEscapeTrackingEnded(/* outOfSpace */ !atTlgcLimit);
        region->StopEscapeTracking();
    }

    SatoriLocalCollectionRetry retry = SatoriLocalCollectionRetry::NotAttempted;
    bool allocated = false;
    if (atTlgcLimit || collected)
    {
        allocated = region->StartAllocating(requestedBytes, allocBytes);
        retry = allocated ? SatoriLocalCollectionRetry::Succeeded : SatoriLocalCollectionRetry::Failed;
    }

    if (eventEnabled)
    {
        region->ReportThreadLocalCollection(data, requestedBytes, trigger, outcome, retry);
    }
    return allocated;
}

#endif // __SATORI_LOCAL_COLLECTION_H__
