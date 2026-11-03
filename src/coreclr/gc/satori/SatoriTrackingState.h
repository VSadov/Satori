// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef __SATORI_TRACKING_STATE_H__
#define __SATORI_TRACKING_STATE_H__

#include <stdint.h>
#include <stddef.h>

enum class SatoriTrackingReason : uint8_t
{
    None,
    BarrierEscapeLimit,
    CollectionEscapeLimit,
    InsufficientProgress,
    SurvivalAbort,
    AllocationExhausted,
    ConcurrentPreparation,
    ConcurrentOwnStack,
    DemotedRegion,
    OwnerDetach,
    RegionReuse,
    AbandonedHistory,
    GlobalGCDetach,
    ThreadExit
};

enum class SatoriTrackingKind : uint32_t
{
    Start,
    End,
    BackoffSkip,
    History
};

enum class SatoriTrackingCheckpoint : uint32_t
{
    Initialization,
    LocalCollection,
    BeforeRebuild,
    Rebuilt,
    Final
};

struct SatoriTrackingState
{
    enum : uint8_t
    {
        LifetimePartial = 1,
        IntervalPartial = 2,
        Deferred = 4,
        Overflow = 8,
        StartedEnabled = 16,
        IntervalOverflow = 32,
        Rebuilding = 64,
        IdentityUnavailable = 128
    };

    uint64_t lifetime;
    uint64_t owner;
    uint64_t intervalEpoch;
    uint64_t lastEpoch;
    uint64_t sequence;
    uint32_t escapeFnCalls;
    uint32_t escapeRecursivelyCalls;
    uint32_t newlyEscapedObjects;
    uint32_t seedObjects;
    uint32_t segment;
    uint8_t flags;
    SatoriTrackingReason reason;
    uint8_t phase;
    uint8_t reservedByte;

    FORCEINLINE bool Begin(uint64_t id, uint64_t threadTag, uint64_t epoch)
    {
        if (phase == 1 || phase == 2)
        {
            return false;
        }

        lifetime = id;
        owner = threadTag;
        intervalEpoch = lastEpoch = epoch;
        sequence = 0;
        flags = (epoch & 1) ? StartedEnabled : LifetimePartial | IntervalPartial;
        if (id == 0)
        {
            flags |= IdentityUnavailable;
        }
        if (epoch >= UINT64_MAX - 1)
        {
            flags |= Overflow | LifetimePartial | IntervalPartial;
        }
        reason = SatoriTrackingReason::None;
        phase = 1;
        escapeFnCalls = escapeRecursivelyCalls = newlyEscapedObjects = seedObjects = 0;
        segment = 0;
        return true;
    }

    FORCEINLINE void ObserveEpoch(uint64_t epoch)
    {
        if (epoch != lastEpoch)
        {
            flags |= LifetimePartial | IntervalPartial;
            if (epoch >= UINT64_MAX - 1)
            {
                flags |= Overflow;
            }
            lastEpoch = epoch;
        }
    }

    // The barrier calls this before releasing physical ownership. Only scalar stores.
    FORCEINLINE void CaptureStop(SatoriTrackingReason cause, uint64_t epoch, bool deferred)
    {
        if (phase != 1)
        {
            return;
        }

        ObserveEpoch(epoch);
        reason = cause;
        if (deferred)
        {
            flags |= Deferred;
        }
        phase = 2;
    }

    FORCEINLINE bool TryDrain()
    {
        if (phase != 2)
        {
            return false;
        }

        phase = 3;
        return true;
    }

    FORCEINLINE void Count(uint32_t& counter, uint64_t epoch)
    {
        if (!(epoch & 1) || epoch == UINT64_MAX || phase != 1)
        {
            return;
        }

        ObserveEpoch(epoch);
        if (counter == UINT32_MAX)
        {
            flags |= Overflow | IntervalOverflow;
        }
        else
        {
            counter++;
        }
    }

    void ResetInterval(uint64_t epoch)
    {
        if (escapeFnCalls | escapeRecursivelyCalls | newlyEscapedObjects | seedObjects)
        {
            escapeFnCalls = escapeRecursivelyCalls = newlyEscapedObjects = seedObjects = 0;
        }
        intervalEpoch = lastEpoch = epoch;
        flags &= static_cast<uint8_t>(~(IntervalPartial | IntervalOverflow));
        if (!(epoch & 1) || epoch == UINT64_MAX)
        {
            flags |= LifetimePartial | IntervalPartial;
        }
    }

    void NextSegment()
    {
        if (segment == UINT32_MAX)
        {
            flags |= Overflow | IntervalOverflow;
        }
        else
        {
            segment++;
        }
    }

    uint64_t NextSequence()
    {
        if (sequence == UINT64_MAX)
        {
            flags |= Overflow;
            return sequence;
        }

        return ++sequence;
    }
};

static_assert(sizeof(SatoriTrackingState) == 64, "Tracking diagnostics must fit in the unused bitmap header.");

struct SatoriTrackingHistory
{
    size_t stackBase;
    uint8_t backoff;
    uint8_t skip;
    bool tracking;
    bool collected;
    bool attempted;
    uint64_t lifetime;
    uint64_t region;

    void End(bool outOfSpace, int cap)
    {
        if (!tracking)
        {
            return;
        }

        tracking = false;
        if (collected)
        {
            backoff = 0;
        }
        else if (!(outOfSpace && !attempted))
        {
            int next = backoff == 0 ? 1 : backoff * 2;
            backoff = static_cast<uint8_t>(next < cap ? next : cap);
            skip = backoff;
        }
    }
};

#endif // __SATORI_TRACKING_STATE_H__
