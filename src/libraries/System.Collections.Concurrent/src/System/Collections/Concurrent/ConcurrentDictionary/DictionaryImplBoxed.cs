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
// DictionaryImplBoxed.cs
//

#nullable disable

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Internal.Runtime.CompilerServices;

namespace System.Collections.Concurrent
{
    internal sealed class DictionaryImplBoxed<TKey, TValue>
            : DictionaryImpl<TKey, Boxed<TKey>, TValue>
    {
        internal DictionaryImplBoxed(int capacity, ConcurrentDictionary<TKey, TValue> topDict)
            : base(capacity, topDict)
        {
        }

        internal DictionaryImplBoxed(int capacity, DictionaryImplBoxed<TKey, TValue> other)
            : base(capacity, other)
        {
        }

        protected override bool TryClaimSlotForPut(ref Boxed<TKey> entryKey, TKey key)
        {
            var entryKeyValue = entryKey;
            if (entryKeyValue == null)
            {
                entryKeyValue = Interlocked.CompareExchange(ref entryKey, new Boxed<TKey>(key), null);
                if (entryKeyValue == null)
                {
                    // claimed a new slot
                    this.allocatedSlotCount.Increment();
                    return true;
                }
            }

            return _keyComparer.Equals(key, entryKey.Value);
        }

        protected override bool TryClaimSlotForCopy(ref Boxed<TKey> entryKey, Boxed<TKey> key)
        {
            var entryKeyValue = entryKey;
            if (entryKeyValue == null)
            {
                entryKeyValue = Interlocked.CompareExchange(ref entryKey, key, null);
                if (entryKeyValue == null)
                {
                    // claimed a new slot
                    this.allocatedSlotCount.Increment();
                    return true;
                }
            }

            return _keyComparer.Equals(key.Value, entryKey.Value);
        }

        protected override bool keyEqual(TKey key, Boxed<TKey> entryKey)
        {
            //NOTE: slots are claimed in two stages - claim a hash, then set a key
            //      it is possible to observe a slot with a null key, but with hash already set
            //      that is not a match since the key is not yet in the table
            if (entryKey == null)
            {
                return false;
            }

            return _keyComparer.Equals(key, entryKey.Value);
        }

        protected override DictionaryImpl<TKey, Boxed<TKey>, TValue> CreateNew(int capacity)
        {
            return new DictionaryImplBoxed<TKey, TValue>(capacity, this);
        }

        protected override TKey keyFromEntry(Boxed<TKey> entryKey)
        {
            return entryKey.Value;
        }
    }

#pragma warning disable CS0659 // Type overrides Object.Equals(object o) but does not override Object.GetHashCode()
    internal class Boxed<T>
    {
        // 0 - allow writes, 1 - someone is writing, 2 frozen.
        public int writeStatus;
        public T Value;

        public Boxed(T key)
        {
            this.Value = key;
        }

        public override bool Equals(object obj)
        {
            return EqualityComparer<T>.Default.Equals(this.Value, Unsafe.As<Boxed<T>>(obj).Value);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public bool TryVolatileWrite(T value)
        {
            if (Interlocked.CompareExchange(ref writeStatus, 1, 0) == 0)
            {
                Value = value;
                Volatile.Write(ref writeStatus, 0);
                return true;
            }
            return false;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public bool TryCompareExchange(T oldValue, T newValue, out bool changed)
        {
            changed = false;
            if (Interlocked.CompareExchange(ref writeStatus, 1, 0) != 0)
            {
                return false;
            }

            if (EqualityComparer<T>.Default.Equals(Value, oldValue))
            {
                Value = newValue;
                changed = true;
            }

            Volatile.Write(ref writeStatus, 0);
            return true;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal void Freeze()
        {
            // Wait for writers (1) to leave. Already 2 is ok, or set 0 -> 2.
            while (Interlocked.CompareExchange(ref writeStatus, 2, 0) == 1);
        }
    }
#pragma warning restore CS0659 // Type overrides Object.Equals(object o) but does not override Object.GetHashCode()
}
