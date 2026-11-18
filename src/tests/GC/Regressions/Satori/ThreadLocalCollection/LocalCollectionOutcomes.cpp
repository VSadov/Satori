// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "SatoriLocalCollection.h"
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
    SatoriLocalCollectionOutcome nextOutcome;
    SatoriLocalCollectionTrigger expectedTrigger;
    uint32_t expectedIndex;
    bool allocationSucceeds;
    bool eventEnabled;
    bool toggleDuringCollection = false;
    bool tracking = true;
    int enableChecks = 0;
    int collections = 0;
    int allocations = 0;
    int samplingRequests = 0;
    int reports = 0;
    int emissions = 0;
    char operations[32] = {};
    size_t operationCount = 0;
    SatoriLocalCollectionRetry reportedRetry = SatoriLocalCollectionRetry::NotAttempted;
    SatoriLocalCollectionTrigger reportedTrigger = SatoriLocalCollectionTrigger::WindowLimit;

    TestRegion(SatoriLocalCollectionOutcome outcome, SatoriLocalCollectionTrigger trigger, bool succeeds,
        uint32_t index, bool enabled)
        : nextOutcome(outcome), expectedTrigger(trigger), expectedIndex(index),
          allocationSucceeds(succeeds), eventEnabled(enabled)
    {
        current = this;
    }

    void Record(char operation)
    {
        Require(operationCount + 1 < sizeof(operations), "Operation buffer exhausted.");
        operations[operationCount++] = operation;
    }

    static bool ThreadLocalCollectionEventEnabled()
    {
        current->enableChecks++;
        return current->eventEnabled;
    }

    SatoriLocalCollectionOutcome ThreadLocalCollect(size_t allocBytes, bool atTlgcLimit,
        SatoriLocalCollectionData* data)
    {
        Require(allocBytes == 1048576, "Allocation progress argument changed.");
        Require(atTlgcLimit == (expectedTrigger == SatoriLocalCollectionTrigger::WindowLimit), "Wrong collection trigger.");
        Require((data != nullptr) == eventEnabled, "Disabled attempts must not request diagnostic samples.");
        collections++;
        Record('C');
        if (data != nullptr)
        {
            samplingRequests++;
            data->collectionIndex = expectedIndex;
        }
        if (toggleDuringCollection)
        {
            eventEnabled = !eventEnabled;
        }
        return nextOutcome;
    }

    static void OnEscapeTrackingEnded(bool outOfSpace)
    {
        Require(current->tracking, "Tracking must still be active when history is updated.");
        Require(outOfSpace == (current->expectedTrigger == SatoriLocalCollectionTrigger::ExhaustedSpan),
            "Tracking history must preserve the out-of-space distinction.");
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
        Require(requestedBytes == 24576 && allocBytes == 1048576, "Retry arguments changed.");
        allocations++;
        Record('A');
        return allocationSucceeds;
    }

    void ReportThreadLocalCollection(const SatoriLocalCollectionData& data, size_t requestedBytes,
        SatoriLocalCollectionTrigger trigger, SatoriLocalCollectionOutcome outcome, SatoriLocalCollectionRetry retry)
    {
        Require(outcome == nextOutcome && data.collectionIndex == expectedIndex, "Incorrect report identity.");
        Require(requestedBytes == 24576 && trigger == expectedTrigger, "Incorrect report context.");
        reportedRetry = retry;
        reportedTrigger = trigger;
        reports++;
        if (eventEnabled)
        {
            emissions++;
        }
        Record('R');
    }
};

TestRegion* TestRegion::current = nullptr;

static void CheckAttempt(SatoriLocalCollectionOutcome outcome, SatoriLocalCollectionTrigger trigger,
    bool succeeds, uint32_t index, bool enabled)
{
    TestRegion region(outcome, trigger, succeeds, index, enabled);
    bool allocated = SatoriCollectAndRetry(&region, 1048576, 24576, trigger);
    bool retryExpected = trigger == SatoriLocalCollectionTrigger::WindowLimit || outcome == SatoriLocalCollectionOutcome::Completed;
    bool trackingEnded = outcome != SatoriLocalCollectionOutcome::Completed;
    Require(allocated == (retryExpected && succeeds), "Allocation return value changed.");
    Require(region.enableChecks == 1 && region.collections == 1, "Expected one enable check and collection call.");
    Require(region.samplingRequests == (enabled ? 1 : 0), "Incorrect sampling request count.");
    Require(region.reports == (enabled ? 1 : 0) && region.emissions == region.reports, "Incorrect report count.");
    Require(region.allocations == (retryExpected ? 1 : 0), "Diagnostics must not add an allocation retry.");
    Require(region.tracking != trackingEnded, "Tracking lifetime changed.");
    if (enabled)
    {
        SatoriLocalCollectionRetry expectedRetry = !retryExpected ? SatoriLocalCollectionRetry::NotAttempted :
            succeeds ? SatoriLocalCollectionRetry::Succeeded : SatoriLocalCollectionRetry::Failed;
        Require(region.reportedRetry == expectedRetry, "Wrong immediate retry state.");
    }
    const char* expected = trackingEnded ? (retryExpected ? "CESA" : "CES") : "CA";
    Require(strncmp(region.operations, expected, strlen(expected)) == 0, "Incorrect tracking-stop/retry order.");
    Require(strcmp(region.operations + strlen(expected), enabled ? "R" : "") == 0, "Reporting must follow retry.");
}

