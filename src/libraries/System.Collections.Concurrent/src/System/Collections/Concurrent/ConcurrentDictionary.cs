// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#nullable disable
#pragma warning disable CS8632 // The annotation for nullable reference types should only be used in code within a '#nullable' annotations context.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using Internal.Runtime.CompilerServices;
using static System.Collections.Concurrent.DictionaryImpl;

namespace System.Collections.Concurrent
{
    /// <summary>
    /// Represents a thread-safe and lock-free collection of keys and values.
    /// </summary>
    /// <typeparam name="TKey">The type of the keys in the dictionary.</typeparam>
    /// <typeparam name="TValue">The type of the values in the dictionary.</typeparam>
    /// <remarks>
    /// All public and protected members of <see cref="ConcurrentDictionary{TKey,TValue}"/> are thread-safe and may be used
    /// concurrently from multiple threads.
    /// </remarks>
    [DebuggerTypeProxy(typeof(IDictionaryDebugView<,>))]
    [DebuggerDisplay("Count = {Count}")]
    public class ConcurrentDictionary<TKey, TValue> : IDictionary<TKey, TValue>, IDictionary, IReadOnlyDictionary<TKey, TValue> where TKey : notnull
    {
        internal DictionaryImpl<TKey, TValue> _table;
        internal uint _lastResizeTickMillis;
        internal object _sweeperInstance;
        internal int _sweepRequests;

        /// <summary>The default capacity, i.e. the initial # of buckets.</summary>
        /// <remarks>
        /// When choosing this value, we are making a trade-off between the size of a very small dictionary,
        /// and the number of resizes when constructing a large dictionary.
        /// </remarks>
        private const int DefaultCapacity = 0;

        /// <summary>Concurrency level is ignored. However it must be > 0.</summary>
        private static int DefaultConcurrencyLevel => 1;

        /// <summary>
        /// Initializes a new instance of the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// class that is empty, has the default concurrency level, has the default initial capacity, and
        /// uses the default comparer for the key type.
        /// </summary>
        public ConcurrentDictionary() : this(DefaultConcurrencyLevel, DefaultCapacity, null) { }

        /// <summary>
        /// Initializes a new instance of the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// class that is empty, has the specified concurrency level and capacity, and uses the default
        /// comparer for the key type.
        /// </summary>
        /// <param name="concurrencyLevel">The estimated number of threads that will update the
        /// <see cref="ConcurrentDictionary{TKey,TValue}"/> concurrently, or -1 to indicate a default value.</param>
        /// <param name="capacity">The initial number of elements that the <see cref="ConcurrentDictionary{TKey,TValue}"/> can contain.</param>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="concurrencyLevel"/> is less than 1.</exception>
        /// <exception cref="ArgumentOutOfRangeException"> <paramref name="capacity"/> is less than 0.</exception>
        public ConcurrentDictionary(int concurrencyLevel, int capacity) : this(concurrencyLevel, capacity, null) { }

        /// <summary>
        /// Initializes a new instance of the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// class that contains elements copied from the specified <see cref="IEnumerable{T}"/>, has the default concurrency
        /// level, has the default initial capacity, and uses the default comparer for the key type.
        /// </summary>
        /// <param name="collection">The <see
        /// cref="IEnumerable{T}"/> whose elements are copied to the new <see cref="ConcurrentDictionary{TKey,TValue}"/>.</param>
        /// <exception cref="ArgumentNullException"><paramref name="collection"/> is a null reference (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentException"><paramref name="collection"/> contains one or more duplicate keys.</exception>
        public ConcurrentDictionary(IEnumerable<KeyValuePair<TKey, TValue>> collection)
            : this(DefaultConcurrencyLevel, collection, null) { }

        /// <summary>
        /// Initializes a new instance of the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// class that is empty, has the specified concurrency level and capacity, and uses the specified
        /// <see cref="IEqualityComparer{TKey}"/>.
        /// </summary>
        /// <param name="comparer">The <see cref="IEqualityComparer{TKey}"/> implementation to use when comparing keys.</param>
        public ConcurrentDictionary(IEqualityComparer<TKey>? comparer) : this(DefaultConcurrencyLevel, DefaultCapacity, comparer) { }

