//===- TraversalTest.cpp - Traversal proof algebra (RFC 0021) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Traversal.h"

#include "weavec/Core/CheckedIO.h"
#include "weavec/Core/Safety.h"

#include <gtest/gtest.h>

#include <limits>

namespace weavec::core {

static CheckedRequirement traversalRequirement(unsigned index) {
  CheckedRequirement result{.kind = CheckedRequirementKind::Initialized,
                            .path = SummaryPath::param(index),
                            .other = {},
                            .begin = {},
                            .end = PathAffine::ofConstant(4),
                            .family = {}};
  result.when.require(SummaryPath::param(7), ValueFact::ofConstant(3));
  return result;
}

TEST(Traversal, SharedRequirementsDetachBeforeEveryMutation) {
  const auto first = traversalRequirement(0);
  const auto second = traversalRequirement(1);
  CheckedRequirements original{first};
  auto copy = original;
  EXPECT_EQ(&*original.begin(), &*copy.begin());
  EXPECT_FALSE(copy.insert(first).second);
  EXPECT_EQ(&*original.begin(), &*copy.begin());
  EXPECT_TRUE(copy.insert(second).second);
  EXPECT_EQ(original.size(), 1U);
  EXPECT_EQ(copy.size(), 2U);
  EXPECT_EQ(*original.begin(), first);
  original.clear();
  EXPECT_TRUE(original.empty());
  EXPECT_EQ(copy.size(), 2U);
  original = copy;
  copy.assign({second});
  EXPECT_EQ(original.size(), 2U);
  EXPECT_EQ(copy.size(), 1U);
  copy.intersect(original);
  EXPECT_EQ(copy, (CheckedRequirements{second}));
  original.intersect(copy);
  EXPECT_EQ(original, copy);
  copy.clear();
  EXPECT_EQ(original, (CheckedRequirements{second}));
  original.intersect(copy);
  EXPECT_TRUE(original.empty());
}

TEST(Traversal, SharedContractRemappingVisitsEveryGlobalBearingField) {
  using Expr = IntegerExpression<SummaryPath>;
  const IntegerType type{.width = 32, .isSigned = false};
  for (int globalField = -1; globalField < 11; ++globalField) {
    SCOPED_TRACE(globalField);
    const auto requirement = [&](std::uint32_t global) {
      const auto path = [&](int field) {
        return field == globalField
                   ? SummaryPath::global(global).deref().field("value")
                   : SummaryPath::param(static_cast<std::uint32_t>(field));
      };
      const auto expression = [&](int field) {
        return *Expr::operation(IntegerOp::Add, Expr::input(path(field), type),
                                Expr::constant(IntegerValue::ofBits(type, 1)));
      };
      auto result = traversalRequirement(0);
      result.path = path(0);
      result.other = path(1);
      result.begin = globalField == 4 ? PathAffine::ofExpression(expression(4))
                                      : PathAffine::ofPath(path(2));
      result.end = globalField == 5 ? PathAffine::ofExpression(expression(5))
                                    : PathAffine::ofPath(path(3));
      result.when.require(path(6), ValueFact::ofConstant(1));
      result.when.requirePointer(path(7), path(8), true);
      result.when.requireInteger(
          {.lhs = expression(9), .op = IntegerOp::Less, .rhs = expression(10)});
      return result;
    };
    FunctionSummary original;
    original.checked.computed = true;
    original.checked.require(requirement(3));
    original.checked.establish(requirement(3));
    const auto mapped = remapGlobals(original, [](std::uint32_t id) {
      EXPECT_EQ(id, 3U);
      return std::optional(9U);
    });
    EXPECT_EQ(mapped.checked.requirements,
              (CheckedRequirements{requirement(9)}));
    EXPECT_EQ(mapped.checked.establishes,
              (CheckedRequirements{requirement(9)}));
    const auto missing = remapGlobals(
        original, [](std::uint32_t) { return std::optional<std::uint32_t>{}; });
    EXPECT_EQ(missing.checked.limited, globalField >= 0);
    EXPECT_EQ(missing.checked.requirements.empty(), globalField >= 0);
    EXPECT_EQ(missing.checked.establishes.empty(), globalField >= 0);
    EXPECT_EQ(original.checked.requirements,
              (CheckedRequirements{requirement(3)}));
    if (globalField < 0) {
      EXPECT_EQ(&*mapped.checked.requirements.begin(),
                &*original.checked.requirements.begin());
      EXPECT_EQ(&*mapped.checked.establishes.begin(),
                &*original.checked.establishes.begin());
    }
  }
}

TEST(Traversal, SharedRequirementIntersectionsMatchIndependentSets) {
  for (unsigned leftMask = 0; leftMask < 32; ++leftMask) {
    for (unsigned rightMask = 0; rightMask < 32; ++rightMask) {
      CheckedRequirements left;
      CheckedRequirements right;
      std::set<CheckedRequirement> expected;
      std::set<CheckedRequirement> originalLeft;
      std::set<CheckedRequirement> originalRight;
      for (unsigned bit = 0; bit < 5; ++bit) {
        const auto entry = traversalRequirement(bit);
        if ((leftMask & (1U << bit)) != 0) {
          left.insert(entry);
          originalLeft.insert(entry);
        }
        if ((rightMask & (1U << bit)) != 0) {
          right.insert(entry);
          originalRight.insert(entry);
        }
        if ((leftMask & rightMask & (1U << bit)) != 0)
          expected.insert(entry);
      }
      const auto savedLeft = left;
      const auto savedRight = right;
      left.intersect(right);
      EXPECT_EQ((std::set<CheckedRequirement>(left.begin(), left.end())),
                expected);
      EXPECT_EQ(right, savedRight);
      right.intersect(savedLeft);
      EXPECT_EQ(left, right);
      left.intersect(left);
      EXPECT_EQ(left, right);
      EXPECT_EQ(
          (std::set<CheckedRequirement>(savedLeft.begin(), savedLeft.end())),
          originalLeft);
      EXPECT_EQ(
          (std::set<CheckedRequirement>(savedRight.begin(), savedRight.end())),
          originalRight);
    }
  }
}

TEST(Traversal, SharedContractsPreserveCapsJoinsAndPortableContents) {
  const auto names = [](std::uint32_t id) { return std::to_string(id); };
  const auto resolve = [](std::string_view) { return std::optional(0U); };
  CheckedContract original;
  original.computed = true;
  for (unsigned i = 0; i < MaxSafetyRequirements; ++i)
    original.require(traversalRequirement(i));
  original.establish(traversalRequirement(0));
  const auto encoded = printCheckedContract(original, names);
  auto joined = original;
  joined.join(original);
  EXPECT_FALSE(joined.limited);
  EXPECT_EQ(printCheckedContract(joined, names), encoded);
  EXPECT_EQ(&*joined.requirements.begin(), &*original.requirements.begin());
  CheckedContract more;
  more.computed = true;
  more.require(traversalRequirement(MaxSafetyRequirements));
  joined.join(more);
  EXPECT_TRUE(joined.limited);
  EXPECT_EQ(joined.requirements, original.requirements);
  EXPECT_TRUE(joined.establishes.empty());
  EXPECT_FALSE(original.limited);
  EXPECT_EQ(printCheckedContract(original, names), encoded);
  auto decoded = parseCheckedContract(encoded, resolve);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, original);
  decoded->requirements.clear();
  EXPECT_EQ(original.requirements.size(), MaxSafetyRequirements);
}

