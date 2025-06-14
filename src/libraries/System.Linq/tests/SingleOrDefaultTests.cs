// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using Xunit;

namespace System.Linq.Tests
{
    public class SingleOrDefaultTests : EnumerableTests
    {
        [Fact]
        public void SameResultsRepeatCallsIntQuery()
        {
            var q = from x in new[] { 0.12335f }
                    select x;

            Assert.Equal(q.SingleOrDefault(), q.SingleOrDefault());
        }

        [Fact]
        public void SameResultsRepeatCallsStringQuery()
        {
            var q = from x in new[] { "" }
                    select x;

            Assert.Equal(q.SingleOrDefault(string.IsNullOrEmpty), q.SingleOrDefault(string.IsNullOrEmpty));
        }

        [Fact]
        public void Empty()
        {
            foreach (IEnumerable<int?> source in CreateSources<int?>([]))
            {
                Assert.Null(source.SingleOrDefault());
                Assert.Equal(5, source.SingleOrDefault(5));
            }
        }

        [Fact]
        public void SingleElement()
        {
            foreach (IEnumerable<int?> source in CreateSources<int?>([4]))
            {
                Assert.Equal(4, source.SingleOrDefault());
                Assert.Equal(4, source.SingleOrDefault(5));
            }
        }

        [Fact]
        public void ManyElementIList()
        {
            foreach (IEnumerable<int?> source in CreateSources<int?>([4, 4, 4, 4, 4]))
            {
                Assert.Throws<InvalidOperationException>(() => source.SingleOrDefault());
                Assert.Throws<InvalidOperationException>(() => source.SingleOrDefault(4));
            }
        }

        [Fact]
        public void EmptySourceWithPredicate()
        {
            int[] source = { };
            int expected = default(int);

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0));
            });
        }

        [Fact]
        public void EmptySourceWithPredicateDefault()
        {
            int[] source = { };
            int expected = 5;

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0, 5));
            });
        }

        [Fact]
        public void SingleElementPredicateTrue()
        {
            int[] source = { 4 };
            int expected = 4;

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0));
            });
        }

        [Fact]
        public void SingleElementPredicateTrueDefault()
        {
            int[] source = { 4 };
            int expected = 4;

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0, 5));
            });
        }

        [Fact]
        public void SingleElementPredicateFalse()
        {
            int[] source = { 3 };
            int expected = default(int);

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0));
            });
        }

        [Fact]
        public void SingleElementPredicateFalseDefault()
        {
            int[] source = { 3 };
            int expected = 5;

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0, 5));
            });
        }

        [Fact]
        public void ManyElementsPredicateFalseForAll()
        {
            int[] source = { 3, 1, 7, 9, 13, 19 };
            int expected = default(int);

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0));
            });
        }

        [Fact]
        public void ManyElementsPredicateFalseForAllDefault()
        {
            int[] source = { 3, 1, 7, 9, 13, 19 };
            int expected = 5;

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0, 5));
            });
        }

        [Fact]
        public void ManyElementsPredicateTrueForLast()
        {
            int[] source = { 3, 1, 7, 9, 13, 19, 20 };
            int expected = 20;

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0));
            });
        }

        [Fact]
        public void ManyElementsPredicateTrueForLastDefault()
        {
            int[] source = { 3, 1, 7, 9, 13, 19, 20 };
            int expected = 20;

            Assert.All(CreateSources(source), source =>
            {
                Assert.Equal(expected, source.SingleOrDefault(i => i % 2 == 0, 5));
            });
        }

        [Fact]
        public void ManyElementsPredicateTrueForFirstAndFifth()
        {
            int[] source = { 2, 3, 1, 7, 10, 13, 19, 9 };

            Assert.All(CreateSources(source), source =>
            {
                Assert.Throws<InvalidOperationException>(() => source.SingleOrDefault(i => i % 2 == 0));
            });
        }

        [Fact]
        public void ManyElementsPredicateTrueForFirstAndFifthDefault()
        {
            int[] source = { 2, 3, 1, 7, 10, 13, 19, 9 };

            Assert.All(CreateSources(source), source =>
            {
                Assert.Throws<InvalidOperationException>(() => source.SingleOrDefault(i => i % 2 == 0, 5));
            });
        }

        [Theory]
        [InlineData(1, 100)]
        [InlineData(42, 100)]
        public void FindSingleMatch(int target, int range)
        {
            Assert.All(CreateSources(Enumerable.Range(0, range)), source =>
            {
                Assert.Equal(target, source.SingleOrDefault(i => i == target));
            });
        }

        [Theory]
        [InlineData(1, 100)]
        [InlineData(42, 100)]
        public void RunOnce(int target, int range)
        {
            Assert.All(CreateSources(Enumerable.Range(0, range)), source =>
            {
                Assert.Equal(target, source.RunOnce().SingleOrDefault(i => i == target));
            });
        }

        [Fact]
        public void ThrowsOnNullSource()
        {
            int[] source = null;
            AssertExtensions.Throws<ArgumentNullException>("source", () => source.SingleOrDefault());
            AssertExtensions.Throws<ArgumentNullException>("source", () => source.SingleOrDefault(i => i % 2 == 0));
        }

        [Fact]
        public void ThrowsOnNullSourceDefault()
        {
            int[] source = null;
            AssertExtensions.Throws<ArgumentNullException>("source", () => source.SingleOrDefault(5));
            AssertExtensions.Throws<ArgumentNullException>("source", () => source.SingleOrDefault(i => i % 2 == 0, 5));
        }

        [Fact]
        public void ThrowsOnNullPredicate()
        {
            int[] source = { };
            Func<int, bool> nullPredicate = null;
            AssertExtensions.Throws<ArgumentNullException>("predicate", () => source.SingleOrDefault(nullPredicate));
        }

        [Fact]
        public void ThrowsOnNullPredicateDefault()
        {
            int[] source = { };
            Func<int, bool> nullPredicate = null;
            AssertExtensions.Throws<ArgumentNullException>("predicate", () => source.SingleOrDefault(nullPredicate, 5));
        }
    }
}
