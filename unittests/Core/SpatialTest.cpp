//===- SpatialTest.cpp - Tests for Affine and SpatialTracker --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Spatial.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

constexpr PlaceId P{0};
constexpr PlaceId Q{1};
constexpr PlaceId N{7};

// RFC 0011, *Spatial records*.
TEST(Affine, ScalesAndShifts) {
  const Affine n = Affine::ofPlace(N);
  EXPECT_FALSE(n.isConstant());
  EXPECT_EQ(n.times(4), Affine::ofPlace(N, 4, 0));
  EXPECT_EQ(n.shifted(1), Affine::ofPlace(N, 1, 1));
  EXPECT_EQ(Affine::ofPlace(N, 1, 1).times(4), Affine::ofPlace(N, 4, 4));
  const Affine sixteen = Affine::ofConstant(16);
  EXPECT_TRUE(sixteen.isConstant());
  EXPECT_EQ(sixteen.times(2), Affine::ofConstant(32));
  EXPECT_EQ(sixteen.shifted(-16), Affine::ofConstant(0));
  EXPECT_FALSE(Affine::ofConstant(INT64_MAX).shifted(1)) << "overflow";
  EXPECT_FALSE(Affine::ofConstant(INT64_MAX).times(2)) << "overflow";
  EXPECT_EQ(sixteen.toString(), "16");
  EXPECT_EQ(Affine::ofPlace(N, 4, 8).toString(), "p7*4+8");
}

TEST(SpatialTracker, DerivedRecordsComposeOffsets) {
  SpatialTracker tracker;
  const SpatialRecord fresh{.extent = Affine::ofPlace(N, 4, 0),
                            .offset = PointerOffset::zero(),
                            .location = {}};
  tracker.set(P, fresh);
  tracker.set(Q, fresh.derived(PointerOffset::ofElements(2)));
  ASSERT_TRUE(tracker.recordOf(Q));
  EXPECT_EQ(tracker.recordOf(Q)->offset, PointerOffset::ofElements(2));
  EXPECT_EQ(tracker.recordOf(Q)->extent, fresh.extent);
  EXPECT_EQ(tracker.recordOf(Q)->derived(PointerOffset::ofElements(-2)).offset,
            PointerOffset::zero());

  // A write to the counter drops every extent expressed in it.
  tracker.dropExtentsOn(N);
  EXPECT_FALSE(tracker.recordOf(P)->extent);
  EXPECT_FALSE(tracker.recordOf(Q)->extent);
  EXPECT_EQ(tracker.recordOf(Q)->offset, PointerOffset::ofElements(2))
      << "the offset survives";
  tracker.forget(Q);
  EXPECT_FALSE(tracker.recordOf(Q));
}

TEST(SpatialTracker, JoinKeepsWhatBothSidesAgreeOn) {
  const SpatialRecord sixteen{.extent = Affine::ofConstant(16),
                              .offset = PointerOffset::zero(),
                              .location = {}};
  const SpatialRecord eight{.extent = Affine::ofConstant(8),
                            .offset = PointerOffset::zero(),
                            .location = {}};
  SpatialTracker a;
  a.set(P, sixteen);
  a.set(Q, sixteen);
  SpatialTracker b;
  b.set(P, eight);
  // Q only on one side: its extent goes, and a record at the start with no
  // extent says nothing (it is gone). P on both with different extents: the
  // extent goes too, the offset stays.
  EXPECT_TRUE(a.join(b));
  EXPECT_FALSE(a.recordOf(Q));
  EXPECT_FALSE(a.recordOf(P));
  EXPECT_FALSE(a.join(b)) << "fixpoint";

  // Same extent, different offsets: the offset is unknown.
  SpatialTracker c;
  c.set(P, sixteen);
  SpatialTracker d;
  d.set(P, sixteen.derived(PointerOffset::ofElements(1)));
  EXPECT_TRUE(c.join(d));
  EXPECT_EQ(c.recordOf(P)->extent, Affine::ofConstant(16));
  EXPECT_TRUE(c.recordOf(P)->offset.isUnknown());
  SpatialTracker same;
  same.set(P, sixteen);
  SpatialTracker other;
  other.set(P, sixteen);
  EXPECT_FALSE(same.join(other));
}