TEST(Traversal, TerminationWitnessesRequireEveryJoinAndValueDependency) {
  const PlaceId storage{0};
  const PlaceId copy{1};
  const PlaceId zero{2};
  const PlaceId input{3};
  SafetyState state;
  state.termination[storage].push_back(
      {.begin = {}, .zero = Affine::ofPlace(zero), .input = input, .when = {}});
  state.copyMemory(storage, copy);
  ASSERT_EQ(state.termination.size(), 2U);
  auto same = state;
  EXPECT_FALSE(state.join(same));
  same.termination.erase(copy);
  EXPECT_TRUE(state.join(same));
  EXPECT_FALSE(state.termination.contains(copy));
  state.forgetDependency(zero);
  EXPECT_TRUE(state.termination.empty());
  state = same;
  state.forget(storage);
  EXPECT_TRUE(state.termination.empty());
  state = same;
  state.forgetZeros();
  EXPECT_TRUE(state.termination.empty());
}

TEST(Traversal, TerminatorQuantitiesAreDistinctPortableAndStrict) {
  const auto names = [](std::uint32_t) { return std::string("input"); };
  const auto resolve = [](std::string_view) { return std::optional(3U); };
  const auto pointer = SummaryPath::global(3);
  const auto zero = PathAffine::ofTerminator(pointer);
  EXPECT_NE(zero, PathAffine::ofPath(pointer));
  EXPECT_EQ(printAffine(zero, names), "terminator global input scale 1 plus 0");
  CheckedContract contract;
  contract.computed = true;
  contract.require({.kind = CheckedRequirementKind::Terminated,
                    .path = pointer,
                    .other = {},
                    .begin = PathAffine::ofConstant(2),
                    .end = {},
                    .family = {}});
  contract.require({.kind = CheckedRequirementKind::Writable,
                    .path = SummaryPath::param(0),
                    .other = {},
                    .begin = {},
                    .end = PathAffine::ofTerminator(pointer, 1, 1),
                    .family = {}});
  contract.establish({.kind = CheckedRequirementKind::Terminated,
                      .path = pointer,
                      .other = {},
                      .begin = {},
                      .end = zero,
                      .family = {}});
  EXPECT_EQ(
      parseCheckedContract(printCheckedContract(contract, names), resolve),
      contract);
  for (unsigned variant = 0; variant < 4; ++variant) {
    auto malformed = zero;
    if (variant == 0)
      malformed.path.reset();
    if (variant == 1)
      malformed.path = SummaryPath::result();
    if (variant == 2)
      malformed.quantity = static_cast<AffineQuantity>(2);
    if (variant == 3)
      malformed.expression = IntegerExpression<SummaryPath>::constant(
          IntegerValue::ofBits({.width = 32, .isSigned = false}, 4));
    auto bad = contract;
    bad.require({.kind = CheckedRequirementKind::Extent,
                 .path = pointer,
                 .other = {},
                 .begin = {},
                 .end = malformed,
                 .family = {}});
    EXPECT_FALSE(
        parseCheckedContract(printCheckedContract(bad, names), resolve));
  }
}

