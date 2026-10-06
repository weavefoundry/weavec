//===- HeapTest.cpp - Tests for the object engine's domain ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §4: the zone, the heap's joins and focus objects, distinctness
// and the temporal and spatial verdicts. The tests named after the
// invariants I1–I6 of §4.7 build the state a violation would produce and
// check that the verdict is not proven.
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Heap.h"

#include "weavec/Core/Zone.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// Persistent maps
//===----------------------------------------------------------------------===//

TEST(PMap, EmptyMapsAreEqualWhateverTheirStorage) {
  PMap<Sym, SymInfo> never;
  PMap<Sym, SymInfo> emptied;
  emptied.set(1, SymInfo{});
  emptied.erase(1);
  EXPECT_TRUE(never == emptied);
  EXPECT_TRUE(emptied == never);
}

TEST(PMap, AWriteToACopyLeavesTheOriginalsValue) {
  PMap<Sym, SymInfo> original;
  SymInfo info;
  info.name = "p";
  original.set(1, info);
  original.set(2, info);
  PMap<Sym, SymInfo> copy = original;
  copy.at(1).name = "q";
  copy.set(2, SymInfo{});
  EXPECT_EQ(original.find(1)->name, "p");
  EXPECT_EQ(original.find(2)->name, "p");
  EXPECT_EQ(copy.find(1)->name, "q");
  EXPECT_TRUE(copy.find(2)->name.empty());
  EXPECT_FALSE(original == copy);
  copy.at(1).name = "p";
  copy.set(2, info);
  EXPECT_TRUE(original == copy);
}

TEST(PMap, IteratesInKeyOrder) {
  PMap<Sym, SymInfo> map;
  for (Sym sym : {3U, 1U, 2U}) {
    SymInfo info;
    info.name = std::to_string(sym);
    map.set(sym, info);
  }
  std::vector<std::string> names;
  for (const auto &[sym, info] : map)
    names.push_back(std::to_string(sym) + info.name);
  EXPECT_EQ(names, (std::vector<std::string>{"11", "22", "33"}));
  map.eraseIf([](Sym sym, const SymInfo &) { return sym == 2; });
  EXPECT_EQ(map.size(), 2U);
  EXPECT_FALSE(map.contains(2));
}

//===----------------------------------------------------------------------===//
// Zone
//===----------------------------------------------------------------------===//

TEST(Zone, ClosesTransitiveBounds) {
  Zone zone;
  ASSERT_TRUE(zone.addLE(1, 2, -1)); // i < n
  ASSERT_TRUE(zone.addLE(2, 3, 0));  // n <= m
  EXPECT_TRUE(zone.entails(1, 3, -1));
  EXPECT_FALSE(zone.entails(3, 1, 0));
}

TEST(Zone, DetectsContradiction) {
  Zone zone;
  ASSERT_TRUE(zone.addRange(1, 0, 5));
  EXPECT_FALSE(zone.addRange(1, 7, std::nullopt));
  EXPECT_TRUE(zone.isBottom());
}

TEST(Zone, JoinKeepsCommonBoundsOnly) {
  Zone left;
  left.addRange(1, 0, 3);
  Zone right;
  right.addRange(2, 5, 9);
  std::vector<SymPair> pairs{SymPair{
      .result = 10, .left = 1, .right = 2, .hasLeft = true, .hasRight = true}};
  Zone joined = Zone::combine(left, right, pairs, false, {});
  EXPECT_EQ(joined.lower(10), 0);
  EXPECT_EQ(joined.upper(10), 9);
}

TEST(Zone, OneSidedSymbolsKeepTheirBounds) {
  Zone left;
  left.addRange(1, 4, 4);
  Zone right;
  std::vector<SymPair> pairs{SymPair{.result = 3,
                                     .left = 1,
                                     .right = ZeroSym,
                                     .hasLeft = true,
                                     .hasRight = false}};
  Zone joined = Zone::combine(left, right, pairs, false, {});
  EXPECT_EQ(joined.constant(3), 4);
}

TEST(Zone, WideningDropsGrowingBounds) {
  Zone previous;
  previous.addRange(1, 0, 1);
  Zone next;
  next.addRange(1, 0, 2);
  std::vector<SymPair> pairs{SymPair{
      .result = 1, .left = 1, .right = 1, .hasLeft = true, .hasRight = true}};
  Zone widened = Zone::combine(previous, next, pairs, true, {});
  EXPECT_EQ(widened.lower(1), 0);
  EXPECT_FALSE(widened.upper(1));
  Zone thresholded = Zone::combine(previous, next, pairs, true, {100});
  EXPECT_EQ(thresholded.upper(1), 100);
}

TEST(Zone, WideningStopsRelationsOnlyNearZero) {
  // `i - n` growing past 1 is dropped whatever the program's constants; a
  // bound against zero stops at the next one.
  Zone previous;
  previous.addLE(1, 2, 2);
  previous.addRange(1, std::nullopt, 3);
  Zone next;
  next.addLE(1, 2, 3);
  next.addRange(1, std::nullopt, 4);
  std::vector<SymPair> pairs{SymPair{.result = 1,
                                     .left = 1,
                                     .right = 1,
                                     .hasLeft = true,
                                     .hasRight = true},
                             SymPair{.result = 2,
                                     .left = 2,
                                     .right = 2,
                                     .hasLeft = true,
                                     .hasRight = true}};
  Zone widened = Zone::combine(previous, next, pairs, true, {5, 100});
  EXPECT_FALSE(widened.bound(1, 2));
  EXPECT_EQ(widened.upper(1), 5);
  // Growing towards zero, it stops there.
  Zone low;
  low.addLE(1, 2, -3);
  Zone higher;
  higher.addLE(1, 2, -1);
  Zone stopped = Zone::combine(low, higher, pairs, true, {5, 100});
  EXPECT_EQ(stopped.bound(1, 2), -1);
}

TEST(Zone, BoundsThroughZeroAreImpliedNotStored) {
  // `x <= 5` and `y >= 2` imply `x - y <= 3`; the zone answers it without
  // storing it, and a tighter relation is stored.
  Zone zone;
  zone.addRange(1, std::nullopt, 5);
  zone.addRange(2, 2, std::nullopt);
  EXPECT_EQ(zone.bound(1, 2), 3);
  Zone same;
  same.addRange(1, std::nullopt, 5);
  same.addRange(2, 2, std::nullopt);
  same.addLE(1, 2, 3); // what zero implies: the same zone
  EXPECT_TRUE(zone == same);
  same.addLE(1, 2, 1);
  EXPECT_EQ(same.bound(1, 2), 1);
  EXPECT_FALSE(zone == same);
}

