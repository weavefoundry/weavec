//===- DataflowIntervalProofs.cpp - Byte sums and relational bounds -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Relational proofs over integer places for extent requirements whose
// accessed interval starts inside the object (RFC 0011, *Bounds checks*):
// the end of `[start, start + need)` as one nonwrapping byte sum, and
// `a <= b` from the path's relations and numeric conditions as a difference
// constraint system (RFC 0017).
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

#include <limits>

using namespace clang;

namespace weavec::analysis {

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

core::DifferenceConstraints
FunctionDataflow::differenceConstraints(const core::AnalysisState &state) {
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
  if (result.limited())
    inferred.incomplete.insert("traversal relational limit reached");
  return result;
}

bool FunctionDataflow::provedAtMost(const core::Affine &lhs,
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
  if (const auto sum = traversalSumBound(
          a, numericExpressions, state,
          [&](const core::Affine &index, const core::Affine &length) {
            return provedAtMost(index, length, state);
          });
      sum && provedAtMost(*sum, b, state))
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
  const auto relations = differenceConstraints(state);
  if (relations.limited())
    inferred.incomplete.insert("traversal relational limit reached");
  return relations.implies(a.place, b.place, bound);
}

std::optional<FunctionDataflow::NumericExpression>
FunctionDataflow::byteExpression(const core::Affine &value,
                                 const core::AnalysisState &state) {
  const core::IntegerType bytes{.width = 64, .isSigned = false};
  if (value.scale < 0 || (!value.place && value.constant < 0))
    return std::nullopt;
  if (!value.place)
    return NumericExpression::constant(core::IntegerValue::ofBits(
        bytes, static_cast<std::uint64_t>(value.constant)));
  std::optional<NumericExpression> expression;
  if (const auto symbolic = numericExpressions.find(*value.place);
      symbolic != numericExpressions.end()) {
    expression = symbolic->second;
  } else if (const auto stored = state.numericValues.find(*value.place);
             stored != state.numericValues.end()) {
    expression = stored->second;
  } else {
    const auto *decl =
        dyn_cast_or_null<ValueDecl>(builder.declFor(*value.place));
    auto type = decl ? integerTypeOf(*decl, context) : std::nullopt;
    if (!type)
      if (const auto fact = state.scalars.factOf(*value.place);
          fact && fact->integer)
        type = fact->integer->type;
    if (type)
      expression = NumericExpression::input(*value.place, *type);
  }
  if (!expression)
    return std::nullopt;
  const auto evaluated = evaluateNumericExpression(*expression, state);
  if (evaluated.mayBeInvalid || evaluated.values.empty() ||
      evaluated.values.minimum()->negative())
    return std::nullopt;
  expression = expression->converted(bytes);
  if (value.scale != 1) {
    const auto factor = NumericExpression::constant(core::IntegerValue::ofBits(
        bytes, static_cast<std::uint64_t>(value.scale)));
    if (!operationDoesNotOverflow(core::IntegerOp::Multiply, *expression,
                                  factor, bytes, state))
      return std::nullopt;
    expression = NumericExpression::operation(core::IntegerOp::Multiply,
                                              *expression, factor);
  }
  if (expression && value.constant != 0) {
    const bool positive = value.constant >= 0;
    const auto magnitude =
        positive
            ? static_cast<std::uint64_t>(value.constant)
            : std::uint64_t{0} - static_cast<std::uint64_t>(value.constant);
    const auto shift = NumericExpression::constant(
        core::IntegerValue::ofBits(bytes, magnitude));
    const auto operation =
        positive ? core::IntegerOp::Add : core::IntegerOp::Subtract;
    if (!operationDoesNotOverflow(operation, *expression, shift, bytes, state))
      return std::nullopt;
    expression = NumericExpression::operation(operation, *expression, shift);
  }
  return expression;
}

std::optional<core::Affine>
FunctionDataflow::byteSum(const core::Affine &lhs, const core::Affine &rhs,
                          const core::AnalysisState &state) {
  if (const auto linear = sumOf(lhs, rhs))
    return linear;
  const auto cancel =
      [&](const core::Affine &cursor,
          const core::Affine &remaining) -> std::optional<core::Affine> {
    if (!cursor.place || !remaining.place || cursor.scale != 1 ||
        remaining.scale != 1)
      return std::nullopt;
    const auto found = numericExpressions.find(*remaining.place);
    if (found == numericExpressions.end())
      return std::nullopt;
    const auto &root = found->second.all().back();
    const auto parts = found->second.operands();
    if (root.kind != core::IntegerNodeKind::Operation ||
        root.op != core::IntegerOp::Subtract || root.type.isSigned ||
        parts.size() != 2 || !parts.front().inputKey() ||
        parts.back().inputKey() != cursor.place ||
        !provedAtMost(core::Affine::ofPlace(*cursor.place),
                      core::Affine::ofPlace(*parts.front().inputKey()), state))
      return std::nullopt;
    std::int64_t constant = 0;
    if (__builtin_add_overflow(cursor.constant, remaining.constant, &constant))
      return std::nullopt;
    return core::Affine::ofPlace(*parts.front().inputKey(), 1, constant);
  };
  if (const auto exact = cancel(lhs, rhs))
    return exact;
  if (const auto exact = cancel(rhs, lhs))
    return exact;
  // Byte endpoints carry mathematical displacements separately from their
  // evaluated C values. Keep that normal form when two symbolic bases are
  // combined, so a strict bound on a+b also covers the endpoint a+b+1.
  std::int64_t displacement = 0;
  if (__builtin_add_overflow(lhs.constant, rhs.constant, &displacement))
    return std::nullopt;
  auto left = lhs;
  auto right = rhs;
  left.constant = 0;
  right.constant = 0;
  const auto a = byteExpression(left, state);
  const auto b = byteExpression(right, state);
  if (!a || !b)
    return std::nullopt;
  const auto sum = NumericExpression::operation(core::IntegerOp::Add, *a, *b);
  if (!sum)
    return std::nullopt;
  if (!operationDoesNotOverflow(core::IntegerOp::Add, *a, *b, a->type(), state))
    return std::nullopt;
  auto saved = expressionPlaces.find(*sum);
  if (saved == expressionPlaces.end()) {
    const auto place = places.create("checked byte sum");
    saved = expressionPlaces.emplace(*sum, place).first;
    numericExpressions.emplace(place, *sum);
  }
  return core::Affine::ofPlace(saved->second, 1, displacement);
}

} // namespace weavec::analysis
