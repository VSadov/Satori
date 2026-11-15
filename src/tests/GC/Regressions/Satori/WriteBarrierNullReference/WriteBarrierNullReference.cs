// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using Xunit;

// A reference store into a field of a null object faults in the write barrier, and the fault must surface as
// a NullReferenceException. The Satori barrier stores values that need no cards, such as null, with a separate
// instruction (RhpAssignRefAVLocationNotHeap), which the runtime must also recognize as a write barrier fault.
public static class WriteBarrierNullReference
{
    private sealed class Holder
    {
        public object? Value;
    }

    [Fact]
    public static void TestEntryPoint()
    {
        Assert.Throws<NullReferenceException>(() => Store(null, null));
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Store(Holder? holder, object? value)
    {
        holder!.Value = value;
    }
}