// RFC 0020: the optimized join must preserve the former pad-then-join
// operation, including its witnesses and exact change detection.
TEST(SpatialTracker, AbsentObjectJoinMatchesPaddingReference) {
  const std::vector<std::optional<SpatialRecord>> variants{
      std::nullopt,
      SpatialRecord{},
      SpatialRecord{.extent = Affine::ofConstant(8),
                    .location = {.file = "first.c", .line = 3},
                    .declared = true},
      SpatialRecord{.extent = Affine::ofConstant(16),
                    .offset = PointerOffset::ofElements(2),
                    .boundsOffset = PointerOffset::ofElements(1)},
      SpatialRecord{.extent = Affine::ofPlace(N),
                    .offset = PointerOffset::unknown()},
      SpatialRecord{.string = StringFact{.length = Affine::ofConstant(4)}},
      SpatialRecord{.string = StringFact{.unterminated = true,
                                         .location = {.file = "first.c"}}},
      SpatialRecord{.string = StringFact{.unterminated = true,
                                         .location = {.file = "second.c"}}},
      SpatialRecord{.boundsOffset = PointerOffset::ofElements(1)}};
  const auto check = [](const SpatialTracker &first,
                        const SpatialTracker &second, unsigned mask) {
    const auto absentHere = [mask](PlaceId place) {
      return ((mask + place.value) & 1U) != 0;
    };
    const auto absentThere = [mask](PlaceId place) {
      return ((mask + place.value) & 2U) != 0;
    };
    auto expected = first;
    auto paddedRight = second;
    for (const auto &[place, record] : first.all()) {
      if (!paddedRight.has(place) && absentThere(place))
        paddedRight.set(place, record);
    }
    for (const auto &[place, record] : second.all()) {
      if (!expected.has(place) && absentHere(place))
        expected.set(place, record);
    }
    expected.join(paddedRight);
    auto actual = first;
    EXPECT_EQ(actual.joinWithAbsentObjects(second, absentHere, absentThere),
              expected != first);
    EXPECT_EQ(actual, expected);
    auto self = actual;
    auto expectedSelf = actual;
    expectedSelf.join(actual);
    EXPECT_EQ(self.joinWithAbsentObjects(self, absentHere, absentThere),
              expectedSelf != actual);
    EXPECT_EQ(self, expectedSelf);
  };
  for (std::size_t i = 0; i < variants.size(); ++i) {
    for (std::size_t j = 0; j < variants.size(); ++j) {
      SpatialTracker left;
      SpatialTracker right;
      for (std::uint32_t cell = 0; cell < 5; ++cell) {
        const auto &a = variants[(i + cell) % variants.size()];
        const auto &b =
            variants[(j + (std::size_t{cell} * 2)) % variants.size()];
        if (a)
          left.set(PlaceId{cell}, *a);
        if (b)
          right.set(PlaceId{cell}, *b);
      }
      for (unsigned mask = 0; mask < 4; ++mask) {
        SCOPED_TRACE(::testing::Message() << i << "," << j << "," << mask);
        check(left, right, mask);
        check(right, left, mask);
      }
    }
  }
}

TEST(SpatialTracker, AbsentRecordsStandAtTheStart) {
  // `if (c) p++;`: one path stepped `p`, the other never gave it a record.
  // The join says "may not point to the start", whichever side is missing.
  const SpatialRecord stepped{.extent = std::nullopt,
                              .offset = PointerOffset::ofElements(1),
                              .location = {}};
  SpatialTracker a;
  a.set(P, stepped);
  SpatialTracker b;
  EXPECT_TRUE(a.join(b));
  ASSERT_TRUE(a.recordOf(P));
  EXPECT_TRUE(a.recordOf(P)->offset.isUnknown());
  EXPECT_FALSE(a.join(b)) << "fixpoint";

  SpatialTracker c;
  SpatialTracker d;
  d.set(P, stepped);
  EXPECT_TRUE(c.join(d));
  ASSERT_TRUE(c.recordOf(P));
  EXPECT_TRUE(c.recordOf(P)->offset.isUnknown());
  EXPECT_FALSE(c.join(d)) << "fixpoint";

  // A record at the start with no extent is the absent record.
  SpatialTracker e;
  e.set(P, SpatialRecord{.extent = Affine::ofConstant(8),
                         .offset = PointerOffset::zero(),
                         .location = {}});
  SpatialTracker f;
  EXPECT_TRUE(e.join(f));
  EXPECT_FALSE(e.recordOf(P));
}