TEST(Zone, RelationsThroughAStoredOneReachTheirBounds) {
  // j - i <= 1 stored; bounding i bounds j.
  Zone zone;
  zone.addLE(2, 1, 1);
  zone.addRange(1, std::nullopt, 7);
  EXPECT_EQ(zone.upper(2), 8);
  zone.addRange(2, 3, std::nullopt);
  EXPECT_EQ(zone.lower(1), 2);
}

TEST(Zone, JoinKeepsWhatTwoCountersShareOnEachSide) {
  // i = j = 0 on one side, i = j = 1 on the other: i - j <= 0 on both,
  // which neither result bound implies.
  Zone left;
  left.addRange(1, 0, 0);
  left.addRange(2, 0, 0);
  Zone right;
  right.addRange(1, 1, 1);
  right.addRange(2, 1, 1);
  std::vector<SymPair> pairs{SymPair{.result = 1,
                                     .left = 1,
                                     .right = 1,
                                     .hasLeft = true,
                                     .hasRight = true},
                             SymPair{.result = 2,
                                     .left = 2,
                                     .right = 2,
                                     .hasLeft = true,
                                     .hasRight = true}};
  Zone joined = Zone::combine(left, right, pairs, false, {});
  EXPECT_EQ(joined.bound(1, 2), 0);
  EXPECT_EQ(joined.bound(2, 1), 0);
  EXPECT_EQ(joined.upper(1), 1);
}

//===----------------------------------------------------------------------===//
// Heap fixtures
//===----------------------------------------------------------------------===//

namespace {
/// Every type may alias every other; unwritten cells read as fresh
/// unknown values.
class TestOracle : public HeapOracle {
public:
  bool strict = false;
  [[nodiscard]] bool typesMayAlias(Handle first, Handle second) const override {
    return !strict || first == 0 || second == 0 || first == second;
  }
  Sym unwritten(HeapState &state, ObjectId object, CellKey key,
                const SymInfo &hint) const override {
    SymInfo value;
    value.type = hint.type;
    Sym sym = state.nextSym++;
    state.syms.set(sym, value);
    if (!key.isSummary())
      state.objects.at(object).cells.set(key, sym);
    return sym;
  }
};

class HeapTest : public ::testing::Test {
protected:
  ObjectTable table;
  TestOracle oracle;
  Heap heap{table, oracle};

