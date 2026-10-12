// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Diagnostics.Tracing;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Diagnostics.NETCore.Client;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Parsers;
using Microsoft.Diagnostics.Tracing.Parsers.Clr;
using Tracing.Tests.Common;
using Xunit;

namespace Tracing.Tests.GCDumpAsync;

public class GCDumpAsyncTest
{
    private const int PayloadCount = 128;
    private static readonly HeapDumpGcTracker s_gcTracker = new HeapDumpGcTracker();
    private static readonly ManualResetEvent s_gcStopReceived = new ManualResetEvent(false);
    private static readonly TaskCompletionSource<int> s_gate =
        new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
    private static readonly HashSet<ulong> s_expectedPayloadSizes = new HashSet<ulong>();
    private static readonly HashSet<ulong> s_observedPayloadSizes = new HashSet<ulong>();
    private static readonly ulong s_byteArrayTypeId = (ulong)(nuint)typeof(byte[]).TypeHandle.Value;
    private static List<Task<long>> s_liveStateMachines;
    private static int s_bulkTypeCount;
    private static int s_bulkNodeCount;
    private static int s_emptyTypeNameCount;
    private static bool s_sawStateMachineType;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static async Task<long> Suspended(byte[] captured)
    {
        await s_gate.Task;
        long sum = 0;
        for (int i = 0; i < captured.Length; i++)
        {
            sum += captured[i];
        }

        return sum;
    }

    [SkipOnCoreClr("Heap snapshots require fully optimized code.", RuntimeTestModes.AnyJitOptimizationStress)]
    [Fact]
    public static int TestEntryPoint()
    {
        s_liveStateMachines = new List<Task<long>>(PayloadCount);
        for (int i = 0; i < PayloadCount; i++)
        {
            byte[] payload = new byte[4096 + i * 16];
            payload[0] = (byte)i;
            // Array base size is SyncBlock + MethodTable pointer + length/padding.
            s_expectedPayloadSizes.Add((ulong)(3 * IntPtr.Size + payload.Length));
            s_liveStateMachines.Add(Suspended(payload));
        }

        List<EventPipeProvider> providers = new List<EventPipeProvider>
        {
            new EventPipeProvider("Microsoft-Windows-DotNETRuntime", eventLevel: EventLevel.Verbose,
                keywords: (long)ClrTraceEventParser.Keywords.GCHeapSnapshot)
        };

        try
        {
            return IpcTraceTest.RunAndValidateEventCounts(
                new Dictionary<string, ExpectedEventCount>(), GenerateEvents, providers, 1024, Validate);
        }
        finally
        {
            s_gate.SetResult(0);
            Task.WaitAll(s_liveStateMachines.ToArray());
        }
    }

    private static void GenerateEvents()
    {
        Assert.True(s_gcStopReceived.WaitOne(30000), "The diagnostic collection did not finish.");
        GC.Collect();
    }

    private static Func<int> Validate(EventPipeEventSource source)
    {
        source.Clr.GCStart += (GCStartTraceData data) => s_gcTracker.GCStarted(data.Count);
        source.Clr.TypeBulkType += (GCBulkTypeTraceData data) =>
        {
            s_bulkTypeCount += data.Count;
            for (int i = 0; i < data.Count; i++)
            {
                string name = data.Values(i).TypeName;
                if (string.IsNullOrEmpty(name))
                {
                    s_emptyTypeNameCount++;
                }
                else if (name.Contains("AsyncStateMachineBox"))
                {
                    s_sawStateMachineType = true;
                }
            }
        };

        source.Clr.GCBulkNode += (GCBulkNodeTraceData data) =>
        {
            s_gcTracker.HeapEvent();
            s_bulkNodeCount += data.Count;
            for (int i = 0; i < data.Count; i++)
            {
                GCBulkNodeValues node = data.Values(i);
                if (node.TypeID == s_byteArrayTypeId)
                {
                    s_observedPayloadSizes.Add(node.Size);
                }
            }
        };
        source.Clr.GCBulkEdge += _ => s_gcTracker.HeapEvent();
        source.Clr.GCBulkRootEdge += _ => s_gcTracker.HeapEvent();
        source.Clr.GCBulkRootConditionalWeakTableElementEdge += _ => s_gcTracker.HeapEvent();
        source.Clr.GCBulkRootStaticVar += _ => s_gcTracker.HeapEvent();
        source.Clr.GCStop += (GCEndTraceData data) =>
        {
            s_gcTracker.GCStopped(data.Count);
            if (s_gcTracker.IsComplete)
            {
                s_gcStopReceived.Set();
            }
        };

        return () =>
        {
            GC.KeepAlive(s_liveStateMachines);
            Assert.True(s_gcTracker.IsComplete, "The snapshot GC did not complete.");
            Assert.False(s_gcTracker.HasInvalidHeapEvent, "Heap events escaped the snapshot collection.");
            Assert.True(s_bulkTypeCount > 50, $"Only {s_bulkTypeCount} type records were received.");
            Assert.True(s_bulkNodeCount > 50, $"Only {s_bulkNodeCount} object records were received.");
            Assert.Equal(PayloadCount, s_expectedPayloadSizes.Count);
            foreach (ulong size in s_expectedPayloadSizes)
            {
                Assert.Contains(size, s_observedPayloadSizes);
            }

            if (!TestLibrary.Utilities.IsNativeAot)
            {
                Assert.True(s_sawStateMachineType, "The suspended Task state-machine type was not logged.");
                Assert.Equal(0, s_emptyTypeNameCount);
            }

            Console.WriteLine($"Task-based async heap dump: types={s_bulkTypeCount}, nodes={s_bulkNodeCount}, all 128 captured payloads found.");

            return 100;
        };
    }
}