// RFC 0011, *Bounds checks*, rules 1-5 on constants.
TEST(BoundsVerdict, ConstantsDecideOutright) {
  using Kind = BoundsVerdict::Kind;
  const Affine ten = Affine::ofConstant(10);
  // `buf[9]` on 10 bytes needs 10: in bounds. `buf[10]` needs 11: out.
  EXPECT_FALSE(boundsVerdict(Affine::ofConstant(10), ten, std::nullopt));
  EXPECT_EQ(boundsVerdict(Affine::ofConstant(11), ten, std::nullopt),
            (BoundsVerdict{.kind = Kind::OutOfBounds}));
  // An access ending at or before the start began before it: `p[-1]`.
  EXPECT_EQ(boundsVerdict(Affine::ofConstant(0), ten, std::nullopt),
            (BoundsVerdict{.kind = Kind::BeforeStart}));
  EXPECT_EQ(boundsVerdict(Affine::ofConstant(-3), ten, std::nullopt),
            (BoundsVerdict{.kind = Kind::BeforeStart}));
  // A constant need against a symbolic extent says nothing without a fact.
  EXPECT_FALSE(
      boundsVerdict(Affine::ofConstant(11), Affine::ofPlace(N), std::nullopt));
  // ... but a non-positive need is before the start whatever the extent.
  EXPECT_EQ(
      boundsVerdict(Affine::ofConstant(0), Affine::ofPlace(N), std::nullopt),
      (BoundsVerdict{.kind = Kind::BeforeStart}));
}

// RFC 0011, *Bounds checks*, rules 2-4 on one counter.
TEST(BoundsVerdict, SymbolicNeedsAgainstTheSameCounter) {
  using Kind = BoundsVerdict::Kind;
  const Affine n = Affine::ofPlace(N);
  const Affine n4 = Affine::ofPlace(N, 4, 0);
  // `p[n]` on `n` elements of 4 bytes: needs `4n + 4`, has `4n`.
  EXPECT_EQ(boundsVerdict(Affine::ofPlace(N, 4, 4), n4, Relation::Equal),
            (BoundsVerdict{.kind = Kind::OutOfBounds}));
  // `p[n - 1]`: needs `4n`, has `4n`: in bounds.
  EXPECT_FALSE(boundsVerdict(n4, n4, Relation::Equal));
  // `n` bytes on `n + 1`: fine; `n + 2` on `n + 1`: out.
  EXPECT_FALSE(boundsVerdict(n, Affine::ofPlace(N, 1, 1), Relation::Equal));
  EXPECT_EQ(boundsVerdict(Affine::ofPlace(N, 1, 2), Affine::ofPlace(N, 1, 1),
                          Relation::Equal),
            (BoundsVerdict{.kind = Kind::OutOfBounds}));
  // Different scales on one counter are not compared.
  EXPECT_FALSE(boundsVerdict(Affine::ofPlace(N, 2, 0), n4, Relation::Equal));
}

// RFC 0011, *Bounds checks*, rule 3: `p[i]` with `i` against `n`.
TEST(BoundsVerdict, RelationsBetweenTwoCounters) {
  using Kind = BoundsVerdict::Kind;
  constexpr PlaceId I{3};
  const Affine i4plus4 = Affine::ofPlace(I, 4, 4); // `p[i]` on 4-byte elements
  const Affine n4 = Affine::ofPlace(N, 4, 0);      // `n` elements
  // `i < n`: `4i + 4 <= 4n`: in bounds.
  EXPECT_FALSE(boundsVerdict(i4plus4, n4, Relation::Less));
  // `i <= n`: `i` may equal `n`, which is one past.
  EXPECT_EQ(boundsVerdict(i4plus4, n4, Relation::LessEqual),
            (BoundsVerdict{.kind = Kind::MayBeOutOfBounds, .boundary = 0}));
  // `p[i + 1]` under `i < n`: `i` may be `n - 1`.
  EXPECT_EQ(boundsVerdict(Affine::ofPlace(I, 4, 8), n4, Relation::Less),
            (BoundsVerdict{.kind = Kind::MayBeOutOfBounds, .boundary = -1}));
  // `i >= n`: every value is past the end.
  EXPECT_EQ(boundsVerdict(i4plus4, n4, Relation::GreaterEqual),
            (BoundsVerdict{.kind = Kind::OutOfBounds}));
  EXPECT_EQ(boundsVerdict(i4plus4, n4, Relation::Greater),
            (BoundsVerdict{.kind = Kind::OutOfBounds}));
  // No relation: nothing is said.
  EXPECT_FALSE(boundsVerdict(i4plus4, n4, std::nullopt));
}

