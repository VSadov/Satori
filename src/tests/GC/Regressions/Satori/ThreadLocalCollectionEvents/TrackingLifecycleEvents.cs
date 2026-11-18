// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics.Tracing;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;
using Microsoft.Diagnostics.NETCore.Client;
using Microsoft.Diagnostics.Tracing;
using Tracing.Tests.Common;
using Xunit;

public static class TrackingLifecycleEvents
{
    private static object? s_published;
    private static readonly object[] s_destination = new object[3];
    private static int s_ownerNativeId;

    [DllImport("kernel32.dll")]
    private static extern int GetCurrentThreadId();

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    [InlineData(5)]
    [InlineData(6)]
    [InlineData(7)]
    [InlineData(8)]
    [InlineData(9)]
    public static void LifecycleAndObservedCounts(int scenario)
    {
        Assert.True(GC.GetConfigurationVariables().TryGetValue("SatoriGC", out object? value) && value is true);
        ValidateManifest();
        if (scenario == 9)
        {
            var disabled = new Thread(GenerateLiveVector);
            disabled.Start();
            disabled.Join();
        }
        var providers = new List<EventPipeProvider>
        {
            new EventPipeProvider("Microsoft-Windows-DotNETRuntime", EventLevel.Verbose, 1)
        };
        var expected = new Dictionary<string, ExpectedEventCount>
        {
            { "Microsoft-Windows-DotNETRuntime", -1 }
        };
        byte[]? rebuildHeadroom = null;
        if (scenario == 8)
        {
            rebuildHeadroom = new byte[32 * 1024 * 1024];
            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: false);
        }
        Assert.Equal(100, IpcTraceTest.RunAndValidateEventCounts(expected,
            () =>
            {
                if (scenario is 7 or 8)
                {
                    using var stopped = new AutoResetEvent(false);
                    using var release = new AutoResetEvent(false);
                    using var ownerDone = new ManualResetEvent(false);
                    using var cancellation = new CancellationTokenSource();
                    Exception? failure = null;
                    var thread = new Thread(() =>
                    {
                        try
                        {
                            if (scenario == 7)
                            {
                                GenerateDeferredStop(stopped, release);
                            }
                            else
                            {
                                GenerateRebuildCounts(stopped, release, cancellation.Token);
                            }
                        }
                        catch (Exception exception)
                        {
                            Volatile.Write(ref failure, exception);
                        }
                        finally
                        {
                            ownerDone.Set();
                            stopped.Set();
                        }
                    });
                    thread.Start();
                    try
                    {
                        do
                        {
                            if (scenario == 8 && ownerDone.WaitOne(0))
                            {
                                break;
                            }
                            Assert.True(stopped.WaitOne(TimeSpan.FromSeconds(60)), "Owner boundary was not reached.");
                            Assert.Null(Volatile.Read(ref failure));
                            if (scenario == 8 && ownerDone.WaitOne(0))
                            {
                                break;
                            }
                            GC.Collect(1, GCCollectionMode.Forced, blocking: true, compacting: false);
                            release.Set();
                        }
                        while (scenario == 8);
                    }
                    finally
                    {
                        cancellation.Cancel();
                        release.Set();
                        Assert.True(thread.Join(TimeSpan.FromSeconds(60)), "Owner thread did not finish.");
                    }
                    Assert.Null(Volatile.Read(ref failure));
                }
                else
                {
                    var thread = new Thread(() => Generate(scenario, providers));
                    thread.Start();
                    thread.Join();
                }
                s_published = null;
                Array.Clear(s_destination);
            }, providers, 32, source => Validate(source, scenario)));
        GC.KeepAlive(rebuildHeadroom);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void GenerateDeferredStop(AutoResetEvent stopped, AutoResetEvent release)
    {
        byte[] anchor = AcquireTrackedRegion();
        int remaining = 16384;
        while (GC.GetGeneration(anchor) == 0 && --remaining > 0)
        {
            s_published = new byte[4096];
        }
        Assert.True(remaining > 0);
        stopped.Set();
        release.WaitOne();
        GC.KeepAlive(anchor);
    }