  ObjectId local(Handle decl, const char *name = "x") {
    ObjectInfo info;
    info.name = name;
    return table.intern(ObjectKey{.kind = ObjectKind::Local, .handle = decl},
                        info);
  }
  ObjectId allocation(Handle site) {
    ObjectInfo info;
    info.name = "heap";
    return table.intern(
        ObjectKey{.kind = ObjectKind::HeapRecent, .handle = site}, info);
  }
  /// Every earlier allocation of a site: several runtime objects.
  ObjectId allocations(Handle site) {
    ObjectInfo info;
    info.name = "heap";
    info.singular = false;
    return table.intern(ObjectKey{.kind = ObjectKind::HeapOld, .handle = site},
                        info);
  }
  ObjectId entry(std::uint32_t param, ObjectId ownedFrom = 0,
                 bool owning = false, Handle type = 0) {
    ObjectInfo info;
    info.ownedFrom = ownedFrom;
    info.fromOwningSlot = owning;
    info.type = type;
    SummaryPath path = SummaryPath::param(param).deref();
    if (ownedFrom != 0)
      path = path.field("next").deref();
    return table.intern(
        ObjectKey{.kind = ObjectKind::Entry, .handle = ownedFrom, .path = path},
        info);
  }
  Sym pointsTo(HeapState &state, ObjectId object) {
    heap.ensure(state, object);
    return heap.pointer(state, {Target{.object = object}},
                        PointerNull::NonNull);
  }
  /// Stores `value` in the local `holder`'s cell 0.
  void set(HeapState &state, ObjectId holder, Sym value) {
    heap.ensure(state, holder);
    heap.write(state, holder, CellKey{}, value, false);
  }
  Sym get(const HeapState &state, ObjectId holder) {
    return *heap.read(state, holder, CellKey{});
  }
  ReleaseRecord freed() {
    ReleaseRecord record;
    record.reason = ReleaseRecord::Reason::Freed;
    record.family = "malloc";
    return record;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// I1: stores and releases through any alias reach the object
//===----------------------------------------------------------------------===//

TEST_F(HeapTest, I1_StoreThroughAliasIsSeenThroughTheOriginal) {
  // struct box bx; q = &bx; bx.buf = malloc(16); q->buf = malloc(2);
  HeapState state;
  ObjectId bx = local(1, "bx");
  ObjectId big = allocation(10);
  ObjectId small = allocation(11);
  heap.ensure(state, bx);
  Sym toBig = pointsTo(state, big);
  heap.object(state, big).extent = Extent{Term::of(64), ExtentClass::Exact};
  Sym toSmall = pointsTo(state, small);
  heap.object(state, small).extent = Extent{Term::of(8), ExtentClass::Exact};
  heap.write(state, bx, CellKey{}, toBig, false);
  // q holds &bx; the store through q is a store to bx's cell.
  Sym q = pointsTo(state, bx);
  const SymInfo &qInfo = heap.info(state, q);
  ASSERT_EQ(qInfo.targets.size(), 1U);
  heap.write(state, qInfo.targets[0].object, CellKey{}, toSmall, false);
  // bx.buf[10]: 40..44 bytes into the object bx.buf now points to.
  Sym buf = get(state, bx);
  SpatialVerdict verdict = heap.spatial(state, buf, Term::of(40), 4);
  EXPECT_EQ(verdict.kind, SpatialVerdict::Kind::Violation);
}

TEST_F(HeapTest, I1_ReleaseIsSeenThroughAPointerIntoTheObject) {
  HeapState state;
  ObjectId block = allocation(10);
  Sym p = pointsTo(state, block);
  // q = p + 4: a different symbol, the same object.
  Sym q = heap.pointer(state, {Target{.object = block, .offset = Term::of(4)}},
                       PointerNull::NonNull);
  heap.release(state, p, freed());
  TemporalVerdict verdict = heap.temporal(state, q);
  EXPECT_EQ(verdict.kind, TemporalVerdict::Kind::Violation);
}

TEST_F(HeapTest, I1_ReleaseThroughAHeapCellAlias) {
  // *slot = o; alias = slot; free(o); (*alias)->v
  HeapState state;
  ObjectId slotObject = allocation(1);
  ObjectId o = allocation(2);
  Sym toO = pointsTo(state, o);
  heap.ensure(state, slotObject);
  heap.write(state, slotObject, CellKey{}, toO, false);
  heap.release(state, toO, freed());
  Sym loaded = *heap.read(state, slotObject, CellKey{});
  EXPECT_EQ(heap.temporal(state, loaded).kind,
            TemporalVerdict::Kind::Violation);
}

//===----------------------------------------------------------------------===//
// I2 and §4.5: distinctness
//===----------------------------------------------------------------------===//

TEST_F(HeapTest, I2_EntryObjectsMayAliasUnlessDistinct) {
  HeapState state;
  ObjectId p = entry(0);
  ObjectId q = entry(1);
  EXPECT_TRUE(heap.mayOverlap(state, p, q));
  oracle.strict = true;
  ObjectId typedP = entry(2, 0, false, 100);
  ObjectId typedQ = entry(3, 0, false, 200);
  EXPECT_FALSE(heap.mayOverlap(state, typedP, typedQ)); // D1
}

TEST_F(HeapTest, D2_OwningPlacesAreDistinct) {
  HeapState state;
  ObjectId parent = entry(0);
  ObjectId a = entry(1, 0, true);
  ObjectId b = entry(2, 0, true);
  EXPECT_FALSE(heap.mayOverlap(state, a, b));
  EXPECT_TRUE(heap.mayOverlap(state, parent, a) ||
              !heap.mayOverlap(state, parent, a));
}

TEST_F(HeapTest, D3_OwnerForest) {
  HeapState state;
  ObjectId head = entry(0);
  ObjectId next = entry(0, head, true);
  EXPECT_FALSE(heap.mayOverlap(state, head, next));
  EXPECT_TRUE(heap.ownedBelow(next, head));
}

TEST_F(HeapTest, D4_FreshObjectsAreNotEntryObjects) {
  HeapState state;
  EXPECT_FALSE(heap.mayOverlap(state, entry(0), allocation(5)));
  EXPECT_FALSE(heap.mayOverlap(state, entry(0), local(9)));
  EXPECT_FALSE(heap.mayOverlap(state, allocation(5), allocation(6)));
}

TEST_F(HeapTest, ReleasingAnEntryObjectTaintsItsPossibleAliases) {
  HeapState state;
  ObjectId p = entry(0);
  ObjectId q = entry(1);
  Sym ps = pointsTo(state, p);
  Sym qs = pointsTo(state, q);
  heap.release(state, ps, freed());
  EXPECT_EQ(heap.temporal(state, ps).kind, TemporalVerdict::Kind::Violation);
  EXPECT_EQ(heap.temporal(state, qs).kind,
            TemporalVerdict::Kind::MayAliasReleased);
}

TEST_F(HeapTest, D6_DerivedValuesAreNotTaintedByTheirReleaser) {
  HeapState state;
  ObjectId head = entry(0);
  ObjectId other = entry(1);
  Sym p = pointsTo(state, head);
  Sym next = pointsTo(state, other);
  heap.infoMut(state, next).ancestors = {p};
  heap.release(state, p, freed());
  EXPECT_EQ(heap.temporal(state, next).kind, TemporalVerdict::Kind::Proven);
}

//===----------------------------------------------------------------------===//
// I3: path joins keep releases as possible
//===----------------------------------------------------------------------===//

TEST_F(HeapTest, I3_ReleaseOnOnePathIsPossibleAfterTheJoin) {
  HeapState before;
  ObjectId holder = local(1);
  ObjectId block = allocation(2);
  Sym p = pointsTo(before, block);
  set(before, holder, p);
  HeapState freedPath = before;
  heap.release(freedPath, p, freed());
  HeapState joined = heap.join(freedPath, before, 99);
  Sym after = get(joined, holder);
  EXPECT_EQ(heap.temporal(joined, after).kind,
            TemporalVerdict::Kind::MayReleased);
}

TEST_F(HeapTest, I3_ReleaseOnBothPathsStaysDefinite) {
  HeapState before;
  ObjectId holder = local(1);
  ObjectId block = allocation(2);
  Sym p = pointsTo(before, block);
  set(before, holder, p);
  HeapState a = before;
  HeapState b = before;
  heap.release(a, p, freed());
  heap.release(b, p, freed());
  HeapState joined = heap.join(a, b, 99);
  EXPECT_EQ(heap.temporal(joined, get(joined, holder)).kind,
            TemporalVerdict::Kind::Violation);
}

TEST_F(HeapTest, APossibleReleaseIsNeverDefinite) {
  // RFC 0034 §6.3: `munmap` of a range that may not cover the object.
  HeapState state;
  ObjectId holder = local(1);
  ObjectId block = allocation(2);
  Sym p = pointsTo(state, block);
  set(state, holder, p);
  heap.release(state, p, freed(), /*possibly=*/true);
  EXPECT_EQ(heap.temporal(state, get(state, holder)).kind,
            TemporalVerdict::Kind::MayReleased);
  EXPECT_EQ(state.objects.at(block).life, Life::MayReleased);
}

//===----------------------------------------------------------------------===//
// I4: weak cells hold every value
//===----------------------------------------------------------------------===//

TEST_F(HeapTest, I4_SummaryCellHoldsEveryStoredValue) {
  HeapState state;
  ObjectId array = allocation(1);
  ObjectId a = allocation(2);
  ObjectId b = allocation(3);
  Sym pa = pointsTo(state, a);
  Sym pb = pointsTo(state, b);
  heap.ensure(state, array);
  CellKey element{.offset = 0, .stride = 8};
  heap.write(state, array, element, pa, true);
  heap.write(state, array, element, pb, true);
  heap.release(state, pa, freed());
  Sym loaded = *heap.read(state, array, element);
  // The element may be the freed one; elements are not told apart, so the
  // verdict is an aliasing one, not a diagnostic.
  TemporalVerdict verdict = heap.temporal(state, loaded);
  EXPECT_NE(verdict.kind, TemporalVerdict::Kind::Proven);
}

TEST_F(HeapTest, I4_ConcreteElementWriteReachesEveryElementRead) {
  // §4.2 *Amendment (arrays)*: the summary cell holds what stores through
  // unknown indices wrote; a read of "some element" or of an element at an
  // unknown index still sees a concrete element's value.
  HeapState state;
  ObjectId array = allocation(1);
  ObjectId a = allocation(2);
  Sym pa = pointsTo(state, a);
  heap.ensure(state, array);
  CellKey summary{.offset = 0, .stride = 8};
  Sym unknown = heap.fresh(state, SymInfo{.type = SymInfo::Type::Pointer});
  heap.write(state, array, summary, unknown, true);
  heap.write(state, array, CellKey{.offset = 16}, pa, false);
  heap.release(state, pa, freed());
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym some = heap.load(state, array, summary, hint);
  EXPECT_NE(heap.temporal(state, some).kind, TemporalVerdict::Kind::Proven);
  Sym index = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  Sym selected = heap.load(
      state, array, CellKey{.offset = 0, .stride = 8, .index = index}, hint);
  EXPECT_NE(heap.temporal(state, selected).kind, TemporalVerdict::Kind::Proven);
}

//===----------------------------------------------------------------------===//
// I5: the zone decides bounds
//===----------------------------------------------------------------------===//

TEST_F(HeapTest, I5_IndexBelowCountIsProven) {
  HeapState state;
  ObjectId block = allocation(1);
  Sym n = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  Sym i = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  heap.ensure(state, block).extent =
      Extent{Term::ofSym(n, 4), ExtentClass::Exact};
  state.zone.addRange(i, 0, std::nullopt);
  state.zone.addLE(i, n, -1); // i < n
  Sym p = pointsTo(state, block);
  // p[i]: 4*i .. 4*i + 4 against 4*n.
  EXPECT_EQ(heap.spatial(state, p, Term::ofSym(i, 4), 4).kind,
            SpatialVerdict::Kind::Proven);
  Sym j = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  state.zone.addRange(j, 0, std::nullopt);
  EXPECT_EQ(heap.spatial(state, p, Term::ofSym(j, 4), 4).kind,
            SpatialVerdict::Kind::Checkable);
  Sym k = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  state.zone.addLE(n, k, 0); // k >= n
  EXPECT_EQ(heap.spatial(state, p, Term::ofSym(k, 4), 4).kind,
            SpatialVerdict::Kind::Violation);
}

TEST_F(HeapTest, UnknownExtentIsUnresolved) {
  HeapState state;
  ObjectId p = entry(0);
  Sym ps = pointsTo(state, p);
  EXPECT_EQ(heap.spatial(state, ps, Term::of(0), 4).kind,
            SpatialVerdict::Kind::UnknownExtent);
  heap.object(state, p).extent = Extent{Term::of(8), ExtentClass::LowerBound};
  EXPECT_EQ(heap.spatial(state, ps, Term::of(0), 4).kind,
            SpatialVerdict::Kind::Proven);
  EXPECT_EQ(heap.spatial(state, ps, Term::of(8), 4).kind,
            SpatialVerdict::Kind::UnknownExtent);
}

//===----------------------------------------------------------------------===//
// I6 and §4.6: collection, dead copies and focus objects
//===----------------------------------------------------------------------===//

TEST_F(HeapTest, I6_UnreachableOwnedAllocationIsALeak) {
  HeapState state;
  ObjectId holder = local(1);
  ObjectId block = allocation(2);
  set(state, holder, pointsTo(state, block));
  heap.object(state, block).owned = true;
  set(state, holder, heap.constant(state, 0));
  std::vector<ObjectId> leaked;
  heap.collect(state, {holder}, [&](ObjectId id) { leaked.push_back(id); });
  ASSERT_EQ(leaked.size(), 1U);
  EXPECT_EQ(leaked[0], block);
  EXPECT_FALSE(state.objects.contains(block));
}

TEST_F(HeapTest, I6_ReleasedEntryObjectBecomesADeadCopyThatStillTaints) {
  HeapState state;
  ObjectId holder = local(1);
  ObjectId p = entry(0);
  ObjectId q = entry(1);
  Sym ps = pointsTo(state, p);
  Sym qs = pointsTo(state, q);
  set(state, holder, qs);
  heap.release(state, ps, freed());
  heap.collect(state, {holder}, nullptr);
  EXPECT_FALSE(state.objects.contains(p));
  ObjectId dead = table.deadCopy(p);
  ASSERT_TRUE(state.objects.contains(dead));
  EXPECT_EQ(heap.temporal(state, get(state, holder)).kind,
            TemporalVerdict::Kind::MayAliasReleased);
}

TEST_F(HeapTest, ListDestructorLoopReachesAFixpointWithoutTaint) {
  // while (p) { next = p->next; free(p); p = next; }
  ObjectId pVar = local(1, "p");
  ObjectId nextVar = local(2, "next");
  ObjectId head = entry(0);
  ObjectId second = entry(0, head, true);
  ObjectId third = entry(0, second, true);
  HeapState entryState;
  Sym p0 = pointsTo(entryState, head);
  set(entryState, pVar, p0);
  heap.ensure(entryState, nextVar);

  // One iteration from an input state where p points to `from`, whose next
  // field points to `to`.
  auto iterate = [&](HeapState state, ObjectId to) {
    Sym p = get(state, pVar);
    heap.ensure(state, to);
    Sym next = pointsTo(state, to);
    heap.infoMut(state, next).ancestors = {p};
    set(state, nextVar, next);
    TemporalVerdict atLoad = heap.temporal(state, p);
    EXPECT_EQ(atLoad.kind, TemporalVerdict::Kind::Proven);
    heap.release(state, p, freed());
    set(state, pVar, next);
    heap.collect(state, {pVar, nextVar}, nullptr);
    return state;
  };
  HeapState afterFirst = iterate(entryState, second);
  HeapState head1 = heap.join(entryState, afterFirst, 7);
  // p points to one focus object whose candidates are head and second.
  Sym p1 = get(head1, pVar);
  ASSERT_EQ(heap.info(head1, p1).targets.size(), 1U);
  EXPECT_EQ(table.info(heap.info(head1, p1).targets[0].object).key.kind,
            ObjectKind::Focus);
  EXPECT_EQ(heap.temporal(head1, p1).kind, TemporalVerdict::Kind::Proven);
  HeapState afterSecond = iterate(head1, third);
  HeapState head2 = heap.join(entryState, afterSecond, 7);
  EXPECT_EQ(heap.temporal(head2, get(head2, pVar)).kind,
            TemporalVerdict::Kind::Proven);
}

TEST_F(HeapTest, WriteThroughFocusReachesTheCandidates) {
  ObjectId pVar = local(1, "p");
  ObjectId a = entry(0);
  ObjectId b = entry(1);
  HeapState left;
  set(left, pVar, pointsTo(left, a));
  HeapState right;
  set(right, pVar, pointsTo(right, b));
  HeapState joined = heap.join(left, right, 3);
  Sym p = get(joined, pVar);
  ObjectId focus = heap.info(joined, p).targets.at(0).object;
  ObjectId block = allocation(9);
  Sym stored = pointsTo(joined, block);
  heap.write(joined, focus, CellKey{}, stored, false);
  heap.release(joined, stored, freed());
  // Reading a's cell directly may see the freed pointer.
  Sym viaA = *heap.read(joined, a, CellKey{});
  EXPECT_NE(heap.temporal(joined, viaA).kind, TemporalVerdict::Kind::Proven);
}

//===----------------------------------------------------------------------===//
// Elements (§4.2 *Amendment (arrays)*, RFC 0015)
//===----------------------------------------------------------------------===//

namespace {
CellKey elementAt(Sym index, std::int64_t offset = 0) {
  return CellKey{.offset = offset, .stride = 8, .index = index};
}
} // namespace

TEST_F(HeapTest, ElementsSelectedByOneIndexAreOneCell) {
  // free(a[i]); free(a[i]): the same symbol, so a definite double free.
  HeapState state;
  ObjectId array = allocation(1);
  heap.ensure(state, array);
  Sym i = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym first = heap.load(state, array, elementAt(i), hint);
  heap.release(state, first, freed());
  Sym second = heap.load(state, array, elementAt(i), hint);
  EXPECT_EQ(first, second);
  EXPECT_EQ(heap.temporal(state, second).kind,
            TemporalVerdict::Kind::Violation);
}

TEST_F(HeapTest, ElementsAtUnrelatedIndicesAreNotTheSameValue) {
  // zap(a, i, j): a[i] and a[j] read before a release are different
  // symbols; a[j] read after `free(a[i])` may be the freed element: a
  // possible release, never a definite one.
  HeapState state;
  ObjectId array = allocation(1);
  heap.ensure(state, array);
  Sym i = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  Sym j = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym before = heap.load(state, array, elementAt(j), hint);
  Sym atI = heap.load(state, array, elementAt(i), hint);
  EXPECT_NE(before, atI);
  heap.release(state, atI, freed());
  EXPECT_NE(heap.temporal(state, before).kind,
            TemporalVerdict::Kind::Violation);
  Sym k = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  Sym atK = heap.load(state, array, elementAt(k), hint);
  EXPECT_EQ(heap.temporal(state, atK).kind, TemporalVerdict::Kind::MayReleased);
}

TEST_F(HeapTest, ElementsAtDistinctIndicesAreApart) {
  // free(a[i]); use(a[i + 1]): the zone separates the indices.
  HeapState state;
  ObjectId array = allocation(1);
  heap.ensure(state, array);
  Sym i = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  Sym next = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  ASSERT_TRUE(state.zone.addEq(next, i, 1));
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym atI = heap.load(state, array, elementAt(i), hint);
  heap.release(state, atI, freed());
  Sym atNext = heap.load(state, array, elementAt(next), hint);
  EXPECT_EQ(heap.temporal(state, atNext).kind, TemporalVerdict::Kind::Proven);
  // An index the zone makes equal reads the same cell.
  Sym same = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  ASSERT_TRUE(state.zone.addEq(same, next, -1));
  EXPECT_EQ(heap.load(state, array, elementAt(same), hint), atI);
}

TEST_F(HeapTest, AnIntegerCellAnElementMayBeKeepsItsPointerTargets) {
  // struct { T *kids[4]; int n; }: kids[j] with j bounded only by n may be
  // n's cell (offset 32). The read stays a pointer to what kids[0] holds
  // and to any object (RFC 0034 §6.3, a release in a loop left early), so
  // `free(p->kids[j])` still releases, possibly, what the elements hold.
  HeapState state;
  ObjectId panel = allocation(1);
  heap.ensure(state, panel);
  ObjectId block = allocation(2);
  heap.write(state, panel, CellKey{.offset = 0}, pointsTo(state, block), false);
  Sym count = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  heap.write(state, panel, CellKey{.offset = 32}, count, false);
  Sym j = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  ASSERT_TRUE(state.zone.addRange(j, 0, std::nullopt));
  Sym kid = heap.load(state, panel, elementAt(j),
                      SymInfo{.type = SymInfo::Type::Pointer});
  const SymInfo &value = heap.info(state, kid);
  ASSERT_EQ(value.type, SymInfo::Type::Pointer);
  EXPECT_TRUE(value.rawCast);
  EXPECT_TRUE(std::ranges::any_of(value.targets, [&](const Target &target) {
    return target.object == block;
  }));
  heap.release(state, kid, freed());
  EXPECT_EQ(state.objects.at(block).life, Life::MayReleased);
}

TEST_F(HeapTest, AnIntegerInTheSummaryCellLeavesAnElementReadAPointer) {
  // A loop folded the count `n` into the element position (`kids[*]`): a
  // pointer read of some element is still a pointer, raw on that side.
  HeapState state;
  ObjectId panel = allocation(1);
  heap.ensure(state, panel);
  Sym count = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  heap.write(state, panel, CellKey{.offset = 0, .stride = 8}, count, true);
  Sym j = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  Sym kid = heap.load(state, panel, elementAt(j),
                      SymInfo{.type = SymInfo::Type::Pointer});
  EXPECT_EQ(heap.info(state, kid).type, SymInfo::Type::Pointer);
  EXPECT_TRUE(heap.info(state, kid).rawCast);
}

TEST_F(HeapTest, ReleasedRangeDecidesTheElementsInIt) {
  // drop(a, 3): every element of [0, 3) released.
  HeapState state;
  ObjectId array = allocation(1);
  heap.ensure(state, array);
  SymInfo hint{.type = SymInfo::Type::Pointer};
  CellKey position{.offset = 0, .stride = 8};
  // Each element its own allocation (a fill loop's).
  ObjectId blocks = allocations(2);
  Sym element = pointsTo(state, blocks);
  heap.writeElements(state, array, position, Term::of(0), Term::of(4), element,
                     false);
  heap.releaseElements(state, array, position, Term::of(0), Term::of(3),
                       freed(), hint);
  Sym inside = heap.load(state, array, CellKey{.offset = 16}, hint);
  EXPECT_EQ(heap.temporal(state, inside).kind,
            TemporalVerdict::Kind::Violation);
  Sym outside = heap.load(state, array, CellKey{.offset = 24}, hint);
  EXPECT_NE(heap.temporal(state, outside).kind,
            TemporalVerdict::Kind::Violation);
  EXPECT_NE(heap.temporal(state, outside).kind,
            TemporalVerdict::Kind::MayReleased);
  // An index that may or may not be in the range: possible.
  Sym k = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  Sym maybe = heap.load(state, array, elementAt(k), hint);
  EXPECT_EQ(heap.temporal(state, maybe).kind,
            TemporalVerdict::Kind::MayReleased);
}

TEST_F(HeapTest, CleanupLoopFoldsIntoAReleasedRange) {
  // for (i = 0; i < n; i++) free(a[i]); at the loop head: the head's state
  // (i = 0) joined with the back edge's (a[0] released, i = 1) is one range
  // [0, i) that holds released values, and a[i] is not in it.
  ObjectId array = allocation(1);
  ObjectId counter = local(2, "i");
  HeapState head;
  heap.ensure(head, array).stride = 8;
  set(head, counter, heap.constant(head, 0));
  HeapState back = head;
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym first = heap.load(back, array, CellKey{.offset = 0}, hint);
  heap.release(back, first, freed());
  set(back, counter, heap.constant(back, 1));
  HeapState joined = heap.join(head, back, 7, /*loopHead=*/true);
  const ObjectState &folded = *joined.objects.find(array);
  ASSERT_EQ(folded.segments.size(), 1U);
  Sym i = get(joined, counter);
  EXPECT_EQ(folded.segments[0].to, Term::ofSym(i));
  // a[i]: the element the next iteration releases is not released yet.
  Sym next = heap.load(joined, array, elementAt(i), hint);
  EXPECT_EQ(heap.temporal(joined, next).kind, TemporalVerdict::Kind::Proven);
  // Where the loop ran (i >= 1), a[0] is released.
  ASSERT_TRUE(joined.zone.addRange(i, 1, std::nullopt));
  Sym done = heap.load(joined, array, CellKey{.offset = 0}, hint);
  EXPECT_EQ(heap.temporal(joined, done).kind, TemporalVerdict::Kind::Violation);
}

TEST_F(HeapTest, AGlobalOneSideNeverReadHoldsAPossibleRelease) {
  // `do { ... fclose(stdout); ... } while (...)`: at the loop head the back
  // edge read `stdout` and released what it points to; the head never read
  // it, so there it holds its initial (live) value. The joined value is a
  // possible release, never a definite one.
  ObjectInfo info;
  info.name = "stdout";
  ObjectId global =
      table.intern(ObjectKey{.kind = ObjectKind::Global, .handle = 9}, info);
  ObjectId file = entry(0);
  HeapState head;
  HeapState back;
  Sym value = pointsTo(back, file);
  set(back, global, value);
  heap.release(back, value, freed());
  HeapState joined = heap.join(head, back, 7, /*loopHead=*/true);
  Sym loaded = get(joined, global);
  EXPECT_EQ(heap.temporal(joined, loaded).kind,
            TemporalVerdict::Kind::MayReleased);
}

TEST_F(HeapTest, EvictedElementStaysAWeakWrite) {
  // A selected cell whose index goes away keeps what it held for every
  // element it may be (soundness of the join, §4.8).
  HeapState state;
  ObjectId array = allocation(1);
  heap.ensure(state, array);
  Sym i = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  ObjectId block = allocation(2);
  Sym stored = pointsTo(state, block);
  heap.write(state, array, elementAt(i), stored, false);
  heap.release(state, stored, freed());
  heap.evictCell(state, array, elementAt(i));
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym some = heap.load(state, array, CellKey{.offset = 8}, hint);
  EXPECT_NE(heap.temporal(state, some).kind, TemporalVerdict::Kind::Proven);
}

TEST_F(HeapTest, IndicesOneConstantApartAreDifferentCells) {
  // free(a[i]); use(a[i + 1]) where `i + 1` may wrap (no zone relation):
  // the two values still differ, so the cells do.
  HeapState state;
  ObjectId array = allocation(1);
  heap.ensure(state, array);
  IntegerType type{.width = 32, .isSigned = true};
  Sym i =
      heap.fresh(state, SymInfo{.type = SymInfo::Type::Int, .intType = type});
  SymInfo sum{.type = SymInfo::Type::Int, .intType = type};
  sum.defined = SymDefinition{.op = IntegerOp::Add, .left = i, .constant = 1};
  Sym next = heap.fresh(state, sum);
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym atI = heap.load(state, array, elementAt(i), hint);
  heap.release(state, atI, freed());
  Sym atNext = heap.load(state, array, elementAt(next), hint);
  EXPECT_EQ(heap.temporal(state, atNext).kind, TemporalVerdict::Kind::Proven);
}

TEST_F(HeapTest, CopiedRangeReadsEachSourceElement) {
  // memcpy(d, s, n * sizeof *s) from an unstored entry object: d[5] is the
  // value s[5] held, so free(s[5]) is seen through it; a store into `s`
  // after the copy does not reach `d`.
  HeapState state;
  ObjectId source = entry(0);
  ObjectId dest = entry(1);
  heap.ensure(state, source);
  heap.ensure(state, dest);
  Sym n = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
  ASSERT_TRUE(state.zone.addRange(n, 10, std::nullopt));
  CellKey position{.offset = 0, .stride = 8};
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym some = heap.load(state, source, position, hint);
  heap.copyElements(state, dest, position, Term::of(0), Term::ofSym(n), some,
                    source, 0);
  Sym s5 = heap.load(state, source, CellKey{.offset = 40}, hint);
  heap.release(state, s5, freed());
  Sym d5 = heap.load(state, dest, CellKey{.offset = 40}, hint);
  EXPECT_EQ(d5, s5);
  EXPECT_EQ(heap.temporal(state, d5).kind, TemporalVerdict::Kind::Violation);
  Sym other = pointsTo(state, allocation(2));
  heap.write(state, source, CellKey{.offset = 48}, other, false);
  Sym d6 = heap.load(state, dest, CellKey{.offset = 48}, hint);
  EXPECT_NE(d6, other);
}

TEST_F(HeapTest, StoreThroughAnUnknownIndexReachesConcreteReads) {
  HeapState state;
  ObjectId array = allocation(1);
  heap.ensure(state, array);
  ObjectId block = allocation(2);
  Sym stored = pointsTo(state, block);
  heap.write(state, array, CellKey{.offset = 0, .stride = 8}, stored, true);
  heap.release(state, stored, freed());
  SymInfo hint{.type = SymInfo::Type::Pointer};
  Sym element = heap.load(state, array, CellKey{.offset = 40}, hint);
  EXPECT_NE(heap.temporal(state, element).kind, TemporalVerdict::Kind::Proven);
}

TEST_F(HeapTest, JoinIsDeterministic) {
  ObjectId holder = local(1);
  HeapState left;
  set(left, holder, heap.constant(left, 1));
  HeapState right;
  set(right, holder, heap.constant(right, 2));
  HeapState once = heap.join(left, right, 1);
  HeapState twice = heap.join(left, right, 1);
  EXPECT_EQ(once, twice);
  HeapState widened = heap.widen(once, heap.join(once, right, 1), 1, {});
  EXPECT_EQ(widened, heap.widen(once, heap.join(once, right, 1), 1, {}));
}

//===----------------------------------------------------------------------===//
// Bytes, offsets and definitions across joins (§4.2, §4.8, §5.3)
//===----------------------------------------------------------------------===//

TEST_F(HeapTest, ForgottenBytesAreNeitherZeroNorAString) {
  // calloc'd, "abc" known at its start, then a memcpy of unknown bytes.
  HeapState state;
  ObjectId block = allocation(10);
  ObjectState &object = heap.ensure(state, block);
  object.zeroed = true;
  object.nulWithin = Term::of(3);
  object.nulFrom = Term::of(0);
  heap.forgetCells(state, block, 0, 4);
  const ObjectState &after = *heap.findObject(state, block);
  // Only the four bytes read as unknown; the rest are still zeros.
  EXPECT_FALSE(after.havocked);
  EXPECT_EQ(after.forgotten,
            (std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 4}}));
  EXPECT_TRUE(after.forgets(CellKey{.offset = 2}));
  EXPECT_FALSE(after.forgets(CellKey{.offset = 4}));
  EXPECT_FALSE(after.nulWithin.has_value());
  EXPECT_FALSE(after.nulFrom.has_value());
  // Without a size, every byte.
  heap.forgetCells(state, block, 0, std::nullopt);
  EXPECT_TRUE(heap.findObject(state, block)->havocked);
  EXPECT_TRUE(heap.findObject(state, block)->forgotten.empty());
}

TEST_F(HeapTest, JoinKeepsBytesForgottenOnOneSideAsPossible) {
  // Bytes 8..24 rewritten on one path, 16..32 on the other: 16..24 on both.
  ObjectId record = local(1, "r");
  HeapState left;
  heap.ensure(left, record);
  HeapState right = left;
  heap.forgetCells(left, record, 8, 16);
  heap.forgetCells(right, record, 16, 16);
  HeapState joined = heap.join(left, right, 7);
  const ObjectState &object = *heap.findObject(joined, record);
  using Ranges = std::vector<std::pair<std::int64_t, std::int64_t>>;
  EXPECT_EQ(object.forgotten, (Ranges{{16, 24}}));
  EXPECT_EQ(object.mayForgotten, (Ranges{{8, 32}}));
  EXPECT_TRUE(object.forgets(CellKey{.offset = 16}));
  EXPECT_FALSE(object.forgets(CellKey{.offset = 8}));
  EXPECT_TRUE(object.mayForget(CellKey{.offset = 8}));
  EXPECT_FALSE(object.mayForget(CellKey{.offset = 0}));
}

TEST_F(HeapTest, AJoinWithAStoredNullSaysSo) {
  // A value only maybe-null (an entry value) joined with a null pointer is
  // `nullJoined`; one joined with an unassigned (null) value is not, nor is
  // the maybe-null value itself.
  HeapState state;
  ObjectId block = allocation(10);
  Sym maybe =
      heap.pointer(state, {Target{.object = block}}, PointerNull::Maybe);
  Sym null = heap.pointer(state, {}, PointerNull::Null);
  SymInfo unassigned;
  unassigned.type = SymInfo::Type::Pointer;
  unassigned.null = PointerNull::Null;
  unassigned.uninit = true;
  Sym garbage = heap.fresh(state, unassigned);
  EXPECT_FALSE(heap.info(state, maybe).nullJoined);
  EXPECT_TRUE(heap.info(state, heap.mergeWeak(state, maybe, null)).nullJoined);
  EXPECT_FALSE(
      heap.info(state, heap.mergeWeak(state, maybe, garbage)).nullJoined);
}

TEST_F(HeapTest, AJoinInsideAnExpressionKeepsTheNumbersBeforeIt) {
  // Two copies of one state, as a call through a hook applies each of its
  // functions: a value both still hold keeps its number, and every other
  // result is numbered past what the state had, so an operand the caller
  // evaluated before the call names no other value.
  HeapState before;
  ObjectId x = local(1, "x");
  heap.ensure(before, x);
  Sym kept = pointsTo(before, allocation(10));
  heap.write(before, x, CellKey{}, kept, false);
  Sym operand = heap.constant(before, 0, std::nullopt);
  const Sym keepBelow = before.nextSym;
  HeapState left = before;
  HeapState right = before;
  heap.write(left, x, CellKey{}, pointsTo(left, allocation(11)), false);
  HeapState joined = heap.join(left, right, 7, false, keepBelow);
  EXPECT_GE(joined.nextSym, keepBelow);
  Sym fresh = heap.fresh(joined, SymInfo{});
  EXPECT_NE(fresh, operand);
  EXPECT_NE(fresh, kept);
}

TEST_F(HeapTest, TrimmingKeepsEachPositionsNewestRanges) {
  // Six ranges of one position: the newest four stay, the two oldest become
  // weak writes to every element (the summary cell).
  HeapState state;
  ObjectId block = allocation(10);
  heap.ensure(state, block);
  const CellKey position{.offset = 0, .stride = 4, .index = ZeroSym};
  std::vector<Segment> segments;
  for (int i = 0; i < 6; ++i) {
    SymInfo value;
    value.type = SymInfo::Type::Int;
    Sym sym = heap.fresh(state, value);
    state.zone.addRange(sym, i, i);
    segments.push_back(Segment{.position = position,
                               .from = Term::of(2 * i),
                               .to = Term::of(2 * i + 1),
                               .value = sym});
  }
  heap.object(state, block).segments = segments;
  heap.trimSegments(state, block);
  const ObjectState &trimmed = heap.object(state, block);
  ASSERT_EQ(trimmed.segments.size(), Heap::MaxSegmentsPerPosition);
  EXPECT_EQ(trimmed.segments.front().value, segments.front().value);
  const Sym *summary = trimmed.cells.find(position);
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(state.zone.lower(*summary), 4);
  EXPECT_EQ(state.zone.upper(*summary), 5);
}

TEST_F(HeapTest, WeakenedCellsKeepWhatTheyHeld) {
  // `ls->fs = fs`, then a callee that may have rewritten `*ls`: the cell
  // still reaches `fs` beside an unknown value.
  ObjectId holder = local(1, "ls");
  ObjectId frame = local(2, "fs");
  ObjectId other = local(3, "unknown");
  HeapState state;
  set(state, holder, pointsTo(state, frame));
  heap.weakenCells(state, holder, 0, 8,
                   [&](Sym) { return pointsTo(state, other); });
  const SymInfo &value = heap.info(state, get(state, holder));
  std::vector<ObjectId> reached;
  for (const Target &target : value.targets)
    reached.push_back(target.object);
  EXPECT_NE(std::find(reached.begin(), reached.end(), frame), reached.end());
  EXPECT_NE(std::find(reached.begin(), reached.end(), other), reached.end());
  // An unwritten cell there may read as unknown, not must.
  EXPECT_FALSE(heap.findObject(state, holder)->forgets(CellKey{.offset = 0}));
  EXPECT_TRUE(heap.findObject(state, holder)->mayForget(CellKey{.offset = 0}));
}

TEST_F(HeapTest, JoinKeepsDifferingOffsetsAsARange) {
  // A cursor at buf + 0 on one path and buf + 1 on the other.
  ObjectId holder = local(1, "p");
  ObjectId buf = local(2, "buf");
  HeapState left;
  heap.ensure(left, buf).extent = Extent{Term::of(16), ExtentClass::Exact};
  set(left, holder, pointsTo(left, buf));
  HeapState right = left;
  set(right, holder,
      heap.pointer(right, {Target{.object = buf, .offset = Term::of(1)}},
                   PointerNull::NonNull));
  HeapState joined = heap.join(left, right, 7);
  Sym cursor = get(joined, holder);
  const SymInfo &value = heap.info(joined, cursor);
  ASSERT_EQ(value.targets.size(), 1U);
  ASSERT_TRUE(value.targets[0].offset.known);
  // In bounds for one byte, not for sixteen.
  EXPECT_EQ(heap.spatial(joined, cursor, Term::of(0), 1).kind,
            SpatialVerdict::Kind::Proven);
  EXPECT_NE(heap.spatial(joined, cursor, Term::of(0), 16).kind,
            SpatialVerdict::Kind::Proven);
}

TEST_F(HeapTest, JoinRelatesAOneSidedExtentToItsCountCell) {
  // `b->data` sized by `b->cap`, materialised on one path only: its extent
  // stays the value the joined cell holds.
  ObjectId box = entry(0);
  ObjectId data = entry(1);
  HeapState left;
  heap.ensure(left, box);
  Sym capLeft = heap.fresh(left, SymInfo{.type = SymInfo::Type::Int});
  heap.write(left, box, CellKey{.offset = 8}, capLeft, false);
  HeapState right;
  heap.ensure(right, box);
  Sym capRight = heap.fresh(right, SymInfo{.type = SymInfo::Type::Int});
  heap.write(right, box, CellKey{.offset = 8}, capRight, false);
  heap.ensure(right, data).extent =
      Extent{Term::ofSym(capRight), ExtentClass::Declared};
  HeapState joined = heap.join(left, right, 3);
  Sym cap = *heap.read(joined, box, CellKey{.offset = 8});
  const ObjectState *object = heap.findObject(joined, data);
  ASSERT_NE(object, nullptr);
  ASSERT_TRUE(object->extent.has_value());
  EXPECT_EQ(object->extent->bytes.var, cap);
}

TEST_F(HeapTest, AnOperationBothPathsComputeSurvivesTheJoin) {
  ObjectId count = local(1, "n");
  ObjectId bytes = local(2, "bytes");
  auto path = [&](HeapState &state) {
    Sym n = heap.fresh(state, SymInfo{.type = SymInfo::Type::Int});
    set(state, count, n);
    SymInfo product;
    product.type = SymInfo::Type::Int;
    product.ctype = 1;
    product.defined = SymDefinition{
        .op = IntegerOp::Multiply, .left = n, .constant = 4, .exact = false};
    set(state, bytes, heap.fresh(state, product));
  };
  HeapState left;
  path(left);
  HeapState right;
  path(right);
  HeapState joined = heap.join(left, right, 5);
  const SymInfo &value = heap.info(joined, get(joined, bytes));
  ASSERT_TRUE(value.defined.has_value());
  EXPECT_EQ(value.defined->op, IntegerOp::Multiply);
  EXPECT_EQ(value.defined->left, get(joined, count));
  EXPECT_EQ(value.defined->constant, 4);
}

TEST_F(HeapTest, AJoinKeepsTheConstantsBothSidesExclude) {
  // RFC 0034 §6.2: `t` is not 1 on either side, and not 3 on the left; on
  // the right its bounds leave 5 out, which the left excludes.
  ObjectId holder = local(1, "t");
  HeapState before;
  Sym t = heap.fresh(before, SymInfo{.type = SymInfo::Type::Int});
  set(before, holder, t);
  HeapState left = before;
  HeapState right = before;
  heap.infoMut(left, t).excluded = {1, 3, 5};
  heap.infoMut(right, t).excluded = {1};
  ASSERT_TRUE(right.zone.addRange(t, 0, 4));
  HeapState joined = heap.join(left, right, 9);
  EXPECT_EQ(heap.info(joined, get(joined, holder)).excluded,
            (std::vector<std::int64_t>{1, 5}));
  // A side that excludes nothing keeps nothing.
  HeapState joinedAgain = heap.join(joined, before, 9);
  EXPECT_TRUE(
      heap.info(joinedAgain, get(joinedAgain, holder)).excluded.empty());
}

} // namespace weavec::core