// RFC 0011, *Bounds checks*, rule 6: a constant on one side against a place
// bounded above by a constant on the other.
TEST(BoundsVerdict, ConstantUpperBounds) {
  using Kind = BoundsVerdict::Kind;
  constexpr PlaceId I{3};
  const Affine four = Affine::ofConstant(4);
  const Affine i1plus1 = Affine::ofPlace(I, 1, 1); // `buf[i]` on bytes
  // `for (i = 0; i < 8; i++) buf[i]` on 4 bytes: `i` may be 7, past the end.
  EXPECT_EQ(boundsVerdict(i1plus1, four, std::nullopt, {.needAtMost = 7}),
            (BoundsVerdict{.kind = Kind::MayReachPastEnd, .boundary = 7}));
  // `i < 4`: `i` is at most 3, in bounds.
  EXPECT_FALSE(boundsVerdict(i1plus1, four, std::nullopt, {.needAtMost = 3}));
  // Scaled: `ints[i]` with `i <= 1` on 8 bytes fits; `i <= 2` does not.
  EXPECT_FALSE(boundsVerdict(Affine::ofPlace(I, 4, 4), Affine::ofConstant(8),
                             std::nullopt, {.needAtMost = 1}));
  EXPECT_EQ(boundsVerdict(Affine::ofPlace(I, 4, 4), Affine::ofConstant(8),
                          std::nullopt, {.needAtMost = 2}),
            (BoundsVerdict{.kind = Kind::MayReachPastEnd, .boundary = 2}));
  // No bound: nothing is said.
  EXPECT_FALSE(boundsVerdict(i1plus1, four, std::nullopt));

  // `if (n > 4) return; p = malloc(n); p[6]`: the object has at most 4
  // bytes, so a constant access at 7 is out on every path.
  EXPECT_EQ(boundsVerdict(Affine::ofConstant(7), Affine::ofPlace(N),
                          std::nullopt, {.haveAtMost = 4}),
            (BoundsVerdict{.kind = Kind::OutOfBounds}));
  EXPECT_FALSE(boundsVerdict(Affine::ofConstant(4), Affine::ofPlace(N),
                             std::nullopt, {.haveAtMost = 4}));
  // A bound on the extent's place says nothing about a symbolic need.
  EXPECT_FALSE(boundsVerdict(i1plus1, Affine::ofPlace(N), std::nullopt,
                             {.haveAtMost = 4}));
}

// RFC 0012, *Lower bounds*: `if (i >= 8) buf[i]` on 8 bytes is out on every
// value `i` may take; `if (i <= -1) buf[i]` is before the start on every
// value.
TEST(BoundsVerdict, ConstantLowerBounds) {
  using Kind = BoundsVerdict::Kind;
  constexpr PlaceId I{3};
  const Affine eight = Affine::ofConstant(8);
  const Affine i1plus1 = Affine::ofPlace(I, 1, 1);
  EXPECT_EQ(boundsVerdict(i1plus1, eight, std::nullopt, {.needAtLeast = 8}),
            (BoundsVerdict{.kind = Kind::AtLeastPastEnd, .boundary = 8}));
  // `i >= 7`: the smallest value fits; nothing more is known.
  EXPECT_FALSE(boundsVerdict(i1plus1, eight, std::nullopt, {.needAtLeast = 7}));
  // Both bounds: the lower decides first (`8 <= i <= 12` is out outright).
  EXPECT_EQ(boundsVerdict(i1plus1, eight, std::nullopt,
                          {.needAtMost = 12, .needAtLeast = 8}),
            (BoundsVerdict{.kind = Kind::AtLeastPastEnd, .boundary = 8}));
  // `2 <= i <= 12`: the upper says "may reach".
  EXPECT_EQ(boundsVerdict(i1plus1, eight, std::nullopt,
                          {.needAtMost = 12, .needAtLeast = 2}),
            (BoundsVerdict{.kind = Kind::MayReachPastEnd, .boundary = 12}));
  // Scaled: `ints[i]` with `i >= 2` on 8 bytes needs at least 12.
  EXPECT_EQ(boundsVerdict(Affine::ofPlace(I, 4, 4), eight, std::nullopt,
                          {.needAtLeast = 2}),
            (BoundsVerdict{.kind = Kind::AtLeastPastEnd, .boundary = 2}));
  // `i <= -1`: `buf[i]` ends at or before the start on every value.
  EXPECT_EQ(boundsVerdict(i1plus1, eight, std::nullopt, {.needAtMost = -1}),
            (BoundsVerdict{.kind = Kind::BeforeStart, .boundary = -1}));
  EXPECT_EQ(boundsVerdict(i1plus1, Affine::ofPlace(N), std::nullopt,
                          {.needAtMost = -1}),
            (BoundsVerdict{.kind = Kind::BeforeStart, .boundary = -1}))
      << "whatever the extent";
  // A lower bound on the extent's place says nothing about a constant
  // need: the object may be larger.
  EXPECT_FALSE(boundsVerdict(Affine::ofConstant(7), Affine::ofPlace(N),
                             std::nullopt, {.haveAtLeast = 4}));
}

