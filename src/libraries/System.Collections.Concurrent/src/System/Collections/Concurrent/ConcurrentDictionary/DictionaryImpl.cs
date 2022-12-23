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
// DictionaryImpl.cs
//

#nullable disable

using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace System.Collections.Concurrent
{
    internal abstract class DictionaryImpl
    {
        internal DictionaryImpl() { }

        internal enum ValueMatch
        {
            Any,            // sets new value unconditionally, used by index set and TryRemove(key)
            NullOrDead,     // set value if original value is null or dead, used by Add/TryAdd
            NotNullOrDead,  // set value if original value is alive, used by Remove
            OldValue,       // sets new value if old value matches
        }

        internal sealed class Prime
        {
            internal object originalValue;

            public Prime(object originalValue)
            {
                this.originalValue = originalValue;
            }
        }

        internal static readonly object TOMBSTONE = new object();
        internal static readonly Prime TOMBPRIME = new Prime(TOMBSTONE);
        internal static readonly object NULLVALUE = new object();

        // represents a trivially copied empty entry
        // we insert it in the old table during rehashing
        // to reduce chances that more entries are added
        protected const int TOMBPRIMEHASH = 1 << 31;

        // we cannot distigush zero keys from uninitialized state
        // so we force them to have this special hash instead
        protected const int ZEROHASH = 1 << 30;

        // all regular hashes have both these bits set
        // to be different from either 0, TOMBPRIMEHASH or ZEROHASH
        // having only these bits set in a case of Ref key means that the slot is permanently deleted.
        protected const int SPECIAL_HASH_BITS = TOMBPRIMEHASH | ZEROHASH;

        // Heuristic to decide if we have reprobed toooo many times.  Running over
        // the reprobe limit on a 'get' call acts as a 'miss'; on a 'put' call it
        // can trigger a table resize.  Several places must have exact agreement on
        // what the reprobe_limit is, so we share it here.
        protected const int REPROBE_LIMIT = 4;
        protected const int REPROBE_LIMIT_SHIFT = 8;

        protected static int ReprobeLimit(int lenMask)
        {
            // 1/2 of table with some extra
            return REPROBE_LIMIT + (lenMask >> REPROBE_LIMIT_SHIFT);
        }

        protected static bool EntryValueNullOrDead(object entryValue)
        {
            return entryValue == null || entryValue == TOMBSTONE;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        protected static int ReduceHashToIndex(int fullHash, int lenMask)
        {
            var h = (uint)fullHash;

            // xor-shift some upper bits down, in case if variations are mostly in high bits
            // and scatter the bits a little to break up clusters if hashes are periodic (like 42, 43, 44, ...)
            // long clusters can cause long reprobes. small clusters are ok though.
            h ^= h >> 15;
            h ^= h >> 8;
            h += (h >> 3) * 2654435769u;

            return (int)h & lenMask;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal static object ToObjectValue<TValue>(TValue value)
        {
            if (default(TValue) != null)
            {
                return new Boxed<TValue>(value);
            }

            return (object)value ?? NULLVALUE;
        }
    }
}
