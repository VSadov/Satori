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
// DictionaryImpl`2.cs
//

#nullable disable

using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using Internal.Runtime.CompilerServices;
using System.Runtime.CompilerServices;

namespace System.Collections.Concurrent
{
    internal abstract class DictionaryImpl<TKey, TValue>
        : DictionaryImpl
    {
        internal readonly bool valueIsValueType = typeof(TValue).IsValueType;
        internal IEqualityComparer<TKey> _keyComparer;

        internal DictionaryImpl() { }

        internal abstract void Clear();
        internal abstract int Count { get; }

        internal abstract object TryGetValue(TKey key);
        internal abstract bool PutIfMatch(TKey key, TValue newVal, ref TValue oldValue, ValueMatch match);
        internal abstract bool RemoveIfMatch(TKey key, ref TValue oldValue, ValueMatch match);
        internal abstract TValue GetOrAdd(TKey key, Func<TKey, TValue> valueFactory);

        internal abstract Snapshot GetSnapshot();

        internal abstract class Snapshot
        {
            protected int _idx;
            protected TKey _curKey;
            protected TValue _curValue;

            public abstract int Count { get; }
            public abstract bool MoveNext();
            public abstract void Reset();

            internal DictionaryEntry Entry
            {
                get
                {
                    return new DictionaryEntry(_curKey, _curValue);
                }
            }

            internal KeyValuePair<TKey, TValue> Current
            {
                get
                {
                    return new KeyValuePair<TKey, TValue>(this._curKey, _curValue);
                }
            }
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        protected TValue FromObjectValue(object obj)
        {
            // regular value type
            if (default(TValue) != null)
            {
                return Unsafe.As<Boxed<TValue>>(obj).Value;
            }

            // null
            if (obj == NULLVALUE)
            {
                return default(TValue);
            }

            // ref type
            if (!valueIsValueType)
            {
                return Unsafe.As<object, TValue>(ref obj);
            }

            // nullable
            return (TValue)obj;
        }
    }
}
