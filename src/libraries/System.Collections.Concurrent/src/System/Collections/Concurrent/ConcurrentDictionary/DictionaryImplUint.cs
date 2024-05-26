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
// DictionaryImplUint.cs
//

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace System.Collections.Concurrent
{
    internal sealed class DictionaryImplUint<TValue>
                : DictionaryImpl<uint, uint, TValue>
    {
        internal DictionaryImplUint(int capacity, ConcurrentDictionary<uint, TValue> topDict)
            : base(capacity, topDict)
        {
        }

        internal DictionaryImplUint(int capacity, DictionaryImplUint<TValue> other)
            : base(capacity, other)
        {
        }

        protected override bool TryClaimSlotForPut(ref uint entryKey, uint key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        protected override bool TryClaimSlotForCopy(ref uint entryKey, uint key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        private bool TryClaimSlot(ref uint entryKey, uint key)
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

        protected override int hash(uint key)
        {
            if (key == 0)
            {
                return ZEROHASH;
            }

            return base.hash(key);
        }

        protected override bool keyEqual(uint key, uint entryKey)
        {
            return key == entryKey || _keyComparer.Equals(key, entryKey);
        }

        protected override DictionaryImpl<uint, uint, TValue> CreateNew(int capacity)
        {
            return new DictionaryImplUint<TValue>(capacity, this);
        }

        protected override uint keyFromEntry(uint entryKey)
        {
            return entryKey;
        }
    }

    internal sealed class DictionaryImplUintNoComparer<TValue>
            : DictionaryImpl<uint, uint, TValue>
    {
        internal DictionaryImplUintNoComparer(int capacity, ConcurrentDictionary<uint, TValue> topDict)
            : base(capacity, topDict)
        {
        }

        internal DictionaryImplUintNoComparer(int capacity, DictionaryImplUintNoComparer<TValue> other)
            : base(capacity, other)
        {
        }

        protected override bool TryClaimSlotForPut(ref uint entryKey, uint key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        protected override bool TryClaimSlotForCopy(ref uint entryKey, uint key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        private bool TryClaimSlot(ref uint entryKey, uint key)
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
        internal override object TryGetValue(uint key)
        {
            return base.TryGetValue(key);
        }

        protected override int hash(uint key)
        {
            return (key == 0) ?
                ZEROHASH :
                (int)key | SPECIAL_HASH_BITS;
        }

        protected override bool keyEqual(uint key, uint entryKey)
        {
            return key == entryKey;
        }

        protected override DictionaryImpl<uint, uint, TValue> CreateNew(int capacity)
        {
            return new DictionaryImplUintNoComparer<TValue>(capacity, this);
        }

        protected override uint keyFromEntry(uint entryKey)
        {
            return entryKey;
        }
    }
}
