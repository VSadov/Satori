// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using Xunit;

namespace System.Linq.Tests
{
    public class AggregateByTests : EnumerableTests
    {
        [Fact]
        public void Empty()
        {
            Assert.All(CreateSources<int>([]), source =>
            {
                Assert.Equal([], source.AggregateBy(i => i, i => i, (a, i) => a + i));
                Assert.Equal([], source.AggregateBy(i => i, 0, (a, i) => a + i));
            });
        }

        [Fact]
        public void AggregateBy_SourceNull_ThrowsArgumentNullException()
        {
            string[] first = null;

            AssertExtensions.Throws<ArgumentNullException>("source", () => first.AggregateBy(x => x, string.Empty, (x, y) => x + y));
            AssertExtensions.Throws<ArgumentNullException>("source", () => first.AggregateBy(x => x, string.Empty, (x, y) => x + y, new AnagramEqualityComparer()));
            AssertExtensions.Throws<ArgumentNullException>("source", () => first.AggregateBy(x => x, x => x, (x, y) => x + y));
            AssertExtensions.Throws<ArgumentNullException>("source", () => first.AggregateBy(x => x, x => x, (x, y) => x + y, new AnagramEqualityComparer()));
        }

        [Fact]
        public void AggregateBy_KeySelectorNull_ThrowsArgumentNullException()
        {
            string[] source = ["test"];
            Func<string, string> keySelector = null;

            AssertExtensions.Throws<ArgumentNullException>("keySelector", () => source.AggregateBy(keySelector, string.Empty, (x, y) => x + y));
            AssertExtensions.Throws<ArgumentNullException>("keySelector", () => source.AggregateBy(keySelector, string.Empty, (x, y) => x + y, new AnagramEqualityComparer()));
            AssertExtensions.Throws<ArgumentNullException>("keySelector", () => source.AggregateBy(keySelector, x => x, (x, y) => x + y));
            AssertExtensions.Throws<ArgumentNullException>("keySelector", () => source.AggregateBy(keySelector, x => x, (x, y) => x + y, new AnagramEqualityComparer()));
        }

        [Fact]
        public void AggregateBy_SeedSelectorNull_ThrowsArgumentNullException()
        {
            string[] source = ["test"];
            Func<string, string> seedSelector = null;

            AssertExtensions.Throws<ArgumentNullException>("seedSelector", () => source.AggregateBy(x => x, seedSelector, (x, y) => x + y));
            AssertExtensions.Throws<ArgumentNullException>("seedSelector", () => source.AggregateBy(x => x, seedSelector, (x, y) => x + y, new AnagramEqualityComparer()));
        }

        [Fact]
        public void AggregateBy_FuncNull_ThrowsArgumentNullException()
        {
            string[] source = ["test"];
            Func<string, string, string> func = null;

            AssertExtensions.Throws<ArgumentNullException>("func", () => source.AggregateBy(x => x, string.Empty, func));
            AssertExtensions.Throws<ArgumentNullException>("func", () => source.AggregateBy(x => x, string.Empty, func, new AnagramEqualityComparer()));
            AssertExtensions.Throws<ArgumentNullException>("func", () => source.AggregateBy(x => x, x => x, func));
            AssertExtensions.Throws<ArgumentNullException>("func", () => source.AggregateBy(x => x, x => x, func, new AnagramEqualityComparer()));
        }

        [Fact]
        public void AggregateBy_SourceThrowsOnGetEnumerator()
        {
            IEnumerable<int> source = new ThrowsOnGetEnumerator();

            var enumerator = source.AggregateBy(x => x, 0, (x, y) => x + y).GetEnumerator();

            Assert.Throws<InvalidOperationException>(() => enumerator.MoveNext());
        }

        [Fact]
        public void AggregateBy_SourceThrowsOnMoveNext()
        {
            IEnumerable<int> source = new ThrowsOnMoveNext();

            var enumerator = source.AggregateBy(x => x, 0, (x, y) => x + y).GetEnumerator();

            Assert.Throws<InvalidOperationException>(() => enumerator.MoveNext());
        }