TEST(Traversal, EntryTerminationRecordsRejectAmbiguousEndpoints) {
  const auto names = [](std::uint32_t) { return std::string("input"); };
  const auto resolve = [](std::string_view) { return std::optional(0U); };
  for (unsigned variant = 0; variant < 4; ++variant) {
    CheckedRequirement requirement{.kind = CheckedRequirementKind::Terminated,
                                   .path = SummaryPath::param(0),
                                   .other = {},
                                   .begin = {},
                                   .end = {},
                                   .family = {}};
    if (variant == 0)
      requirement.end = PathAffine::ofConstant(4);
    if (variant == 1)
      requirement.end = PathAffine::ofTerminator(SummaryPath::param(0));
    if (variant == 2)
      requirement.begin = PathAffine::ofConstant(-1);
    if (variant == 3)
      requirement.family = "ignored";
    CheckedContract contract;
    contract.computed = true;
    contract.require(requirement);
    EXPECT_FALSE(
        parseCheckedContract(printCheckedContract(contract, names), resolve));
  }
}

TEST(Traversal, CursorAndReturnedCountContractsArePortableAndStrict) {
  const auto names = [](std::uint32_t) { return std::string("origin"); };
  const auto resolve = [](std::string_view) { return std::optional(7U); };
  CheckedContract contract;
  contract.computed = true;
  CheckedRequirement position{.kind = CheckedRequirementKind::Position,
                              .path = SummaryPath::result(),
                              .other = SummaryPath::global(7),
                              .begin = PathAffine::ofConstant(-4),
                              .end = PathAffine::ofConstant(8),
                              .family = {},
                              .on = Outcome::NonNull};
  contract.establish(position);
  contract.establish({.kind = CheckedRequirementKind::Initialized,
                      .path = SummaryPath::param(0),
                      .other = {},
                      .begin = {},
                      .end = PathAffine::ofPath(SummaryPath::result()),
                      .family = {},
                      .on = Outcome::Positive});
  const auto encoded = printCheckedContract(contract, names);
  EXPECT_EQ(parseCheckedContract(encoded, resolve), contract);
  for (std::size_t length = 0; length < encoded.size(); ++length)
    EXPECT_FALSE(parseCheckedContract(encoded.substr(0, length), resolve));
  FunctionSummary summary;
  summary.checked = contract;
  const auto missing = remapGlobals(
      summary, [](std::uint32_t) { return std::optional<std::uint32_t>{}; });
  EXPECT_TRUE(missing.checked.limited);
  for (unsigned variant = 0; variant < 5; ++variant) {
    auto invalid = position;
    if (variant == 0)
      invalid.path = SummaryPath::param(0);
    if (variant == 1)
      invalid.other = SummaryPath::result();
    if (variant == 2)
      invalid.end = PathAffine::ofConstant(-5);
    if (variant == 3)
      invalid.begin = PathAffine::ofPath(SummaryPath::result());
    if (variant == 4)
      invalid.family = "ignored-field";
    CheckedContract bad;
    bad.computed = true;
    bad.establish(invalid);
    EXPECT_FALSE(
        parseCheckedContract(printCheckedContract(bad, names), resolve));
  }
  contract.requirements = contract.establishes;
  contract.establishes.clear();
  EXPECT_FALSE(
      parseCheckedContract(printCheckedContract(contract, names), resolve));
}

