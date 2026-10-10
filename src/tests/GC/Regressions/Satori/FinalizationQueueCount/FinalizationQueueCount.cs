// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using System.Threading;
using Xunit;

public static class FinalizationQueueCount
{
    // Match the initial queue capacity in SatoriFinalizationQueue.cpp.
#if DEBUG
    private const int QueueCapacity = 32;
#else
    private const int QueueCapacity = 4096;
#endif

    private static readonly ManualResetEventSlim s_finalizerStarted = new(false);
    private static readonly ManualResetEventSlim s_releaseFinalizer = new(false);
    private static int s_finalizedCount;

    [Fact]
    public static int TestEntryPoint()
    {
        if (!GC.GetConfigurationVariables().TryGetValue("SatoriGC", out object? satori) || satori is not true)
        {
            return 100;
        }

        GC.Collect(2, GCCollectionMode.Forced, blocking: true);
        GC.WaitForPendingFinalizers();

        for (int iteration = 0; iteration < 2; iteration++)
        {
            if (!FillQueueAndVerifyCounts())
            {
                return 1;
            }

            int expectedFinalizers = (iteration + 1) * QueueCapacity;
            if (Volatile.Read(ref s_finalizedCount) != expectedFinalizers || !VerifyPendingCount(0))
            {
                Console.WriteLine($"Expected {expectedFinalizers} drained finalizers; observed {s_finalizedCount}.");
                return 2;
            }
        }

        return 100;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool FillQueueAndVerifyCounts()
    {
        s_finalizerStarted.Reset();
        s_releaseFinalizer.Reset();

        try
        {
            AllocateBlocker();
            GC.Collect(2, GCCollectionMode.Forced, blocking: true);
            if (!s_finalizerStarted.Wait(TimeSpan.FromSeconds(30)))
            {
                throw new InvalidOperationException("The blocking finalizer did not start.");
            }

            if (!VerifyPendingCount(0))
            {
                return false;
            }

            PrepareThreadLocalRegion();
            int globalCollections = GC.CollectionCount(1);
            int queued = 0;
            foreach (int count in new[] { 1, QueueCapacity - 2, 1 })
            {
                AllocateFinalizables(count);
                TriggerThreadLocalCollection();
                queued += count;

                if (GC.CollectionCount(1) != globalCollections)
                {
                    throw new InvalidOperationException("A global collection would not exercise the queue's single-item enqueue path.");
                }

                if (!VerifyPendingCount(queued))
                {
                    return false;
                }
            }

            return true;
        }
        finally
        {
            s_releaseFinalizer.Set();
            GC.WaitForPendingFinalizers();
        }
    }

    private static bool VerifyPendingCount(int expected)
    {
        long pending = GC.GetGCMemoryInfo().FinalizationPendingCount;
        if (pending != expected)
        {
            Console.WriteLine($"Expected {expected} pending finalizers; observed {pending}.");
            return false;
        }

        return true;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void AllocateBlocker()
    {
        _ = new BlockingFinalizable();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void AllocateFinalizables(int count)
    {
        for (int i = 0; i < count; i++)
        {
            var value = new Finalizable();
            if (GC.GetGeneration(value) != 0)
            {
                throw new InvalidOperationException("The finalizable objects must be in the thread-local Gen0 region.");
            }

            GC.KeepAlive(value);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void PrepareThreadLocalRegion()
    {
        const int AllocationSize = 16 * 1024;
        const int AllocationLimit = 16 * 1024 * 1024;

        for (int allocated = 0; allocated < AllocationLimit; allocated += AllocationSize)
        {
            byte[] buffer = new byte[AllocationSize];
            if (GC.GetGeneration(buffer) == 0)
            {
                return;
            }
        }

        throw new InvalidOperationException("Could not acquire an escape-tracked Gen0 allocation region.");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void TriggerThreadLocalCollection()
    {
        const int AllocationSize = 1024;
        const int AllocationLimit = 16 * 1024 * 1024;
        int collections = GC.CollectionCount(0);

        for (int allocated = 0; allocated < AllocationLimit; allocated += AllocationSize)
        {
            GC.KeepAlive(new byte[AllocationSize]);
            if (GC.CollectionCount(0) != collections)
            {
                return;
            }
        }

        throw new InvalidOperationException("The allocation workload did not trigger a thread-local collection.");
    }

    private sealed class Finalizable
    {
        ~Finalizable()
        {
            Interlocked.Increment(ref s_finalizedCount);
        }
    }

    private sealed class BlockingFinalizable
    {
        ~BlockingFinalizable()
        {
            s_finalizerStarted.Set();
            s_releaseFinalizer.Wait();
        }
    }
}