        [Fact]
        public void AggregateBy_SourceThrowsOnCurrent()
        {
            IEnumerable<int> source = new ThrowsOnCurrentEnumerator();

            var enumerator = source.AggregateBy(x => x, 0, (x, y) => x + y).GetEnumerator();

            Assert.Throws<InvalidOperationException>(() => enumerator.MoveNext());
        }

        [Fact]
        public void AggregateBy_HasExpectedOutput()
        {
            Validate(
                source: Enumerable.Empty<int>(),
                keySelector: x => x,
                seedSelector: x => 0,
                func: (x, y) => x + y,
                comparer: null,
                expected: Enumerable.Empty<KeyValuePair<int,int>>());

            Validate(
                source: Enumerable.Range(0, 10),
                keySelector: x => x,
                seedSelector: x => 0,
                func: (x, y) => x + y,
                comparer: null,
                expected: Enumerable.Range(0, 10).Select(x => new KeyValuePair<int, int>(x, x)));

            Validate(
                source: Enumerable.Range(5, 10),
                keySelector: x => true,
                seedSelector: x => 0,
                func: (x, y) => x + y,
                comparer: null,
                expected: Enumerable.Repeat(true, 1).Select(x => new KeyValuePair<bool, int>(x, 95)));

            Validate(
                source: Enumerable.Range(0, 20),
                keySelector: x => x % 5,
                seedSelector: x => 0,
                func: (x, y) => x + y,
                comparer: null,
                expected: Enumerable.Range(0, 5).Select(x => new KeyValuePair<int, int>(x, 30 + 4 * x)));

            Validate(
                source: Enumerable.Repeat(5, 20),
                keySelector: x => x,
                seedSelector: x => 0,
                func: (x, y) => x + y,
                comparer: null,
                expected: Enumerable.Repeat(5, 1).Select(x => new KeyValuePair<int, int>(x, 100)));

            Validate(
                source: new string[] { "Bob", "bob", "tim", "Bob", "Tim" },
                keySelector: x => x,
                seedSelector: x => string.Empty,
                func: (x, y) => x + y,
                null,
                expected:
                [
                    new("Bob", "BobBob"),
                    new("bob", "bob"),
                    new("tim", "tim"),
                    new("Tim", "Tim"),
                ]);

            Validate(
                source: new string[] { "Bob", "bob", "tim", "Bob", "Tim" },
                keySelector: x => x,
                seedSelector: x => string.Empty,
                func: (x, y) => x + y,
                StringComparer.OrdinalIgnoreCase,
                expected:
                [
                    new("Bob", "BobbobBob"),
                    new("tim", "timTim")
                ]);

            Validate(
                source: new (string Name, int Age)[] { ("Tom", 20), ("Dick", 30), ("Harry", 40) },
                keySelector: x => x.Age,
                seedSelector: x => $"I am {x} and my name is ",
                func: (x, y) => x + y.Name,
                comparer: null,
                expected:
                [
                    new(20, "I am 20 and my name is Tom"),
                    new(30, "I am 30 and my name is Dick"),
                    new(40, "I am 40 and my name is Harry")
                ]);

            Validate(
                source: new (string Name, int Age)[] { ("Tom", 20), ("Dick", 20), ("Harry", 40) },
                keySelector: x => x.Age,
                seedSelector: x => $"I am {x} and my name is",
                func: (x, y) => $"{x} maybe {y.Name}",
                comparer: null,
                expected:
                [
                    new(20, "I am 20 and my name is maybe Tom maybe Dick"),
                    new(40, "I am 40 and my name is maybe Harry")
                ]);

            Validate(
                source: new (string Name, int Age)[] { ("Bob", 20), ("bob", 20), ("Harry", 20) },
                keySelector: x => x.Name,
                seedSelector: x => 0,
                func: (x, y) => x + y.Age,
                comparer: null,
                expected: new string[] { "Bob", "bob", "Harry" }.Select(x => new KeyValuePair<string, int>(x, 20)));

            Validate(
                source: new (string Name, int Age)[] { ("Bob", 20), ("bob", 30), ("Harry", 40) },
                keySelector: x => x.Name,
                seedSelector: x => 0,
                func: (x, y) => x + y.Age,
                comparer: StringComparer.OrdinalIgnoreCase,
                expected:
                [
                    new("Bob", 50),
                    new("Harry", 40)
                ]);

            static void Validate<TSource, TKey, TAccumulate>(IEnumerable<TSource> source, Func<TSource, TKey> keySelector, Func<TKey, TAccumulate> seedSelector, Func<TAccumulate, TSource, TAccumulate> func, IEqualityComparer<TKey>? comparer, IEnumerable<KeyValuePair<TKey, TAccumulate>> expected)
            {
                Assert.Equal(expected, source.AggregateBy(keySelector, seedSelector, func, comparer));
                Assert.Equal(expected, source.RunOnce().AggregateBy(keySelector, seedSelector, func, comparer));
            }
        }