static void CheckTwoCalls(SatoriLocalCollectionOutcome secondOutcome, bool enabled)
{
    TestRegion region(SatoriLocalCollectionOutcome::Completed, SatoriLocalCollectionTrigger::WindowLimit, false, 42, enabled);
    Require(!SatoriCollectAndRetry(&region, 1048576, 24576, region.expectedTrigger), "First retry must fail.");
    Require(region.tracking, "Completed collection with failed retry must retain tracking.");
    if (enabled)
    {
        Require(region.reportedRetry == SatoriLocalCollectionRetry::Failed, "First report must describe its own retry.");
    }
    region.nextOutcome = secondOutcome;
    region.expectedIndex = secondOutcome == SatoriLocalCollectionOutcome::Completed ? 43u : 0u;
    region.expectedTrigger = SatoriLocalCollectionTrigger::ExhaustedSpan;
    region.allocationSucceeds = true;
    bool allocated = SatoriCollectAndRetry(&region, 1048576, 24576, region.expectedTrigger);
    bool completed = secondOutcome == SatoriLocalCollectionOutcome::Completed;
    Require(allocated == completed, "Second call changed the exhausted-span decision.");
    Require(region.enableChecks == 2 && region.collections == 2, "Two attempts must remain two calls.");
    Require(region.reports == (enabled ? 2 : 0), "Enabled attempts need independent reports.");
    if (enabled)
    {
        Require(region.reportedTrigger == SatoriLocalCollectionTrigger::ExhaustedSpan, "Second trigger was lost.");
        Require(region.reportedRetry == (completed ? SatoriLocalCollectionRetry::Succeeded :
            SatoriLocalCollectionRetry::NotAttempted), "Second retry was lost.");
    }
    const char* expected = enabled ? (completed ? "CARCAR" : "CARCESR") : (completed ? "CACA" : "CACES");
    Require(strcmp(region.operations, expected) == 0, "Incorrect two-call sequence.");
}

static void CheckEnablementChange(bool initiallyEnabled)
{
    TestRegion region(SatoriLocalCollectionOutcome::Completed, SatoriLocalCollectionTrigger::WindowLimit, true, 42, initiallyEnabled);
    region.toggleDuringCollection = true;
    Require(SatoriCollectAndRetry(&region, 1048576, 24576, region.expectedTrigger), "Enablement changed allocation behavior.");
    Require(region.enableChecks == 1, "Enablement must be captured at entry.");
    Require(region.samplingRequests == (initiallyEnabled ? 1 : 0), "Sampling must use entry enablement.");
    Require(region.reports == (initiallyEnabled ? 1 : 0), "Mid-attempt enablement must not fabricate samples.");
    Require(region.emissions == 0, "Disabling the listener must suppress emission.");
}

#ifdef SATORI_LOCAL_COLLECTION_TEST_LIBRARY
extern "C" DLL_EXPORT int RunLocalCollectionOutcomes()
#else
int main()
#endif
{
    int cases = 0;
    for (uint32_t enabled = 0; enabled <= 1; enabled++)
    {
        for (uint32_t outcome = 0; outcome <= 3; outcome++)
        {
            for (uint32_t trigger = 0; trigger <= 1; trigger++)
            {
                for (uint32_t succeeds = 0; succeeds <= 1; succeeds++)
                {
                    CheckAttempt(static_cast<SatoriLocalCollectionOutcome>(outcome),
                        static_cast<SatoriLocalCollectionTrigger>(trigger), succeeds != 0, outcome >= 2 ? 42u : 0u, enabled != 0);
                    cases++;
                }
                if (outcome >= 2)
                {
                    CheckAttempt(static_cast<SatoriLocalCollectionOutcome>(outcome),
                        static_cast<SatoriLocalCollectionTrigger>(trigger), true, 0, enabled != 0);
                    cases++;
                }
            }
        }
        CheckTwoCalls(SatoriLocalCollectionOutcome::InsufficientProgress, enabled != 0);
        CheckTwoCalls(SatoriLocalCollectionOutcome::Completed, enabled != 0);
        cases += 2;
    }
    CheckEnablementChange(false);
    CheckEnablementChange(true);
    cases += 2;
    printf("PASS: %d deterministic outcome/retry/enablement cases\n", cases);
    return 0;
}
