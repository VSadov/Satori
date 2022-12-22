// Copyright (c) 2022 Vladimir Sadov
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// CounterBase.cs
//

using System;
using System.Collections;
using System.Runtime.CompilerServices;

namespace System.Collections.Concurrent
{
    /// <summary>
    /// Scalable counter base.
    /// </summary>
    internal class CounterBase
    {
        private protected const int CACHE_LINE = 64;
        private protected const int OBJ_HEADER_SIZE = 8;

        private protected static readonly int s_MaxCellCount = HashHelpers.AlignToPowerOfTwo(Environment.ProcessorCount) + 1;

        // how many cells we have
        private protected int cellCount;

        // delayed count time
        private protected uint lastCountTicks;

        private protected CounterBase()
        {
            // touch a static
            _ = s_MaxCellCount;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private protected static unsafe int GetIndex(uint cellCount)
        {
            nuint addr = (nuint)(&cellCount);
            return (int)(addr % cellCount);
        }
    }
}