TEST(Traversal, ProgressContractsRetainPathsGuardsAndOutcomes) {
  const auto names = [](std::uint32_t) { return std::string("cursor"); };
  const auto resolve = [](std::string_view) { return std::optional(7U); };
  CheckedRequirement progress{.kind = CheckedRequirementKind::Progress,
                              .path = SummaryPath::param(0).deref(),
                              .other = SummaryPath::global(7),
                              .begin = {},
                              .end = PathAffine::ofConstant(-1),
                              .family = {},
                              .on = Outcome::Positive};
  CheckedContract contract;
  contract.computed = true;
  contract.establish(progress);
  EXPECT_EQ(
      parseCheckedContract(printCheckedContract(contract, names), resolve),
      contract);
  FunctionSummary summary;
  summary.checked = contract;
  const auto remapped =
      remapGlobals(summary, [](std::uint32_t) { return std::optional(9U); });
  EXPECT_EQ(remapped.checked.establishes.begin()->other,
            SummaryPath::global(9));
  for (unsigned variant = 0; variant < 8; ++variant) {
    auto invalid = progress;
    if (variant == 0)
      invalid.path = SummaryPath::result();
    if (variant == 1)
      invalid.other = SummaryPath::result();
    if (variant == 2)
      invalid.path = SummaryPath::param(0);
    if (variant == 3)
      invalid.other = SummaryPath::param(1);
    if (variant == 4)
      invalid.other = invalid.path;
    if (variant == 5)
      invalid.begin = PathAffine::ofConstant(1);
    if (variant == 6)
      invalid.end = PathAffine::ofPath(SummaryPath::param(2));
    if (variant == 7)
      invalid.family = "ignored";
    auto bad = contract;
    bad.establishes.clear();
    bad.establish(invalid);
    EXPECT_FALSE(
        parseCheckedContract(printCheckedContract(bad, names), resolve));
  }
}

