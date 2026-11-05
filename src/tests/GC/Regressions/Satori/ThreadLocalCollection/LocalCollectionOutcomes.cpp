// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#define FORCEINLINE inline
#include "SatoriLocalCollection.h"
#undef FORCEINLINE
#ifdef SATORI_LOCAL_COLLECTION_TEST_LIBRARY
#include "platformdefines.h"
#endif
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
        nextResult.allocationBytes = 1048576;
        nextResult.allocationBytesBaseline = 983040;
        nextResult.usableBefore = 4096;
        nextResult.usableAfter = 2048;
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

    static void OnEscapeTrackingEnded(bool outOfSpace)
    {
        Require(current->tracking, "Tracking must still be active when history is updated.");
        Require(outOfSpace == (current->expectedTrigger == SatoriLocalCollectionTrigger::ExhaustedSpan),
            "Tracking history must preserve the main out-of-space distinction.");
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
        Require(result.allocationBytes == nextResult.allocationBytes &&
            result.allocationBytesBaseline == nextResult.allocationBytesBaseline,
            "Allocation progress must survive tracking-stop and retry.");
        Require(result.progressStatus == nextResult.progressStatus,
            "A completed collection must not make unavailable progress available.");
        Require(result.usableBefore == nextResult.usableBefore && result.usableAfter == nextResult.usableAfter,
            "Raw usable recovery may be negative and must not be clamped.");
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
    bool trackingEnded = outcome != SatoriLocalCollectionOutcome::Completed;
    Require(allocated == (retryExpected && succeeds), "Allocation return value changed.");
    Require(region.collections == 1 && region.reports == 1, "Expected one companion record per call.");
    Require(region.allocations == (retryExpected ? 1 : 0), "Diagnostics must not add an allocation retry.");
    Require(region.tracking != trackingEnded, "Tracking lifetime changed.");
    SatoriLocalCollectionRetry expectedRetry = !retryExpected ? SatoriLocalCollectionRetry::NotAttempted :
        succeeds ? SatoriLocalCollectionRetry::Succeeded : SatoriLocalCollectionRetry::Failed;
    Require(region.reportedRetry == expectedRetry, "Wrong immediate same-region retry state.");
    Require(strcmp(region.operations, trackingEnded ? (retryExpected ? "CESAR" : "CESR") : "CAR") == 0,
        "Report must follow the actual retry and preserve tracking-stop order.");
}

static void CheckTwoCalls(SatoriLocalCollectionOutcome secondOutcome)
{
    TestRegion region(SatoriLocalCollectionOutcome::Completed, SatoriLocalCollectionTrigger::DyadicLimit, false, 42);
    Require(!SatoriCollectAndRetry(&region, 1048576, 81920, region.expectedTrigger), "First retry must fail.");
    Require(region.tracking, "A completed collection must leave tracking active.");
    Require(region.reportedRetry == SatoriLocalCollectionRetry::Failed, "First report must describe the first retry only.");

    region.nextResult = { secondOutcome, secondOutcome == SatoriLocalCollectionOutcome::Completed ? 43u : 0u };
    region.nextResult.allocationBytes = 1048576;
    region.nextResult.allocationBytesBaseline = 983040;
    region.nextResult.usableBefore = 4096;
    region.nextResult.usableAfter = 2048;
    region.expectedIndex = region.nextResult.collectionIndex;
    region.expectedTrigger = SatoriLocalCollectionTrigger::ExhaustedSpan;
    region.allocationSucceeds = true;
    bool allocated = SatoriCollectAndRetry(&region, 1048576, 81920, region.expectedTrigger);
    bool secondCompleted = secondOutcome == SatoriLocalCollectionOutcome::Completed;
    Require(allocated == secondCompleted, "Second call changed the exhausted-span allocation decision.");
    Require(region.collections == 2 && region.reports == 2, "Two calls require two independent reports.");
    Require(strcmp(region.operations, secondCompleted ? "CARCAR" : "CARCESR") == 0, "Incorrect two-call sequence.");
}

static int CheckProgress()
{
    using Status = SatoriLocalCollectionProgressStatus;
    Require(SatoriProgressStatus(0, 0, Status::Available) == Status::Available, "Zero progress is available.");
    Require(SatoriProgressStatus(1048576, 983040, Status::Available) == Status::Available, "First or repeated progress.");
    Require(SatoriProgressStatus(983039, 983040, Status::Available) == Status::CounterRollback, "Returned claim rollback.");
    Require(SatoriProgressStatus(1048576, 983040, Status::AccountingRebased) == Status::AccountingRebased,
        "A rebase must be unavailable even when the counter increased.");
    Require(SatoriProgressStatus(1048576, 983040, Status::CounterRollback) == Status::CounterRollback,
        "Regrowing past a rolled-back baseline must not erase the discontinuity.");
    Require(SatoriProgressAfterClaim(INT64_MAX - 16, 16, Status::Available) == Status::Available, "No signed overflow at boundary.");
    Require(SatoriProgressAfterClaim(INT64_MAX - 16, 17, Status::Available) == Status::CounterOverflow, "Signed claim overflow.");
    Require(SatoriProgressAfterClaim(SIZE_MAX, 0, Status::Available) == Status::CounterOverflow,
        "A negative allocation counter converted to size_t is unavailable.");
    Require(SatoriProgressStatus(0, SIZE_MAX, Status::Available) == Status::CounterOverflow,
        "A negative marking baseline is not an ordinary rollback.");
    Require(SatoriProgressStatus(0, SIZE_MAX, Status::CounterOverflow) == Status::CounterOverflow,
        "Overflow must not become an ordinary rollback.");
    Require(SatoriProgressAfterClaim(100, 8, Status::CounterOverflow) == Status::CounterOverflow, "Overflow persists.");
    for (uint32_t status = 0; status <= 3; status++)
    {
        TestRegion region(SatoriLocalCollectionOutcome::Completed, SatoriLocalCollectionTrigger::DyadicLimit, true, 42);
        region.nextResult.progressStatus = static_cast<Status>(status);
        if (status == 0)
        {
            region.nextResult.allocationBytesBaseline = region.nextResult.allocationBytes;
            region.nextResult.usableBefore = region.nextResult.usableAfter = 0;
        }
        Require(SatoriCollectAndRetry(&region, 1048576, 81920, region.expectedTrigger),
            "Validity reporting must not change the allocation result.");
    }

    return 15;
}

#ifdef SATORI_LOCAL_COLLECTION_TEST_LIBRARY
extern "C" DLL_EXPORT int RunLocalCollectionOutcomes()
#else
int main()
#endif
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
    printf("PASS: %d allocation-progress validity cases\n", CheckProgress());
    return 0;
}