        /// <summary>
        /// Initializes a new instance of the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// class that contains elements copied from the specified <see cref="IEnumerable"/>, has the default concurrency
        /// level, has the default initial capacity, and uses the specified <see cref="IEqualityComparer{TKey}"/>.
        /// </summary>
        /// <param name="collection">The <see cref="IEnumerable{T}"/> whose elements are copied to the new <see cref="ConcurrentDictionary{TKey,TValue}"/>.</param>
        /// <param name="comparer">The <see cref="IEqualityComparer{TKey}"/> implementation to use when comparing keys.</param>
        /// <exception cref="ArgumentNullException"><paramref name="collection"/> is a null reference (Nothing in Visual Basic).</exception>
        public ConcurrentDictionary(IEnumerable<KeyValuePair<TKey, TValue>> collection, IEqualityComparer<TKey>? comparer)
            : this(DefaultConcurrencyLevel, GetCapacityFromCollection(collection), comparer)
        {
            ArgumentNullException.ThrowIfNull(collection);

            InitializeFromCollection(collection);
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// class that contains elements copied from the specified <see cref="IEnumerable"/>,
        /// has the specified concurrency level, has the specified initial capacity, and uses the specified
        /// <see cref="IEqualityComparer{TKey}"/>.
        /// </summary>
        /// <param name="concurrencyLevel">
        /// The estimated number of threads that will update the <see cref="ConcurrentDictionary{TKey,TValue}"/> concurrently, or -1 to indicate a default value.
        /// </param>
        /// <param name="collection">The <see cref="IEnumerable{T}"/> whose elements are copied to the new
        /// <see cref="ConcurrentDictionary{TKey,TValue}"/>.</param>
        /// <param name="comparer">The <see cref="IEqualityComparer{TKey}"/> implementation to use when comparing keys.</param>
        /// <exception cref="ArgumentNullException"><paramref name="collection"/> is a null reference (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="concurrencyLevel"/> is less than 1.</exception>
        /// <exception cref="ArgumentException"><paramref name="collection"/> contains one or more duplicate keys.</exception>
        public ConcurrentDictionary(int concurrencyLevel, IEnumerable<KeyValuePair<TKey, TValue>> collection, IEqualityComparer<TKey>? comparer)
            : this(concurrencyLevel, DefaultCapacity, comparer)
        {
            ArgumentNullException.ThrowIfNull(collection);

            InitializeFromCollection(collection);
        }

        private void InitializeFromCollection(IEnumerable<KeyValuePair<TKey, TValue>> collection)
        {
            foreach (KeyValuePair<TKey, TValue> pair in collection)
            {
                if (pair.Key is null)
                {
                    ThrowHelper.ThrowKeyNullException();
                }

                if (!this.TryAdd(pair.Key, pair.Value))
                {
                    throw new ArgumentException(SR.ConcurrentDictionary_SourceContainsDuplicateKeys);
                }
            }
        }

        // We want to call DictionaryImpl.CreateRef<TKey, TValue>(topDict, capacity)
        // TKey is a reference type, but that is not statically known, so
        // we use the following to get around "as class" contraint.
        internal static Func<ConcurrentDictionary<TKey, TValue>, int, DictionaryImpl<TKey, TValue>> CreateRefUnsafe =
            (ConcurrentDictionary<TKey, TValue> topDict, int capacity) =>
            {
                var method = typeof(DictionaryImpl).
                    GetMethod("CreateRef", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static).
                    MakeGenericMethod(new Type[] { typeof(TKey), typeof(TValue) });

                var del = (Func<ConcurrentDictionary<TKey, TValue>, int, DictionaryImpl<TKey, TValue>>)Delegate.CreateDelegate(
                    typeof(Func<ConcurrentDictionary<TKey, TValue>, int, DictionaryImpl<TKey, TValue>>),
                    method);

                var result = del(topDict, capacity);
                CreateRefUnsafe = del;

                return result;
            };

        /// <summary>
        /// Initializes a new instance of the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// class that is empty, has the specified concurrency level, has the specified initial capacity, and
        /// uses the specified <see cref="IEqualityComparer{TKey}"/>.
        /// </summary>
        /// <param name="concurrencyLevel">The estimated number of threads that will update the <see cref="ConcurrentDictionary{TKey,TValue}"/> concurrently, or -1 to indicate a default value.</param>
        /// <param name="capacity">The initial number of elements that the <see cref="ConcurrentDictionary{TKey,TValue}"/> can contain.</param>
        /// <param name="comparer">The <see cref="IEqualityComparer{TKey}"/> implementation to use when comparing keys.</param>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="concurrencyLevel"/> is less than 1. -or- <paramref name="capacity"/> is less than 0.</exception>
        public ConcurrentDictionary(int concurrencyLevel, int capacity, IEqualityComparer<TKey>? comparer)
        {
            if (concurrencyLevel < 1)
            {
                if (concurrencyLevel != -1)
                {
                    throw new ArgumentOutOfRangeException(nameof(concurrencyLevel), SR.ConcurrentDictionary_ConcurrencyLevelMustBePositiveOrNegativeOne);
                }

            // add some extra so that filled to capacity would be at 50% density
            capacity = Math.Max(capacity, capacity * 2);

            if (!typeof(TKey).IsValueType)
            {
                _table = CreateRefUnsafe(this, capacity);
                _table._keyComparer = comparer ?? EqualityComparer<TKey>.Default;
                return;
            }
            else
            {
                if (typeof(TKey) == typeof(int) || (typeof(TKey) == typeof(uint) && comparer == null))
                {
                    if (comparer == null)
                    {
                        _table = Unsafe.As<DictionaryImpl<TKey, TValue>>(new DictionaryImplIntNoComparer<TValue>(capacity, Unsafe.As<ConcurrentDictionary<int, TValue>>(this)));
                    }
                    else
                    {
                        _table = Unsafe.As<DictionaryImpl<TKey, TValue>>(new DictionaryImplInt<TValue>(capacity, Unsafe.As<ConcurrentDictionary<int, TValue>>(this)));
                        _table._keyComparer = comparer;
                    }
                    return;
                }

                if (typeof(TKey) == typeof(long) || (typeof(TKey) == typeof(ulong) && comparer == null))
                {
                    if (comparer == null)
                    {
                        _table = Unsafe.As<DictionaryImpl<TKey, TValue>>(new DictionaryImplLongNoComparer<TValue>(capacity, Unsafe.As<ConcurrentDictionary<long, TValue>>(this)));
                    }
                    else
                    {
                        _table = Unsafe.As<DictionaryImpl<TKey, TValue>>(new DictionaryImplLong<TValue>(capacity, Unsafe.As<ConcurrentDictionary<long, TValue>>(this)));
                        _table._keyComparer = comparer;
                    }
                    return;
                }

                if (typeof(TKey) == typeof(nint) || (typeof(TKey) == typeof(nuint) && comparer == null))
                {
                    if (comparer == null)
                    {
                        _table = Unsafe.As<DictionaryImpl<TKey, TValue>>(new DictionaryImplNintNoComparer<TValue>(capacity, Unsafe.As<ConcurrentDictionary<nint, TValue>>(this)));
                    }
                    else
                    {
                        _table = Unsafe.As<DictionaryImpl<TKey, TValue>>(new DictionaryImplNint<TValue>(capacity, Unsafe.As<ConcurrentDictionary<nint, TValue>>(this)));
                        _table._keyComparer = comparer;
                    }
                    return;
                }
            }

            _table = new DictionaryImplBoxed<TKey, TValue>(capacity, this);
            _table._keyComparer = comparer ?? EqualityComparer<TKey>.Default;
        }

        /// <summary>
        /// Attempts to add the specified key and value to the <see cref="ConcurrentDictionary{TKey, TValue}"/>.
        /// </summary>
        /// <param name="key">The key of the element to add.</param>
        /// <param name="value">The value of the element to add. The value can be a null reference (Nothing
        /// in Visual Basic) for reference types.</param>
        /// <returns>
        /// true if the key/value pair was added to the <see cref="ConcurrentDictionary{TKey, TValue}"/> successfully; otherwise, false.
        /// </returns>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is null reference (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The <see cref="ConcurrentDictionary{TKey, TValue}"/> contains too many elements.</exception>
        public bool TryAdd(TKey key, TValue value)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            TValue oldVal = default;
            return _table.PutIfMatch(key, value, ref oldVal, ValueMatch.NullOrDead);
        }

        /// <summary>
        /// Determines whether the <see cref="ConcurrentDictionary{TKey, TValue}"/> contains the specified key.
        /// </summary>
        /// <param name="key">The key to locate in the <see cref="ConcurrentDictionary{TKey, TValue}"/>.</param>
        /// <returns>true if the <see cref="ConcurrentDictionary{TKey, TValue}"/> contains an element with the specified key; otherwise, false.</returns>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference (Nothing in Visual Basic).</exception>
        public bool ContainsKey(TKey key) => TryGetValue(key, out _);

            return _table.TryGetValue(key, out _);
        }

        /// <summary>
        /// Attempts to remove and return the value with the specified key from the <see cref="ConcurrentDictionary{TKey, TValue}"/>.
        /// </summary>
        /// <param name="key">The key of the element to remove and return.</param>
        /// <param name="value">
        /// When this method returns, <paramref name="value"/> contains the object removed from the
        /// <see cref="ConcurrentDictionary{TKey,TValue}"/> or the default value of <typeparamref
        /// name="TValue"/> if the operation failed.
        /// </param>
        /// <returns>true if an object was removed successfully; otherwise, false.</returns>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference (Nothing in Visual Basic).</exception>
        public bool TryRemove(TKey key, [MaybeNullWhen(false)] out TValue value)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            value = default;
            return _table.RemoveIfMatch(key, ref value, ValueMatch.NotNullOrDead);
        }