TEST(Traversal, RelationDirectionsMatchConcreteIntegers) {
  const PlaceId x{0};
  const PlaceId y{1};
  for (const auto relation :
       {Relation::Less, Relation::LessEqual, Relation::Equal,
        Relation::GreaterEqual, Relation::Greater})
    for (int offset = -2; offset <= 2; ++offset) {
      DifferenceConstraints facts;
      facts.learn(x, {.relation = relation, .offset = offset}, y);
      for (int a = -4; a <= 4; ++a)
        for (int b = -4; b <= 4; ++b) {
          bool holds = false;
          switch (relation) {
          case Relation::Less:
            holds = a < b + offset;
            break;
          case Relation::LessEqual:
            holds = a <= b + offset;
            break;
          case Relation::Equal:
            holds = a == b + offset;
            break;
          case Relation::GreaterEqual:
            holds = a >= b + offset;
            break;
          case Relation::Greater:
            holds = a > b + offset;
            break;
          }
          if (!holds)
            continue;
          if (const auto bound = facts.bound(x, y))
            EXPECT_LE(a - b, *bound);
          if (const auto bound = facts.bound(y, x))
            EXPECT_LE(b - a, *bound);
        }
    }
}

TEST(Traversal, CheckedJoinsRetainAConservativeBoundOnDifferentOffsets) {
  const PlaceId current{0};
  const PlaceId entry{1};
  RelationTracker initial;
  RelationTracker advanced;
  initial.learn(current, Relation::Equal, entry);
  advanced.learn(current, Relation::Equal, entry, -1);
  auto ordinary = initial;
  ordinary.join(advanced);
  EXPECT_FALSE(ordinary.edgeBetween(current, entry));
  initial.join(advanced, true);
  ASSERT_TRUE(initial.edgeBetween(current, entry));
  EXPECT_EQ(initial.edgeBetween(current, entry)->relation, Relation::LessEqual);
  EXPECT_EQ(initial.edgeBetween(current, entry)->offset, 0);
}

TEST(Traversal, AcyclicScalarUnionsRetainFiniteAlternatives) {
  const PlaceId value{0};
  ScalarTracker a;
  ScalarTracker b;
  a.set(value, ValueFact::ofConstant(7));
  b.set(value, ValueFact::ofConstant(31));
  a.join(b, false);
  const auto fact = a.factOf(value);
  ASSERT_TRUE(fact);
  const IntegerType type{.width = 32, .isSigned = true};
  const auto range = fact->inType(type);
  EXPECT_EQ(range.minimum()->signedValue(), 7);
  EXPECT_EQ(range.maximum()->signedValue(), 31);
  EXPECT_TRUE(range.contains(IntegerValue::ofBits(type, 7)));
  EXPECT_TRUE(range.contains(IntegerValue::ofBits(type, 31)));
}

TEST(Traversal, PositionJoinsAndInvalidationRequireEveryPremise) {
  const PlaceId p{0};
  const PlaceId q{1};
  const PlaceId array{2};
  const PlaceId n{3};
  SafetyState state;
  state.positions[p] = {.storage = array,
                        .offset = Affine::ofPlace(n),
                        .extent = Affine::ofConstant(8),
                        .input = q};
  state.copyMemory(p, q);
  ASSERT_EQ(state.positions.size(), 2U);
  auto other = state;
  EXPECT_FALSE(state.join(other));
  other.positions[q].offset = Affine::ofConstant(1);
  EXPECT_TRUE(state.join(other));
  EXPECT_FALSE(state.positions.contains(q));
  state.forgetDependency(n);
  EXPECT_TRUE(state.positions.empty());
  state = other;
  state.forget(array);
  EXPECT_TRUE(state.positions.empty());
}

TEST(Traversal, SymbolicInitializationMergesOnlyExactAdjacentMustWrites) {
  const PlaceId storage{0};
  const PlaceId index{1};
  SafetyState state;
  const auto i = Affine::ofPlace(index);
  const auto next = *i.shifted(1);
  state.initialize(storage, {.begin = {}, .end = i});
  state.initialize(storage, {.begin = i, .end = next});
  ASSERT_EQ(state.memory.at(storage).size(), 1U);
  EXPECT_EQ(state.memory.at(storage).front().begin, Affine{});
  EXPECT_EQ(state.memory.at(storage).front().end, next);
  state.initialize(storage, {.begin = *i.shifted(2), .end = *i.shifted(3)});
  EXPECT_EQ(state.memory.at(storage).size(), 2U);
  auto skipped = state;
  skipped.memory.clear();
  state.join(skipped);
  EXPECT_TRUE(state.memory.empty() || state.memory.at(storage).empty());
}

