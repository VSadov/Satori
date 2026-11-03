// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <stdio.h>
#include <stdlib.h>
#define FORCEINLINE __forceinline
#include "SatoriTrackingState.h"

static int s_checks;

static void Check(bool condition)
{
    s_checks++;
    if (!condition)
    {
        fprintf(stderr, "FAIL: tracking contract check %d\n", s_checks);
        exit(1);
    }
}

int main()
{
    for (int consumer = 0; consumer != 4; consumer++)
    {
        // The same pending snapshot reaches owner continuation, GC, detach, or reuse.
        SatoriTrackingState state = {};
        Check(state.Begin(11, 42, 1));
        state.Count(state.escapeFnCalls, 1);
        state.Count(state.escapeRecursivelyCalls, 1);
        state.Count(state.newlyEscapedObjects, 1);
        state.CaptureStop(SatoriTrackingReason::BarrierEscapeLimit, 1, true);
        Check(!state.Begin(12, 43, 1));
        state.CaptureStop(SatoriTrackingReason::GlobalGCDetach, 1, false);
        Check(state.reason == SatoriTrackingReason::BarrierEscapeLimit);
        Check(state.lifetime == 11 && state.owner == 42);
        Check(state.flags & SatoriTrackingState::Deferred);
        state.Count(state.newlyEscapedObjects, 1);
        Check(state.newlyEscapedObjects == 1);
        Check(state.TryDrain());
        Check(!state.TryDrain());
        Check(state.escapeFnCalls == 1 && state.escapeRecursivelyCalls == 1);
        state.ResetInterval(1);
        Check(state.newlyEscapedObjects == 0);
        Check(state.Begin(12, 43, 1));
        Check(state.lifetime == 12 && state.sequence == 0 && state.segment == 0);
    }

    for (int reason = 1; reason <= static_cast<int>(SatoriTrackingReason::ThreadExit); reason++)
    {
        SatoriTrackingState state = {};
        Check(state.Begin(100 + reason, 42, 1));
        state.CaptureStop(static_cast<SatoriTrackingReason>(reason), 1, false);
        Check(state.TryDrain());
        Check(static_cast<int>(state.reason) == reason);
        Check(!(state.flags & SatoriTrackingState::Deferred));
    }

    SatoriTrackingState state = {};
    Check(state.Begin(1, 42, 0));
    state.Count(state.escapeFnCalls, 0);
    Check(state.escapeFnCalls == 0);
    Check(state.flags & SatoriTrackingState::LifetimePartial);
    state.Count(state.escapeFnCalls, 1);
    Check(state.escapeFnCalls == 1 && state.lastEpoch == 1);
    Check(state.flags & SatoriTrackingState::IntervalPartial);
    state.ResetInterval(1);
    Check(!(state.flags & SatoriTrackingState::IntervalPartial));
    state.Count(state.escapeFnCalls, 3);
    Check(state.escapeFnCalls == 1);
    Check(state.flags & SatoriTrackingState::IntervalPartial);
    Check(state.intervalEpoch == 1 && state.lastEpoch == 3);
    state.ResetInterval(3);

    uint64_t sum = 0;
    for (int interval = 0; interval != 3; interval++)
    {
        for (int i = 0; i != 7; i++)
        {
            state.Count(state.newlyEscapedObjects, 3);
        }
        sum += state.newlyEscapedObjects;
        state.ResetInterval(3);
    }
    state.Count(state.newlyEscapedObjects, 3);
    state.CaptureStop(SatoriTrackingReason::ThreadExit, 3, false);
    Check(state.TryDrain());
    sum += state.newlyEscapedObjects;
    Check(sum == 22);
    Check(!state.TryDrain());
    Check(state.Begin(2, 42, 3));
    state.Count(state.seedObjects, 3);
    Check(state.seedObjects == 1 && state.newlyEscapedObjects == 0);
    state.ResetInterval(3);
    state.NextSegment();
    Check(state.segment == 1 && state.lifetime == 2 && state.phase == 1);
    state.Count(state.seedObjects, 3);
    state.ResetInterval(3);
    state.Count(state.newlyEscapedObjects, 3);
    Check(state.newlyEscapedObjects == 1 && state.seedObjects == 0);

    state.escapeFnCalls = UINT32_MAX;
    state.Count(state.escapeFnCalls, 3);
    Check(state.escapeFnCalls == UINT32_MAX);
    Check((state.flags & (SatoriTrackingState::Overflow | SatoriTrackingState::IntervalOverflow)) ==
        (SatoriTrackingState::Overflow | SatoriTrackingState::IntervalOverflow));
    state.ResetInterval(3);
    Check(state.flags & SatoriTrackingState::Overflow);
    Check(!(state.flags & SatoriTrackingState::IntervalOverflow));
    state.sequence = UINT64_MAX;
    Check(state.NextSequence() == UINT64_MAX);
    state.segment = UINT32_MAX;
    state.NextSegment();
    Check(state.segment == UINT32_MAX && (state.flags & SatoriTrackingState::Overflow));

    SatoriTrackingState exhausted = {};
    Check(exhausted.Begin(0, 42, UINT64_MAX));
    exhausted.Count(exhausted.escapeFnCalls, UINT64_MAX);
    Check(exhausted.escapeFnCalls == 0);
    Check((exhausted.flags & (SatoriTrackingState::Overflow | SatoriTrackingState::IdentityUnavailable)) ==
        (SatoriTrackingState::Overflow | SatoriTrackingState::IdentityUnavailable));
    exhausted.ResetInterval(UINT64_MAX);
    Check(exhausted.flags & SatoriTrackingState::IntervalPartial);

    const int caps[] = {0, 1, 4, 255};
    for (int cap : caps)
    {
        SatoriTrackingHistory history = {};
        history.tracking = true;
        history.End(true, cap);
        Check(history.backoff == 0 && history.skip == 0 && !history.tracking);
        for (int probe = 0; probe != 10; probe++)
        {
            history.tracking = true;
            history.attempted = true;
            int expected = history.backoff == 0 ? 1 : history.backoff * 2;
            expected = expected < cap ? expected : cap;
            history.End(true, cap);
            Check(history.backoff == expected && history.skip == expected);
        }
        uint8_t skip = history.skip;
        history.tracking = history.collected = true;
        history.End(false, cap);
        Check(history.backoff == 0 && history.skip == skip);
        history.End(false, cap);
        Check(history.backoff == 0 && history.skip == skip);
    }

    printf("PASS: %d deterministic tracking handoff, history, epoch, segment and conservation checks\n", s_checks);
    return 0;
}