        /// <summary>Removes a key and value from the dictionary.</summary>
        /// <param name="item">The <see cref="KeyValuePair{TKey,TValue}"/> representing the key and value to remove.</param>
        /// <returns>
        /// true if the key and value represented by <paramref name="item"/> are successfully
        /// found and removed; otherwise, false.
        /// </returns>
        /// <remarks>
        /// Both the specified key and value must match the entry in the dictionary for it to be removed.
        /// The key is compared using the dictionary's comparer (or the default comparer for <typeparamref name="TKey"/>
        /// if no comparer was provided to the dictionary when it was constructed).  The value is compared using the
        /// default comparer for <typeparamref name="TValue"/>.
        /// </remarks>
        /// <exception cref="ArgumentNullException">
        /// The <see cref="KeyValuePair{TKey, TValue}.Key"/> property of <paramref name="item"/> is a null reference.
        /// </exception>
        public bool TryRemove(KeyValuePair<TKey, TValue> item)
        {
            if (item.Key is null)
            {
                ThrowHelper.ThrowArgumentNullException(nameof(item), SR.ConcurrentDictionary_ItemKeyIsNull);
            }

            TValue oldVal = item.Value;
            return _table.RemoveIfMatch(item.Key, ref oldVal, ValueMatch.OldValue);
        }

