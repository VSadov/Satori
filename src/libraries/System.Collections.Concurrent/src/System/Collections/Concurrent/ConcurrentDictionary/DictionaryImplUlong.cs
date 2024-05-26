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
// DictionaryImplUlong.cs
//

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace System.Collections.Concurrent
{
    internal sealed class DictionaryImplUlong<TValue>
                : DictionaryImpl<ulong, ulong, TValue>
    {
        internal DictionaryImplUlong(int capacity, ConcurrentDictionary<ulong, TValue> topDict)
            : base(capacity, topDict)
        {
        }

        internal DictionaryImplUlong(int capacity, DictionaryImplUlong<TValue> other)
            : base(capacity, other)
        {
        }

        protected override bool TryClaimSlotForPut(ref ulong entryKey, ulong key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        protected override bool TryClaimSlotForCopy(ref ulong entryKey, ulong key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        private bool TryClaimSlot(ref ulong entryKey, ulong key)
        {
            var entryKeyValue = entryKey;
            //zero keys are claimed via hash
            if (entryKeyValue == 0 & key != 0)
            {
                entryKeyValue = Interlocked.CompareExchange(ref entryKey, key, 0);
                if (entryKeyValue == 0)
                {
                    // claimed a new slot
                    this.allocatedSlotCount.Increment();
                    return true;
                }
            }

            return key == entryKeyValue || _keyComparer.Equals(key, entryKey);
        }

        protected override int hash(ulong key)
        {
            if (key == 0)
            {
                return ZEROHASH;
            }

            return base.hash(key);
        }

        protected override bool keyEqual(ulong key, ulong entryKey)
        {
            return key == entryKey || _keyComparer.Equals(key, entryKey);
        }

        protected override DictionaryImpl<ulong, ulong, TValue> CreateNew(int capacity)
        {
            return new DictionaryImplUlong<TValue>(capacity, this);
        }

        protected override ulong keyFromEntry(ulong entryKey)
        {
            return entryKey;
        }
    }

    internal sealed class DictionaryImplUlongNoComparer<TValue>
            : DictionaryImpl<ulong, ulong, TValue>
    {
        internal DictionaryImplUlongNoComparer(int capacity, ConcurrentDictionary<ulong, TValue> topDict)
            : base(capacity, topDict)
        {
        }

        internal DictionaryImplUlongNoComparer(int capacity, DictionaryImplUlongNoComparer<TValue> other)
            : base(capacity, other)
        {
        }

        protected override bool TryClaimSlotForPut(ref ulong entryKey, ulong key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        protected override bool TryClaimSlotForCopy(ref ulong entryKey, ulong key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        private bool TryClaimSlot(ref ulong entryKey, ulong key)
        {
            var entryKeyValue = entryKey;
            //zero keys are claimed via hash
            if (entryKeyValue == 0 & key != 0)
            {
                entryKeyValue = Interlocked.CompareExchange(ref entryKey, key, 0);
                if (entryKeyValue == 0)
                {
                    // claimed a new slot
                    this.allocatedSlotCount.Increment();
                    return true;
                }
            }

            return key == entryKeyValue;
        }

        // inline the base implementation to devirtualize calls to hash and keyEqual
        internal override object TryGetValue(ulong key)
        {
            return base.TryGetValue(key);
        }

        protected override int hash(ulong key)
        {
            return (key == 0) ?
                ZEROHASH :
                key.GetHashCode() | SPECIAL_HASH_BITS;
        }

        protected override bool keyEqual(ulong key, ulong entryKey)
        {
            return key == entryKey;
        }

        protected override DictionaryImpl<ulong, ulong, TValue> CreateNew(int capacity)
        {
            return new DictionaryImplUlongNoComparer<TValue>(capacity, this);
        }

        protected override ulong keyFromEntry(ulong entryKey)
        {
            return entryKey;
        }
    }
}
