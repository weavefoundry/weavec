//===- DataflowTraversalRelations.cpp - Checked relations (RFC 0021) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

#include "llvm/ADT/ScopeExit.h"

#include <limits>

using namespace clang;

namespace weavec::analysis {

std::optional<core::Affine>
FunctionDataflow::checkedTraversalSum(const NumericExpression &expression,
                                      const core::AnalysisState &state) {
  if (!state.safety || expression.type().isSigned)
    return std::nullopt;
  std::optional<core::PlaceId> length;
  std::optional<core::PlaceId> index;
  std::optional<core::PlaceId> added;
  std::int64_t constant = 0;
  bool valid = true;
  const auto input = [&](const NumericExpression &value) {
    const auto key = value.inputKey();
    return key && !numericExpressions.contains(*key) ? key : std::nullopt;
  };
  const std::function<void(const NumericExpression &)> flatten =
      [&](const NumericExpression &value) {
        if (!valid || value.type() != expression.type()) {
          valid = false;
          return;
        }
        if (const auto exact = value.constantValue()) {
          valid =
              exact->bits <= INT64_MAX &&
              !__builtin_add_overflow(
                  constant, static_cast<std::int64_t>(exact->bits), &constant);
          return;
        }
        const auto &node = value.all().back();
        const auto parts = value.operands();
        if (node.kind == core::IntegerNodeKind::Operation &&
            node.op == core::IntegerOp::Add) {
          for (const auto &part : parts)
            flatten(part);
          return;
        }
        if (node.kind == core::IntegerNodeKind::Operation &&
            node.op == core::IntegerOp::Subtract && parts.size() == 2 &&
            parts.back().constantValue()) {
          const auto count = parts.back().constantValue()->bits;
          valid =
              count <= INT64_MAX &&
              operationDoesNotOverflow(core::IntegerOp::Subtract, parts.front(),
                                       parts.back(), value.type(), state) &&
              !__builtin_sub_overflow(
                  constant, static_cast<std::int64_t>(count), &constant);
          if (valid)
            flatten(parts.front());
          return;
        }
        if (node.kind == core::IntegerNodeKind::Operation &&
            node.op == core::IntegerOp::Subtract && parts.size() == 2 &&
            !length) {
          length = input(parts.front());
          index = input(parts.back());
          valid = length && index &&
                  checkedAtMost(core::Affine::ofPlace(*index),
                                core::Affine::ofPlace(*length), state);
          return;
        }
        const auto key = input(value);
        if (!key || added)
          valid = false;
        else
          added = key;
      };
  flatten(expression);
  if (!valid || !length || !index ||
      !checkedAtMost(added ? core::Affine::ofPlace(*added) : core::Affine{},
                     core::Affine::ofPlace(*index), state))
    return std::nullopt;
  // With 0 <= output <= input <= length, output + (length-input) <= length.
  // This bound follows from represented values; it does not equate cursors.
  return core::Affine::ofPlace(*length, 1, constant);
}

static std::optional<core::Affine> traversalSumBound(
    const core::Affine &value,
    const std::map<core::PlaceId, core::IntegerExpression<core::PlaceId>>
        &expressions,
    const core::AnalysisState &state,
    const std::function<bool(const core::Affine &, const core::Affine &)>
        &atMost) {
  if (!value.place || value.scale <= 0)
    return std::nullopt;
  const auto stored = expressions.find(*value.place);
  if (stored == expressions.end())
    return std::nullopt;
  const auto &sum = stored->second;
  const auto &node = sum.all().back();
  if (node.kind != core::IntegerNodeKind::Operation ||
      node.op != core::IntegerOp::Add || node.type.isSigned)
    return std::nullopt;
  const auto operands = sum.operands();
  if (operands.size() != 2)
    return std::nullopt;
  const auto endpoint =
      [&](const auto &expression) -> std::optional<core::Affine> {
    if (const auto constant = expression.constantValue()) {
      const auto value = constant->signedValue();
      return value && *value >= 0
                 ? std::optional(core::Affine::ofConstant(*value))
                 : std::nullopt;
    }
    const auto input = expression.inputKey();
    return input && !expressions.contains(*input)
               ? std::optional(core::Affine::ofPlace(*input))
               : std::nullopt;
  };
  const auto same = [&](const auto &a, const auto &b) {
    if (a == b)
      return true;
    const auto x = a.inputKey();
    const auto y = b.inputKey();
    return a.type() == b.type() && x && y && !expressions.contains(*x) &&
           !expressions.contains(*y) &&
           state.relations.between(*x, *y) == core::Relation::Equal;
  };
  for (const auto &predicate : state.numericConditions.integers) {
    if (predicate.range)
      continue;
    auto count = predicate.lhs;
    auto remaining = predicate.rhs;
    auto relation = predicate.op;
    if (relation == core::IntegerOp::Greater ||
        relation == core::IntegerOp::GreaterEqual) {
      std::swap(count, remaining);
      relation = core::reverseComparison(relation);
    }
    const auto &root = remaining.all().back();
    if ((relation != core::IntegerOp::Less &&
         relation != core::IntegerOp::LessEqual) ||
        root.kind != core::IntegerNodeKind::Operation ||
        root.op != core::IntegerOp::Subtract ||
        remaining.type() != sum.type() || count.type() != sum.type())
      continue;
    const auto parts = remaining.operands();
    if (parts.size() != 2 || parts.front().type() != sum.type() ||
        parts.back().type() != sum.type())
      continue;
    const auto length = endpoint(parts.front());
    const auto index = endpoint(parts.back());
    if (!length || !index || !atMost(*index, *length) ||
        !((same(operands.front(), count) &&
           same(operands.back(), parts.back())) ||
          (same(operands.back(), count) &&
           same(operands.front(), parts.back()))))
      continue;
    const auto offset = relation == core::IntegerOp::Less ? -1 : 0;
    std::int64_t displacement = 0;
    if (__builtin_mul_overflow(static_cast<std::int64_t>(offset), value.scale,
                               &displacement) ||
        __builtin_add_overflow(displacement, value.constant, &displacement))
      continue;
    const auto scaled = length->times(value.scale);
    return scaled ? scaled->shifted(displacement) : std::nullopt;
  }
  return std::nullopt;
}

void FunctionDataflow::checkedDifferenceCondition(const Expr &lhs,
                                                  BinaryOperatorKind op,
                                                  const Expr &rhs, bool holds,
                                                  core::AnalysisState &state) {
  // RFC 0029: retain the coordinate tested by (size_t)(cursor-base), rather
  // than only the range known on this particular visit to the loop header.
  const Expr *value = lhs.IgnoreParens();
  std::vector<const CastExpr *> conversions;
  while (const auto *cast = dyn_cast<CastExpr>(value)) {
    if (conversions.size() == 12)
      return;
    conversions.push_back(cast);
    value = cast->getSubExpr()->IgnoreParens();
  }
  if (const auto *subtraction = dyn_cast<BinaryOperator>(value);
      subtraction && subtraction->getOpcode() == BO_Sub &&
      subtraction->getLHS()->getType()->isPointerType()) {
    if (lhs.HasSideEffects(context) || rhs.HasSideEffects(context) ||
        !subtraction->getLHS()->getType()->getPointeeType()->isCharType())
      return;
    auto range = checkedPointerRange(*subtraction, state);
    if (!range)
      return;
    for (const auto *cast : llvm::reverse(conversions)) {
      const auto type = integerTypeOf(cast->getType(), context);
      if (!type || !conversionPreserves(*range, *type))
        return;
      range = range->converted(*type);
    }
    const auto cursor = checkedMemory(*subtraction->getLHS(), {}, {}, state);
    const auto base = checkedMemory(*subtraction->getRHS(), {}, {}, state);
    auto limit = integerAffineOf(rhs, state);
    // Keep the live bound cell even when this case knows its current value.
    // Its relation survives ordinary CFG widening more precisely than a
    // succession of constant cursor hulls.
    if (const auto bound = builder.resolve(*rhs.IgnoreParenImpCasts()))
      if (const auto *decl =
              dyn_cast_or_null<ValueDecl>(builder.declFor(bound->place));
          decl && bound->element.isWhole() &&
          integerTypeOf(*decl, context) ==
              integerTypeOf(rhs.getType(), context))
        limit = core::Affine::ofPlace(bound->place);
    if (!cursor || !base || !limit || !base->begin.isConstant() ||
        !cursor->begin.place || cursor->begin.scale != 1 || limit->scale != 1)
      return;
    std::optional<core::Relation> relation;
    switch (op) {
    case BO_LT:
      relation = holds ? core::Relation::Less : core::Relation::GreaterEqual;
      break;
    case BO_LE:
      relation = holds ? core::Relation::LessEqual : core::Relation::Greater;
      break;
    case BO_GT:
      relation = holds ? core::Relation::Greater : core::Relation::LessEqual;
      break;
    case BO_GE:
      relation = holds ? core::Relation::GreaterEqual : core::Relation::Less;
      break;
    case BO_EQ:
    case BO_NE:
      if ((op == BO_EQ) == holds)
        relation = core::Relation::Equal;
      break;
    default:
      break;
    }
    std::int64_t offset = 0;
    if (!relation ||
        __builtin_add_overflow(limit->constant, base->begin.constant,
                               &offset) ||
        __builtin_sub_overflow(offset, cursor->begin.constant, &offset))
      return;
    const auto coordinate = *cursor->begin.place;
    if (limit->place) {
      state.relations.learn(coordinate, *relation, *limit->place, offset);
    } else {
      if (*relation == core::Relation::Less && offset != INT64_MIN)
        state.relations.learnAtMost(coordinate, offset - 1);
      if (*relation == core::Relation::Greater && offset != INT64_MAX)
        state.relations.learnAtLeast(coordinate, offset + 1);
      if (*relation == core::Relation::LessEqual ||
          *relation == core::Relation::Equal)
        state.relations.learnAtMost(coordinate, offset);
      if (*relation == core::Relation::GreaterEqual ||
          *relation == core::Relation::Equal)
        state.relations.learnAtLeast(coordinate, offset);
    }
    checkedSpanCountBounds(state);
    return;
  }
  const auto *difference = dyn_cast<BinaryOperator>(lhs.IgnoreParenImpCasts());
  const auto constant = integerConstant(rhs, context);
  if (!difference || difference->getOpcode() != BO_Sub || !constant ||
      difference->HasSideEffects(context) ||
      !difference->getType()->isUnsignedIntegerType() ||
      !ASTContext::hasSameUnqualifiedType(lhs.getType(), difference->getType()))
    return;
  const auto input = [&](const Expr &operand) -> std::optional<core::PlaceId> {
    const Expr *value = operand.IgnoreParens();
    while (const auto *cast = dyn_cast<CastExpr>(value)) {
      if (!preservesInteger(*cast, state))
        return std::nullopt;
      value = cast->getSubExpr()->IgnoreParens();
    }
    const auto place = builder.resolve(*value);
    return place && place->element.isWhole() ? std::optional(place->place)
                                             : std::nullopt;
  };
  const auto a = input(*difference->getLHS());
  const auto b = input(*difference->getRHS());
  if (!a || !b ||
      !checkedAtMost(core::Affine::ofPlace(*b), core::Affine::ofPlace(*a),
                     state))
    return;
  std::optional<core::Relation> relation;
  switch (op) {
  case BO_LT:
    relation = holds ? core::Relation::Less : core::Relation::GreaterEqual;
    break;
  case BO_LE:
    relation = holds ? core::Relation::LessEqual : core::Relation::Greater;
    break;
  case BO_GT:
    relation = holds ? core::Relation::Greater : core::Relation::LessEqual;
    break;
  case BO_GE:
    relation = holds ? core::Relation::GreaterEqual : core::Relation::Less;
    break;
  case BO_EQ:
    if (holds)
      relation = core::Relation::Equal;
    break;
  case BO_NE:
    if (!holds)
      relation = core::Relation::Equal;
    break;
  default:
    break;
  }
  if (relation)
    state.relations.learn(*a, *relation, *b, *constant);
}

core::Affine
FunctionDataflow::checkedStableAffine(const core::Affine &value,
                                      const core::AnalysisState &state) {
  if (!value.place)
    return value;
  for (const auto &[pair, stored] : state.relations.all()) {
    if (pair.first != *value.place && pair.second != *value.place)
      continue;
    const auto other = pair.first == *value.place ? pair.second : pair.first;
    if (other == *value.place ||
        (!stableSummaryPathOf(other) &&
         !numericSnapshotExpressions.contains(other) &&
         !std::ranges::any_of(checkedCoordinates, [&](const auto &entry) {
           return entry.second == other;
         })))
      continue;
    const auto edge =
        pair.first == *value.place ? std::optional(stored) : stored.flipped();
    std::int64_t offset = 0;
    if (edge && edge->relation == core::Relation::Equal &&
        !__builtin_mul_overflow(edge->offset, value.scale, &offset) &&
        !__builtin_add_overflow(offset, value.constant, &offset))
      return core::Affine::ofPlace(other, value.scale, offset);
  }
  return value;
}

std::vector<std::pair<core::PlaceId, core::Affine>>
FunctionDataflow::checkedJoinBoundaries(const core::AnalysisState &first,
                                        const core::AnalysisState &second) {
  std::vector<std::pair<core::PlaceId, core::Affine>> result;
  if (!first.safety || !second.safety)
    return result;
  std::size_t candidates = 0;
  std::optional<core::DifferenceConstraints> firstBounds;
  std::optional<core::DifferenceConstraints> secondBounds;
  for (const auto &[holder, coordinate] : checkedCoordinates) {
    const auto a = first.safety->positions.find(holder);
    const auto b = second.safety->positions.find(holder);
    if (a == first.safety->positions.end() ||
        b == second.safety->positions.end() || a->second != b->second ||
        a->second.offset != core::Affine::ofPlace(coordinate))
      continue;
    std::vector<core::Affine> endpoints;
    if (a->second.extent)
      endpoints.push_back(*a->second.extent);
    if (const auto found = first.safety->termination.find(a->second.storage);
        found != first.safety->termination.end())
      for (const auto &witness : found->second)
        endpoints.push_back(witness.zero);
    for (const auto &[otherHolder, position] : first.safety->positions) {
      if (otherHolder == holder || position.storage != a->second.storage ||
          position.offset.place == coordinate ||
          checkedCoordinates.contains(otherHolder))
        continue;
      const auto other = second.safety->positions.find(otherHolder);
      if (other != second.safety->positions.end() && other->second == position)
        endpoints.push_back(position.offset);
    }
    for (const auto &endpoint : endpoints) {
      if (++candidates > core::MaxTraversalIterations) {
        inferred.checked.limited = true;
        inferred.incomplete.insert(
            "traversal invariant candidate limit reached");
        return result;
      }
      if (endpoint.place == coordinate || endpoint.scale != 1 ||
          !checkedAtMost(a->second.offset, endpoint, first) ||
          !checkedAtMost(b->second.offset, endpoint, second))
        continue;
      if (endpoint.place)
        if (const auto expression = numericExpressions.find(*endpoint.place);
            expression != numericExpressions.end() &&
            expression->second.dependsOn(coordinate))
          continue;
      result.emplace_back(coordinate, endpoint);
    }
    for (const auto &[otherHolder, other] : checkedCoordinates) {
      if (coordinate == other)
        continue;
      const auto x = first.safety->positions.find(otherHolder);
      const auto y = second.safety->positions.find(otherHolder);
      if (x == first.safety->positions.end() ||
          y == second.safety->positions.end() || x->second != y->second ||
          x->second.offset != core::Affine::ofPlace(other))
        continue;
      if (++candidates > core::MaxTraversalIterations) {
        inferred.checked.limited = true;
        inferred.incomplete.insert(
            "traversal invariant candidate limit reached");
        return result;
      }
      if (!firstBounds) {
        firstBounds = checkedRelations(first);
        secondBounds = checkedRelations(second);
      }
      const auto left = firstBounds->bound(coordinate, other);
      const auto right = secondBounds->bound(coordinate, other);
      if (firstBounds->limited() || secondBounds->limited()) {
        inferred.checked.limited = true;
        inferred.incomplete.insert("traversal relational limit reached");
        continue;
      }
      if (left && right && *left <= 0 && *right <= 0)
        result.emplace_back(
            coordinate,
            core::Affine::ofPlace(other, 1, *left < 0 && *right < 0 ? -1 : 0));
    }
  }
  return result;
}

bool FunctionDataflow::checkedJoinPremises(core::AnalysisState &target,
                                           core::AnalysisState &incoming) {
  if (!target.safety || !incoming.safety)
    return false;
  const auto normalize = [&](core::AnalysisState &state) {
    bool changed = false;
    for (auto &[storage, ranges] : state.safety->memory) {
      (void)storage;
      const auto before = ranges;
      std::erase_if(
          ranges, [&](auto &range) { return !pruneGuard(range.when, state); });
      std::ranges::sort(ranges);
      ranges.erase(std::ranges::unique(ranges).begin(), ranges.end());
      changed |= before != ranges;
    }
    return changed;
  };
  const bool normalized = normalize(target);
  normalize(incoming);
  bool cursorPremises = false;
  std::size_t candidates = 0;
  std::optional<core::DifferenceConstraints> targetBounds;
  std::optional<core::DifferenceConstraints> incomingBounds;
  for (const auto &[holder, position] : target.safety->positions) {
    (void)position;
    const auto coordinate = checkedCoordinates.find(holder);
    if (coordinate == checkedCoordinates.end() ||
        !incoming.safety->positions.contains(holder))
      continue;
    for (const auto &[other, otherPosition] : target.safety->positions) {
      (void)otherPosition;
      const auto otherCoordinate = checkedCoordinates.find(other);
      if (!(holder < other) || otherCoordinate == checkedCoordinates.end() ||
          !incoming.safety->positions.contains(other))
        continue;
      if (++candidates > core::MaxTraversalIterations) {
        inferred.checked.limited = true;
        inferred.incomplete.insert(
            "traversal invariant candidate limit reached");
        break;
      }
      for (const bool reverse : {false, true}) {
        const auto a = reverse ? otherCoordinate->second : coordinate->second;
        const auto b = reverse ? coordinate->second : otherCoordinate->second;
        // Both edges must prove the numeric candidate independently, including
        // an edge where the order is implicit through a saved call input.
        // Offsets in different objects may be related; this establishes no
        // common pointer provenance (RFC 0029).
        if (!targetBounds) {
          targetBounds = checkedRelations(target);
          incomingBounds = checkedRelations(incoming);
        }
        const auto left = targetBounds->bound(a, b);
        const auto right = incomingBounds->bound(a, b);
        if (targetBounds->limited() || incomingBounds->limited()) {
          inferred.checked.limited = true;
          inferred.incomplete.insert("traversal relational limit reached");
          continue;
        }
        if (!left || !right)
          continue;
        // Do not tighten an established order to an incidental first-trip
        // distance: widening that distance can erase a strict loop guard.
        const auto displacement = std::max({*left, *right, std::int64_t{0}});
        const auto previous = target.relations;
        target.relations.learn(a, core::Relation::LessEqual, b, displacement);
        incoming.relations.learn(a, core::Relation::LessEqual, b, displacement);
        cursorPremises |= previous != target.relations;
      }
    }
  }
  // Complete the representations with facts already true on that edge.
  // In particular [0,i) is empty on an entry with i=0. No candidate is
  // assumed: a skipped store on any back edge removes it at the normal join.
  const auto complete = [&](core::AnalysisState &to,
                            const core::AnalysisState &from) {
    bool changed = false;
    for (const auto &[storage, ranges] : from.safety->memory) {
      for (const auto &range : ranges) {
        if (range.source || !range.when.trivial())
          continue;
        const auto found = to.safety->memory.find(storage);
        if (found != to.safety->memory.end() &&
            std::ranges::find(found->second, range) != found->second.end())
          continue;
        if (++candidates > core::MaxTraversalIterations) {
          inferred.checked.limited = true;
          inferred.incomplete.insert(
              "traversal invariant candidate limit reached");
          break;
        }
        if (!checkedAtMost(range.begin, range.end, to) ||
            !checkedAtMost(range.end, range.begin, to))
          continue;
        to.safety->initialize(storage, range);
        changed = true;
      }
    }
    for (const bool upper : {false, true}) {
      const auto &bounds =
          upper ? from.relations.allAtMost() : from.relations.allAtLeast();
      for (const auto &[place, bound] : bounds) {
        if (snapshotPlaces.contains(place) ||
            (upper ? to.relations.atMost(place) : to.relations.atLeast(place)))
          continue;
        if (++candidates > core::MaxTraversalIterations) {
          inferred.checked.limited = true;
          inferred.incomplete.insert(
              "traversal invariant candidate limit reached");
          break;
        }
        const auto value = core::Affine::ofPlace(place);
        const auto limit = core::Affine::ofConstant(bound);
        if (!checkedAtMost(upper ? value : limit, upper ? limit : value, to))
          continue;
        if (upper)
          to.relations.learnAtMost(place, bound);
        else
          to.relations.learnAtLeast(place, bound);
        changed = true;
      }
    }
    for (const auto &[pair, original] : from.relations.allBounds()) {
      if (snapshotPlaces.contains(pair.first) ||
          snapshotPlaces.contains(pair.second) ||
          to.relations.edgeBetween(pair.first, pair.second) == original)
        continue;
      // Equality comprises two bounds. An incoming edge can establish one
      // half (output <= input after skipping) without establishing equality.
      std::vector<core::RelationEdge> edges{original};
      if (original.relation == core::Relation::Equal)
        edges = {
            {.relation = core::Relation::LessEqual, .offset = original.offset},
            {.relation = core::Relation::GreaterEqual,
             .offset = original.offset}};
      for (const auto &edge : edges) {
        if (to.relations.edgeBetween(pair.first, pair.second) == edge)
          continue;
        if (++candidates > core::MaxTraversalIterations) {
          inferred.checked.limited = true;
          inferred.incomplete.insert(
              "traversal invariant candidate limit reached");
          break;
        }
        const auto lhs = core::Affine::ofPlace(pair.first);
        const auto rhs =
            core::Affine::ofPlace(pair.second).shifted(edge.offset);
        if (!rhs)
          continue;
        const auto less = rhs->shifted(-1);
        const auto greater = lhs.shifted(-1);
        bool proved = false;
        switch (edge.relation) {
        case core::Relation::Less:
          proved = less && checkedAtMost(lhs, *less, to);
          break;
        case core::Relation::LessEqual:
          proved = checkedAtMost(lhs, *rhs, to);
          break;
        case core::Relation::Equal:
          proved = checkedAtMost(lhs, *rhs, to) && checkedAtMost(*rhs, lhs, to);
          break;
        case core::Relation::GreaterEqual:
          proved = checkedAtMost(*rhs, lhs, to);
          break;
        case core::Relation::Greater:
          proved = greater && checkedAtMost(*rhs, *greater, to);
          break;
        }
        if (proved) {
          const auto previous = to.relations;
          to.relations.learn(pair.first, edge.relation, pair.second,
                             edge.offset);
          changed |= previous != to.relations;
        }
      }
    }
    return changed;
  };
  const bool changed = complete(target, incoming);
  complete(incoming, target);
  return changed || normalized || cursorPremises;
}

core::DifferenceConstraints
FunctionDataflow::checkedRelations(const core::AnalysisState &state) {
  core::DifferenceConstraints result;
  std::set<core::PlaceId> variables;
  for (const auto &[pair, edge] : state.relations.allBounds()) {
    result.learn(pair.first, edge, pair.second);
    variables.insert(pair.first);
    variables.insert(pair.second);
  }
  for (const auto place : variables) {
    const auto [lower, upper] = integerBounds(place, state);
    if (upper)
      result.constrain(place, {}, *upper);
    if (lower && *lower != std::numeric_limits<std::int64_t>::min())
      result.constrain({}, place, -*lower);
  }
  for (const auto &[place, limit] : state.relations.allAtMost())
    result.constrain(place, {}, limit);
  for (const auto &[place, limit] : state.relations.allAtLeast())
    if (limit != std::numeric_limits<std::int64_t>::min())
      result.constrain({}, place, -limit);
  // Conditional contracts can supply typed comparisons without a source
  // comparison statement. Only direct values in the same C type establish
  // mathematical difference edges; conversions/operations need other proofs.
  for (const auto &predicate : state.numericConditions.integers) {
    const auto a = predicate.lhs.inputKey();
    if (predicate.range) {
      if (!a || predicate.range->empty())
        continue;
      if (const auto upper = predicate.range->maximum()->signedValue())
        result.constrain(a, {}, *upper);
      if (const auto lower = predicate.range->minimum()->signedValue();
          lower && *lower != std::numeric_limits<std::int64_t>::min())
        result.constrain({}, a, -*lower);
      continue;
    }
    if (predicate.lhs.type() != predicate.rhs.type())
      continue;
    const auto b = predicate.rhs.inputKey();
    const auto ac = predicate.lhs.constantValue();
    const auto bc = predicate.rhs.constantValue();
    if ((!a && (!ac || !ac->signedValue())) ||
        (!b && (!bc || !bc->signedValue())))
      continue;
    std::int64_t shift = 0;
    if (__builtin_sub_overflow(bc ? *bc->signedValue() : 0,
                               ac ? *ac->signedValue() : 0, &shift))
      continue;
    auto upper = shift;
    if (predicate.op == core::IntegerOp::Less &&
        __builtin_sub_overflow(shift, std::int64_t{1}, &upper))
      continue;
    if (predicate.op == core::IntegerOp::Less ||
        predicate.op == core::IntegerOp::LessEqual ||
        predicate.op == core::IntegerOp::Equal)
      result.constrain(a, b, upper);
    std::int64_t lower = 0;
    if (__builtin_sub_overflow(std::int64_t{0}, shift, &lower) ||
        (predicate.op == core::IntegerOp::Greater &&
         __builtin_sub_overflow(lower, std::int64_t{1}, &lower)))
      continue;
    if (predicate.op == core::IntegerOp::Greater ||
        predicate.op == core::IntegerOp::GreaterEqual ||
        predicate.op == core::IntegerOp::Equal)
      result.constrain(b, a, lower);
  }
  // A remaining-length test is a difference constraint only when the C
  // subtraction cannot wrap. Its own comparison must not prove that premise.
  for (const auto &predicate : state.numericConditions.integers) {
    if (predicate.range)
      continue;
    auto difference = predicate.lhs;
    auto compared = predicate.rhs;
    auto operation = predicate.op;
    if (difference.all().back().op != core::IntegerOp::Subtract &&
        compared.all().back().kind == core::IntegerNodeKind::Operation &&
        compared.all().back().op == core::IntegerOp::Subtract) {
      std::swap(difference, compared);
      operation = core::reverseComparison(operation);
    }
    const auto &root = difference.all().back();
    const auto evaluated =
        compared.evaluate([&](core::PlaceId place, core::IntegerType type) {
          return integerRangeAt(place, type, state);
        });
    if (root.kind != core::IntegerNodeKind::Operation ||
        root.op != core::IntegerOp::Subtract || evaluated.mayBeInvalid ||
        evaluated.values.empty() || root.type.isSigned ||
        compared.type() != root.type)
      continue;
    const auto operands = difference.operands();
    if (operands.size() != 2 || operands.front().type() != root.type ||
        operands.back().type() != root.type)
      continue;
    const auto endpoint =
        [&](NumericExpression value) -> std::optional<core::Affine> {
      while (value.all().back().kind == core::IntegerNodeKind::Convert) {
        const auto operand = value.operands().front();
        const auto range =
            operand.evaluate([&](core::PlaceId place, core::IntegerType type) {
              return integerRangeAt(place, type, state);
            });
        if (range.mayBeInvalid ||
            !conversionPreserves(range.values, value.type()))
          return std::nullopt;
        value = operand;
      }
      if (const auto input = value.inputKey())
        return core::Affine::ofPlace(*input);
      if (const auto constant = value.constantValue())
        if (const auto number = constant->signedValue())
          return core::Affine::ofConstant(*number);
      return std::nullopt;
    };
    const auto a = endpoint(operands.front());
    const auto b = endpoint(operands.back());
    std::int64_t displacement = 0;
    if (!a || !b ||
        __builtin_sub_overflow(a->constant, b->constant, &displacement) ||
        !result.implies(b->place, a->place, displacement))
      continue;
    const auto lower = evaluated.values.minimum()->signedValue();
    const auto upper = evaluated.values.maximum()->signedValue();
    if (lower && (operation == core::IntegerOp::Greater ||
                  operation == core::IntegerOp::GreaterEqual ||
                  operation == core::IntegerOp::Equal)) {
      std::int64_t bound = 0;
      if (!__builtin_sub_overflow(displacement, *lower, &bound) &&
          (operation != core::IntegerOp::Greater ||
           !__builtin_sub_overflow(bound, std::int64_t{1}, &bound)))
        result.constrain(b->place, a->place, bound);
    }
    if (upper && (operation == core::IntegerOp::Less ||
                  operation == core::IntegerOp::LessEqual ||
                  operation == core::IntegerOp::Equal)) {
      std::int64_t bound = 0;
      if (!__builtin_sub_overflow(*upper, displacement, &bound) &&
          (operation != core::IntegerOp::Less ||
           !__builtin_sub_overflow(bound, std::int64_t{1}, &bound)))
        result.constrain(a->place, b->place, bound);
    }
  }
  inferred.checked.limited |= result.limited();
  if (result.limited())
    inferred.incomplete.insert("traversal relational limit reached");
  return result;
}

bool FunctionDataflow::checkedAtMost(const core::Affine &lhs,
                                     const core::Affine &rhs,
                                     const core::AnalysisState &state) {
  const auto a = foldAffine(lhs, state);
  const auto b = foldAffine(rhs, state);
  if (a.place == b.place && a.scale == b.scale)
    return a.constant <= b.constant;
  const auto limit = [&](const core::Affine &value,
                         bool upper) -> std::optional<std::int64_t> {
    if (!value.place)
      return value.constant;
    const auto bounds = integerBounds(*value.place, state);
    auto point = upper == (value.scale >= 0) ? bounds.second : bounds.first;
    if (!point || __builtin_mul_overflow(*point, value.scale, &*point) ||
        __builtin_add_overflow(*point, value.constant, &*point))
      return std::nullopt;
    return point;
  };
  const auto upper = limit(a, true);
  const auto lower = limit(b, false);
  if (upper && lower && *upper <= *lower)
    return true;
  if (a.place && a.scale > 0)
    if (const auto expression = numericExpressions.find(*a.place);
        expression != numericExpressions.end())
      if (const auto sum = checkedTraversalSum(expression->second, state)) {
        const auto scaled = sum->times(a.scale);
        const auto shifted =
            scaled ? scaled->shifted(a.constant) : std::nullopt;
        if (shifted && checkedAtMost(*shifted, b, state))
          return true;
      }
  if (const auto sum = traversalSumBound(
          a, numericExpressions, state,
          [&](const core::Affine &index, const core::Affine &length) {
            return checkedAtMost(index, length, state);
          });
      sum && checkedAtMost(*sum, b, state))
    return true;
  if ((a.place && b.place && a.scale != b.scale) || (a.place && a.scale <= 0) ||
      (b.place && b.scale <= 0))
    return false;
  const auto scale = a.place ? a.scale : b.scale;
  std::int64_t shift = 0;
  if (__builtin_sub_overflow(b.constant, a.constant, &shift))
    return false;
  // Floor division is needed for negative byte displacements.
  auto bound = shift / scale;
  if (shift % scale < 0)
    --bound;
  const auto relations = checkedRelations(state);
  const bool proved = relations.implies(a.place, b.place, bound);
  inferred.checked.limited |= relations.limited();
  if (relations.limited())
    inferred.incomplete.insert("traversal relational limit reached");
  if (proved || bufferObjects.empty() || provingBufferBound)
    return proved;
  // RFC 0026: compare represented nonwrapping sums with a min/max growth
  // choice. A maximum supplies either operand's lower bound on both arms.
  // Recursive arithmetic premises use the base relation solver, preventing a
  // sum from proving its own no-overflow condition.
  provingBufferBound = true;
  llvm::scope_exit reset([&] { provingBufferBound = false; });
  const auto expression =
      [&](const core::Affine &value) -> std::optional<NumericExpression> {
    if (!value.place) {
      if (value.constant < 0)
        return std::nullopt;
      return NumericExpression::constant(core::IntegerValue::ofBits(
          {.width = 64, .isSigned = false},
          static_cast<std::uint64_t>(value.constant)));
    }
    auto found = numericExpressions.find(*value.place);
    std::optional<NumericExpression> result;
    if (found != numericExpressions.end())
      result = found->second;
    else if (const auto stored = state.numericValues.find(*value.place);
             stored != state.numericValues.end())
      result = stored->second;
    else if (const auto *decl =
                 dyn_cast_or_null<ValueDecl>(builder.declFor(*value.place)))
      if (const auto type = integerTypeOf(*decl, context))
        result = NumericExpression::input(*value.place, *type);
    if (result && result->inputKey())
      for (const auto &[pair, edge] : state.relations.all()) {
        if (edge.relation != core::Relation::Equal || edge.offset != 0 ||
            (pair.first != *value.place && pair.second != *value.place))
          continue;
        const auto other =
            pair.first == *value.place ? pair.second : pair.first;
        if (const auto stored = state.numericValues.find(other);
            stored != state.numericValues.end()) {
          result = stored->second.converted(result->type());
          break;
        }
      }
    if (!result || result->type().isSigned || value.scale != 1)
      return std::nullopt;
    for (unsigned depth = 0; depth < 8; ++depth) {
      const auto expanded = result->substitute<core::PlaceId>(
          [&](core::PlaceId key,
              core::IntegerType type) -> std::optional<NumericExpression> {
            // A represented entry case can fix an operand (for example
            // append_bytes(..., 1)). Use its target integer value inside
            // the expression as well as when the whole bound is constant.
            if (const auto fact = state.scalars.factOf(key))
              if (const auto exact = fact->inType(type).constant())
                return NumericExpression::constant(*exact);
            const auto stored = state.numericValues.find(key);
            if (stored != state.numericValues.end() &&
                !stored->second.dependsOn(key))
              return stored->second.converted(type);
            return NumericExpression::input(key, type);
          });
      if (!expanded)
        return std::nullopt;
      if (*expanded == *result)
        break;
      result = expanded;
    }
    if (value.constant != 0) {
      const auto magnitude =
          value.constant > 0
              ? static_cast<std::uint64_t>(value.constant)
              : std::uint64_t{0} - static_cast<std::uint64_t>(value.constant);
      const auto amount = NumericExpression::constant(
          core::IntegerValue::ofBits(result->type(), magnitude));
      const auto op =
          value.constant > 0 ? core::IntegerOp::Add : core::IntegerOp::Subtract;
      if (!operationDoesNotOverflow(op, *result, amount, result->type(), state))
        return std::nullopt;
      result = NumericExpression::operation(op, *result, amount);
    }
    return result;
  };
  const auto left = expression(a);
  const auto right = expression(b);
  if (!left || !right || left->type() != right->type())
    return false;
  const auto compare = [&](auto &&self, const NumericExpression &x,
                           const NumericExpression &y, unsigned depth) -> bool {
    if (depth > 8 || x.type() != y.type())
      return false;
    if (x == y)
      return true;
    const auto &xr = x.all().back();
    const auto &yr = y.all().back();
    const auto xp = x.operands();
    const auto yp = y.operands();
    if (yr.kind == core::IntegerNodeKind::Operation &&
        yr.op == core::IntegerOp::Maximum && yp.size() == 2)
      return self(self, x, yp.front(), depth + 1) ||
             self(self, x, yp.back(), depth + 1);
    if (xr.kind == core::IntegerNodeKind::Operation &&
        xr.op == core::IntegerOp::Minimum && xp.size() == 2)
      return self(self, xp.front(), y, depth + 1) ||
             self(self, xp.back(), y, depth + 1);
    std::vector<NumericExpression> xs;
    std::vector<NumericExpression> ys;
    std::uint64_t xc = 0;
    std::uint64_t yc = 0;
    const auto flatten = [&](auto &&walk, const NumericExpression &part,
                             std::vector<NumericExpression> &terms,
                             std::uint64_t &constant) -> bool {
      if (const auto exact = part.constantValue())
        return !__builtin_add_overflow(constant, exact->bits, &constant);
      const auto &root = part.all().back();
      const auto parts = part.operands();
      if (root.kind == core::IntegerNodeKind::Operation &&
          root.op == core::IntegerOp::Add && parts.size() == 2) {
        if (!operationDoesNotOverflow(root.op, parts.front(), parts.back(),
                                      root.type, state))
          return false;
        return walk(walk, parts.front(), terms, constant) &&
               walk(walk, parts.back(), terms, constant);
      }
      terms.push_back(part);
      return true;
    };
    if (!flatten(flatten, x, xs, xc) || !flatten(flatten, y, ys, yc))
      return false;
    std::ranges::sort(xs);
    std::ranges::sort(ys);
    // Every remaining unsigned summand is nonnegative. Flattening above
    // already checked that each addition has its mathematical value.
    return xc <= yc && std::ranges::includes(ys, xs);
  };
  return compare(compare, *left, *right, 0);
}

std::optional<core::PathAffine>
FunctionDataflow::checkedRequirementEnvelope(const core::Affine &need,
                                             const core::AnalysisState &state) {
  if (!need.place || need.scale <= 0)
    return std::nullopt;
  if (const auto expression = numericExpressions.find(*need.place);
      expression != numericExpressions.end())
    if (const auto sum = checkedTraversalSum(expression->second, state)) {
      const auto scaled = sum->times(need.scale);
      const auto shifted =
          scaled ? scaled->shifted(need.constant) : std::nullopt;
      if (shifted)
        if (const auto projected = summaryAffineOf(shifted))
          return projected;
    }
  if (const auto sum = traversalSumBound(
          need, numericExpressions, state,
          [&](const core::Affine &index, const core::Affine &length) {
            return checkedAtMost(index, length, state);
          }))
    if (const auto projected = summaryAffineOf(sum))
      return projected;
  const auto relations = checkedRelations(state);
  std::set<core::PlaceId> candidates;
  for (const auto &[pair, edge] : state.relations.all()) {
    (void)edge;
    candidates.insert(pair.first);
    candidates.insert(pair.second);
  }
  for (const auto candidate : candidates) {
    if (candidate == *need.place)
      continue;
    const auto distance = relations.bound(need.place, candidate);
    inferred.checked.limited |= relations.limited();
    if (relations.limited())
      inferred.incomplete.insert("traversal relational limit reached");
    if (!distance)
      continue;
    std::int64_t shift = 0;
    if (__builtin_mul_overflow(*distance, need.scale, &shift) ||
        __builtin_add_overflow(shift, need.constant, &shift))
      continue;
    const core::Affine bound{
        .place = candidate, .scale = need.scale, .constant = shift};
    if (const auto projected = summaryAffineOf(bound))
      return projected;
  }
  // A type maximum is not a useful inferred traversal capacity. Project a
  // constant only from an explicit boundary or an actual narrowed value fact.
  auto upper = state.relations.atMost(*need.place);
  std::optional<core::IntegerType> type;
  if (const auto *decl =
          dyn_cast_or_null<ValueDecl>(builder.declFor(*need.place)))
    type = integerTypeOf(*decl, context);
  if (const auto fact = state.scalars.factOf(*need.place)) {
    if (!type && fact->integer)
      type = fact->integer->type;
    if (type) {
      const auto range = fact->inType(*type);
      const auto maximum = range.maximum();
      const auto bound = maximum ? maximum->signedValue() : std::nullopt;
      const auto typeMaximum = core::IntegerRange::full(*type).maximum();
      if (bound && (fact->constant ||
                    (typeMaximum && maximum->bits != typeMaximum->bits)))
        upper = upper ? std::min(*upper, *bound) : bound;
    }
  }
  std::int64_t last = 0;
  if (upper && !__builtin_mul_overflow(*upper, need.scale, &last) &&
      !__builtin_add_overflow(last, need.constant, &last) && last >= 0)
    return core::PathAffine::ofConstant(last);
  return std::nullopt;
}

} // namespace weavec::analysis