        [Fact]
        public void GroupBy()
        {
            static IEnumerable<KeyValuePair<TKey, List<TSource>>> GroupBy<TSource, TKey>(IEnumerable<TSource> source, Func<TSource, TKey> keySelector) =>
                source.AggregateBy(
                    keySelector,
                    seedSelector: _ => new List<TSource>(),
                    (group, element) => { group.Add(element); return group; });

            IEnumerable<KeyValuePair<bool, List<int>>> oddsEvens = GroupBy(
                new int[] { 1, 2, 3, 4 },
                i => i % 2 == 0);

            var e = oddsEvens.GetEnumerator();

            Assert.True(e.MoveNext());
            KeyValuePair<bool, List<int>> oddsItem = e.Current;
            Assert.False(oddsItem.Key);
            List<int> odds = oddsItem.Value;
            Assert.True(odds.Contains(1));
            Assert.True(odds.Contains(3));
            Assert.False(odds.Contains(2));
            Assert.False(odds.Contains(4));

            Assert.True(e.MoveNext());
            KeyValuePair<bool, List<int>> evensItem = e.Current;
            Assert.True(evensItem.Key);
            List<int> evens = evensItem.Value;
            Assert.True(evens.Contains(2));
            Assert.True(evens.Contains(4));
            Assert.False(evens.Contains(1));
            Assert.False(evens.Contains(3));
        }

        [Fact]
        public void LongCountBy()
        {
            static IEnumerable<KeyValuePair<TKey, long>> LongCountBy<TSource, TKey>(IEnumerable<TSource> source, Func<TSource, TKey> keySelector) =>
                source.AggregateBy(
                    keySelector,
                    seed: 0L,
                    (count, _) => ++count);

            IEnumerable<KeyValuePair<bool, long>> oddsEvens = LongCountBy(
                new int[] { 1, 2, 3, 4 },
                i => i % 2 == 0);

            var e = oddsEvens.GetEnumerator();

            Assert.True(e.MoveNext());
            KeyValuePair<bool, long> oddsItem = e.Current;
            Assert.False(oddsItem.Key);
            Assert.Equal(2, oddsItem.Value);

            Assert.True(e.MoveNext());
            KeyValuePair<bool, long> evensItem = e.Current;
            Assert.True(evensItem.Key);
            Assert.Equal(2, oddsItem.Value);
        }

        [Fact]
        public void Score()
        {
            var data = new (string id, int score)[]
            {
                ("0", 42),
                ("1", 5),
                ("2", 4),
                ("1", 10),
                ("0", 25),
            };

            var scores = data.AggregateBy(
                keySelector: entry => entry.id,
                seed: 0,
                (totalScore, curr) => totalScore + curr.score)
                .ToArray();

            Assert.Equal([new("0", 67), new("1", 15), new("2", 4)], scores);
        }
    }
}