TEST(Traversal, ConditionalMustWritesMergeWithoutFillingGuardHoles) {
  const PlaceId bytes{0};
  const PlaceId value{1};
  const IntegerType type{.width = 8, .isSigned = false};
  SafetyState state;
  for (const auto interval : {std::pair{1U, 3U}, std::pair{7U, 9U}}) {
    InitializedRange range{.begin = {}, .end = Affine::ofConstant(4)};
    range.when.require(value,
                       ValueFact::ofInteger(IntegerRange::between(
                           IntegerValue::ofBits(type, interval.first),
                           IntegerValue::ofBits(type, interval.second))));
    state.initialize(bytes, range);
  }
  ASSERT_EQ(state.memory.at(bytes).size(), 1U);
  const auto &when = state.memory.at(bytes).front().when;
  ASSERT_TRUE(when.conditions.contains(value));
  const auto represented = when.conditions.at(value).inType(type);
  for (unsigned input = 0; input < 256; ++input)
    EXPECT_EQ(represented.contains(IntegerValue::ofBits(type, input)),
              (input >= 1 && input <= 3) || (input >= 7 && input <= 9));
}

TEST(Traversal, DifferenceClosurePreservesDirectionAndZeroIdentity) {
  const PlaceId x{0};
  const PlaceId y{1};
  const PlaceId z{2};
  DifferenceConstraints facts;
  facts.constrain(x, y, -1);
  facts.constrain(y, z, 2);
  EXPECT_EQ(facts.bound(x, z), 1);
  EXPECT_FALSE(facts.bound(z, x));
  facts.constrain(z, {}, 8);
  EXPECT_EQ(facts.bound(x, {}), 9);
  EXPECT_FALSE(facts.bound({}, x));
}

TEST(Traversal, UpdatesAndJoinsEncloseConcreteStates) {
  const PlaceId x{0};
  const PlaceId y{1};
  const PlaceId z{2};
  for (int limit = -2; limit <= 2; ++limit) {
    DifferenceConstraints facts;
    facts.constrain(x, y, limit);
    facts.constrain(y, z, 1);
    auto updated = facts;
    updated.assign(x, x, 2);
    auto merged = facts;
    merged.join(updated);
    for (int a = -3; a <= 3; ++a)
      for (int b = -3; b <= 3; ++b)
        for (int c = -3; c <= 3; ++c) {
          if (a - b > limit || b - c > 1)
            continue;
          ASSERT_TRUE(facts.bound(x, z));
          EXPECT_LE(a - c, *facts.bound(x, z));
          ASSERT_TRUE(updated.bound(x, z));
          EXPECT_LE(a + 2 - c, *updated.bound(x, z));
          ASSERT_TRUE(merged.bound(x, z));
          EXPECT_LE(a - c, *merged.bound(x, z));
          EXPECT_LE(a + 2 - c, *merged.bound(x, z));
        }
  }
}

TEST(Traversal, AssignmentForgetsOldDestinationAndCopiesEqualities) {
  const PlaceId x{0};
  const PlaceId y{1};
  const PlaceId z{2};
  DifferenceConstraints facts;
  facts.assign(x, {}, 2);
  facts.assign(y, x, 3);
  facts.assign(x, z, 1);
  EXPECT_FALSE(facts.bound(y, {}));
  EXPECT_TRUE(facts.implies(x, z, 1));
  EXPECT_TRUE(facts.implies(z, x, -1));
  facts.forget(z);
  EXPECT_FALSE(facts.bound(x, z));
}