    [Fact]
    public static void LateAttachAndOffOnGapRemainPartial()
    {
        var providers = new List<EventPipeProvider>
        {
            new EventPipeProvider("Microsoft-Windows-DotNETRuntime", EventLevel.Verbose, 1)
        };
        var expected = new Dictionary<string, ExpectedEventCount>
        {
            { "Microsoft-Windows-DotNETRuntime", -1 }
        };
        using var actions = new BlockingCollection<Action>();
        var thread = new Thread(() =>
        {
            foreach (Action action in actions.GetConsumingEnumerable())
            {
                action();
            }
        });
        thread.Start();
        void Execute(Action action)
        {
            Exception? failure = null;
            using var completed = new ManualResetEventSlim();
            actions.Add(() =>
            {
                try
                {
                    action();
                }
                catch (Exception exception)
                {
                    failure = exception;
                }
                finally
                {
                    completed.Set();
                }
            });
            Assert.True(completed.Wait(TimeSpan.FromSeconds(60)), "Owner action timed out.");
            Assert.Null(failure);
        }

        byte[] anchor = Array.Empty<byte>();
        var starts = new HashSet<ulong>();
        var lateEnds = new HashSet<ulong>();
        var gapEnds = new HashSet<ulong>();
        try
        {
            Execute(() =>
            {
                if (OperatingSystem.IsWindows())
                {
                    s_ownerNativeId = GetCurrentThreadId();
                }
                anchor = AcquireTrackedRegion();
            });
            Assert.Equal(100, IpcTraceTest.RunAndValidateEventCounts(expected, () =>
            {
                Execute(() =>
                {
                    Allocate(4096, publish: true);
                    anchor = AcquireTrackedRegion();
                });
            }, providers, 32, source => Validate(source, -1, starts, lateEnds)));
            Assert.NotEmpty(lateEnds);
            Execute(() =>
            {
                s_published = anchor;
                for (int i = 0; i < 128; i++)
                {
                    GCHandle handle = GCHandle.Alloc(anchor);
                    handle.Free();
                }
                Assert.Equal(0, GC.GetGeneration(anchor));
            });
            Assert.Equal(100, IpcTraceTest.RunAndValidateEventCounts(expected, () =>
            {
                Execute(() => Allocate(4096, publish: true));
                actions.CompleteAdding();
                thread.Join();
            }, providers, 32, source => Validate(source, -1, partialEnds: gapEnds)));
            Assert.True(starts.Overlaps(gapEnds), "No physical lifetime survived the off/on gap.");
        }
        finally
        {
            actions.CompleteAdding();
            thread.Join();
            s_published = null;
            GC.KeepAlive(anchor);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Generate(int scenario, List<EventPipeProvider> providers)
    {
        byte[] anchor = AcquireTrackedRegion();
        if (scenario == 2)
        {
            object[] graph = new object[3];
            graph[0] = graph;
            graph[1] = graph[2] = new byte[128];
            s_published = graph;
            for (int i = 0; i < 128; i++)
            {
                Array.Copy(graph, s_destination, 3);
                s_published = graph;
            }
        }
        if (scenario == 3)
        {
            GC.Collect(1, GCCollectionMode.Forced, blocking: true, compacting: false);
        }
        if (scenario == 4)
        {
            using EventPipeSession second = new DiagnosticsClient(Environment.ProcessId)
                .StartEventPipeSession(providers, requestRundown: false, circularBufferMB: 4);
            Task drain = Task.Run(() => second.EventStream.CopyTo(System.IO.Stream.Null));
            Allocate(1024, publish: false);
            second.Stop();
            drain.GetAwaiter().GetResult();
        }
        if (scenario == 5)
        {
            AllocateFinalizables();
        }
        if (scenario == 6)
        {
            object[] graph = new object[] { new byte[128] };
            for (int i = 0; i < 128; i++)
            {
                GCHandle handle = GCHandle.Alloc(graph);
                handle.Free();
            }
        }
        if (scenario == 9)
        {
            GenerateLiveVector();
        }
        Allocate(scenario == 1 ? 4096 : 1024, publish: scenario == 1);
        GC.KeepAlive(anchor);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void GenerateLiveVector()
    {
        byte[] anchor = AcquireTrackedRegion();
        Vector128<int> value = Vector128<int>.Zero;
        Vector128<int> increment = Vector128.Create(1, 2, 3, 4);
        for (int i = 0; i < 32768; i++)
        {
            s_published = new byte[4096];
            value += increment;
        }
        for (int lane = 0; lane < Vector128<int>.Count; lane++)
        {
            Assert.Equal(32768 * (lane + 1), value.GetElement(lane));
        }
        GC.KeepAlive(anchor);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void GenerateRebuildCounts(AutoResetEvent request, AutoResetEvent resume,
        CancellationToken cancellationToken)
    {
        byte[] anchor = Array.Empty<byte>();
        if (OperatingSystem.IsWindows())
        {
            s_ownerNativeId = GetCurrentThreadId();
        }
        bool prepared = false;
        for (int attempt = 0; attempt < 16; attempt++)
        {
            anchor = AcquireTrackedRegion();
            object[] warmup = new object[] { anchor };
            GCHandle warmupHandle = GCHandle.Alloc(warmup);
            warmupHandle.Free();
            Array.Copy(warmup, s_destination, 1);
            request.Set();
            if (!WaitForOwnerResume(resume, cancellationToken))
            {
                return;
            }
            if (GC.GetGeneration(anchor) == 0)
            {
                prepared = true;
                break;
            }
        }
        Assert.True(prepared, "No physical tracked lifetime survived the warmup GC; demoted reused regions may stop tracking.");
        object direct = new object();
        Assert.Equal(0, GC.GetGeneration(direct));
        s_published = direct;
        request.Set();
        if (!WaitForOwnerResume(resume, cancellationToken))
        {
            return;
        }
        object[] graph = new object[3];
        graph[0] = graph;
        graph[1] = graph[2] = new byte[128];
        Assert.Equal(0, GC.GetGeneration(graph));
        s_published = graph;
        for (int i = 0; i < 128; i++)
        {
            s_published = graph;
            Array.Copy(graph, s_destination, 3);
            GCHandle handle = GCHandle.Alloc(graph);
            handle.Free();
        }
        request.Set();
        if (!WaitForOwnerResume(resume, cancellationToken))
        {
            return;
        }
        GC.KeepAlive(anchor);
        GC.KeepAlive(direct);
        GC.KeepAlive(graph);
    }

    private static bool WaitForOwnerResume(AutoResetEvent resume, CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested)
        {
            return false;
        }
        Assert.True(resume.WaitOne(TimeSpan.FromSeconds(60)), "Owner was not resumed.");

        return !cancellationToken.IsCancellationRequested;
    }

    private sealed class FinalizableGraph
    {
        private readonly byte[] _payload = new byte[128];

        ~FinalizableGraph() => GC.KeepAlive(_payload);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void AllocateFinalizables()
    {
        for (int i = 0; i < 512; i++)
        {
            _ = new FinalizableGraph();
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static byte[] AcquireTrackedRegion()
    {
        for (int i = 0; i < 16 * 1024; i++)
        {
            byte[] buffer = new byte[4096];
            if (GC.GetGeneration(buffer) == 0)
            {
                return buffer;
            }
        }
        throw new InvalidOperationException("No actual escape-tracked Satori region was acquired within 64 MiB.");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Allocate(int length, bool publish)
    {
        for (int i = 0; i < 32768; i++)
        {
            byte[] buffer = new byte[length];
            if (publish)
            {
                s_published = buffer;
            }
            GC.KeepAlive(buffer);
        }
    }

    private static Func<int> Validate(EventPipeEventSource source, int scenario,
        HashSet<ulong>? startsObserved = null, HashSet<ulong>? partialEnds = null)
    {
        var lifetimes = new Dictionary<(int Pid, ushort Clr, ulong Lifetime), List<byte[]>>();
        var startThreads = new Dictionary<(int Pid, ushort Clr, ulong Lifetime), int>();
        var indices = new HashSet<(int Pid, ushort Clr, uint Index)>();
        int checkpoints = 0;
        int barrierStops = 0;
        int rebuilds = 0;
        ulong newlyEscaped = 0;
        ulong recursive = 0;
        ulong barrier = 0;
        source.Dynamic.All += data =>
        {
            if (data.ProviderName != "Microsoft-Windows-DotNETRuntime")
            {
                return;
            }
            int id = (int)data.ID;
            byte[] bytes = data.EventData();
            if (id == 239)
            {
                indices.Add((data.ProcessID, BinaryPrimitives.ReadUInt16LittleEndian(bytes),
                    BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(2))));
                return;
            }
            if (id is not (236 or 237))
            {
                return;
            }
            Assert.Equal(0, data.Version);
            Assert.Equal(5, (int)data.Level);
            Assert.Equal(1UL, (ulong)data.Keywords);
            Assert.Equal(86, bytes.Length);
            ushort clr = BinaryPrimitives.ReadUInt16LittleEndian(bytes);
            ulong lifetime = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(18));
            ulong sequence = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(42));
            if (sequence != 0)
            {
                var key = (data.ProcessID, clr, lifetime);
                if (!lifetimes.TryGetValue(key, out List<byte[]>? records))
                {
                    lifetimes.Add(key, records = new List<byte[]>());
                }
                // Preserve the family without depending on parser metadata for these unreleased events.
                records.Add(new[] { (byte)id }.Concat(bytes).ToArray());
                if (id == 237 && BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(50)) == 0)
                {
                    startThreads[key] = data.ThreadID;
                }
            }
            if (id == 236)
            {
                checkpoints++;
                uint kind = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(54));
                Assert.InRange(kind, 0u, 4u);
                uint hasIndex = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(62));
                Assert.InRange(hasIndex, 0u, 1u);
                Assert.Equal(kind == 1, hasIndex == 1);
                if (hasIndex == 1)
                {
                    uint index = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(66));
                    Assert.Contains((data.ProcessID, clr, index), indices);
                }
                uint fn = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(70));
                uint rec = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(74));
                uint flags = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(58));
                if ((flags & (2u | 32u)) == 0)
                {
                    Assert.True(rec >= fn);
                }
                barrier += fn;
                recursive += rec;
                newlyEscaped += BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(78));
                if (kind == 3)
                {
                    rebuilds++;
                }
            }
            else
            {
                uint kind = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(50));
                Assert.InRange(kind, 0u, 3u);
                if (kind == 1)
                {
                    uint reason = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(54));
                    Assert.InRange(reason, 1u, 13u);
                    if (reason == 1)
                    {
                        barrierStops++;
                        Assert.NotEqual(0u, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(58)) & 4);
                    }
                }
            }
        };
        return () =>
        {
            Assert.Equal(0, source.EventsLost);
            Assert.True(checkpoints > 0, "No actual tracking checkpoints were observed.");
            int complete = 0;
            int noCollectionRemainders = 0;
            int directCheckpoints = 0;
            int graphCheckpoints = 0;
            bool ownerOnly = OperatingSystem.IsWindows() && (startsObserved is not null || partialEnds is not null);
            ulong ownerTag = 0;
            if (ownerOnly)
            {
                var ownerStart = lifetimes.First(pair => startThreads.GetValueOrDefault(pair.Key) == s_ownerNativeId);
                ownerTag = BinaryPrimitives.ReadUInt64LittleEndian(ownerStart.Value[0].AsSpan(3));
            }
            foreach (var lifetimeRecords in lifetimes)
            {
                List<byte[]> records = lifetimeRecords.Value;
                if (scenario == 8 && OperatingSystem.IsWindows() &&
                    startThreads.GetValueOrDefault(lifetimeRecords.Key) == s_ownerNativeId)
                {
                    Console.WriteLine($"Owner lifetime {lifetimeRecords.Key.Lifetime}: " +
                        string.Join(";", records.Select(r => Convert.ToHexString(r))));
                    foreach (byte[] checkpoint in records.Where(r => r[0] == 236 &&
                        BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(55)) == 3))
                    {
                        Assert.NotEqual(0u, BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(59)) & 64);
                        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(71)));
                        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(75)));
                        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(79)));
                        Assert.True(BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(83)) > 0,
                            "Global-GC reconstruction must report seed marks separately from new escapes.");
                    }
                    foreach (byte[] checkpoint in records.Where(r => r[0] == 236 &&
                        BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(55)) == 2))
                    {
                        uint segment = BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(51));
                        if (segment is 1 or 2)
                        {
                            Assert.Equal(segment == 1 ? 1u : 2u,
                                BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(79)));
                            Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(83)));
                            if (segment == 1)
                            {
                                Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(71)));
                                Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(75)));
                                directCheckpoints++;
                            }
                            else
                            {
                                Assert.True(BinaryPrimitives.ReadUInt32LittleEndian(checkpoint.AsSpan(75)) >= 128);
                                graphCheckpoints++;
                            }
                        }
                    }
                }
                records.Sort((a, b) => BinaryPrimitives.ReadUInt64LittleEndian(a.AsSpan(43))
                    .CompareTo(BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan(43))));
                Assert.Equal(records.Count, records.Select(r => BinaryPrimitives.ReadUInt64LittleEndian(r.AsSpan(43))).Distinct().Count());
                int starts = records.Count(r => r[0] == 237 && BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(51)) == 0);
                int ends = records.Count(r => r[0] == 237 && BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(51)) == 1);
                Assert.InRange(starts, 0, 1);
                Assert.InRange(ends, 0, 1);
                ulong lifetime = BinaryPrimitives.ReadUInt64LittleEndian(records[0].AsSpan(19));
                bool owner = !ownerOnly || BinaryPrimitives.ReadUInt64LittleEndian(records[0].AsSpan(3)) == ownerTag;
                if (starts == 1 && owner)
                {
                    startsObserved?.Add(lifetime);
                }
                if (starts == 0 && ends == 1 && owner && records.Any(r =>
                    (BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(59)) & 1) != 0))
                {
                    partialEnds?.Add(lifetime);
                }
                if (starts == 1 && ends == 1)
                {
                    for (int i = 0; i < records.Count; i++)
                    {
                        Assert.Equal((ulong)i + 1, BinaryPrimitives.ReadUInt64LittleEndian(records[i].AsSpan(43)));
                    }
                    Assert.Single(records, r => r[0] == 236 && BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(55)) == 4);
                    if (!records.Any(r => r[0] == 236 && BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(55)) == 1) &&
                        records.Any(r => r[0] == 236 && BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(55)) == 4 &&
                            BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(71)) > 0 &&
                            BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(79)) > 0) &&
                        records.Any(r => r[0] == 237 && BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(51)) == 1 &&
                            BinaryPrimitives.ReadUInt32LittleEndian(r.AsSpan(55)) == 1))
                    {
                        noCollectionRemainders++;
                    }
                    complete++;
                }
            }
            Assert.True(complete > 0, "No trace-observed start-to-end lifetime was exercised.");
            if (scenario == 1)
            {
                Assert.True(barrierStops > 0 && barrier > 0 && newlyEscaped > 0);
            }
            if (scenario == 2)
            {
                Assert.True(recursive > 0 && newlyEscaped >= 2);
            }
            if (scenario == 3)
            {
                Assert.True(rebuilds > 0, "Blocking GC did not preserve and reconstruct an actual tracked lifetime.");
            }
            if (scenario == 5)
            {
                Assert.True(newlyEscaped >= 512, "Finalizer publication was not included before checkpoint publication.");
            }
            if (scenario == 6)
            {
                Assert.True(recursive > barrier && newlyEscaped >= 2,
                    "Handle creation must enter recursive escape independently of the write-barrier entry.");
            }
            if (scenario == 7)
            {
                Assert.True(barrierStops > 0 && noCollectionRemainders > 0,
                    "GC must drain the original barrier stop and remainder without another owner allocation or local collection.");
            }
            if (scenario == 8 && OperatingSystem.IsWindows())
            {
                Assert.True(directCheckpoints == 1 && graphCheckpoints == 1,
                    $"Owner OS thread {s_ownerNativeId}: direct checkpoints={directCheckpoints}, graph checkpoints={graphCheckpoints}; " +
                    $"start emitter threads={string.Join(',', startThreads.Values.Distinct())}");
            }
            return 100;
        };
    }

    private static void ValidateManifest()
    {
        using System.IO.Stream stream = typeof(TrackingLifecycleEvents).Assembly
            .GetManifestResourceStream("SatoriEventManifest")!;
        XElement provider = XDocument.Load(stream).Descendants().Single(e => e.Name.LocalName == "provider" &&
            (string?)e.Attribute("name") == "Microsoft-Windows-DotNETRuntime");
        const string Common = "ClrInstanceID:UInt16,ThreadTag:UInt64,RegionID:UInt64,Lifetime:UInt64,";
        foreach ((int id, string expected) in new[]
        {
            (237, Common + "RelatedLifetime:UInt64,Epoch:UInt64,Sequence:UInt64,Kind:UInt32,Reason:UInt32,Flags:UInt32,BackoffBefore:UInt32,BackoffAfter:UInt32,SkipBefore:UInt32,SkipAfter:UInt32,Attempted:Boolean,Productive:Boolean"),
            (236, Common + "EpochBegin:UInt64,EpochEnd:UInt64,Sequence:UInt64,Segment:UInt32,Kind:UInt32,Flags:UInt32,HasCollectionIndex:Boolean,CollectionIndex:UInt32,EscapeFnCalls:UInt32,EscapeRecursivelyCalls:UInt32,NewlyEscapedObjects:UInt32,SeedObjects:UInt32")
        })
        {
            XElement definition = provider.Descendants().Single(e => e.Name.LocalName == "event" && (int?)e.Attribute("value") == id);
            XElement opcode = provider.Descendants().Single(e => e.Name.LocalName == "opcode" &&
                (string?)e.Attribute("name") == (string?)definition.Attribute("opcode"));
            Assert.Equal(id, (int)opcode.Attribute("value")!);
            XElement template = provider.Descendants().Single(e => e.Name.LocalName == "template" &&
                (string?)e.Attribute("tid") == (string?)definition.Attribute("template"));
            Assert.Equal(expected, string.Join(",", template.Elements().Where(e => e.Name.LocalName == "data")
                .Select(e => $"{e.Attribute("name")!.Value}:{e.Attribute("inType")!.Value[4..]}")));
        }
    }
}
