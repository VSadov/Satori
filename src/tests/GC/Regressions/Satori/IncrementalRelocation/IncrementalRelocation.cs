// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Diagnostics;
using System.Runtime;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Xunit;

// Incremental relocation moves a few sparse Gen2 regions and updates only the references that the concurrent marking recorded.
// In low latency mode regular relocation is off, thus a Gen2 GC that compacts there relocated incrementally.
// The test makes sparse Gen2 regions, refers to their survivors from other objects, statics, handles and through
// finalization, rewrites references while the GCs are concurrent, and checks every reference after every GC.
public static class IncrementalRelocation
{
    // 1 of this many nodes survives, so the regions of the nodes are sparse and have no large free spans.
    private const int NodesPerSurvivor = 8;
    private const int SurvivorCount = 64 * 1024;
    private const int FinalizableEvery = 64;
    private const int HandleEvery = 997;
    private const int RequiredCompactingGCs = 2;
    private const int MaxGCs = 64;
    private const ulong CheckMultiplier = 0x9E3779B97F4A7C15;

    private static Node[] s_survivors = Array.Empty<Node>();
    private static int[] s_peerIndex = Array.Empty<int>();
    private static Node? s_static;
    private static GCHandle[] s_strongHandles = Array.Empty<GCHandle>();
    private static GCHandle[] s_weakHandles = Array.Empty<GCHandle>();
    private static int s_finalizableCount;
    private static int s_finalizedCount;
    private static int s_finalizedWrong;