        /// <summary>
        /// Attempts to get the value associated with the specified key from the <see cref="ConcurrentDictionary{TKey,TValue}"/>.
        /// </summary>
        /// <param name="key">The key of the value to get.</param>
        /// <param name="value">
        /// When this method returns, <paramref name="value"/> contains the object from
        /// the <see cref="ConcurrentDictionary{TKey,TValue}"/> with the specified key or the default value of
        /// <typeparamref name="TValue"/>, if the operation failed.
        /// </param>
        /// <returns>true if the key was found in the <see cref="ConcurrentDictionary{TKey,TValue}"/>; otherwise, false.</returns>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference (Nothing in Visual Basic).</exception>
        public bool TryGetValue(TKey key, [MaybeNullWhen(false)] out TValue value)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            return _table.TryGetValue(key, out value);
        }
                    }
                        {
                            value = n._value;
                            return true;
                        }
                    }
                }
                else
                {
                    for (Node? n = Volatile.Read(ref tables.GetBucket(hashcode)); n != null; n = n._next)
                    {
                        if (hashcode == n._hashcode && _defaultComparer.Equals(n._key, key))
                        {
                            value = n._value;
                            return true;
                        }
                    }
                }
            }
            else
            {
                for (Node? n = Volatile.Read(ref tables.GetBucket(hashcode)); n != null; n = n._next)
                {
            TValue oldVal = comparisonValue;
            return _table.PutIfMatch(key, newValue, ref oldVal, ValueMatch.OldValue);
                    {
                        value = n._value;
                        return true;
                                if (s_isValueWriteAtomic)
                                {
                                    node._value = newValue;
                                }
                            return false;
                        }

                        prev = node;
                    }

                    // didn't find the key
                    return false;
                }
            }
        }

        /// <summary>
        /// Removes all keys and values from the <see cref="ConcurrentDictionary{TKey,TValue}"/>.
        /// </summary>
        public void Clear() => _table.Clear();

                // If the dictionary is already empty, then there's nothing to clear.
            CopyToPairs(array, index);
        }
        /// cref="KeyValuePair{TKey,TValue}"/> elements copied from the <see  cref="ICollection"/>. The array must have zero-based indexing.
        /// </param>
        /// <param name="index">The zero-based index in <paramref name="array"/> at which copying begins.</param>
        /// <exception cref="ArgumentNullException"><paramref name="array"/> is a null reference (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="index"/> is less than 0.</exception>
        /// <exception cref="ArgumentException">
        /// <paramref name="index"/> is equal to or greater than the length of the <paramref name="array"/>. -or- The number of
        /// elements in the source <see cref="ICollection"/> is greater than the available space from <paramref name="index"/> to
        /// the end of the destination <paramref name="array"/>.
        /// </exception>
        void ICollection<KeyValuePair<TKey, TValue>>.CopyTo(KeyValuePair<TKey, TValue>[] array, int index)
            int count = snapshot.Count;
            if (count == 0)
            {
                return Array.Empty<KeyValuePair<TKey, TValue>>();
            }
                throw new ArgumentOutOfRangeException(nameof(index), SR.ConcurrentDictionary_IndexIsNegative);
            }

            int locksAcquired = 0;
            try
                array[idx++] = snapshot.Current;
                AcquireAllLocks(ref locksAcquired);

            if (idx != array.Length)
            {
                Array.Resize(ref array, idx);
            }
            finally
            return array;
        }

                int count = 0;
                int[] countPerLock = _tables._countPerLock;
                for (int i = 0; i < countPerLock.Length; i++)
            if (array is null)
                    checked
                ThrowHelper.ThrowArgumentNullException(nameof(array));
            }
                    return Array.Empty<KeyValuePair<TKey, TValue>>();
            if (index < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(index), SR.ConcurrentDictionary_IndexIsNegative);
            }
        }

        /// <summary>Copy dictionary contents to an array.</summary>
        private void CopyToPairs(KeyValuePair<TKey, TValue>[] array, int index)
        {
            Node?[] buckets = _tables._buckets;
            for (int i = 0; i < buckets.Length; i++)
            {
                for (Node? current = buckets[i]; current != null; current = current._next)
                {
                    array[index] = new KeyValuePair<TKey, TValue>(current._key, current._value);
                    array[index++] = entry;
                }
                else
                {
                    throw new ArgumentException(SR.ConcurrentDictionary_ArrayNotLargeEnough);
                }
            }
        }
            private const int StateOuterloop = 1;
        /// <summary>Copy dictionary contents to an array.</summary>
        private void CopyToEntries(DictionaryEntry[] array, int index)
                _i = -1;
            if (array is null)
            }
                ThrowHelper.ThrowArgumentNullException(nameof(array));
            }
            {
            if (index < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(index), SR.ConcurrentDictionary_IndexIsNegative);
            }
                        ConcurrentDictionary<TKey, TValue>.Node?[]? buckets = _buckets;
            var length = array.Length;
            if (index >= length)
            {
                throw new ArgumentException(SR.ConcurrentDictionary_ArrayNotLargeEnough);
            }
                            // this protects us from reading fields ('_key', '_value' and '_next') of different instances.
            foreach (var entry in this)
            {
                if ((uint)index < (uint)length)
                {
                    array[index++] = new DictionaryEntry(entry.Key, entry.Value);
                }
                else
                {
                    throw new ArgumentException(SR.ConcurrentDictionary_ArrayNotLargeEnough);
                }
            }
        }

        /// <summary>Copy dictionary contents to an array.</summary>
        private void CopyToObjects(object[] array, int index)
        {
            if (array is null)
            {
                ThrowHelper.ThrowArgumentNullException(nameof(array));
            }
                nullableHashcode ??
                (comparer is null ? key.GetHashCode() : comparer.GetHashCode(key));

            while (true)
            {
                Tables tables = _tables;
            var length = array.Length;
            if (index > length)
            {
                throw new ArgumentException(SR.ConcurrentDictionary_ArrayNotLargeEnough);
            }

            foreach (var entry in this)
            {
                if ((uint)index < (uint)length)

                    // If the table just got resized, we may not be holding the right lock, and must retry.
                    // This should be a rare occurrence.
                    if (tables != _tables)
                    {
                        continue;
                    }

                    // Try to find this key in the bucket
                    Node? prev = null;
        /// <summary>Returns an enumerator that iterates through the <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>.</summary>
        /// <returns>An enumerator for the <see cref="ConcurrentDictionary{TKey,TValue}"/>.</returns>
        /// <remarks>
        /// The enumerator returned from the dictionary is safe to use concurrently with
        /// reads and writes to the dictionary, however it does not represent a moment-in-time snapshot
        /// of the dictionary.  The contents exposed through the enumerator may contain modifications
        /// made to the dictionary after <see cref="GetEnumerator"/> was called.
        /// </remarks>
        public IEnumerator<KeyValuePair<TKey, TValue>> GetEnumerator()
        {
            return new SnapshotEnumerator(_table.GetSnapshot());
                                else
                                {
                                    var newNode = new Node(node._key, value, hashcode, node._next);
                                    if (prev is null)
                                    {
                                        Volatile.Write(ref bucket, newNode);
                                    }
                                    else
                                    {
                                        prev._next = newNode;
                                    }
                                }
                                resultingValue = value;
                            }
                            else
                            {
                                resultingValue = node._value;
                            }
                            return false;
                        }
                        prev = node;
                    }

            if (index < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(index), SR.ConcurrentDictionary_IndexIsNegative);
            }

                    //
                    // If the number of elements guarded by this lock has exceeded the budget, resize the bucket table.
                    // It is also possible that GrowTable will increase the budget but won't resize the bucket table.
                    // That happens if the bucket table is found to be poorly utilized due to a bad hash function.
                    //
                    if (tables._countPerLock[lockNo] > _budget)
                    {
                        resizeDesired = true;
                    }
                }
                finally
                TValue oldVal = default;
                _table.PutIfMatch(key, value, ref oldVal, ValueMatch.Any);
                    array[index++] = entry;
                }
                else
                {
                    throw new ArgumentException(SR.ConcurrentDictionary_ArrayNotLargeEnough);
                }
            }
        }

                //
                // The fact that we got here means that we just performed an insertion. If necessary, we will grow the table.
                //
                // Concurrency notes:
                // - Notice that we are not holding any locks at when calling GrowTable. This is necessary to prevent deadlocks.
                // - As a result, it is possible that GrowTable will be called unnecessarily. But, GrowTable will obtain lock 0
                //   and then verify that the table we passed to it as the argument is still the current table.
                //
                if (resizeDesired)
                {
                    GrowTable(tables);
                }

                resultingValue = value;
                return true;
        public IEqualityComparer<TKey> Comparer => _table._keyComparer ?? EqualityComparer<TKey>.Default;
        }

        /// <summary>Gets or sets the value associated with the specified key.</summary>
        /// <param name="key">The key of the value to get or set.</param>
        /// <value>
        /// The value associated with the specified key. If the specified key is not found, a get operation throws a
        /// <see cref="KeyNotFoundException"/>, and a set operation creates a new element with the specified key.
        /// </value>
        /// <exception cref="ArgumentNullException">
        /// <paramref name="key"/> is a null reference (Nothing in Visual Basic).
        /// </exception>
        /// <exception cref="KeyNotFoundException">
        public int Count => _table.Count;

        /// <summary>Throws a KeyNotFoundException.</summary>
        /// <remarks>Separate from ThrowHelper to avoid boxing at call site while reusing this generic instantiation.</remarks>
        [DoesNotReturn]
        private static void ThrowKeyNotFoundException(TKey key) =>
            throw new KeyNotFoundException(SR.Format(SR.Arg_KeyNotFoundWithKey, key.ToString()));

        /// <summary>
        /// Gets the <see cref="IEqualityComparer{TKey}" />
        /// that is used to determine equality of keys for the dictionary.
        /// </summary>
        /// <value>
        /// The <see cref="IEqualityComparer{TKey}" /> generic interface implementation
        /// that is used to determine equality of keys for the current
        /// <see cref="ConcurrentDictionary{TKey, TValue}" /> and to provide hash values for the keys.
        /// </value>
        /// <remarks>
        /// <see cref="ConcurrentDictionary{TKey, TValue}" /> requires an equality implementation to determine
        /// whether keys are equal. You can specify an implementation of the <see cref="IEqualityComparer{TKey}" />
        /// generic interface by using a constructor that accepts a comparer parameter;
        /// if you do not specify one, the default generic equality comparer <see cref="EqualityComparer{TKey}.Default" /> is used.
        /// </remarks>
        public IEqualityComparer<TKey> Comparer => _comparer ?? _defaultComparer;

        /// <summary>
        /// Gets the number of key/value pairs contained in the <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>.
        /// </summary>
            return _table.GetOrAdd(key, valueFactory);
            get
            {
                int acquiredLocks = 0;
                try
                {
                    // Acquire all locks
                    AcquireAllLocks(ref acquiredLocks);

                    return GetCountInternal();
                }
                finally
                {
                    // Release locks that have been acquired earlier
                    ReleaseLocks(0, acquiredLocks);
                }
            }
        }

        /// <summary>
        /// Gets the number of key/value pairs contained in the <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>. Should only be used after all locks
        /// have been acquired.
        /// </summary>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <value>The number of key/value pairs contained in the <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>.</value>
        /// <remarks>Count has snapshot semantics and represents the number of items in the <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>
        /// at the moment when Count was accessed.</remarks>
            if (_table.TryGetValue(key, out var value))
            {
                return value;
            }
            else
            {
                TValue newValue = valueFactory(key, factoryArgument);
                TValue oldVal = default;
                if (_table.PutIfMatch(key, newValue, ref oldVal, ValueMatch.NullOrDead))
                {
                    return newValue;
                }
                else
                {
                    return oldVal;
                }
            for (int i = 0; i < countPerLocks.Length; i++)
            {
                count += countPerLocks[i];
            }

            return count;
        }

        /// <summary>
        /// Adds a key/value pair to the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// if the key does not already exist.
        /// </summary>
        /// <param name="key">The key of the element to add.</param>
        /// <param name="valueFactory">The function used to generate a value for the key</param>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentNullException"><paramref name="valueFactory"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <returns>The value for the key.  This will be either the existing value for the key if the
        /// key is already in the dictionary, or the new value for the key as returned by valueFactory
            TValue oldVal = default;
            if (_table.PutIfMatch(key, value, ref oldVal, ValueMatch.NullOrDead))
            {
                return value;
            }

            if (valueFactory is null)
            {
                ThrowHelper.ThrowArgumentNullException(nameof(valueFactory));
            }

            IEqualityComparer<TKey>? comparer = _comparer;
            int hashcode = comparer is null ? key.GetHashCode() : comparer.GetHashCode(key);

            if (!TryGetValueInternal(key, hashcode, out TValue? resultingValue))
            {
                TryAddInternal(key, hashcode, valueFactory(key), updateIfExists: false, acquireLock: true, out resultingValue);
            }

            return resultingValue;
        }

        /// <summary>
        /// Adds a key/value pair to the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// if the key does not already exist.
        /// </summary>
        /// <param name="key">The key of the element to add.</param>
        /// <param name="valueFactory">The function used to generate a value for the key</param>
        /// <param name="factoryArgument">An argument value to pass into <paramref name="valueFactory"/>.</param>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentNullException"><paramref name="valueFactory"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <returns>The value for the key.  This will be either the existing value for the key if the
        /// key is already in the dictionary, or the new value for the key as returned by valueFactory
        /// if the key was not in the dictionary.</returns>
        public TValue GetOrAdd<TArg>(TKey key, Func<TKey, TArg, TValue> valueFactory, TArg factoryArgument)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            if (valueFactory is null)
            {
                ThrowHelper.ThrowArgumentNullException(nameof(valueFactory));
            TValue tValue2;
            int hashcode = comparer is null ? key.GetHashCode() : comparer.GetHashCode(key);

                TValue tValue;
                if (this.TryGetValue(key, out tValue))
            {
                    tValue2 = updateValueFactory(key, tValue, factoryArgument);
                    if (this.TryUpdate(key, tValue2, tValue))

        /// <summary>
        /// Adds a key/value pair to the <see cref="ConcurrentDictionary{TKey,TValue}"/>
        /// if the key does not already exist.
        /// </summary>
        /// <param name="key">The key of the element to add.</param>
                    tValue2 = addValueFactory(key, factoryArgument);
                    if (this.TryAdd(key, tValue2))
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <returns>The value for the key.  This will be either the existing value for the key if the
        /// key is already in the dictionary, or the new value if the key was not in the dictionary.</returns>
        public TValue GetOrAdd(TKey key, TValue value)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            IEqualityComparer<TKey>? comparer = _comparer;
            int hashcode = comparer is null ? key.GetHashCode() : comparer.GetHashCode(key);

            if (!TryGetValueInternal(key, hashcode, out TValue? resultingValue))
            {
                TryAddInternal(key, hashcode, value, updateIfExists: false, acquireLock: true, out resultingValue);
            }

            return oldVal;
        }


        /// <summary>
        /// Adds a key/value pair to the <see cref="ConcurrentDictionary{TKey,TValue}"/> if the key does not already
        /// exist, or updates a key/value pair in the <see cref="ConcurrentDictionary{TKey,TValue}"/> if the key
        /// already exists.
        /// </summary>
        /// <param name="key">The key to be added or whose value should be updated</param>
        /// <param name="addValueFactory">The function used to generate a value for an absent key</param>
        /// <param name="updateValueFactory">The function used to generate a new value for an existing key
        /// based on the key's existing value</param>
        /// <param name="factoryArgument">An argument to pass into <paramref name="addValueFactory"/> and <paramref name="updateValueFactory"/>.</param>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentNullException"><paramref name="addValueFactory"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentNullException"><paramref name="updateValueFactory"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <returns>The new value for the key.  This will be either be the result of addValueFactory (if the key was
        /// absent) or the result of updateValueFactory (if the key was present).</returns>
        public TValue AddOrUpdate<TArg>(
            TKey key, Func<TKey, TArg, TValue> addValueFactory, Func<TKey, TValue, TArg, TValue> updateValueFactory, TArg factoryArgument)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            if (addValueFactory is null)
            TValue tValue2;

            if (updateValueFactory is null)
                TValue tValue;
                if (this.TryGetValue(key, out tValue))
                ThrowHelper.ThrowArgumentNullException(nameof(updateValueFactory));
                    tValue2 = updateValueFactory(key, tValue);
                    if (this.TryUpdate(key, tValue2, tValue))
            int hashcode = comparer is null ? key.GetHashCode() : comparer.GetHashCode(key);

            while (true)
            {
                if (TryGetValueInternal(key, hashcode, out TValue? oldValue))
                {
                    tValue2 = addValueFactory(key);
                    if (this.TryAdd(key, tValue2))
                    if (TryUpdateInternal(key, hashcode, newValue, oldValue))
                    {
                        return tValue2;
                    }
                }
                else
                {
                    // key doesn't exist, try to add
                    if (TryAddInternal(key, hashcode, addValueFactory(key, factoryArgument), updateIfExists: false, acquireLock: true, out TValue resultingValue))
                    {
                        return tValue2;
                    }
                }

                if (tables != _tables)
                {
                    tables = _tables;
                    if (!ReferenceEquals(comparer, tables._comparer))
                    {
                        comparer = tables._comparer;
                        hashcode = GetHashCode(comparer, key);
                    }
                }
            }
        }

        /// <summary>
        /// Adds a key/value pair to the <see cref="ConcurrentDictionary{TKey,TValue}"/> if the key does not already
        /// exist, or updates a key/value pair in the <see cref="ConcurrentDictionary{TKey,TValue}"/> if the key
        /// already exists.
        /// </summary>
        /// <param name="key">The key to be added or whose value should be updated</param>
        /// <param name="addValueFactory">The function used to generate a value for an absent key</param>
        /// <param name="updateValueFactory">The function used to generate a new value for an existing key
        /// based on the key's existing value</param>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentNullException"><paramref name="addValueFactory"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="ArgumentNullException"><paramref name="updateValueFactory"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <returns>The new value for the key.  This will be either the result of addValueFactory (if the key was
        /// absent) or the result of updateValueFactory (if the key was present).</returns>
        public TValue AddOrUpdate(TKey key, Func<TKey, TValue> addValueFactory, Func<TKey, TValue, TValue> updateValueFactory)
        {
            TValue tValue2;
            }

                TValue tValue;
                if (this.TryGetValue(key, out tValue))
            {
                    tValue2 = updateValueFactory(key, tValue);
                    if (this.TryUpdate(key, tValue2, tValue))
            if (updateValueFactory is null)
            {
                ThrowHelper.ThrowArgumentNullException(nameof(updateValueFactory));
            }
                else if (this.TryAdd(key, addValue))
                {
                    return addValue;
                {
                    // key exists, try to update
                    TValue newValue = updateValueFactory(key, oldValue);
                    if (TryUpdateInternal(key, hashcode, newValue, oldValue))
                    {
                        break;
                    }
                }
                else
                {
                    // key doesn't exist, try to add
                    if (TryAddInternal(key, hashcode, addValueFactory(key), updateIfExists: false, acquireLock: true, out TValue resultingValue))
                    {
                        break;
                    }
                }

                if (tables != _tables)
                {
        public bool IsEmpty => _table.Count == 0;
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            if (updateValueFactory is null)
            {
                ThrowHelper.ThrowArgumentNullException(nameof(updateValueFactory));
            }

            IEqualityComparer<TKey>? comparer = _comparer;
            int hashcode = comparer is null ? key.GetHashCode() : comparer.GetHashCode(key);

            while (true)
            {
                if (TryGetValueInternal(key, hashcode, out TValue? oldValue))
                {
                    // key exists, try to update
                    TValue newValue = updateValueFactory(key, oldValue);
                    if (TryUpdateInternal(key, hashcode, newValue, oldValue))
                    {
                        return tValue2;
                    }
                }
                else
                {
                    // key doesn't exist, try to add
                    if (TryAddInternal(key, hashcode, addValue, updateIfExists: false, acquireLock: true, out TValue resultingValue))
                    {
                        return resultingValue;
                    }
                }

                if (tables != _tables)
                {
                    tables = _tables;
                    if (!ReferenceEquals(comparer, tables._comparer))
                    {
                        comparer = tables._comparer;
                        hashcode = GetHashCode(comparer, key);
                    }
                }
            }
        }

        /// <summary>
        /// Gets a value that indicates whether the <see cref="ConcurrentDictionary{TKey,TValue}"/> is empty.
        /// </summary>
        /// <value>true if the <see cref="ConcurrentDictionary{TKey,TValue}"/> is empty; otherwise,
        /// false.</value>
        public bool IsEmpty
        {
            get
            {
                // Check if any buckets are non-empty, without acquiring any locks.
                // This fast path should generally suffice as collections are usually not empty.
                if (!AreAllBucketsEmpty())
                {
                    return false;
                }

                // We didn't see any buckets containing items, however we can't be sure
                // the collection was actually empty at any point in time as items may have been
                // added and removed while iterating over the buckets such that we never saw an
                // empty bucket, but there was always an item present in at least one bucket.
                int acquiredLocks = 0;
                try
                {
                    // Acquire all locks
                    AcquireAllLocks(ref acquiredLocks);

                    return AreAllBucketsEmpty();
                }
                finally
                {
                    // Release locks that have been acquired earlier
                    ReleaseLocks(0, acquiredLocks);
                }


            }
        }

        #region IDictionary<TKey,TValue> members

        /// <summary>
        /// Adds the specified key and value to the <see
        /// cref="IDictionary{TKey,TValue}"/>.
        /// </summary>
        /// <param name="key">The object to use as the key of the element to add.</param>
        /// <param name="value">The object to use as the value of the element to add.</param>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <exception cref="ArgumentException">
        /// An element with the same key already exists in the <see
        bool ICollection<KeyValuePair<TKey, TValue>>.Contains(KeyValuePair<TKey, TValue> keyValuePair)
        {
            TValue value;
            return TryGetValue(keyValuePair.Key, out value) &&
                EqualityComparer<TValue>.Default.Equals(value, keyValuePair.Value);
        }
            }
        }

        /// <summary>
        /// Removes the element with the specified key from the <see
        /// cref="IDictionary{TKey,TValue}"/>.
        /// </summary>
        /// <param name="key">The key of the element to remove.</param>
        /// <returns>true if the element is successfully remove; otherwise false. This method also returns
        /// false if
        /// <paramref name="key"/> was not found in the original <see
        /// cref="IDictionary{TKey,TValue}"/>.
        /// </returns>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        bool IDictionary<TKey, TValue>.Remove(TKey key) => TryRemove(key, out _);

        /// <summary>
        /// Gets a collection containing the keys in the <see
        /// cref="Dictionary{TKey,TValue}"/>.
        /// </summary>
        /// <value>An <see cref="ICollection{TKey}"/> containing the keys in the
        /// <see cref="Dictionary{TKey,TValue}"/>.</value>
        public ICollection<TKey> Keys => GetKeys();

        /// <summary>
        /// Gets an <see cref="IEnumerable{TKey}"/> containing the keys of
        /// the <see cref="IReadOnlyDictionary{TKey,TValue}"/>.
        /// </summary>
        /// <value>An <see cref="IEnumerable{TKey}"/> containing the keys of
        /// the <see cref="IReadOnlyDictionary{TKey,TValue}"/>.</value>
        IEnumerable<TKey> IReadOnlyDictionary<TKey, TValue>.Keys => GetKeys();

        /// <summary>
        /// Gets a collection containing the values in the <see
        /// cref="Dictionary{TKey,TValue}"/>.
        /// </summary>
        /// <value>An <see cref="ICollection{TValue}"/> containing the values in
        /// the
        /// <see cref="Dictionary{TKey,TValue}"/>.</value>
        public ICollection<TValue> Values => GetValues();

        /// <summary>
        /// Gets an <see cref="IEnumerable{TValue}"/> containing the values
        /// in the <see cref="IReadOnlyDictionary{TKey,TValue}"/>.
        /// </summary>
        /// <value>An <see cref="IEnumerable{TValue}"/> containing the
        /// values in the <see cref="IReadOnlyDictionary{TKey,TValue}"/>.</value>
        IEnumerable<TValue> IReadOnlyDictionary<TKey, TValue>.Values => GetValues();
        #endregion

        #region ICollection<KeyValuePair<TKey,TValue>> Members

        /// <summary>
        /// Adds the specified value to the <see cref="ICollection{TValue}"/>
        /// with the specified key.
        /// </summary>
        /// <param name="keyValuePair">The <see cref="KeyValuePair{TKey,TValue}"/>
        /// structure representing the key and value to add to the <see
        /// cref="Dictionary{TKey,TValue}"/>.</param>
        /// <exception cref="ArgumentNullException">The <paramref name="keyValuePair"/> of <paramref
        /// name="keyValuePair"/> is null.</exception>
        /// <exception cref="OverflowException">The <see
        /// cref="Dictionary{TKey,TValue}"/>
        /// contains too many elements.</exception>
        /// <exception cref="ArgumentException">An element with the same key already exists in the
        /// <see cref="Dictionary{TKey,TValue}"/></exception>
        void ICollection<KeyValuePair<TKey, TValue>>.Add(KeyValuePair<TKey, TValue> keyValuePair) => ((IDictionary<TKey, TValue>)this).Add(keyValuePair.Key, keyValuePair.Value);

        /// <summary>
        /// Determines whether the <see cref="ICollection{T}"/>
        /// contains a specific key and value.
        /// </summary>
        /// <param name="keyValuePair">The <see cref="KeyValuePair{TKey,TValue}"/>
        /// structure to locate in the <see
        /// cref="ICollection{TValue}"/>.</param>
        /// <returns>true if the <paramref name="keyValuePair"/> is found in the <see
        /// cref="ICollection{T}"/>; otherwise, false.</returns>
        bool ICollection<KeyValuePair<TKey, TValue>>.Contains(KeyValuePair<TKey, TValue> keyValuePair)
        {
            if (!TryGetValue(keyValuePair.Key, out TValue? value))
            {
                return false;
            }
            return EqualityComparer<TValue>.Default.Equals(value, keyValuePair.Value);
        }

        /// <summary>
        /// Gets a value indicating whether the dictionary is read-only.
        /// </summary>
        /// <value>true if the <see cref="ICollection{T}"/> is
        /// read-only; otherwise, false. For <see
        /// cref="Dictionary{TKey,TValue}"/>, this property always returns
        /// false.</value>
        bool ICollection<KeyValuePair<TKey, TValue>>.IsReadOnly => false;

        /// <summary>
        /// Removes a key and value from the dictionary.
        /// </summary>
        /// <param name="keyValuePair">The <see
        /// cref="KeyValuePair{TKey,TValue}"/>
        /// structure representing the key and value to remove from the <see
        /// cref="Dictionary{TKey,TValue}"/>.</param>
        /// <returns>true if the key and value represented by <paramref name="keyValuePair"/> is successfully
        /// found and removed; otherwise, false.</returns>
        /// <exception cref="ArgumentNullException">The Key property of <paramref
        /// name="keyValuePair"/> is a null reference (Nothing in Visual Basic).</exception>
        bool ICollection<KeyValuePair<TKey, TValue>>.Remove(KeyValuePair<TKey, TValue> keyValuePair) =>
            TryRemove(keyValuePair);

        #endregion

        #region IEnumerable Members

        /// <summary>Returns an enumerator that iterates through the <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>.</summary>
        /// <returns>An enumerator for the <see cref="ConcurrentDictionary{TKey,TValue}"/>.</returns>
        /// <remarks>
        /// The enumerator returned from the dictionary is safe to use concurrently with
        /// reads and writes to the dictionary, however it does not represent a moment-in-time snapshot
        /// of the dictionary.  The contents exposed through the enumerator may contain modifications
        /// made to the dictionary after <see cref="GetEnumerator"/> was called.
        /// </remarks>
        IEnumerator IEnumerable.GetEnumerator() => ((ConcurrentDictionary<TKey, TValue>)this).GetEnumerator();

        #endregion

        #region IDictionary Members

        /// <summary>
        /// Adds the specified key and value to the dictionary.
        /// </summary>
        /// <param name="key">The object to use as the key.</param>
        /// <param name="value">The object to use as the value.</param>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        /// <exception cref="OverflowException">The dictionary contains too many
        /// elements.</exception>
        /// <exception cref="ArgumentException">
        /// <paramref name="key"/> is of a type that is not assignable to the key type <typeparamref
        /// name="TKey"/> of the <see cref="Dictionary{TKey,TValue}"/>. -or-
        /// <paramref name="value"/> is of a type that is not assignable to <typeparamref name="TValue"/>,
        /// the type of values in the <see cref="Dictionary{TKey,TValue}"/>.
        /// -or- A value with the same key already exists in the <see
        /// cref="Dictionary{TKey,TValue}"/>.
        /// </exception>
        void IDictionary.Add(object key, object? value)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            if (!(key is TKey))
            {
                throw new ArgumentException(SR.ConcurrentDictionary_TypeOfKeyIncorrect);
            }

            ThrowIfInvalidObjectValue(value);

            ((IDictionary<TKey, TValue>)this).Add((TKey)key, (TValue)value!);
        }

        /// <summary>
        /// Gets whether the <see cref="IDictionary"/> contains an
        /// element with the specified key.
        /// </summary>
        /// <param name="key">The key to locate in the <see
        /// cref="IDictionary"/>.</param>
        /// <returns>true if the <see cref="IDictionary"/> contains
        /// an element with the specified key; otherwise, false.</returns>
        /// <exception cref="ArgumentNullException"> <paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        bool IDictionary.Contains(object key)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            return key is TKey tkey && ContainsKey(tkey);
        }

        /// <summary>Provides an <see cref="IDictionaryEnumerator"/> for the
        /// <see cref="IDictionary"/>.</summary>
        /// <returns>An <see cref="IDictionaryEnumerator"/> for the <see
        /// cref="IDictionary"/>.</returns>
        IDictionaryEnumerator IDictionary.GetEnumerator() => new SnapshotIDictionaryEnumerator(_table.GetSnapshot());

        /// <summary>
        /// Gets a value indicating whether the <see
        /// cref="IDictionary"/> has a fixed size.
        /// </summary>
        /// <value>true if the <see cref="IDictionary"/> has a
        /// fixed size; otherwise, false. For <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>, this property always
        /// returns false.</value>
        bool IDictionary.IsFixedSize => false;

        /// <summary>
        /// Gets a value indicating whether the <see
        /// cref="IDictionary"/> is read-only.
        /// </summary>
        /// <value>true if the <see cref="IDictionary"/> is
        /// read-only; otherwise, false. For <see
        /// cref="ConcurrentDictionary{TKey,TValue}"/>, this property always
        /// returns false.</value>
        bool IDictionary.IsReadOnly => false;

        /// <summary>
        /// Gets an <see cref="ICollection"/> containing the keys of the <see
        /// cref="IDictionary"/>.
        /// </summary>
        /// <value>An <see cref="ICollection"/> containing the keys of the <see
        /// cref="IDictionary"/>.</value>
        ICollection IDictionary.Keys => GetKeys();

        /// <summary>
        /// Removes the element with the specified key from the <see
        /// cref="IDictionary"/>.
        /// </summary>
        /// <param name="key">The key of the element to remove.</param>
        /// <exception cref="ArgumentNullException"><paramref name="key"/> is a null reference
        /// (Nothing in Visual Basic).</exception>
        void IDictionary.Remove(object key)
        {
            if (key is null)
            {
                ThrowHelper.ThrowKeyNullException();
            }

            if (key is TKey tkey)
            {
                TryRemove(tkey, out _);
            }
        }

        /// <summary>
        /// Gets an <see cref="ICollection"/> containing the values in the <see
        /// cref="IDictionary"/>.
        /// </summary>
        /// <value>An <see cref="ICollection"/> containing the values in the <see
        /// cref="IDictionary"/>.</value>
        ICollection IDictionary.Values => GetValues();

        /// <summary>
        /// Gets or sets the value associated with the specified key.
        /// </summary>
        /// <param name="key">The key of the value to get or set.</param>
        /// <value>The value associated with the specified key, or a null reference (Nothing in Visual Basic)
        /// if <paramref name="key"/> is not in the dictionary or <paramref name="key"/> is of a type that is
        /// not assignable to the key type <typeparamref name="TKey"/> of the <see

            // To be consistent with the behavior of ICollection.CopyTo() in Dictionary<TKey,TValue>,
            // we recognize three types of target arrays:
            //    - an array of KeyValuePair<TKey, TValue> structs
            //    - an array of DictionaryEntry structs
            //    - an array of objects
                }

                if (key is TKey tkey && TryGetValue(tkey, out TValue? value))
                {
                    return value;
                }

                return null;
            }
            set
            {
                if (key is null)
                {
                    ThrowHelper.ThrowKeyNullException();
                }

                if (!(key is TKey))
                {
                    throw new ArgumentException(SR.ConcurrentDictionary_TypeOfKeyIncorrect);
            throw new ArgumentException(SR.ConcurrentDictionary_ArrayIncorrectType, nameof(array));
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static void ThrowIfInvalidObjectValue(object? value)
        {
            if (value is not null)
            {
                if (!(value is TValue))
                {
                    ThrowHelper.ThrowValueNullException();
                }
            }
            else if (default(TValue) is not null)
            {
                ThrowHelper.ThrowValueNullException();
            }
        }

        #endregion

        #region ICollection Members
                    for (int i = tables._locks.Length; i < newLocks.Length; i++)
                    {
                        newLocks[i] = new object();
                    }
                }
            var keys = new List<TKey>(Count);
            foreach (var kv in this)
            {
                keys.Add(kv.Key);
            }
                        {
            return new ReadOnlyCollection<TKey>(keys);
        }
                // Adjust the budget
                _budget = Math.Max(1, newBuckets.Length / newLocks.Length);

                // Replace tables with the new versions
                _tables = newTables;
            }
            var values = new List<TValue>(Count);
            foreach (var kv in this)
            {
                values.Add(kv.Value);
            }
            }
            return new ReadOnlyCollection<TValue>(values);
        }
            Debug.Assert(locksAcquired == _tables._locks.Length);
        }

        /// <summary>
        /// Acquires a contiguous range of locks for this hash table, and increments locksAcquired
        /// by the number of locks that were successfully acquired. The locks are acquired in an
        /// increasing order.
        /// </summary>
        private void AcquireLocks(int fromInclusive, int toExclusive, ref int locksAcquired)
            public KeyValuePair<TKey, TValue> Current => _snapshot.Current;
            object IEnumerator.Current => _snapshot.Current;

            public bool MoveNext() => _snapshot.MoveNext();
            public void Reset() => _snapshot.Reset();
            public void Dispose() { }
        }
                        locksAcquired++;
        internal class SnapshotIDictionaryEnumerator : IDictionaryEnumerator
        {
            private DictionaryImpl<TKey, TValue>.Snapshot _snapshot;
            public SnapshotIDictionaryEnumerator(DictionaryImpl<TKey, TValue>.Snapshot snapshot)
            {
                _snapshot = snapshot;
            }

        /// <summary>
        /// Gets a collection containing the keys in the dictionary.
        /// </summary>
        private ReadOnlyCollection<TKey> GetKeys()
        {
            int locksAcquired = 0;
            try
            {
                AcquireAllLocks(ref locksAcquired);

                int count = GetCountInternal();
                if (count < 0)
                {
                    ThrowHelper.ThrowOutOfMemoryException();
                }

                var keys = new List<TKey>(count);
                Node?[] buckets = _tables._buckets;
                for (int i = 0; i < buckets.Length; i++)
                {
                    for (Node? current = buckets[i]; current != null; current = current._next)
                    {
                        keys.Add(current._key);
                    }
                }

                return new ReadOnlyCollection<TKey>(keys);
            }
            finally
            {
                ReleaseLocks(0, locksAcquired);
            }
        }

        /// <summary>
        /// Gets a collection containing the values in the dictionary.
        /// </summary>
        private ReadOnlyCollection<TValue> GetValues()
        {
            int locksAcquired = 0;
            try
            {
                AcquireAllLocks(ref locksAcquired);

                int count = GetCountInternal();
                if (count < 0)
                {
                    ThrowHelper.ThrowOutOfMemoryException();
                }

                var values = new List<TValue>(count);
                Node?[] buckets = _tables._buckets;
                for (int i = 0; i < buckets.Length; i++)
                {
                    for (Node? current = buckets[i]; current != null; current = current._next)
                    {
                        values.Add(current._value);
                    }
                }

                return new ReadOnlyCollection<TValue>(values);
            }
            finally
            {
                ReleaseLocks(0, locksAcquired);
            }
        }

        internal class SnapshotEnumerator : IEnumerator<KeyValuePair<TKey, TValue>>
        {
            private DictionaryImpl<TKey, TValue>.Snapshot _snapshot;
            public SnapshotEnumerator(DictionaryImpl<TKey, TValue>.Snapshot snapshot)
            {
                _snapshot = snapshot;
            }

        /// <summary>Tables that hold the internal state of the ConcurrentDictionary</summary>
        /// <remarks>
        /// Wrapping the three tables in a single object allows us to atomically
        /// replace all tables at once.
        /// </remarks>
        private sealed class Tables
        {
            /// <summary>A singly-linked list for each bucket.</summary>
            internal readonly Node?[] _buckets;
            /// <summary>A set of locks, each guarding a section of the table.</summary>
            internal readonly object[] _locks;
            /// <summary>The number of elements guarded by each lock.</summary>
            internal readonly int[] _countPerLock;
            /// <summary>Pre-computed multiplier for use on 64-bit performing faster modulo operations.</summary>
            internal readonly ulong _fastModBucketsMultiplier;

            internal Tables(Node?[] buckets, object[] locks, int[] countPerLock)
            {
                _buckets = buckets;
                _locks = locks;
                _countPerLock = countPerLock;
                if (IntPtr.Size == 8)
                {
                    _fastModBucketsMultiplier = HashHelpers.GetFastModMultiplier((uint)buckets.Length);
                }
            }

            /// <summary>Computes a ref to the bucket for a particular key.</summary>
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            internal ref Node? GetBucket(int hashcode)
            {
                Node?[] buckets = _buckets;
                if (IntPtr.Size == 8)
                {
                    return ref buckets[HashHelpers.FastMod((uint)hashcode, (uint)buckets.Length, _fastModBucketsMultiplier)];
                }
                else
                {
                    return ref buckets[(uint)hashcode % (uint)buckets.Length];
                }
            }

            /// <summary>Computes the bucket and lock number for a particular key.</summary>
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            internal ref Node? GetBucketAndLock(int hashcode, out uint lockNo)
            {
                Node?[] buckets = _buckets;
                uint bucketNo;
                if (IntPtr.Size == 8)
                {
                    bucketNo = HashHelpers.FastMod((uint)hashcode, (uint)buckets.Length, _fastModBucketsMultiplier);
                }
                else
                {
                    bucketNo = (uint)hashcode % (uint)buckets.Length;
                }
                lockNo = bucketNo % (uint)_locks.Length; // doesn't use FastMod, as it would require maintaining a different multiplier
                return ref buckets[bucketNo];
            }
        }

            public DictionaryEntry Entry => _snapshot.Entry;
            object IEnumerator.Current => _snapshot.Entry;

            public object Key => _snapshot.Current.Key;
            public object Value => _snapshot.Current.Value;

            public bool MoveNext() => _snapshot.MoveNext();
            public void Reset() => _snapshot.Reset();
            public void Dispose() { }
        }
    }

    internal static class ConcurrentDictionaryTypeProps<T>
    {
        /// <summary>Whether T's type can be written atomically (i.e., with no danger of torn reads).</summary>
        internal static readonly bool IsWriteAtomic = IsWriteAtomicPrivate();

        private static bool IsWriteAtomicPrivate()
        {
            // Section 12.6.6 of ECMA CLI explains which types can be read and written atomically without
            // the risk of tearing. See https://www.ecma-international.org/publications/files/ECMA-ST/ECMA-335.pdf

            if (!typeof(T).IsValueType ||
                typeof(T) == typeof(IntPtr) ||
                typeof(T) == typeof(UIntPtr))
            {
                return true;
            }

            switch (Type.GetTypeCode(typeof(T)))
            {
                case TypeCode.Boolean:
                case TypeCode.Byte:
                case TypeCode.Char:
                case TypeCode.Int16:
                case TypeCode.Int32:
                case TypeCode.SByte:
                case TypeCode.Single:
                case TypeCode.UInt16:
                case TypeCode.UInt32:
                    return true;

                case TypeCode.Double:
                case TypeCode.Int64:
                case TypeCode.UInt64:
                    return IntPtr.Size == 8;

                default:
                    return false;
            }
        }
    }

    internal sealed class IDictionaryDebugView<TKey, TValue> where TKey : notnull
    {
        private readonly IDictionary<TKey, TValue> _dictionary;

        public IDictionaryDebugView(IDictionary<TKey, TValue> dictionary)
        {
            if (dictionary is null)
            {
                ThrowHelper.ThrowArgumentNullException(nameof(dictionary));
            }

            _dictionary = dictionary;
        }

        [DebuggerBrowsable(DebuggerBrowsableState.RootHidden)]
        public KeyValuePair<TKey, TValue>[] Items
        {
            get
            {
                var items = new KeyValuePair<TKey, TValue>[_dictionary.Count];
                _dictionary.CopyTo(items, 0);
                return items;
            }
        }
    }
}