// RFC 0012, *String facts*: a record carries what is known about the string
// the object holds; a write to the length's counter drops it; the join
// keeps only what both paths agree on.
TEST(SpatialTracker, StringFactsFollowExtents) {
  SpatialTracker t;
  // A fact on a place with no record makes one.
  t.setString(P, StringFact{.length = Affine::ofConstant(5)});
  ASSERT_TRUE(t.recordOf(P));
  EXPECT_FALSE(t.recordOf(P)->extent);
  EXPECT_EQ(t.recordOf(P)->string->length, Affine::ofConstant(5));
  // An empty fact is no fact.
  t.setString(P, StringFact{});
  EXPECT_FALSE(t.recordOf(P)->string);
  // Beside an extent.
  t.set(P, SpatialRecord{.extent = Affine::ofPlace(N), .offset = {}});
  t.setString(P, StringFact{.length = Affine::ofPlace(N, 1, -1)});
  EXPECT_TRUE(t.recordOf(P)->string);
  t.dropStringFacts(P);
  EXPECT_FALSE(t.recordOf(P)->string);
  EXPECT_TRUE(t.recordOf(P)->extent) << "the extent stays";
  // The length's counter written: the length is gone, the extent (in
  // another counter) stays.
  constexpr PlaceId M{9};
  t.setString(P, StringFact{.length = Affine::ofPlace(M)});
  t.dropExtentsOn(M);
  EXPECT_FALSE(t.recordOf(P)->string);
  EXPECT_TRUE(t.recordOf(P)->extent);
  // A derived pointer carries the object's string fact.
  t.setString(P, StringFact{.unterminated = true});
  const SpatialRecord stepped =
      t.recordOf(P)->derived(PointerOffset::ofElements(2));
  EXPECT_TRUE(stepped.string->unterminated);

  // Join: the same fact stays; `unterminated` on both stays; a length only
  // one side knows, or two different lengths, is unknown.
  SpatialTracker a;
  SpatialTracker b;
  a.setString(P, StringFact{.length = Affine::ofConstant(5)});
  b.setString(P, StringFact{.length = Affine::ofConstant(5)});
  EXPECT_FALSE(a.join(b));
  EXPECT_EQ(a.recordOf(P)->string->length, Affine::ofConstant(5));
  b.setString(P, StringFact{.length = Affine::ofConstant(6)});
  EXPECT_TRUE(a.join(b));
  EXPECT_FALSE(a.recordOf(P)) << "nothing left: the record is gone";

  SpatialTracker c;
  SpatialTracker d;
  c.setString(P, StringFact{.unterminated = true, .location = {}});
  d.setString(P,
              StringFact{.unterminated = true,
                         .location = SourceLocation{.file = "a.c", .line = 3}});
  EXPECT_FALSE(c.join(d)) << "the first location is kept";
  EXPECT_TRUE(c.recordOf(P)->string->unterminated);
  SpatialTracker e;
  EXPECT_TRUE(c.join(e));
  EXPECT_FALSE(c.recordOf(P)) << "unknown on one path: unknown";
}

} // namespace
} // namespace weavec::core