    [Fact]
    public static int TestEntryPoint()
    {
        if (!GC.GetConfigurationVariables().TryGetValue("SatoriGC", out object? satori) || satori is not true)
        {
            return 100;
        }

        MakeSparseGen2();

        GCLatencyMode latencyMode = GCSettings.LatencyMode;
        GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency;
        int compactingGCs = 0;
        try
        {
            var random = new Random(1);
            for (int i = 0; i < MaxGCs && compactingGCs < RequiredCompactingGCs; i++)
            {
                if (!CollectConcurrently(random))
                {
                    Console.WriteLine("A concurrent Gen2 GC did not complete.");
                    return 4;
                }

                GCMemoryInfo info = GC.GetGCMemoryInfo(GCKind.FullBlocking);
                bool compacted = info.Generation == 2 && info.Compacted;
                if (compacted)
                {
                    compactingGCs++;
                }

                if (!Verify())
                {
                    return 1;
                }

                // one relocation usually takes all the sparse regions, a new sparse set gives the next one something to move.
                if (compacted && compactingGCs < RequiredCompactingGCs)
                {
                    ReleaseSurvivors();
                    MakeSparseGen2();
                }
            }
        }
        finally
        {
            GCSettings.LatencyMode = latencyMode;
        }

        if (compactingGCs < RequiredCompactingGCs)
        {
            Console.WriteLine($"Expected {RequiredCompactingGCs} Gen2 GCs that relocate incrementally, observed {compactingGCs}.");
            return 2;
        }

        // finalization trackers of the relocated finalizable objects must refer to their new locations.
        ReleaseSurvivors();
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        GC.WaitForPendingFinalizers();

        if (Volatile.Read(ref s_finalizedWrong) != 0 || Volatile.Read(ref s_finalizedCount) != s_finalizableCount)
        {
            Console.WriteLine($"Expected {s_finalizableCount} finalizers, observed {s_finalizedCount}, wrong {s_finalizedWrong}.");
            return 3;
        }

        return 100;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void MakeSparseGen2()
    {
        // allocated in sequence, thus the survivors are spread evenly over the regions of the nodes.
        // finalizable nodes are survivors, every FinalizableEvery-th one.
        var all = new Node[SurvivorCount * NodesPerSurvivor];
        for (int i = 0; i < all.Length; i++)
        {
            all[i] = i % (NodesPerSurvivor * FinalizableEvery) == 0 ? new FinalizableNode(i) : new Node(i);
        }

        // everything is alive here, so this only tenures the nodes.
        GC.Collect(2, GCCollectionMode.Forced, blocking: true);
        GC.Collect(2, GCCollectionMode.Forced, blocking: true);

        s_survivors = new Node[SurvivorCount];
        s_peerIndex = new int[SurvivorCount];
        for (int i = 0; i < SurvivorCount; i++)
        {
            s_survivors[i] = all[i * NodesPerSurvivor];
            if (s_survivors[i] is FinalizableNode)
            {
                s_finalizableCount++;
            }
        }

        for (int i = 0; i < SurvivorCount; i++)
        {
            s_survivors[i].Next = s_survivors[(i + 1) % SurvivorCount];
            int peer = (int)((long)i * 7919 % SurvivorCount);
            s_survivors[i].Peer = s_survivors[peer];
            s_peerIndex[i] = peer;
        }

        s_static = s_survivors[SurvivorCount / 2];

        int handleCount = SurvivorCount / HandleEvery;
        s_strongHandles = new GCHandle[handleCount];
        s_weakHandles = new GCHandle[handleCount];
        for (int i = 0; i < handleCount; i++)
        {
            s_strongHandles[i] = GCHandle.Alloc(s_survivors[i * HandleEvery], GCHandleType.Normal);
            s_weakHandles[i] = GCHandle.Alloc(s_survivors[i * HandleEvery + 1], GCHandleType.Weak);
        }
    }

    // Starts a concurrent Gen2 GC and waits for it to complete, rewriting references meanwhile.
    private static bool CollectConcurrently(Random random)
    {
        int gen2Count = GC.CollectionCount(2);
        Stopwatch stopwatch = Stopwatch.StartNew();
        while (GC.CollectionCount(2) == gen2Count)
        {
            if (stopwatch.Elapsed > TimeSpan.FromMinutes(2))
            {
                return false;
            }

            // starts the GC, or helps with it if it is already in progress.
            GC.Collect(2, GCCollectionMode.Forced, blocking: false);
            for (int j = 0; j < 16; j++)
            {
                int index = random.Next(SurvivorCount);
                int peer = random.Next(SurvivorCount);
                s_survivors[index].Peer = s_survivors[peer];
                s_peerIndex[index] = peer;
            }

            Thread.Sleep(1);
        }

        return true;
    }

    private static bool Verify()
    {
        for (int i = 0; i < SurvivorCount; i++)
        {
            Node node = s_survivors[i];
            if (!node.IsIntact(i * NodesPerSurvivor))
            {
                Console.WriteLine($"Survivor {i} has id {node.Id} and check {node.Check:X}.");
                return false;
            }

            if (!ReferenceEquals(node.Next, s_survivors[(i + 1) % SurvivorCount]) ||
                !ReferenceEquals(node.Peer, s_survivors[s_peerIndex[i]]))
            {
                Console.WriteLine($"Survivor {i} refers to wrong objects.");
                return false;
            }
        }

        if (!ReferenceEquals(s_static, s_survivors[SurvivorCount / 2]))
        {
            Console.WriteLine("The static field refers to a wrong object.");
            return false;
        }

        for (int i = 0; i < s_strongHandles.Length; i++)
        {
            if (!ReferenceEquals(s_strongHandles[i].Target, s_survivors[i * HandleEvery]) ||
                !ReferenceEquals(s_weakHandles[i].Target, s_survivors[i * HandleEvery + 1]))
            {
                Console.WriteLine($"Handle {i} refers to a wrong object.");
                return false;
            }
        }

        return true;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void ReleaseSurvivors()
    {
        for (int i = 0; i < s_strongHandles.Length; i++)
        {
            s_strongHandles[i].Free();
            s_weakHandles[i].Free();
        }

        s_survivors = Array.Empty<Node>();
        s_static = null;
    }

    private class Node
    {
        public readonly long Id;
        public readonly ulong Check;
        public Node? Next;
        public Node? Peer;

        public Node(long id)
        {
            Id = id;
            Check = (ulong)id * CheckMultiplier;
        }

        public bool IsIntact(long id) => Id == id && Check == (ulong)id * CheckMultiplier;
    }

    private sealed class FinalizableNode : Node
    {
        public FinalizableNode(long id) : base(id)
        {
        }

        ~FinalizableNode()
        {
            if (!IsIntact(Id) || Id % (NodesPerSurvivor * FinalizableEvery) != 0)
            {
                Interlocked.Increment(ref s_finalizedWrong);
            }

            Interlocked.Increment(ref s_finalizedCount);
        }
    }
}
