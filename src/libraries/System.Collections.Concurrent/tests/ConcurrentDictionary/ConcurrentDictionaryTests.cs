// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace System.Collections.Concurrent.Tests
{
    public class ConcurrentDictionaryTests
    {
        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        public static void TestBasicScenarios()
        {
            ConcurrentDictionary<int, int> cd = new ConcurrentDictionary<int, int>();

            Task[] tks = new Task[2];
            tks[0] = Task.Run(() =>
            {
                var ret = cd.TryAdd(1, 11);
                if (!ret)
                {
                    ret = cd.TryUpdate(1, 11, 111);
                    Assert.True(ret);
                }

                ret = cd.TryAdd(2, 22);
                if (!ret)
                {
                    ret = cd.TryUpdate(2, 22, 222);
                    Assert.True(ret);
                }
            });

            tks[1] = Task.Run(() =>
            {
                var ret = cd.TryAdd(2, 222);
                if (!ret)
                {
                    ret = cd.TryUpdate(2, 222, 22);
                    Assert.True(ret);
                }

                ret = cd.TryAdd(1, 111);
                if (!ret)
                {
                    ret = cd.TryUpdate(1, 111, 11);
                    Assert.True(ret);
                }
            });

            Task.WaitAll(tks);
        }

        [Fact]
        public static void TestAddNullValue_ConcurrentDictionaryOfString_null()
        {
            // using ConcurrentDictionary<TKey, TValue> class
            ConcurrentDictionary<string, string> dict1 = new ConcurrentDictionary<string, string>();
            dict1["key"] = null;
        }

        [Fact]
        public static void TestAddNullValue_IDictionaryOfString_null()
        {
            // using IDictionary<TKey, TValue> interface
            IDictionary<string, string> dict2 = new ConcurrentDictionary<string, string>();
            dict2["key"] = null;
            dict2.Add("key2", null);
        }

        [Fact]
        public static void TestAddNullValue_IDictionary_ReferenceType_null()
        {
            // using IDictionary interface
            IDictionary dict3 = new ConcurrentDictionary<string, string>();
            dict3["key"] = null;
            dict3.Add("key2", null);
        }

        [Fact]
        public static void TestAddNullValue_IDictionary_ValueType_null_indexer()
        {
            // using IDictionary interface and value type values
            Action action = () =>
            {
                IDictionary dict4 = new ConcurrentDictionary<string, int>();
                dict4["key"] = null;
            };
            Assert.Throws<ArgumentException>(action);
        }

        [Fact]
        public static void TestAddNullValue_IDictionary_ValueType_null_add()
        {
            Action action = () =>
            {
                IDictionary dict5 = new ConcurrentDictionary<string, int>();
                dict5.Add("key", null);
            };
            Assert.Throws<ArgumentException>(action);
        }

        [Fact]
        public static void TestAddValueOfDifferentType()
        {
            Action action = () =>
            {
                IDictionary dict = new ConcurrentDictionary<string, string>();
                dict["key"] = 1;
            };
            Assert.Throws<ArgumentException>(action);

            action = () =>
            {
                IDictionary dict = new ConcurrentDictionary<string, string>();
                dict.Add("key", 1);
            };
            Assert.Throws<ArgumentException>(action);
        }

        [ConditionalTheory(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        [InlineData(1, 1, 1, 10000)]
        [InlineData(5, 1, 1, 10000)]
        [InlineData(1, 1, 2, 5000)]
        [InlineData(1, 1, 5, 2000)]
        [InlineData(4, 0, 4, 2000)]
        [InlineData(16, 31, 4, 2000)]
        [InlineData(64, 5, 5, 5000)]
        [InlineData(5, 5, 5, 2500)]
        public static void TestAdd(int cLevel, int initSize, int threads, int addsPerThread)
        {
            ConcurrentDictionary<int, int> dictConcurrent = new ConcurrentDictionary<int, int>(cLevel, 1);
            IDictionary<int, int> dict = dictConcurrent;

            int count = threads;
            using (ManualResetEvent mre = new ManualResetEvent(false))
            {
                for (int i = 0; i < threads; i++)
                {
                    int ii = i;
                    Task.Run(
                        () =>
                        {
                            for (int j = 0; j < addsPerThread; j++)
                            {
                                dict.Add(j + ii * addsPerThread, -(j + ii * addsPerThread));
                            }
                            if (Interlocked.Decrement(ref count) == 0) mre.Set();
                        });
                }
                mre.WaitOne();
            }

            foreach (var pair in dict)
            {
                Assert.Equal(pair.Key, -pair.Value);
            }

            List<int> gotKeys = new List<int>();
            foreach (var pair in dict)
                gotKeys.Add(pair.Key);

            gotKeys.Sort();

            List<int> expectKeys = new List<int>();
            int itemCount = threads * addsPerThread;
            for (int i = 0; i < itemCount; i++)
                expectKeys.Add(i);

            Assert.Equal(expectKeys.Count, gotKeys.Count);

            for (int i = 0; i < expectKeys.Count; i++)
            {
                Assert.True(expectKeys[i].Equals(gotKeys[i]),
                    string.Format("The set of keys in the dictionary is are not the same as the expected" + Environment.NewLine +
                            "TestAdd1(cLevel={0}, initSize={1}, threads={2}, addsPerThread={3})", cLevel, initSize, threads, addsPerThread)
                   );
            }

            // Finally, let's verify that the count is reported correctly.
            int expectedCount = threads * addsPerThread;
            Assert.Equal(expectedCount, dict.Count);
            Assert.Equal(expectedCount, dictConcurrent.ToArray().Length);
        }

        [ConditionalTheory(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        [InlineData(1, 1, 10000)]
        [InlineData(5, 1, 10000)]
        [InlineData(1, 2, 5000)]
        [InlineData(1, 5, 2001)]
        [InlineData(4, 4, 2001)]
        [InlineData(15, 5, 2001)]
        [InlineData(64, 5, 5000)]
        [InlineData(5, 5, 25000)]
        public static void TestUpdate(int cLevel, int threads, int updatesPerThread)
        {
            IDictionary<int, int> dict = new ConcurrentDictionary<int, int>(cLevel, 1);

            for (int i = 1; i <= updatesPerThread; i++) dict[i] = i;

            int running = threads;
            using (ManualResetEvent mre = new ManualResetEvent(false))
            {
                for (int i = 0; i < threads; i++)
                {
                    int ii = i;
                    Task.Run(
                        () =>
                        {
                            for (int j = 1; j <= updatesPerThread; j++)
                            {
                                dict[j] = (ii + 2) * j;
                            }
                            if (Interlocked.Decrement(ref running) == 0) mre.Set();
                        });
                }
                mre.WaitOne();
            }

            foreach (var pair in dict)
            {
                var div = pair.Value / pair.Key;
                var rem = pair.Value % pair.Key;

                Assert.Equal(0, rem);
                Assert.True(div > 1 && div <= threads + 1,
                    string.Format("* Invalid value={3}! TestUpdate1(cLevel={0}, threads={1}, updatesPerThread={2})", cLevel, threads, updatesPerThread, div));
            }

            List<int> gotKeys = new List<int>();
            foreach (var pair in dict)
                gotKeys.Add(pair.Key);
            gotKeys.Sort();

            List<int> expectKeys = new List<int>();
            for (int i = 1; i <= updatesPerThread; i++)
                expectKeys.Add(i);

            Assert.Equal(expectKeys.Count, gotKeys.Count);

            for (int i = 0; i < expectKeys.Count; i++)
            {
                Assert.True(expectKeys[i].Equals(gotKeys[i]),
                   string.Format("The set of keys in the dictionary is are not the same as the expected." + Environment.NewLine +
                           "TestUpdate1(cLevel={0}, threads={1}, updatesPerThread={2})", cLevel, threads, updatesPerThread)
                  );
            }
        }

        [ConditionalTheory(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        [InlineData(1, 1, 10000)]
        [InlineData(5, 1, 10000)]
        [InlineData(1, 2, 5000)]
        [InlineData(1, 5, 2001)]
        [InlineData(4, 4, 2001)]
        [InlineData(15, 5, 2001)]
        [InlineData(64, 5, 5000)]
        [InlineData(5, 5, 25000)]
        public static void TestRead1(int cLevel, int threads, int readsPerThread)
        {
            IDictionary<int, int> dict = new ConcurrentDictionary<int, int>(cLevel, 1);

            for (int i = 0; i < readsPerThread; i += 2) dict[i] = i;

            int count = threads;
            using (ManualResetEvent mre = new ManualResetEvent(false))
            {
                for (int i = 0; i < threads; i++)
                {
                    int ii = i;
                    Task.Run(
                        () =>
                        {
                            for (int j = 0; j < readsPerThread; j++)
                            {
                                int val = 0;
                                if (dict.TryGetValue(j, out val))
                                {
                                    Assert.Equal(0, j % 2);
                                    Assert.Equal(j, val);
                                }
                                else
                                {
                                    Assert.Equal(1, j % 2);
                                }
                            }
                            if (Interlocked.Decrement(ref count) == 0) mre.Set();
                        });
                }
                mre.WaitOne();
            }
        }

        [ConditionalTheory(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        [InlineData(1, 1, 10000)]
        [InlineData(5, 1, 1000)]
        [InlineData(1, 5, 2001)]
        [InlineData(4, 4, 2001)]
        [InlineData(15, 5, 2001)]
        [InlineData(64, 5, 5000)]
        public static void TestRemove1(int cLevel, int threads, int removesPerThread)
        {
            ConcurrentDictionary<int, int> dict = new ConcurrentDictionary<int, int>(cLevel, 1);
            string methodparameters = string.Format("* TestRemove1(cLevel={0}, threads={1}, removesPerThread={2})", cLevel, threads, removesPerThread);
            int N = 2 * threads * removesPerThread;

            for (int i = 0; i < N; i++) dict[i] = -i;

            // The dictionary contains keys [0..N), each key mapped to a value equal to the key.
            // Threads will cooperatively remove all even keys

            int running = threads;
            using (ManualResetEvent mre = new ManualResetEvent(false))
            {
                for (int i = 0; i < threads; i++)
                {
                    int ii = i;
                    Task.Run(
                        () =>
                        {
                            for (int j = 0; j < removesPerThread; j++)
                            {
                                int value;
                                int key = 2 * (ii + j * threads);
                                Assert.True(dict.TryRemove(key, out value), "Failed to remove an element! " + methodparameters);

                                Assert.Equal(-key, value);
                            }

                            if (Interlocked.Decrement(ref running) == 0) mre.Set();
                        });
                }
                mre.WaitOne();
            }

            foreach (var pair in dict)
            {
                Assert.Equal(pair.Key, -pair.Value);
            }

            List<int> gotKeys = new List<int>();
            foreach (var pair in dict)
                gotKeys.Add(pair.Key);
            gotKeys.Sort();

            List<int> expectKeys = new List<int>();
            for (int i = 0; i < (threads * removesPerThread); i++)
                expectKeys.Add(2 * i + 1);

            Assert.Equal(expectKeys.Count, gotKeys.Count);

            for (int i = 0; i < expectKeys.Count; i++)
            {
                Assert.True(expectKeys[i].Equals(gotKeys[i]), "  > Unexpected key value! " + methodparameters);
            }

            // Finally, let's verify that the count is reported correctly.
            Assert.Equal(expectKeys.Count, dict.Count);
            Assert.Equal(expectKeys.Count, dict.ToArray().Length);
        }

        [ConditionalTheory(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        [InlineData(1)]
        [InlineData(10)]
        [InlineData(5000)]
        public static void TestRemove2(int removesPerThread)
        {
            ConcurrentDictionary<int, int> dict = new ConcurrentDictionary<int, int>();

            for (int i = 0; i < removesPerThread; i++) dict[i] = -i;

            // The dictionary contains keys [0..N), each key mapped to a value equal to the key.
            // Threads will cooperatively remove all even keys.
            const int SIZE = 2;
            int running = SIZE;

            bool[][] seen = new bool[SIZE][];
            for (int i = 0; i < SIZE; i++) seen[i] = new bool[removesPerThread];

            using (ManualResetEvent mre = new ManualResetEvent(false))
            {
                for (int t = 0; t < SIZE; t++)
                {
                    int thread = t;
                    Task.Run(
                        () =>
                        {
                            for (int key = 0; key < removesPerThread; key++)
                            {
                                int value;
                                if (dict.TryRemove(key, out value))
                                {
                                    seen[thread][key] = true;

                                    Assert.Equal(-key, value);
                                }
                            }
                            if (Interlocked.Decrement(ref running) == 0) mre.Set();
                        });
                }
                mre.WaitOne();
            }

            Assert.Equal(0, dict.Count);

            for (int i = 0; i < removesPerThread; i++)
            {
                Assert.False(seen[0][i] == seen[1][i],
                    string.Format("> FAILED. Two threads appear to have removed the same element. TestRemove2(removesPerThread={0})", removesPerThread)
                    );
            }
        }

        [Fact]
        public static void TestRemove3()
        {
            ConcurrentDictionary<int, int> dict = new ConcurrentDictionary<int, int>();

            dict[99] = -99;

            ICollection<KeyValuePair<int, int>> col = dict;

            // Make sure we cannot "remove" a key/value pair which is not in the dictionary
            for (int i = 0; i < 200; i++)
            {
                if (i != 99)
                {
                    Assert.False(col.Remove(new KeyValuePair<int, int>(i, -99)), "Should not remove not existing a key/value pair - new KeyValuePair<int, int>(i, -99)");
                    Assert.False(col.Remove(new KeyValuePair<int, int>(99, -i)), "Should not remove not existing a key/value pair - new KeyValuePair<int, int>(99, -i)");
                }
            }

            // Can we remove a key/value pair successfully?
            Assert.True(col.Remove(new KeyValuePair<int, int>(99, -99)), "Failed to remove existing key/value pair");

            // Make sure the key/value pair is gone
            Assert.False(col.Remove(new KeyValuePair<int, int>(99, -99)), "Should not remove the key/value pair which has been removed");

            // And that the dictionary is empty. We will check the count in a few different ways:
            Assert.Equal(0, dict.Count);
            Assert.Equal(0, dict.ToArray().Length);
        }

        [Fact]
        public static void TryRemove_KeyValuePair_ArgumentValidation()
        {
            AssertExtensions.Throws<ArgumentNullException>("item", () => new ConcurrentDictionary<string, int>().TryRemove(new KeyValuePair<string, int>(null, 42)));
            new ConcurrentDictionary<int, int>().TryRemove(new KeyValuePair<int, int>(0, 0)); // no error when using default value type
            new ConcurrentDictionary<int?, int>().TryRemove(new KeyValuePair<int?, int>(0, 0)); // or nullable
        }

        [Fact]
        public static void TryRemove_KeyValuePair_RemovesSuccessfullyAsAppropriate()
        {
            var dict = new ConcurrentDictionary<string, int>();

            for (int i = 0; i < 2; i++)
            {
                Assert.False(dict.TryRemove(KeyValuePair.Create("key", 42)));
                Assert.Equal(0, dict.Count);
                Assert.True(dict.TryAdd("key", 42));
                Assert.Equal(1, dict.Count);
                Assert.True(dict.TryRemove(KeyValuePair.Create("key", 42)));
                Assert.Equal(0, dict.Count);
            }

            Assert.True(dict.TryAdd("key", 42));
            Assert.False(dict.TryRemove(KeyValuePair.Create("key", 43))); // value doesn't match
        }

        [Fact]
        public static void TryRemove_KeyValuePair_MatchesKeyWithDefaultComparer()
        {
            var dict = new ConcurrentDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            dict.TryAdd("key", "value");
            Assert.False(dict.TryRemove(KeyValuePair.Create("key", "VALUE")));
            Assert.True(dict.TryRemove(KeyValuePair.Create("KEY", "value")));
        }

        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        public static void TestGetOrAdd()
        {
            TestGetOrAddOrUpdate(1, 1, 1, 10000, true);
            TestGetOrAddOrUpdate(5, 1, 1, 10000, true);
            TestGetOrAddOrUpdate(1, 1, 2, 5000, true);
            TestGetOrAddOrUpdate(1, 1, 5, 2000, true);
            TestGetOrAddOrUpdate(4, 0, 4, 2000, true);
            TestGetOrAddOrUpdate(16, 31, 4, 2000, true);
            TestGetOrAddOrUpdate(64, 5, 5, 5000, true);
            TestGetOrAddOrUpdate(5, 5, 5, 25000, true);
        }

        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        public static void TestAddOrUpdate()
        {
            TestGetOrAddOrUpdate(1, 1, 1, 10000, false);
            TestGetOrAddOrUpdate(5, 1, 1, 10000, false);
            TestGetOrAddOrUpdate(1, 1, 2, 5000, false);
            TestGetOrAddOrUpdate(1, 1, 5, 2000, false);
            TestGetOrAddOrUpdate(4, 0, 4, 2000, false);
            TestGetOrAddOrUpdate(16, 31, 4, 2000, false);
            TestGetOrAddOrUpdate(64, 5, 5, 5000, false);
            TestGetOrAddOrUpdate(5, 5, 5, 25000, false);
        }

        private static void TestGetOrAddOrUpdate(int cLevel, int initSize, int threads, int addsPerThread, bool isAdd)
        {
            ConcurrentDictionary<int, int> dict = new ConcurrentDictionary<int, int>(cLevel, 1);

            int count = threads;
            using (ManualResetEvent mre = new ManualResetEvent(false))
            {
                for (int i = 0; i < threads; i++)
                {
                    int ii = i;
                    Task.Run(
                        () =>
                        {
                            for (int j = 0; j < addsPerThread; j++)
                            {
                                if (isAdd)
                                {
                                    //call one of the overloads of GetOrAdd
                                    switch (j % 3)
                                    {
                                        case 0:
                                            dict.GetOrAdd(j, -j);
                                            break;
                                        case 1:
                                            dict.GetOrAdd(j, x => -x);
                                            break;
                                        case 2:
                                            dict.GetOrAdd(j, (x,m) => x * m, -1);
                                            break;
                                    }
                                }
                                else
                                {
                                    switch (j % 3)
                                    {
                                        case 0:
                                            dict.AddOrUpdate(j, -j, (k, v) => -j);
                                            break;
                                        case 1:
                                            dict.AddOrUpdate(j, (k) => -k, (k, v) => -k);
                                            break;
                                        case 2:
                                            dict.AddOrUpdate(j, (k,m) => k*m, (k, v, m) => k * m, -1);
                                            break;
                                    }
                                }
                            }
                            if (Interlocked.Decrement(ref count) == 0) mre.Set();
                        });
                }
                mre.WaitOne();
            }

            foreach (var pair in dict)
            {
                Assert.Equal(pair.Key, -pair.Value);
            }

            List<int> gotKeys = new List<int>();
            foreach (var pair in dict)
                gotKeys.Add(pair.Key);
            gotKeys.Sort();

            List<int> expectKeys = new List<int>();
            for (int i = 0; i < addsPerThread; i++)
                expectKeys.Add(i);

            Assert.Equal(expectKeys.Count, gotKeys.Count);

            for (int i = 0; i < expectKeys.Count; i++)
            {
                Assert.True(expectKeys[i].Equals(gotKeys[i]),
                    string.Format("* Test '{4}': Level={0}, initSize={1}, threads={2}, addsPerThread={3})" + Environment.NewLine +
                    "> FAILED.  The set of keys in the dictionary is are not the same as the expected.",
                    cLevel, initSize, threads, addsPerThread, isAdd ? "GetOrAdd" : "GetOrUpdate"));
            }

            // Finally, let's verify that the count is reported correctly.
            Assert.Equal(addsPerThread, dict.Count);
            Assert.Equal(addsPerThread, dict.ToArray().Length);
        }

        [Fact]
        public static void TestBugFix669376()
        {
            var cd = new ConcurrentDictionary<string, int>(EqualityComparer<string>.Create((x, y) => string.Equals(x, y, StringComparison.OrdinalIgnoreCase), x => 0));
            cd["test"] = 10;
            Assert.True(cd.ContainsKey("TEST"), "Customized comparer didn't work");
        }

        [Fact]
        public static void TestConstructor()
        {
            var dictionary = new ConcurrentDictionary<int, int>(new[] { new KeyValuePair<int, int>(1, 1) });
            Assert.False(dictionary.IsEmpty);
            Assert.Equal(1, dictionary.Keys.Count);
            Assert.Equal(1, dictionary.Values.Count);
        }

        [Theory]
        [InlineData(-1)]
        [InlineData(1)]
        [InlineData(10)]
        public static void TestConstructor_ConcurrencyLevel(int concurrencyLevel)
        {
            var dictionary = new ConcurrentDictionary<int, int>(concurrencyLevel, 0);
            Assert.Equal(0, dictionary.Count);
            Assert.True(dictionary.TryAdd(1, 2));
            Assert.Equal(1, dictionary.Count);

            dictionary = new ConcurrentDictionary<int, int>(concurrencyLevel, 10, EqualityComparer<int>.Default);
            Assert.Equal(0, dictionary.Count);
            Assert.True(dictionary.TryAdd(3, 4));
            Assert.Equal(1, dictionary.Count);

            dictionary = new ConcurrentDictionary<int, int>(concurrencyLevel, new[] { new KeyValuePair<int, int>(1, 1) }, null);
            Assert.Equal(1, dictionary.Count);
            Assert.True(dictionary.TryAdd(5, 6));
            Assert.Equal(2, dictionary.Count);
        }

        [Fact]
        public static void TestNullComparer()
        {
            AssertDefaultComparerBehavior(new ConcurrentDictionary<EqualityApiSpy, int>((IEqualityComparer<EqualityApiSpy>)null));

            AssertDefaultComparerBehavior(new ConcurrentDictionary<EqualityApiSpy, int>(new[] { new KeyValuePair<EqualityApiSpy, int>(new EqualityApiSpy(), 1) }, null));

            AssertDefaultComparerBehavior(new ConcurrentDictionary<EqualityApiSpy, int>(1, new[] { new KeyValuePair<EqualityApiSpy, int>(new EqualityApiSpy(), 1) }, null));

            AssertDefaultComparerBehavior(new ConcurrentDictionary<EqualityApiSpy, int>(1, 1, null));

            void AssertDefaultComparerBehavior(ConcurrentDictionary<EqualityApiSpy, int> dictionary)
            {
                object identity = new object();
                var spyKey1 = new EqualityApiSpy(identity);
                var spyKey2 = new EqualityApiSpy(identity);

                Assert.True(dictionary.TryAdd(spyKey1, 1));
                Assert.False(dictionary.TryAdd(spyKey2, 1));

                Assert.False(spyKey1.ObjectApiUsed || spyKey2.ObjectApiUsed);
                Assert.True(spyKey1.IEquatableApiUsed || spyKey2.IEquatableApiUsed);
            }
        }

        [Fact]
        public static void TestComparerGetter()
        {
            AssertComparerBehavior<string>(null);
            AssertComparerBehavior<string>(EqualityComparer<string>.Default);
            AssertComparerBehavior<string>(StringComparer.InvariantCulture);
            AssertComparerBehavior<string>(StringComparer.OrdinalIgnoreCase);
            AssertComparerBehavior<int>(EqualityComparer<int>.Default);
            AssertComparerBehavior<bool>(EqualityComparer<bool>.Default);

            void AssertComparerBehavior<T>(IEqualityComparer<T> comparer)
            {
                var dc = new ConcurrentDictionary<T, object>(comparer);
                object expected = comparer ?? EqualityComparer<T>.Default;
                Assert.Same(expected, dc.Comparer);
            }
        }

        private sealed class EqualityApiSpy : IEquatable<EqualityApiSpy>
        {
            public bool ObjectApiUsed { get; private set; }
            public bool IEquatableApiUsed { get; private set; }

            private object _identity;

            public EqualityApiSpy(object identity)
            {
                _identity = identity;
            }

            public EqualityApiSpy() : this(new object()) { }

            public override bool Equals(object obj)
            {
                ObjectApiUsed = true;
                return ReferenceEquals(this._identity, ((EqualityApiSpy)obj)._identity);
            }

            public override int GetHashCode() => _identity.GetHashCode();

            public bool Equals(EqualityApiSpy other)
            {
                IEquatableApiUsed = true;
                return ReferenceEquals(this._identity, other._identity);
            }
        }

        [Fact]
        public static void TestConstructor_Negative()
        {
            Assert.Throws<ArgumentNullException>(
               () => new ConcurrentDictionary<int, int>((ICollection<KeyValuePair<int, int>>)null));
            // "TestConstructor:  FAILED.  Constructor didn't throw ANE when null collection is passed");

            Assert.Throws<ArgumentNullException>(
               () => new ConcurrentDictionary<int, int>((ICollection<KeyValuePair<int, int>>)null, EqualityComparer<int>.Default));
            // "TestConstructor:  FAILED.  Constructor didn't throw ANE when null collection and non null IEqualityComparer passed");

            Assert.Throws<ArgumentNullException>(
               () => new ConcurrentDictionary<string, int>(new[] { new KeyValuePair<string, int>(null, 1) }));
            // "TestConstructor:  FAILED.  Constructor didn't throw ANE when collection has null key passed");

            // Duplicate keys.
            AssertExtensions.Throws<ArgumentException>(null, () => new ConcurrentDictionary<int, int>(new[] { new KeyValuePair<int, int>(1, 1), new KeyValuePair<int, int>(1, 2) }));

            Assert.Throws<ArgumentNullException>(
               () => new ConcurrentDictionary<int, int>(1, null, EqualityComparer<int>.Default));
            // "TestConstructor:  FAILED.  Constructor didn't throw ANE when null collection is passed");

            Assert.Throws<ArgumentOutOfRangeException>(
               () => new ConcurrentDictionary<int, int>(0, 10));
            // "TestConstructor:  FAILED.  Constructor didn't throw AORE when <1 concurrencyLevel passed");

            Assert.Throws<ArgumentOutOfRangeException>(
               () => new ConcurrentDictionary<int, int>(-2, 0));
            // "TestConstructor:  FAILED.  Constructor didn't throw AORE when < 0 capacity passed");
        }

        [Fact]
        public static void TestExceptions()
        {
            var dictionary = new ConcurrentDictionary<string, int>();
            Assert.Throws<ArgumentNullException>(
               () => dictionary.TryAdd(null, 0));
            //  "TestExceptions:  FAILED.  TryAdd didn't throw ANE when null key is passed");

            Assert.Throws<ArgumentNullException>(
               () => dictionary.ContainsKey(null));
            // "TestExceptions:  FAILED.  Contains didn't throw ANE when null key is passed");

            int item;
            Assert.Throws<ArgumentNullException>(
               () => dictionary.TryRemove(null, out item));
            //  "TestExceptions:  FAILED.  TryRemove didn't throw ANE when null key is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.TryGetValue(null, out item));
            // "TestExceptions:  FAILED.  TryGetValue didn't throw ANE when null key is passed");

            Assert.Throws<ArgumentNullException>(
               () => { var x = dictionary[null]; });
            // "TestExceptions:  FAILED.  this[] didn't throw ANE when null key is passed");
            Assert.Throws<KeyNotFoundException>(
               () => { var x = dictionary["1"]; });
            // "TestExceptions:  FAILED.  this[] TryGetValue didn't throw KeyNotFoundException!");

            Assert.Throws<ArgumentNullException>(
               () => dictionary[null] = 1);
            // "TestExceptions:  FAILED.  this[] didn't throw ANE when null key is passed");

            Assert.Throws<ArgumentNullException>(
               () => dictionary.GetOrAdd(null, (k,m) => 0, 0));
            // "TestExceptions:  FAILED.  GetOrAdd didn't throw ANE when null key is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.GetOrAdd("1", null, 0));
            // "TestExceptions:  FAILED.  GetOrAdd didn't throw ANE when null valueFactory is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.GetOrAdd(null, (k) => 0));
            // "TestExceptions:  FAILED.  GetOrAdd didn't throw ANE when null key is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.GetOrAdd("1", null));
            // "TestExceptions:  FAILED.  GetOrAdd didn't throw ANE when null valueFactory is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.GetOrAdd(null, 0));
            // "TestExceptions:  FAILED.  GetOrAdd didn't throw ANE when null key is passed");

            Assert.Throws<ArgumentNullException>(
               () => dictionary.AddOrUpdate(null, (k, m) => 0, (k, v, m) => 0, 42));
            // "TestExceptions:  FAILED.  AddOrUpdate didn't throw ANE when null key is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.AddOrUpdate("1", (k, m) => 0, null, 42));
            // "TestExceptions:  FAILED.  AddOrUpdate didn't throw ANE when null updateFactory is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.AddOrUpdate("1", null, (k, v, m) => 0, 42));
            // "TestExceptions:  FAILED.  AddOrUpdate didn't throw ANE when null addFactory is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.AddOrUpdate(null, (k) => 0, (k, v) => 0));
            // "TestExceptions:  FAILED.  AddOrUpdate didn't throw ANE when null key is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.AddOrUpdate("1", null, (k, v) => 0));
            // "TestExceptions:  FAILED.  AddOrUpdate didn't throw ANE when null updateFactory is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary.AddOrUpdate(null, (k) => 0, null));
            // "TestExceptions:  FAILED.  AddOrUpdate didn't throw ANE when null addFactory is passed");

            // Duplicate key.
            dictionary.TryAdd("1", 1);
            AssertExtensions.Throws<ArgumentException>(null, () => ((IDictionary<string, int>)dictionary).Add("1", 2));
        }

        [Fact]
        public static void TestIDictionary()
        {
            IDictionary dictionary = new ConcurrentDictionary<string, int>();
            Assert.False(dictionary.IsReadOnly);

            // Empty dictionary should not enumerate
            Assert.Empty(dictionary);

            const int SIZE = 10;
            for (int i = 0; i < SIZE; i++)
                dictionary.Add(i.ToString(), i);

            Assert.Equal(SIZE, dictionary.Count);

            //test contains
            Assert.False(dictionary.Contains(1), "TestIDictionary:  FAILED.  Contain returned true for incorrect key type");
            Assert.False(dictionary.Contains("100"), "TestIDictionary:  FAILED.  Contain returned true for incorrect key");
            Assert.True(dictionary.Contains("1"), "TestIDictionary:  FAILED.  Contain returned false for correct key");

            //test GetEnumerator
            int count = 0;
            foreach (var obj in dictionary)
            {
                DictionaryEntry entry = (DictionaryEntry)obj;
                string key = (string)entry.Key;
                int value = (int)entry.Value;
                int expectedValue = int.Parse(key);
                Assert.True(value == expectedValue,
                    string.Format("TestIDictionary:  FAILED.  Unexpected value returned from GetEnumerator, expected {0}, actual {1}", value, expectedValue));
                count++;
            }

            Assert.Equal(SIZE, count);
            Assert.Equal(SIZE, dictionary.Keys.Count);
            Assert.Equal(SIZE, dictionary.Values.Count);

            //Test Remove
            dictionary.Remove("9");
            Assert.Equal(SIZE - 1, dictionary.Count);

            //Test this[]
            for (int i = 0; i < dictionary.Count; i++)
                Assert.Equal(i, (int)dictionary[i.ToString()]);

            dictionary["1"] = 100; // try a valid setter
            Assert.Equal(100, (int)dictionary["1"]);

            //non-existing key
            Assert.Null(dictionary["NotAKey"]);
        }

        [Fact]
        public static void TestIDictionary_Negative()
        {
            IDictionary dictionary = new ConcurrentDictionary<string, int>();
            Assert.Throws<ArgumentNullException>(
               () => dictionary.Add(null, 1));
            // "TestIDictionary:  FAILED.  Add didn't throw ANE when null key is passed");

            // Invalid key type.
            AssertExtensions.Throws<ArgumentException>(null, () => dictionary.Add(1, 1));

            // Invalid value type.
            AssertExtensions.Throws<ArgumentException>(null, () => dictionary.Add("1", "1"));

            Assert.Throws<ArgumentNullException>(
               () => dictionary.Contains(null));
            // "TestIDictionary:  FAILED.  Contain didn't throw ANE when null key is passed");

            //Test Remove
            Assert.Throws<ArgumentNullException>(
               () => dictionary.Remove(null));
            // "TestIDictionary:  FAILED.  Remove didn't throw ANE when null key is passed");

            //Test this[]
            Assert.Throws<ArgumentNullException>(
               () => { object val = dictionary[null]; });
            // "TestIDictionary:  FAILED.  this[] getter didn't throw ANE when null key is passed");
            Assert.Throws<ArgumentNullException>(
               () => dictionary[null] = 0);
            // "TestIDictionary:  FAILED.  this[] setter didn't throw ANE when null key is passed");

            // Invalid key type.
            AssertExtensions.Throws<ArgumentException>(null, () => dictionary[1] = 0);

            // Invalid value type.
            AssertExtensions.Throws<ArgumentException>(null, () => dictionary["1"] = "0");
        }

        [Fact]
        public static void IDictionary_Remove_NullKeyInKeyValuePair_ThrowsArgumentNullException()
        {
            IDictionary<string, int> dictionary = new ConcurrentDictionary<string, int>();
            Assert.Throws<ArgumentNullException>(() => dictionary.Remove(new KeyValuePair<string, int>(null, 0)));
        }

        [Fact]
        public static void TestICollection()
        {
            ICollection dictionary = new ConcurrentDictionary<int, int>();
            Assert.False(dictionary.IsSynchronized, "TestICollection:  FAILED.  IsSynchronized returned true!");

            int key = -1;
            int value = +1;
            //add one item to the dictionary
            ((ConcurrentDictionary<int, int>)dictionary).TryAdd(key, value);

            var objectArray = new object[1];
            dictionary.CopyTo(objectArray, 0);

            Assert.Equal(key, ((KeyValuePair<int, int>)objectArray[0]).Key);
            Assert.Equal(value, ((KeyValuePair<int, int>)objectArray[0]).Value);

            var keyValueArray = new KeyValuePair<int, int>[1];
            dictionary.CopyTo(keyValueArray, 0);
            Assert.Equal(key, keyValueArray[0].Key);
            Assert.Equal(value, keyValueArray[0].Value);

            var entryArray = new DictionaryEntry[1];
            dictionary.CopyTo(entryArray, 0);
            Assert.Equal(key, (int)entryArray[0].Key);
            Assert.Equal(value, (int)entryArray[0].Value);
        }

        [Fact]
        public static void TestICollection_Negative()
        {
            ICollection dictionary = new ConcurrentDictionary<int, int>();
            Assert.False(dictionary.IsSynchronized, "TestICollection:  FAILED.  IsSynchronized returned true!");

            Assert.Throws<NotSupportedException>(() => { var obj = dictionary.SyncRoot; });
            // "TestICollection:  FAILED.  SyncRoot property didn't throw");
            Assert.Throws<ArgumentNullException>(() => dictionary.CopyTo(null, 0));
            // "TestICollection:  FAILED.  CopyTo didn't throw ANE when null Array is passed");
            Assert.Throws<ArgumentOutOfRangeException>(() => dictionary.CopyTo(new object[] { }, -1));
            // "TestICollection:  FAILED.  CopyTo didn't throw AORE when negative index passed");

            //add one item to the dictionary
            ((ConcurrentDictionary<int, int>)dictionary).TryAdd(1, 1);
            AssertExtensions.Throws<ArgumentException>(null, () => dictionary.CopyTo(new object[] { }, 0));
            // "TestICollection:  FAILED.  CopyTo didn't throw AE when the Array size is smaller than the dictionary count");
        }

        [Fact]
        public static void TestClear()
        {
            var dictionary = new ConcurrentDictionary<int, int>();
            for (int i = 0; i < 10; i++)
                dictionary.TryAdd(i, i);

            Assert.Equal(10, dictionary.Count);

            dictionary.Clear();
            Assert.Equal(0, dictionary.Count);

            int item;
            Assert.False(dictionary.TryRemove(1, out item), "TestClear: FAILED.  TryRemove succeeded after Clear");
            Assert.True(dictionary.IsEmpty, "TestClear: FAILED.  IsEmpty returned false after Clear");
        }

        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        public static void TestTryUpdate()
        {
            var dictionary = new ConcurrentDictionary<string, int>();
            Assert.Throws<ArgumentNullException>(
               () => dictionary.TryUpdate(null, 0, 0));
            // "TestTryUpdate:  FAILED.  TryUpdate didn't throw ANE when null key is passed");

            for (int i = 0; i < 10; i++)
                dictionary.TryAdd(i.ToString(), i);

            for (int i = 0; i < 10; i++)
            {
                Assert.True(dictionary.TryUpdate(i.ToString(), i + 1, i), "TestTryUpdate:  FAILED.  TryUpdate failed!");
                Assert.Equal(i + 1, dictionary[i.ToString()]);
            }

            //test TryUpdate concurrently
            dictionary.Clear();
            for (int i = 0; i < 1000; i++)
                dictionary.TryAdd(i.ToString(), i);

            var mres = new ManualResetEventSlim();
            Task[] tasks = new Task[10];
            ThreadLocal<ThreadData> updatedKeys = new ThreadLocal<ThreadData>(true);
            for (int i = 0; i < tasks.Length; i++)
            {
                // We are creating the Task using TaskCreationOptions.LongRunning because...
                // there is no guarantee that the Task will be created on another thread.
                // There is also no guarantee that using this TaskCreationOption will force
                // it to be run on another thread.
                tasks[i] = Task.Factory.StartNew((obj) =>
                {
                    mres.Wait();
                    int index = (((int)obj) + 1) + 1000;
                    updatedKeys.Value = new ThreadData();
                    updatedKeys.Value.ThreadIndex = index;

                    for (int j = 0; j < dictionary.Count; j++)
                    {
                        if (dictionary.TryUpdate(j.ToString(), index, j))
                        {
                            if (dictionary[j.ToString()] != index)
                            {
                                updatedKeys.Value.Succeeded = false;
                                return;
                            }
                            updatedKeys.Value.Keys.Add(j.ToString());
                        }
                    }
                }, i, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
            }

            mres.Set();
            Task.WaitAll(tasks);

            int numberSucceeded = 0;
            int totalKeysUpdated = 0;
            foreach (var threadData in updatedKeys.Values)
            {
                totalKeysUpdated += threadData.Keys.Count;
                if (threadData.Succeeded)
                    numberSucceeded++;
            }

            Assert.True(numberSucceeded == tasks.Length, "One or more threads failed!");
            Assert.True(totalKeysUpdated == dictionary.Count,
               string.Format("TestTryUpdate:  FAILED.  The updated keys count doesn't match the dictionary count, expected {0}, actual {1}", dictionary.Count, totalKeysUpdated));
            foreach (var value in updatedKeys.Values)
            {
                for (int i = 0; i < value.Keys.Count; i++)
                    Assert.True(dictionary[value.Keys[i]] == value.ThreadIndex,
                       string.Format("TestTryUpdate:  FAILED.  The updated value doesn't match the thread index, expected {0} actual {1}", value.ThreadIndex, dictionary[value.Keys[i]]));
            }

            //test TryUpdate with non atomic values (intPtr > 8)
            var dict = new ConcurrentDictionary<int, Struct16>();
            dict.TryAdd(1, new Struct16(1, -1));
            Assert.True(dict.TryUpdate(1, new Struct16(2, -2), new Struct16(1, -1)), "TestTryUpdate:  FAILED.  TryUpdate failed for non atomic values ( > 8 bytes)");
        }

        [OuterLoop("Runs for several seconds")]
        [ConditionalFact(typeof(PlatformDetection), nameof(PlatformDetection.IsThreadingSupported))]
        public void ConcurrentWriteRead_NoTornValues()
        {
            var cd = new ConcurrentDictionary<int, KeyValuePair<long, long>>();
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            Task.WaitAll(
                Task.Run(() =>
                {
                    for (long i = 0; !cts.IsCancellationRequested; i++)
                    {
                        cd[0] = new KeyValuePair<long, long>(i, i);
                    }
                }),
                Task.Run(() =>
                {
                    while (!cts.IsCancellationRequested)
                    {
                        cd.TryGetValue(0, out KeyValuePair<long, long> item);
                        try
                        {
                            Assert.Equal(item.Key, item.Value);
                        }
                        catch
                        {
                            cts.Cancel();
                            throw;
                        }
                    }
                }));
        }

        [Fact]
        public void GetAlternateLookup_FailsWhenIncompatible()
        {
            var dictionary = new ConcurrentDictionary<string, string>(StringComparer.Ordinal);

            dictionary.GetAlternateLookup<ReadOnlySpan<char>>();
            Assert.True(dictionary.TryGetAlternateLookup<ReadOnlySpan<char>>(out _));

            Assert.Throws<InvalidOperationException>(() => dictionary.GetAlternateLookup<ReadOnlySpan<byte>>());
            Assert.Throws<InvalidOperationException>(() => dictionary.GetAlternateLookup<string>());
            Assert.Throws<InvalidOperationException>(() => dictionary.GetAlternateLookup<int>());

            Assert.False(dictionary.TryGetAlternateLookup<ReadOnlySpan<byte>>(out _));
            Assert.False(dictionary.TryGetAlternateLookup<string>(out _));
            Assert.False(dictionary.TryGetAlternateLookup<int>(out _));
        }

        public static IEnumerable<object[]> Dictionary_GetAlternateLookup_OperationsMatchUnderlyingDictionary_MemberData()
        {
            yield return new object[] { EqualityComparer<string>.Default };
            yield return new object[] { StringComparer.Ordinal };
            yield return new object[] { StringComparer.OrdinalIgnoreCase };
            yield return new object[] { StringComparer.InvariantCulture };
            yield return new object[] { StringComparer.InvariantCultureIgnoreCase };
            yield return new object[] { StringComparer.CurrentCulture };
            yield return new object[] { StringComparer.CurrentCultureIgnoreCase };
        }

        [Theory]
        [MemberData(nameof(Dictionary_GetAlternateLookup_OperationsMatchUnderlyingDictionary_MemberData))]
        public void GetAlternateLookup_OperationsMatchUnderlyingDictionary(IEqualityComparer<string> comparer)
        {
            // Test with a variety of comparers to ensure that the alternate lookup is consistent with the underlying dictionary
            ConcurrentDictionary<string, int> dictionary = new(comparer);
            ConcurrentDictionary<string, int>.AlternateLookup<ReadOnlySpan<char>> lookup = dictionary.GetAlternateLookup<ReadOnlySpan<char>>();
            Assert.Same(dictionary, lookup.Dictionary);
            Assert.Same(lookup.Dictionary, lookup.Dictionary);

            string actualKey;
            int value;

            // Add to the dictionary and validate that the lookup reflects the changes
            dictionary["123"] = 123;
            Assert.True(lookup.ContainsKey("123".AsSpan()));
            Assert.True(lookup.TryGetValue("123".AsSpan(), out value));
            Assert.Equal(123, value);
            Assert.Equal(123, lookup["123".AsSpan()]);
            Assert.False(lookup.TryAdd("123".AsSpan(), 321));
            Assert.True(lookup.TryRemove("123".AsSpan(), out value));
            Assert.Equal(123, value);
            Assert.False(dictionary.ContainsKey("123"));
            Assert.Throws<KeyNotFoundException>(() => lookup["123".AsSpan()]);

            // Add via the lookup and validate that the dictionary reflects the changes
            Assert.True(lookup.TryAdd("123".AsSpan(), 123));
            Assert.True(dictionary.ContainsKey("123"));
            lookup.TryGetValue("123".AsSpan(), out value);
            Assert.Equal(123, value);
            Assert.False(lookup.TryRemove("321".AsSpan(), out actualKey, out value));
            Assert.Null(actualKey);
            Assert.Equal(0, value);
            Assert.True(lookup.TryRemove("123".AsSpan(), out actualKey, out value));
            Assert.Equal("123", actualKey);
            Assert.Equal(123, value);

            // Ensure that case-sensitivity of the comparer is respected
            lookup["a".AsSpan()] = 42;
            if (dictionary.Comparer.Equals(EqualityComparer<string>.Default) ||
                dictionary.Comparer.Equals(StringComparer.Ordinal) ||
                dictionary.Comparer.Equals(StringComparer.InvariantCulture) ||
                dictionary.Comparer.Equals(StringComparer.CurrentCulture))
            {
                Assert.True(lookup.TryGetValue("a".AsSpan(), out actualKey, out value));
                Assert.Equal("a", actualKey);
                Assert.Equal(42, value);
                Assert.True(lookup.TryAdd("A".AsSpan(), 42));
                Assert.True(lookup.TryRemove("a".AsSpan(), out value));
                Assert.Equal(42, value);
                Assert.False(lookup.TryRemove("a".AsSpan(), out value));
                Assert.Equal(0, value);
                Assert.True(lookup.TryRemove("A".AsSpan(), out value));
                Assert.Equal(42, value);
            }
            else
            {
                Assert.True(lookup.TryGetValue("A".AsSpan(), out actualKey, out value));
                Assert.Equal("a", actualKey);
                Assert.Equal(42, value);
                Assert.False(lookup.TryAdd("A".AsSpan(), 42));
                Assert.True(lookup.TryRemove("A".AsSpan(), out value));
                Assert.Equal(42, value);
                Assert.False(lookup.TryRemove("a".AsSpan(), out value));
                Assert.Equal(0, value);
                Assert.False(lookup.TryRemove("A".AsSpan(), out value));
                Assert.Equal(0, value);
            }

            // Validate overwrites
            lookup["a".AsSpan()] = 42;
            Assert.Equal(42, dictionary["a"]);
            lookup["a".AsSpan()] = 43;
            Assert.True(lookup.TryRemove("a".AsSpan(), out actualKey, out value));
            Assert.Equal("a", actualKey);
            Assert.Equal(43, value);

            // Test adding multiple entries via the lookup
            for (int i = 0; i < 10; i++)
            {
                Assert.Equal(i, dictionary.Count);
                Assert.True(lookup.TryAdd(i.ToString().AsSpan(), i));
                Assert.False(lookup.TryAdd(i.ToString().AsSpan(), i));
            }

            Assert.Equal(10, dictionary.Count);

            // Test that the lookup and the dictionary agree on what's in and not in
            for (int i = -1; i <= 10; i++)
            {
                Assert.Equal(dictionary.TryGetValue(i.ToString(), out int dv), lookup.TryGetValue(i.ToString().AsSpan(), out int lv));
                Assert.Equal(dv, lv);
            }

            // Test removing multiple entries via the lookup
            for (int i = 9; i >= 0; i--)
            {
                Assert.True(lookup.TryRemove(i.ToString().AsSpan(), out actualKey, out value));
                Assert.Equal(i.ToString(), actualKey);
                Assert.Equal(i, value);
                Assert.False(lookup.TryRemove(i.ToString().AsSpan(), out actualKey, out value));
                Assert.Null(actualKey);
                Assert.Equal(0, value);
                Assert.Equal(i, dictionary.Count);
            }
        }

        [Fact]
        public void Dictionary_NotCorruptedByThrowingComparer()
        {
            ConcurrentDictionary<string, string> dict = new(new CreateThrowsComparer());

            Assert.Equal(0, dict.Count);

            Assert.Throws<FormatException>(() => dict.GetAlternateLookup<ReadOnlySpan<char>>().TryAdd("123".AsSpan(), "123"));
            Assert.Equal(0, dict.Count);

            Assert.True(dict.TryAdd("123", "123"));
            Assert.Equal(1, dict.Count);
        }

        [Fact]
        public void Dictionary_NotCorruptedByNullReturningComparer()
        {
            ConcurrentDictionary<string, string> dict = new(new NullReturningComparer());

            Assert.Equal(0, dict.Count);

            Assert.ThrowsAny<ArgumentException>(() => dict.GetAlternateLookup<ReadOnlySpan<char>>().TryAdd("123".AsSpan(), "123"));
            Assert.Equal(0, dict.Count);

            Assert.True(dict.TryAdd("123", "123"));
            Assert.Equal(1, dict.Count);
        }

        #region Helper Classes and Methods
        private sealed class CreateThrowsComparer : IEqualityComparer<string>, IAlternateEqualityComparer<ReadOnlySpan<char>, string>
        {
            public bool Equals(string? x, string? y) => EqualityComparer<string>.Default.Equals(x, y);
            public int GetHashCode(string obj) => EqualityComparer<string>.Default.GetHashCode(obj);

            public bool Equals(ReadOnlySpan<char> span, string target) => span.SequenceEqual(target);
            public int GetHashCode(ReadOnlySpan<char> span) => string.GetHashCode(span);
            public string Create(ReadOnlySpan<char> span) => throw new FormatException();
        }

        private sealed class NullReturningComparer : IEqualityComparer<string>, IAlternateEqualityComparer<ReadOnlySpan<char>, string>
        {
            public bool Equals(string? x, string? y) => EqualityComparer<string>.Default.Equals(x, y);
            public int GetHashCode(string obj) => EqualityComparer<string>.Default.GetHashCode(obj);

            public bool Equals(ReadOnlySpan<char> span, string target) => span.SequenceEqual(target);
            public int GetHashCode(ReadOnlySpan<char> span) => string.GetHashCode(span);
            public string Create(ReadOnlySpan<char> span) => null!;
        }

        private class ThreadData
        {
            public int ThreadIndex;
            public bool Succeeded = true;
            public List<string> Keys = new List<string>();
        }

        private struct Struct16 : IEqualityComparer<Struct16>
        {
            public long L1, L2;
            public Struct16(long l1, long l2)
            {
                L1 = l1;
                L2 = l2;
            }

            public bool Equals(Struct16 x, Struct16 y)
            {
                return x.L1 == y.L1 && x.L2 == y.L2;
            }

            public int GetHashCode(Struct16 obj)
            {
                return (int)L1;
            }
        }

        #endregion
    }
}
