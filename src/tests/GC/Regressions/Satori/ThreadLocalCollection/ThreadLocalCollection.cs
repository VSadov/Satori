// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Threading;
using Xunit;

public static class ThreadLocalCollection
{
    private const int RegionSize = 2 * 1024 * 1024;
    private const int AllocationLimit = 32 * 1024 * 1024;
    private static object? s_published;

    private static bool IsSatori =>
        GC.GetConfigurationVariables().TryGetValue("SatoriGC", out object? value) && value is true;

    [Theory]
    [InlineData(70000)]
    [InlineData(80000)]
    [InlineData(84000)]
    public static void LargerAllocationKeepsEscapeTracking(int length)
    {
        if (!IsSatori)
        {
            return;
        }

        RunOnFreshThread(() =>
        {
            GC.Collect(2, GCCollectionMode.Forced, blocking: true);
            byte[] first = AcquireThreadLocalRegion();
            byte[] large = new byte[length];
            byte[] next = new byte[1024];

            Assert.Equal(0, GC.GetGeneration(first));
            Assert.Equal(0, GC.GetGeneration(next));

            GC.KeepAlive(first);
            GC.KeepAlive(large);
            GC.KeepAlive(next);
        });
    }

    [Fact]
    public static void ExistingFreeSpaceDoesNotResetBackoff()
    {
        if (!IsSatori)
        {
            return;
        }

        try
        {
            RunOnFreshThread(VerifyUnproductiveReuseBacksOff);
        }
        finally
        {
            s_published = null;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void VerifyUnproductiveReuseBacksOff()
    {
        byte[][] survivors = CreateSparseRegions();
        GC.Collect(2, GCCollectionMode.Forced, blocking: true);
        int globalCollections = GC.CollectionCount(1);

        byte[] first = AcquireThreadLocalRegion(survivors);
        nuint region = RegionFor(first);
        int localCollections = GC.CollectionCount(0);
        var live = new byte[128][];
        for (int i = 0; i < live.Length; i++)
        {
            live[i] = new byte[1024];
            Assert.Equal(region, RegionFor(live[i]));
        }

        Assert.True(GC.CollectionCount(0) > localCollections, "The reused region must have a thread-local collection.");
        Assert.Equal(0, GC.GetGeneration(first));
        s_published = live;

        bool stoppedTracking = false;
        for (int i = 0; i < 256; i++)
        {
            byte[] buffer = new byte[4096];
            Assert.Equal(region, RegionFor(buffer));
            s_published = buffer;
            if (GC.GetGeneration(buffer) != 0)
            {
                stoppedTracking = true;
                break;
            }
        }

        Assert.True(stoppedTracking, "Publishing more than the escape budget must stop tracking.");

        bool foundNextRegion = false;
        for (int allocated = 0; allocated < 2 * RegionSize; allocated += 16 * 1024)
        {
            byte[] buffer = new byte[16 * 1024];
            if (RegionFor(buffer) != region)
            {
                Assert.Equal(1, GC.GetGeneration(buffer));
                foundNextRegion = true;
                break;
            }
        }

        Assert.True(foundNextRegion, "The allocation workload must advance to another region.");
        Assert.Equal(globalCollections, GC.CollectionCount(1));
        GC.KeepAlive(first);
        GC.KeepAlive(live);
        GC.KeepAlive(survivors);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static byte[][] CreateSparseRegions()
    {
        var survivors = new List<byte[]>();
        nuint firstRegion = 0;
        nuint previousRegion = 0;
        int kept = 0;
        for (int allocated = 0; allocated < AllocationLimit; allocated += 4096)
        {
            byte[] buffer = GC.AllocateArray<byte>(4096, pinned: true);
            nuint region = RegionFor(buffer);
            if (firstRegion == 0)
            {
                firstRegion = region;
            }

            if (region != previousRegion)
            {
                previousRegion = region;
                kept = 0;
            }

            // Keep two objects after a large hole. Exclude the initial, potentially already-used pinned region.
            if (region != firstRegion && OffsetInRegion(buffer) >= RegionSize / 2 && kept < 2)
            {
                survivors.Add(buffer);
                kept++;
            }
        }

        Assert.True(survivors.Count >= 2);
        return survivors.ToArray();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static byte[] AcquireThreadLocalRegion(byte[][]? survivors = null)
    {
        for (int allocated = 0; allocated < AllocationLimit; allocated += 16 * 1024)
        {
            byte[] buffer = new byte[16 * 1024];
            if (GC.GetGeneration(buffer) == 0)
            {
                if (survivors is null)
                {
                    return buffer;
                }

                nuint region = RegionFor(buffer);
                foreach (byte[] survivor in survivors)
                {
                    if (RegionFor(survivor) == region)
                    {
                        return buffer;
                    }
                }
            }
        }

        throw new InvalidOperationException("Could not acquire the requested escape-tracked region.");
    }

    // Region alignment is part of Satori's object-address layout; no private header offsets are needed.
    private static unsafe nuint RegionFor(byte[] buffer)
    {
        fixed (byte* address = buffer)
        {
            return (nuint)address & ~(nuint)(RegionSize - 1);
        }
    }

    private static unsafe nuint OffsetInRegion(byte[] buffer)
    {
        fixed (byte* address = buffer)
        {
            return (nuint)address & (RegionSize - 1);
        }
    }

    private static void RunOnFreshThread(Action action)
    {
        ExceptionDispatchInfo? failure = null;
        var thread = new Thread(() =>
        {
            try
            {
                action();
            }
            catch (Exception exception)
            {
                failure = ExceptionDispatchInfo.Capture(exception);
            }
        });

        thread.Start();
        thread.Join();
        failure?.Throw();
    }
}
