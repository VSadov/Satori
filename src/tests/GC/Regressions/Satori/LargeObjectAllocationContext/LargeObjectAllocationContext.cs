// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using Xunit;

public static class LargeObjectAllocationContext
{
    // Satori allocates objects of LARGE_OBJECT_THRESHOLD (32 KB) and more in large regions.
    private const int LargeObjectThreshold = 32 * 1024;
    private const int RegionSize = 2 * 1024 * 1024;
    private const int Iterations = 10000;

    private static bool IsSatori =>
        GC.GetConfigurationVariables().TryGetValue("SatoriGC", out object? value) && value is true;

    [Fact]
    public static int TestEntryPoint()
    {
        if (!IsSatori)
        {
            return 100;
        }

        int result = 100;
        // The first array is small and refills the allocation context when the rest of the context is too small for it.
        foreach ((int smallLength, int largeLength) in new[] { (16 * 1024, LargeObjectThreshold), (1024, 60 * 1024) })
        {
            int inRegularRegion = CountInRegularRegion(smallLength, largeLength);
            if (inRegularRegion != 0)
            {
                Console.WriteLine($"{inRegularRegion} of {Iterations} arrays of {largeLength} bytes were allocated in the regular region.");
                result = 1;
            }
        }

        return result;
    }

    // Allocates a small array, then a large one, and counts how often the large one is in the region of the
    // small one, that is, in the regular region of the allocation context instead of a large region.
    private static int CountInRegularRegion(int smallLength, int largeLength)
    {
        int count = 0;
        for (int i = 0; i < Iterations; i++)
        {
            int gcCount = GC.CollectionCount(0);
            byte[] small = new byte[smallLength];
            byte[] large = new byte[largeLength];
            if (GC.CollectionCount(0) == gcCount && RegionFor(small) == RegionFor(large))
            {
                count++;
            }
        }

        return count;
    }

    private static unsafe nuint RegionFor(byte[] array)
    {
        fixed (byte* address = array)
        {
            return (nuint)address & ~(nuint)(RegionSize - 1);
        }
    }
}
