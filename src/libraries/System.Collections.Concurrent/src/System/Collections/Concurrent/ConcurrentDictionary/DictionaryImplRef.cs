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
// DictionaryImplRef.cs
//

#nullable disable

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace System.Collections.Concurrent
{
    internal sealed class DictionaryImplRef<TKey, TKeyStore, TValue>
            : DictionaryImpl<TKey, TKey, TValue>
    {
        internal DictionaryImplRef(int capacity, ConcurrentDictionary<TKey, TValue> topDict)
            : base(capacity, topDict)
        {
            Debug.Assert(!typeof(TKey).IsValueType);
        }

        internal DictionaryImplRef(int capacity, DictionaryImplRef<TKey, TKeyStore, TValue> other)
            : base(capacity, other)
        {
            Debug.Assert(!typeof(TKey).IsValueType);
        }

        protected override bool TryClaimSlotForPut(ref TKey entryKey, TKey key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        protected override bool TryClaimSlotForCopy(ref TKey entryKey, TKey key)
        {
            return TryClaimSlot(ref entryKey, key);
        }

        private bool TryClaimSlot(ref TKey entryKey, TKey key)
        {
            ref object keyLocation = ref Unsafe.As<TKey, object>(ref entryKey);
            object entryKeyValue = keyLocation;
            if (entryKeyValue == null)
            {
                entryKeyValue = Interlocked.CompareExchange(ref keyLocation, key, null);
                if (entryKeyValue == null)
                {
                    // claimed a new slot
                    this.allocatedSlotCount.Increment();
                    return true;
                }
            }

            return (object)key == entryKeyValue ||
                _keyComparer.Equals(key, Unsafe.As<object, TKey>(ref entryKeyValue));
        }

        // inline the base implementation to devirtualize calls to hash and keyEqual
        internal override object TryGetValue(TKey key)
        {
            return base.TryGetValue(key);
        }

        protected override int hash(TKey key)
        {
            return base.hash(key);
        }

        protected override bool keyEqual(TKey key, TKey entryKey)
        {
            if ((object)key == (object)entryKey)
            {
                return true;
            }

            //NOTE: slots are claimed in two stages - claim a hash, then set a key
            //      it is possible to observe a slot with a null key, but with hash already set
            //      that is not a match since the key is not yet in the table
            if (entryKey == null)
            {
                return false;
            }

            return _keyComparer.Equals(entryKey, key);
        }

        protected override DictionaryImpl<TKey, TKey, TValue> CreateNew(int capacity)
        {
            return new DictionaryImplRef<TKey, TKeyStore, TValue>(capacity, this);
        }

        protected override TKey keyFromEntry(TKey entryKey)
        {
            return entryKey;
        }
    }
}
