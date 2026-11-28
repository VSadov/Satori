// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime;
using System.Runtime.InteropServices;
using Xunit;

// DOTNET_GCHeapCount is set above the processor count. Satori partitions handles by that count, and a partition
// indexes the handle tables and the dependent handle scan contexts, of which there is one per processor, so the
// partition count must not exceed the processor count. Otherwise GCs scan handle tables past the end of the array.
public static class HandlePartitionCount
{
    [Fact]
    public static void TestEntryPoint()
    {
        object target = new object();
        object secondary = new object();
        GCHandle strong = GCHandle.Alloc(target);
        GCHandle weak = GCHandle.Alloc(target, GCHandleType.Weak);
        var dependent = new DependentHandle(target, secondary);

        GC.Collect(1, GCCollectionMode.Forced, blocking: true);
        GC.Collect(2, GCCollectionMode.Forced, blocking: true);

        Assert.Same(target, strong.Target);
        Assert.Same(target, weak.Target);
        Assert.Same(secondary, dependent.Dependent);

        dependent.Dispose();
        weak.Free();
        strong.Free();
        GC.KeepAlive(target);
    }
}
