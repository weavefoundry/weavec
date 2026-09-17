//===- DataflowCursors.cpp - Stable byte cursor values (RFC 0021) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

using namespace clang;

namespace weavec::analysis {

static constexpr core::IntegerType CursorType{.width = 64, .isSigned = false};

bool FunctionDataflow::checkedPointerComparable(const BinaryOperator &expr,
                                                core::AnalysisState &state) {
  const auto aType = expr.getLHS()->getType();
  const auto bType = expr.getRHS()->getType();
  if (!aType->isPointerType() || !bType->isPointerType() ||
      !ASTContext::hasSameUnqualifiedType(aType->getPointeeType(),
                                          bType->getPointeeType()))
    return false;
  const auto a = checkedMemory(*expr.getLHS(), {}, {}, state);
  const auto b = checkedMemory(*expr.getRHS(), {}, {}, state);
  if (!a || !b || a->storage != b->storage)
    return false;
  const auto position = [&](const CheckedMemory &memory) {
    const bool live = checkedValid(memory, state) ||
                      checkedRequire(core::CheckedRequirementKind::Valid,
                                     memory, expr, state);
    const bool bounds =
        (memory.extent &&
         checkedInterval(memory.begin, memory.end, *memory.extent, state)) ||
        checkedRequire(core::CheckedRequirementKind::Extent, memory, expr,
                       state);
    return live && bounds;
  };
  if (!position(*a) || !position(*b))
    return false;
  if (expr.getOpcode() == BO_Sub && aType->getPointeeType()->isCharType() &&
      a->inputPlace && bufferFact(*a->inputPlace, state) && a->extent &&
      a->extent == b->extent && a->extent->place && a->extent->scale == 1 &&
      a->extent->constant == 0) {
    const auto type = integerTypeOf(expr.getType(), context);
    const auto maximum =
        type ? core::IntegerRange::full(*type).maximum() : std::nullopt;
    const auto limit = maximum ? maximum->signedValue() : std::nullopt;
    if (limit &&
        !checkedAtMost(*a->extent, core::Affine::ofConstant(*limit), state))
      if (const auto extent = summaryAffineOf(a->extent)) {
        // RFC 0029: an explicit sufficient entry bound proves ptrdiff_t
        // representability, independently of provenance and cursor bounds.
        const auto slack =
            NumericExpression::constant(core::IntegerValue::ofBits(
                CursorType,
                CursorType.mask() - static_cast<std::uint64_t>(*limit)));
        const auto projected = summaryIntegerExpression(slack);
        if (projected) {
          if (recording())
            inferred.checked.require(
                {.kind = core::CheckedRequirementKind::SumFits,
                 .path = {},
                 .other = {},
                 .begin = *extent,
                 .end = core::PathAffine::ofExpression(*projected),
                 .family = {}});
          state.relations.learnAtMost(*a->extent->place, *limit);
        }
      }
  }
  return true;
}

void FunctionDataflow::checkedPointerCondition(const BinaryOperator &expr,
                                               bool holds,
                                               core::AnalysisState &state) {
  if (!expr.getLHS()->getType()->isPointerType() ||
      !expr.getRHS()->getType()->isPointerType() ||
      (expr.isRelationalOp() && !checkedPointerComparable(expr, state)))
    return;
  const auto a = checkedMemory(*expr.getLHS(), {}, {}, state);
  const auto b = checkedMemory(*expr.getRHS(), {}, {}, state);
  if (!a || !b || a->storage != b->storage || !checkedValid(*a, state) ||
      !checkedValid(*b, state) || !a->extent || !b->extent ||
      !checkedInterval(a->begin, a->end, *a->extent, state) ||
      !checkedInterval(b->begin, b->end, *b->extent, state))
    return;
  const auto lhs = a->begin;
  const auto rhs = b->begin;
  std::optional<core::Relation> relation;
  switch (expr.getOpcode()) {
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
    if ((expr.getOpcode() == BO_EQ) == holds)
      relation = core::Relation::Equal;
    else if (checkedAtMost(lhs, rhs, state))
      relation = core::Relation::Less;
    else if (checkedAtMost(b->begin, a->begin, state))
      relation = core::Relation::Greater;
    break;
  default:
    break;
  }
  if (!relation || lhs.scale != 1 || rhs.scale != 1)
    return;
  const auto strictlyLess = [&](const core::Affine &a, const core::Affine &b) {
    const auto next = a.shifted(1);
    return next && checkedAtMost(*next, b, state);
  };
  bool impossible = false;
  switch (*relation) {
  case core::Relation::Less:
    // The reversed bound refutes this branch's strict comparison.
    // NOLINTNEXTLINE(readability-suspicious-call-argument)
    impossible = checkedAtMost(rhs, lhs, state);
    break;
  case core::Relation::LessEqual:
    impossible = strictlyLess(rhs, lhs);
    break;
  case core::Relation::Equal:
    impossible = strictlyLess(lhs, rhs) || strictlyLess(rhs, lhs);
    break;
  case core::Relation::GreaterEqual:
    impossible = strictlyLess(lhs, rhs);
    break;
  case core::Relation::Greater:
    impossible = checkedAtMost(lhs, rhs, state);
    break;
  }
  if (impossible) {
    edgeInfeasible = true;
    return;
  }
  std::int64_t offset = 0;
  if (__builtin_sub_overflow(rhs.constant, lhs.constant, &offset))
    return;
  if (lhs.place && rhs.place) {
    state.relations.learn(*lhs.place, *relation, *rhs.place, offset);
  } else if (lhs.place || rhs.place) {
    const auto place = lhs.place ? *lhs.place : *rhs.place;
    if (!lhs.place) {
      if (__builtin_sub_overflow(std::int64_t{0}, offset, &offset))
        return;
      relation = core::flipped(*relation);
    }
    if (*relation == core::Relation::Less && offset != INT64_MIN)
      state.relations.learnAtMost(place, offset - 1);
    if (*relation == core::Relation::Greater && offset != INT64_MAX)
      state.relations.learnAtLeast(place, offset + 1);
    if (*relation == core::Relation::LessEqual ||
        *relation == core::Relation::Equal)
      state.relations.learnAtMost(place, offset);
    if (*relation == core::Relation::GreaterEqual ||
        *relation == core::Relation::Equal)
      state.relations.learnAtLeast(place, offset);
  }
}

void FunctionDataflow::installCheckedPosition(core::PlaceId dest,
                                              core::PointerPosition position,
                                              core::AnalysisState &state) {
  if (!checkedSteppedPointers.contains(dest)) {
    state.safety->positions[dest] = position;
    return;
  }
  auto [slot, inserted] = checkedCoordinates.try_emplace(dest);
  if (inserted)
    slot->second = places.create("position(" + nameOf(dest) + ")");
  const auto coordinate = slot->second;
  const auto sourceOffset = position.offset;
  const bool nonnegative = checkedAtMost({}, sourceOffset, state);
  position.offset = foldAffine(position.offset, state);
  if (position.offset.place == coordinate) {
    // A compound update has its own transfer, which captures the old value.
    // Decline a self-referential assignment that did not preserve a snapshot.
    if (position.offset.constant != 0 || position.offset.scale != 1) {
      state.safety->positions.erase(dest);
      return;
    }
    if (nonnegative)
      state.relations.learnAtLeast(coordinate, 0);
    state.safety->positions[dest] = position;
    return;
  }
  const auto expression = checkedByteExpression(position.offset, state);
  const bool selfDependent = expression && expression->dependsOn(coordinate);
  bool bounded = false;
  if (position.extent && position.extent->place != coordinate) {
    const auto extentExpression =
        position.extent->place
            ? numericExpressions.find(*position.extent->place)
            : numericExpressions.end();
    if (extentExpression == numericExpressions.end() ||
        !extentExpression->second.dependsOn(coordinate))
      bounded = checkedAtMost(sourceOffset, *position.extent, state);
  }
  auto upperBound = position.extent;
  if (bounded) {
    const CheckedMemory memory{.storage = position.storage,
                               .begin = sourceOffset,
                               .end = sourceOffset,
                               .extent = position.extent,
                               .input = {},
                               .pointer = nullptr};
    if (const auto witness = checkedWitness(memory, state);
        witness && witness->zero.isConstant() &&
        checkedAtMost(witness->zero, *upperBound, state)) {
      const auto zero = witness->zero.place
                            ? numericExpressions.find(*witness->zero.place)
                            : numericExpressions.end();
      if (zero == numericExpressions.end() ||
          !zero->second.dependsOn(coordinate))
        upperBound = witness->zero;
    }
  }
  const auto range = expression
                         ? evaluateNumericExpression(*expression, state)
                         : core::IntegerRangeEvaluation{
                               .values = core::IntegerRange::full(CursorType)};
  if (selfDependent)
    snapshotIntegerDependencies(coordinate, nullptr, state);
  snapshotScalar(coordinate, nullptr, state);
  state.dropGuardsOn(coordinate);
  state.relations.forget(coordinate);
  state.scalars.set(coordinate, core::ValueFact::ofInteger(
                                    range.mayBeInvalid
                                        ? core::IntegerRange::full(CursorType)
                                        : range.values.converted(CursorType)));
  if (!range.mayBeInvalid)
    if (const auto value = range.values.converted(CursorType).constant())
      for (const auto &[otherHolder, other] : checkedCoordinates) {
        const auto current = state.safety->positions.find(otherHolder);
        if (other == coordinate || current == state.safety->positions.end() ||
            current->second.offset != core::Affine::ofPlace(other))
          continue;
        const auto known = state.scalars.factOf(other);
        if (known && known->inType(CursorType).constant() == value) {
          // Equal byte counts need no shared object. This edge is established
          // by actual assignments, then updated by ordinary cursor transfers.
          state.relations.learn(coordinate, core::Relation::Equal, other);
          break;
        }
      }
  if (sourceOffset.place && sourceOffset.scale == 1 &&
      sourceOffset.place != coordinate && !selfDependent)
    state.relations.learn(coordinate, core::Relation::Equal,
                          *sourceOffset.place, sourceOffset.constant);
  if (nonnegative)
    state.relations.learnAtLeast(coordinate, 0);
  if (bounded) {
    if (upperBound->isConstant())
      state.relations.learnAtMost(coordinate, upperBound->constant);
    else if (upperBound->scale == 1)
      state.relations.learn(coordinate, core::Relation::LessEqual,
                            *upperBound->place, upperBound->constant);
  }
  position.offset = core::Affine::ofPlace(coordinate);
  state.safety->positions[dest] = position;
}

void FunctionDataflow::checkedAdvancePointer(const Expr &expr,
                                             core::AnalysisState &state) {
  const Expr *operand = nullptr;
  std::optional<core::Affine> shift;
  bool postfix = false;
  if (const auto *unary = dyn_cast<UnaryOperator>(&expr);
      unary && unary->isIncrementDecrementOp() &&
      expr.getType()->isPointerType()) {
    operand = unary->getSubExpr();
    postfix = unary->isPostfix();
    if (const auto unit = byteSizeOf(expr.getType()->getPointeeType(), context))
      shift = core::Affine::ofConstant(unary->isIncrementOp() ? *unit : -*unit);
  } else if (const auto *compound = dyn_cast<CompoundAssignOperator>(&expr);
             compound && expr.getType()->isPointerType()) {
    operand = compound->getLHS();
    const auto unit = byteSizeOf(expr.getType()->getPointeeType(), context);
    const auto count = builder.affineOf(*compound->getRHS());
    if (unit && count)
      shift =
          count->times(compound->getOpcode() == BO_AddAssign ? *unit : -*unit);
  }
  if (!operand)
    return;
  const auto ref = builder.resolve(*operand);
  if (!ref || !ref->element.isWhole())
    return;
  auto holder = ref->place;
  const auto old = checkedMemoryAt(holder, {}, {}, state);
  if (old && old->holder)
    holder = *old->holder;
  if (!old || !shift) {
    state.safety->positions.erase(holder);
    return;
  }
  if (!state.safety->positions.contains(holder))
    installCheckedPosition(holder,
                           {.storage = old->storage,
                            .offset = old->begin,
                            .extent = old->extent,
                            .input = old->inputPlace},
                           state);
  const auto coordinate = checkedCoordinates.find(holder);
  if (coordinate == checkedCoordinates.end() ||
      !state.safety->positions.contains(holder))
    return;
  const auto id = coordinate->second;
  auto position = state.safety->positions.at(holder);
  const auto delta = foldAffine(*shift, state);
  if (!delta.isConstant()) {
    // Retain a representable exact result even when its step is symbolic.
    const auto next =
        checkedByteSum(foldAffine(position.offset, state), delta, state, expr);
    if (next && next->place != id) {
      position.offset = *next;
      installCheckedPosition(holder, position, state);
    } else {
      state.safety->positions.erase(holder);
    }
    return;
  }
  const auto advancedOffset = position.offset.shifted(delta.constant);
  const auto extentExpression =
      position.extent && position.extent->place
          ? numericExpressions.find(*position.extent->place)
          : numericExpressions.end();
  const bool retainsExtent =
      advancedOffset && position.extent && position.extent->place != id &&
      (extentExpression == numericExpressions.end() ||
       !extentExpression->second.dependsOn(id)) &&
      checkedAtMost(*advancedOffset, *position.extent, state);
  const auto previous = integerRangeAt(id, CursorType, state);
  const bool positive = delta.constant >= 0;
  const auto magnitude =
      positive ? static_cast<std::uint64_t>(delta.constant)
               : std::uint64_t{0} - static_cast<std::uint64_t>(delta.constant);
  const auto amount = core::IntegerRange::singleton(
      core::IntegerValue::ofBits(CursorType, magnitude));
  const auto operation =
      positive ? core::IntegerOp::Add : core::IntegerOp::Subtract;
  const auto overflow =
      core::evaluateCheckedInteger(operation, previous, amount, CursorType)
          .overflow.constant();
  bool preserves = overflow && overflow->bits == 0;
  // A scalar hull can include the type maximum even though a relational
  // traversal bound excludes it. Use an established bound, including its
  // target-typed maximum, before retaining mathematical update relations.
  if (!preserves && positive) {
    const auto bounds = checkedRelations(state);
    if (const auto upper = bounds.bound(id, std::nullopt); upper && *upper >= 0)
      preserves =
          static_cast<std::uint64_t>(*upper) <= CursorType.mask() - magnitude;
    std::map<core::PlaceId, core::IntegerType> inputs;
    for (const auto &[other, fact] : state.scalars.all())
      if (fact.integer)
        inputs.emplace(other, fact.integer->type);
    for (const auto &[otherHolder, other] : checkedCoordinates)
      if (state.safety->positions.contains(otherHolder))
        inputs.emplace(other, CursorType);
    for (const auto &[pair, edge] : state.relations.all()) {
      (void)edge;
      for (const auto other : {pair.first, pair.second})
        if (const auto *decl =
                dyn_cast_or_null<ValueDecl>(builder.declFor(other)))
          if (const auto type = integerTypeOf(*decl, context))
            inputs.emplace(other, *type);
    }
    for (const auto &[other, type] : inputs) {
      if (preserves)
        break;
      if (other == id)
        continue;
      // A full target range still bounds nonwrapping arithmetic. Widening
      // may omit that redundant scalar fact while retaining q < p or q < n.
      const auto upper = integerRangeAt(other, type, state).maximum();
      const auto distance = bounds.bound(id, other);
      inferred.checked.limited |= bounds.limited();
      if (bounds.limited())
        inferred.incomplete.insert("traversal relational limit reached");
      if (!upper || upper->negative() || !distance)
        continue;
      std::int64_t adjustment = 0;
      if (__builtin_add_overflow(*distance, delta.constant, &adjustment))
        continue;
      const auto bits = upper->bits;
      std::uint64_t maximum = 0;
      const bool fits =
          adjustment >= 0
              ? !__builtin_add_overflow(
                    bits, static_cast<std::uint64_t>(adjustment), &maximum)
              : !__builtin_sub_overflow(
                    bits,
                    std::uint64_t{0} - static_cast<std::uint64_t>(adjustment),
                    &maximum);
      if (fits) {
        preserves = true;
        break;
      }
    }
  } else if (!preserves && !positive && delta.constant != INT64_MIN) {
    preserves = checkedAtMost(core::Affine::ofConstant(-delta.constant),
                              core::Affine::ofPlace(id), state);
  }
  auto next = core::evaluateInteger(operation, previous, amount).values;
  if (preserves && !previous.empty()) {
    const auto boundary = positive ? previous.minimum() : previous.maximum();
    const auto updated = core::evaluateInteger(
        operation, *boundary,
        core::IntegerValue::ofBits(CursorType, magnitude));
    if (updated.value)
      next = next.satisfying(positive ? core::IntegerOp::GreaterEqual
                                      : core::IntegerOp::LessEqual,
                             core::IntegerRange::singleton(*updated.value));
  }
  const auto relations = state.relations.allBounds();
  const auto lower = state.relations.atLeast(id);
  const auto upper = state.relations.atMost(id);
  const auto reused = valueSnapshots.find({id, &expr});
  std::vector<std::pair<core::PlaceId, core::InitializedRange>> advanced;
  if (preserves)
    for (const auto &[storage, ranges] : state.safety->memory)
      for (auto range : ranges) {
        if (range.when.dependsOn(id) || range.source == id)
          continue;
        if (reused != valueSnapshots.end() &&
            (range.begin.place == reused->second ||
             range.end.place == reused->second ||
             range.when.dependsOn(reused->second)))
          continue;
        bool valid = true;
        bool depends = false;
        for (auto *endpoint : {&range.begin, &range.end}) {
          if (endpoint->place != id)
            continue;
          depends = true;
          std::int64_t change = 0;
          valid &= !__builtin_mul_overflow(endpoint->scale, delta.constant,
                                           &change) &&
                   !__builtin_sub_overflow(endpoint->constant, change,
                                           &endpoint->constant);
        }
        if (valid && depends)
          advanced.emplace_back(storage, std::move(range));
      }
  auto [result, inserted] = checkedPointerResults.try_emplace(&expr);
  if (inserted)
    result->second =
        places.create("pointer-result@" + std::to_string(locate(expr).line));
  const auto saved = result->second;
  state.safety->positions[saved] = position;
  state.safety->objects[saved] = position.storage;
  if (state.safety->pointers.contains(holder))
    state.safety->pointers.insert(saved);
  if (const auto nullness = nullnessAt(holder, state))
    state.nulls.set(saved, *nullness);
  state.aliases.unite(saved, holder, core::PointerOffset::unknown());
  snapshotScalar(id, &expr, state);
  const auto frozen = state.safety->positions.find(saved);
  const auto oldOffset = frozen == state.safety->positions.end()
                             ? std::optional<core::Affine>{}
                             : std::optional(frozen->second.offset);
  state.dropGuardsOn(id);
  state.relations.forget(id);
  state.scalars.set(id, core::ValueFact::ofInteger(next));
  position.offset = core::Affine::ofPlace(id);
  state.safety->positions[holder] = position;
  if (preserves) {
    std::int64_t bound = 0;
    if (lower && !__builtin_add_overflow(*lower, delta.constant, &bound))
      state.relations.learnAtLeast(id, bound);
    if (upper && !__builtin_add_overflow(*upper, delta.constant, &bound))
      state.relations.learnAtMost(id, bound);
    if (oldOffset && oldOffset->place && oldOffset->scale == 1) {
      const auto offset = oldOffset->shifted(delta.constant);
      if (offset)
        state.relations.learn(id, core::Relation::Equal, *offset->place,
                              offset->constant);
    }
    for (const auto &[pair, edge] : relations) {
      // The source-site snapshot now denotes this iteration's old value.
      // An edge mentioning its previous generation cannot be reinstalled.
      if (reused != valueSnapshots.end() &&
          (pair.first == reused->second || pair.second == reused->second))
        continue;
      if (pair.first != id && pair.second != id)
        continue;
      std::int64_t offset = edge.offset;
      const bool valid =
          pair.first == id
              ? !__builtin_add_overflow(offset, delta.constant, &offset)
              : !__builtin_sub_overflow(offset, delta.constant, &offset);
      if (valid)
        state.relations.learn(pair.first, edge.relation, pair.second, offset);
    }
    for (const auto &[storage, range] : advanced)
      state.safety->initialize(storage, range);
    if (retainsExtent) {
      if (position.extent->isConstant())
        state.relations.learnAtMost(id, position.extent->constant);
      else if (position.extent->scale == 1)
        state.relations.learn(id, core::Relation::LessEqual,
                              *position.extent->place,
                              position.extent->constant);
    }
  }
  if (!postfix) {
    state.safety->positions[saved] = position;
  } else if (preserves && delta.constant != INT64_MIN) {
    auto resultPosition = position;
    resultPosition.offset.constant = -delta.constant;
    state.safety->positions[saved] = resultPosition;
  }
}

} // namespace weavec::analysis