TEST(Traversal, MissingJoinPremisesAndNegativeCyclesSupplyNoProof) {
  const PlaceId x{0};
  const PlaceId y{1};
  DifferenceConstraints facts;
  facts.constrain(x, y, -1);
  auto joined = facts;
  EXPECT_TRUE(joined.join({}));
  EXPECT_FALSE(joined.implies(x, y, 100));
  facts.constrain(y, x, -1);
  EXPECT_FALSE(facts.bound(x, y));
}

TEST(Traversal, HostOverflowAndVariableExhaustionLoseProof) {
  DifferenceConstraints facts;
  facts.constrain(PlaceId{0}, PlaceId{1}, INT64_MIN);
  facts.constrain(PlaceId{1}, PlaceId{2}, -1);
  EXPECT_FALSE(facts.bound(PlaceId{0}, PlaceId{2}));
  for (unsigned i = 0; i <= MaxTraversalVariables; ++i)
    facts.constrain(PlaceId{i}, {}, 3);
  EXPECT_TRUE(facts.limited());
  DifferenceConstraints full;
  for (unsigned i = 0; i < MaxTraversalVariables; i += 2)
    EXPECT_TRUE(full.constrain(PlaceId{i}, PlaceId{i + 1}, 3));
  EXPECT_FALSE(full.limited());
  // An endpoint seen only on the right is still an existing variable.
  EXPECT_TRUE(full.constrain(PlaceId{63}, PlaceId{0}, 3));
  EXPECT_TRUE(full.constrain(PlaceId{62}, PlaceId{60}, 3));
  EXPECT_TRUE(full.constrain({}, PlaceId{63}, 3));
  EXPECT_FALSE(full.limited());
  EXPECT_FALSE(full.constrain(PlaceId{64}, {}, 3));
  EXPECT_TRUE(full.limited());
}

TEST(Traversal, PointerDifferenceMatchesIndependentSmallTargetEnumeration) {
  const IntegerType type{.width = 8, .isSigned = true};
  for (int a = -260; a <= 260; ++a)
    for (int b = -8; b <= 8; ++b)
      for (int size : {1, 2, 4}) {
        const int bytes = a - b;
        const bool fits =
            bytes % size == 0 && bytes / size >= -128 && bytes / size <= 127;
        const auto result = pointerDifference(a, b, size, type);
        ASSERT_EQ(result.has_value(), fits);
        if (result)
          EXPECT_EQ(result->signedValue(), bytes / size);
      }
  EXPECT_FALSE(pointerDifference(INT64_MAX, -1, 1, type));
  EXPECT_FALSE(pointerDifference(0, 0, 0, type));
  EXPECT_FALSE(pointerDifference(0, 0, 1, BooleanType));
}

TEST(Traversal, CheckedDifferenceJoinsRetainBothCursorBounds) {
  const PlaceId input{0};
  const PlaceId output{1};
  RelationTracker copied;
  RelationTracker skipped;
  copied.trackDifferences();
  skipped.trackDifferences();
  copied.learn(input, Relation::Equal, output, -1);
  skipped.learn(input, Relation::Equal, output);
  ASSERT_TRUE(copied.join(skipped, true));
  DifferenceConstraints bounds;
  for (const auto &[pair, edge] : copied.allBounds())
    bounds.learn(pair.first, edge, pair.second);
  EXPECT_TRUE(bounds.implies(input, output, 0));
  EXPECT_TRUE(bounds.implies(output, input, 1));
  EXPECT_FALSE(bounds.implies(input, output, -1));
  EXPECT_FALSE(bounds.implies(output, input, 0));
  EXPECT_FALSE(copied.join(skipped, true));
  copied.learn(input, Relation::GreaterEqual, output);
  EXPECT_EQ(copied.edgeBetween(input, output),
            (RelationEdge{.relation = Relation::Equal, .offset = 0}));
  copied.forget(output);
  EXPECT_TRUE(copied.allBounds().empty());
}