namespace weavec::core {
TEST(Zone, JoinOfLoopEntryAndBackEdge) {
  Zone left;
  left.addRange(1, 0, 0);
  Zone right;
  right.addRange(1, 0, 0);
  right.addRange(7, 1, 1);
  right.addLE(1, 7, -1);
  std::vector<SymPair> pairs{SymPair{.result = 1,
                                     .left = 5,
                                     .right = 1,
                                     .hasLeft = true,
                                     .hasRight = true},
                             SymPair{.result = 2,
                                     .left = 1,
                                     .right = 7,
                                     .hasLeft = true,
                                     .hasRight = true}};
  Zone joined = Zone::combine(left, right, pairs, false, {});
  EXPECT_EQ(joined.lower(2), 0);
  EXPECT_EQ(joined.upper(2), 1);
}
} // namespace weavec::core

namespace weavec::core {
TEST(Zone, OneSidedRelationsDoNotTightenSharedSymbols) {
  Zone left;
  left.addRange(1, 0, 0);
  Zone right;
  right.addRange(1, 0, 0);
  right.addRange(7, 1, 1);
  right.addLE(1, 7, -1);
  // r2 has both sides (left 1, right 7); r3 only the right's 1.
  std::vector<SymPair> pairs{SymPair{.result = 2,
                                     .left = 1,
                                     .right = 7,
                                     .hasLeft = true,
                                     .hasRight = true},
                             SymPair{.result = 3,
                                     .left = 0,
                                     .right = 1,
                                     .hasLeft = false,
                                     .hasRight = true}};
  Zone joined = Zone::combine(left, right, pairs, false, {});
  EXPECT_EQ(joined.lower(2), 0);
  EXPECT_EQ(joined.upper(2), 1);
  EXPECT_EQ(joined.constant(3), 0);
}
} // namespace weavec::core
