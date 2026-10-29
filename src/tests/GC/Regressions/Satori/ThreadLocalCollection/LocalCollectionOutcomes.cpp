// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#define FORCEINLINE inline
#include "SatoriLocalCollection.h"
#undef FORCEINLINE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Require(bool condition, const char* message)
{
    if (!condition)
    {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

struct TestRegion
{
    static TestRegion* current;
    SatoriLocalCollectionResult nextResult;
    bool allocationSucceeds;
    bool tracking = true;
    int collections = 0;
    int allocations = 0;
    int reports = 0;
    char operations[32] = {};
    size_t operationCount = 0;
    SatoriLocalCollectionRetry reportedRetry = SatoriLocalCollectionRetry::NotAttempted;
    SatoriLocalCollectionTrigger expectedTrigger;
    uint32_t expectedIndex;

    TestRegion(SatoriLocalCollectionOutcome outcome, SatoriLocalCollectionTrigger trigger, bool succeeds, uint32_t index)
        : nextResult{outcome, index}, allocationSucceeds(succeeds), expectedTrigger(trigger), expectedIndex(index)
    {
        current = this;
    }

    void Record(char operation)
    {
        Require(operationCount + 1 < sizeof(operations), "Operation buffer exhausted.");
        operations[operationCount++] = operation;
    }

    SatoriLocalCollectionResult ThreadLocalCollect(size_t allocBytes, bool atTlgcLimit)
    {
        Require(allocBytes == 1048576, "The original allocation progress must reach collection.");
        Require(atTlgcLimit == (expectedTrigger == SatoriLocalCollectionTrigger::DyadicLimit), "Wrong collection trigger.");
        collections++;
        Record('C');
        return nextResult;
    }

    static void OnEscapeTrackingEnded()
    {
        Require(current->tracking, "Tracking must still be active when history is updated.");
        current->Record('E');
    }

    void StopEscapeTracking()
    {
        Require(tracking, "Tracking must not be stopped twice.");
        tracking = false;
        Record('S');
    }

    bool StartAllocating(size_t requestedBytes, size_t allocBytes)
    {
        Require(requestedBytes == 81920 && allocBytes == 1048576, "Retry arguments changed.");
        allocations++;
        Record('A');
        return allocationSucceeds;
    }

    void ReportThreadLocalCollection(const SatoriLocalCollectionResult& result, size_t requestedBytes,
        SatoriLocalCollectionTrigger trigger, SatoriLocalCollectionRetry retry)
    {
        Require(result.outcome == nextResult.outcome, "Outcome changed before reporting.");
        Require(result.collectionIndex == expectedIndex, "Collection index changed before reporting.");
        bool marked = result.outcome == SatoriLocalCollectionOutcome::SurvivalAbort ||
            result.outcome == SatoriLocalCollectionOutcome::Completed;
        Require(result.HasCollectionIndex() == marked, "Rejected calls must not acquire a collection index.");
        Require(requestedBytes == 81920 && trigger == expectedTrigger, "Incorrect report context.");
        Require(reports + 1 == collections, "Each collection call must have exactly one report.");
        reportedRetry = retry;
        reports++;
        Record('R');
    }
};

TestRegion* TestRegion::current = nullptr;

static void CheckAttempt(SatoriLocalCollectionOutcome outcome, SatoriLocalCollectionTrigger trigger, bool succeeds, uint32_t index)
{
    TestRegion region(outcome, trigger, succeeds, index);
    bool allocated = SatoriCollectAndRetry(&region, 1048576, 81920, trigger);
    bool retryExpected = trigger == SatoriLocalCollectionTrigger::DyadicLimit || outcome == SatoriLocalCollectionOutcome::Completed;
    bool trackingEnded = trigger == SatoriLocalCollectionTrigger::DyadicLimit && outcome != SatoriLocalCollectionOutcome::Completed;
    Require(allocated == (retryExpected && succeeds), "Allocation return value changed.");
    Require(region.collections == 1 && region.reports == 1, "Expected one companion record per call.");
    Require(region.allocations == (retryExpected ? 1 : 0), "Diagnostics must not add an allocation retry.");
    Require(region.tracking != trackingEnded, "Tracking lifetime changed.");
    SatoriLocalCollectionRetry expectedRetry = !retryExpected ? SatoriLocalCollectionRetry::NotAttempted :
        succeeds ? SatoriLocalCollectionRetry::Succeeded : SatoriLocalCollectionRetry::Failed;
    Require(region.reportedRetry == expectedRetry, "Wrong immediate same-region retry state.");
    Require(strcmp(region.operations, trackingEnded ? "CESAR" : retryExpected ? "CAR" : "CR") == 0,
        "Report must follow the actual retry and preserve tracking-stop order.");
}

static void CheckTwoCalls(SatoriLocalCollectionOutcome secondOutcome)
{
    TestRegion region(SatoriLocalCollectionOutcome::Completed, SatoriLocalCollectionTrigger::DyadicLimit, false, 42);
    Require(!SatoriCollectAndRetry(&region, 1048576, 81920, region.expectedTrigger), "First retry must fail.");
    Require(region.tracking, "A completed collection must leave tracking active.");
    Require(region.reportedRetry == SatoriLocalCollectionRetry::Failed, "First report must describe the first retry only.");

    region.nextResult = { secondOutcome, secondOutcome == SatoriLocalCollectionOutcome::Completed ? 43u : 0u };
    region.expectedIndex = region.nextResult.collectionIndex;
    region.expectedTrigger = SatoriLocalCollectionTrigger::ExhaustedSpan;
    region.allocationSucceeds = true;
    bool allocated = SatoriCollectAndRetry(&region, 1048576, 81920, region.expectedTrigger);
    bool secondCompleted = secondOutcome == SatoriLocalCollectionOutcome::Completed;
    Require(allocated == secondCompleted, "Second call changed the exhausted-span allocation decision.");
    Require(region.collections == 2 && region.reports == 2, "Two calls require two independent reports.");
    Require(strcmp(region.operations, secondCompleted ? "CARCAR" : "CARCR") == 0, "Incorrect two-call sequence.");
}

int main()
{
    int cases = 0;
    for (uint32_t outcome = 0; outcome <= 3; outcome++)
    {
        for (uint32_t trigger = 0; trigger <= 1; trigger++)
        {
            bool marked = outcome >= 2;
            for (uint32_t succeeds = 0; succeeds <= 1; succeeds++)
            {
                CheckAttempt(static_cast<SatoriLocalCollectionOutcome>(outcome), static_cast<SatoriLocalCollectionTrigger>(trigger), succeeds != 0, marked ? 42 : 0);
                cases++;
            }
            if (marked)
            {
                CheckAttempt(static_cast<SatoriLocalCollectionOutcome>(outcome), static_cast<SatoriLocalCollectionTrigger>(trigger), true, 0);
                cases++;
            }
        }
    }

    CheckTwoCalls(SatoriLocalCollectionOutcome::InsufficientProgress);
    CheckTwoCalls(SatoriLocalCollectionOutcome::Completed);
    cases += 2;
    printf("PASS: %d deterministic outcome/retry cases\n", cases);
    return 0;
}
