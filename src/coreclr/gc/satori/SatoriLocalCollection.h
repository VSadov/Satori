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

struct SatoriLocalCollectionResult
{
    SatoriLocalCollectionOutcome outcome;
    uint32_t collectionIndex;

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
    if (atTlgcLimit && !result.Collected())
    {
        TRegion::OnEscapeTrackingEnded();
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