TEST(Traversal, WideningDropsChangingDifferencesAndKeepsStableOrder) {
  const PlaceId input{0};
  const PlaceId output{1};
  RelationTracker header;
  RelationTracker back;
  header.trackDifferences();
  back.trackDifferences();
  header.learn(input, Relation::Equal, output);
  back.learn(input, Relation::GreaterEqual, output);
  back.learn(input, Relation::LessEqual, output, 1);
  ASSERT_TRUE(header.join(back, true, true));
  EXPECT_EQ(header.edgeBetween(input, output),
            (RelationEdge{.relation = Relation::GreaterEqual, .offset = 0}));
  EXPECT_FALSE(header.join(back, true, true));
  back.forget(output);
  EXPECT_TRUE(header.join(back, true, true));
  EXPECT_TRUE(header.allBounds().empty());
}

TEST(Traversal, WideningUsesZeroOnceBeforeDroppingACrossedOrder) {
  const PlaceId a{0};
  const PlaceId b{1};
  for (const auto direction : {Relation::GreaterEqual, Relation::LessEqual}) {
    const int sign = direction == Relation::GreaterEqual ? 1 : -1;
    RelationTracker header;
    RelationTracker edge;
    header.trackDifferences();
    edge.trackDifferences();
    header.learn(a, direction, b, static_cast<std::int64_t>(sign) * 3);
    edge.learn(a, direction, b, static_cast<std::int64_t>(sign) * 2);
    EXPECT_TRUE(header.join(edge, true, true));
    EXPECT_EQ(header.edgeBetween(a, b),
              (RelationEdge{.relation = direction, .offset = 0}));
    EXPECT_FALSE(header.join(edge, true, true));
    edge.forget(a);
    edge.learn(a, direction, b, -sign);
    EXPECT_TRUE(header.join(edge, true, true));
    EXPECT_TRUE(header.allBounds().empty());
  }
}

TEST(Traversal, DifferenceHullAndWideningContainEveryConcreteInput) {
  const PlaceId a{0};
  const PlaceId b{1};
  const auto accepts = [](int x, int y, Relation relation, int offset) {
    switch (relation) {
    case Relation::Less:
      return x < y + offset;
    case Relation::LessEqual:
      return x <= y + offset;
    case Relation::Equal:
      return x == y + offset;
    case Relation::GreaterEqual:
      return x >= y + offset;
    case Relation::Greater:
      return x > y + offset;
    }
    return false;
  };
  for (const auto left : {Relation::Less, Relation::LessEqual, Relation::Equal,
                          Relation::GreaterEqual, Relation::Greater})
    for (const auto right :
         {Relation::Less, Relation::LessEqual, Relation::Equal,
          Relation::GreaterEqual, Relation::Greater})
      for (int first = -2; first <= 2; ++first)
        for (int second = -2; second <= 2; ++second)
          for (const bool widening : {false, true}) {
            RelationTracker result;
            RelationTracker incoming;
            result.trackDifferences();
            incoming.trackDifferences();
            result.learn(a, left, b, first);
            incoming.learn(a, right, b, second);
            result.join(incoming, true, widening);
            const auto bounds = result.allBounds();
            for (int x = -8; x <= 8; ++x)
              for (int y = -8; y <= 8; ++y)
                if (accepts(x, y, left, first) || accepts(x, y, right, second))
                  for (const auto &[pair, edge] : bounds) {
                    (void)pair;
                    ASSERT_TRUE(accepts(x, y, edge.relation,
                                        static_cast<int>(edge.offset)));
                  }
          }
}

TEST(Traversal, ClosureWorkExhaustionIsReportedAndSuppliesNoProof) {
  DifferenceConstraints facts;
  for (unsigned i = 0; i + 1 < MaxTraversalVariables; ++i) {
    facts.constrain(PlaceId{i}, PlaceId{i + 1}, 0);
    if (i + 2 < MaxTraversalVariables)
      facts.constrain(PlaceId{i}, PlaceId{i + 2}, 0);
  }
  EXPECT_FALSE(facts.limited());
  EXPECT_FALSE(facts.bound(PlaceId{0}, PlaceId{63}));
  EXPECT_TRUE(facts.limited());
}

} // namespace weavec::core
