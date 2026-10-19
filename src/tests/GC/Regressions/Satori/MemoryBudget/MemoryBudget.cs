// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Threading;
using Xunit;

public static class MemoryBudget
{
    private const ulong MiB = 1024 * 1024;
    private static readonly CountdownEvent s_started = new(4);
    private static readonly long[] s_allocations = new long[4];
    private static int s_stop;
    private static Exception? s_failure;

    [Fact]
    public static void TestEntryPoint()
    {
        if (!GC.GetConfigurationVariables().TryGetValue("SatoriGC", out object? satori) || satori is not true)
        {
            return;
        }

        SetLimit(256 * MiB);
        Thread[] threads = new Thread[4];
        for (int i = 0; i < threads.Length; i++)
        {
            int worker = i;
            threads[i] = new Thread(() => Allocate(worker)) { IsBackground = true };
            threads[i].Start();
        }

        int accepted = 0;
        try
        {
            Assert.True(s_started.Wait(TimeSpan.FromSeconds(30)));
            ulong[] limits = [64 * MiB, 96 * MiB, 128 * MiB, 256 * MiB];
            for (int iteration = 0; iteration < 512; iteration++)
            {
                ulong limit = limits[iteration % limits.Length];
                AppContext.SetData("GCHeapHardLimit", limit);
                try
                {
                    GC.RefreshMemoryLimit();
                }
                catch (InvalidOperationException)
                {
                    continue;
                }

                accepted++;
                GCMemoryInfo info = GC.GetGCMemoryInfo();
                Assert.Equal((long)limit, info.TotalAvailableMemoryBytes);
                Assert.True(info.TotalCommittedBytes <= (long)limit);
                Thread.Yield();
            }
        }
        finally
        {
            Volatile.Write(ref s_stop, 1);
            foreach (Thread thread in threads)
            {
                Assert.True(thread.Join(TimeSpan.FromSeconds(30)));
            }
            SetLimit(256 * MiB);
        }

        Assert.Null(s_failure);
        Assert.True(accepted > 0);
        foreach (long count in s_allocations)
        {
            Assert.True(count > 0);
        }

        GC.Collect(2, GCCollectionMode.Aggressive, blocking: true, compacting: true);
        long committed = GC.GetGCMemoryInfo().TotalCommittedBytes;
        SetLimit((ulong)committed + 16 * MiB);
        byte[] afterExit = new byte[4 * 1024 * 1024];
        afterExit[^1] = 42;
        GC.KeepAlive(afterExit);
    }

    private static void Allocate(int worker)
    {
        long count = 0;
        try
        {
            byte[]?[] retained = new byte[16][];
            byte[] first = new byte[128 * 1024];
            first[0] = 42;
            retained[0] = first;
            count = 1;
            s_started.Signal();
            while (Volatile.Read(ref s_stop) == 0)
            {
                try
                {
                    int slot = (int)(count % retained.Length);
                    byte[]? previous = retained[slot];
                    if (previous is not null)
                    {
                        Assert.Equal(42, previous[0]);
                    }

                    byte[] item = new byte[128 * 1024];
                    item[0] = 42;
                    retained[slot] = item;
                    byte[] temporary = new byte[(1 + (int)(count % 4)) * 1024 * 1024];
                    temporary[^1] = 7;
                    GC.KeepAlive(temporary);
                    count++;
                }
                catch (OutOfMemoryException)
                {
                    Thread.Yield();
                }
            }
            GC.KeepAlive(retained);
        }
        catch (Exception failure)
        {
            Interlocked.CompareExchange(ref s_failure, failure, null);
        }
        finally
        {
            s_allocations[worker] = count;
        }
    }

    private static void SetLimit(ulong bytes)
    {
        AppContext.SetData("GCHeapHardLimit", bytes);
        GC.RefreshMemoryLimit();
        Assert.Equal((long)bytes, GC.GetGCMemoryInfo().TotalAvailableMemoryBytes);
    }
}