namespace weavec::core {

// RFC 0017: no diagnostic and a proof are distinct results.
TEST(SpatialCoverage, UnknownIndexIsNotAProof) {
  const PlaceId i{1};
  const auto check =
      checkSpatialBounds(Affine::ofPlace(i), Affine::ofPlace(i, 1, 1),
                         Affine::ofConstant(8), std::nullopt);
  EXPECT_EQ(check.outcome, SpatialOutcome::Unresolved);
  EXPECT_EQ(check.reason, SpatialReason::UnknownIndex);
}

TEST(SpatialCoverage, BothEndsMustBeEstablished) {
  const PlaceId i{1};
  const auto start = Affine::ofPlace(i);
  const auto end = Affine::ofPlace(i, 1, 1);
  const auto have = Affine::ofConstant(8);
  EXPECT_EQ(
      checkSpatialBounds(start, end, have, std::nullopt, {.needAtMost = 7}, 0)
          .outcome,
      SpatialOutcome::Proven);
  EXPECT_EQ(
      checkSpatialBounds(start, end, have, std::nullopt, {.needAtMost = 7})
          .outcome,
      SpatialOutcome::Unresolved);
  EXPECT_EQ(
      checkSpatialBounds(start, end, have, std::nullopt, {.needAtLeast = 8}, 8)
          .outcome,
      SpatialOutcome::Violation);
  EXPECT_EQ(checkSpatialBounds(Affine::ofConstant(-1), Affine::ofConstant(1),
                               have, std::nullopt)
                .outcome,
            SpatialOutcome::Violation);
}

TEST(SpatialCoverage, RelativeUpperBoundStillNeedsANonnegativeStart) {
  const PlaceId i{1};
  const PlaceId n{2};
  const auto start = Affine::ofPlace(i, 4);
  const auto end = Affine::ofPlace(i, 4, 4);
  const auto have = Affine::ofPlace(n, 4);
  EXPECT_EQ(checkSpatialBounds(start, end, have, Relation::Less, {}, 0).outcome,
            SpatialOutcome::Proven);
  EXPECT_EQ(checkSpatialBounds(start, end, have, Relation::Less).outcome,
            SpatialOutcome::Unresolved);
  EXPECT_EQ(
      checkSpatialBounds(start, end, have, Relation::LessEqual, {}, 0).outcome,
      SpatialOutcome::Violation);
}

} // namespace weavec::core

TEST(Spatial, AnalyzerArithmeticDoesNotWrapAtSignedLimits) {
  using namespace weavec::core;
  const Affine need = Affine::ofPlace(PlaceId{0}, INT64_MAX, INT64_MAX);
  const Affine have = Affine::ofPlace(PlaceId{1}, INT64_MAX, INT64_MAX);
  EXPECT_FALSE(boundsVerdict(need, have, Relation::Greater, {}));
  EXPECT_FALSE(boundsVerdict(Affine::ofPlace(PlaceId{0}, 1, INT64_MIN),
                             Affine::ofPlace(PlaceId{1}, 1, 0), Relation::Less,
                             {}));
  const RelationEdge edge{.relation = Relation::Equal, .offset = INT64_MIN};
  EXPECT_FALSE(edge.flipped());
}

TEST(SpatialChecks, AbstractUpperBoundsProveSafetyButNotReachability) {
  using namespace weavec::core;
  const PlaceId i{0};
  const auto start = Affine::ofPlace(i);
  const auto end = Affine::ofPlace(i, 1, 1);
  const KnownBounds abstract{.needAtMost = 127, .needBoundaryWitness = false};
  EXPECT_EQ(checkSpatialBounds(start, end, Affine::ofConstant(8), std::nullopt,
                               abstract, 0)
                .outcome,
            SpatialOutcome::Unresolved);
  EXPECT_EQ(checkSpatialBounds(start, end, Affine::ofConstant(128),
                               std::nullopt, abstract, 0)
                .outcome,
            SpatialOutcome::Proven);
  EXPECT_EQ(checkSpatialBounds(start, end, Affine::ofConstant(8), std::nullopt,
                               {.needAtMost = 127}, 0)
                .outcome,
            SpatialOutcome::Violation);
}
