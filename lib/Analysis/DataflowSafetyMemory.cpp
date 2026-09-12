//===- DataflowSafetyMemory.cpp - Positive memory evidence (RFC 0018) -----===//
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

std::optional<FunctionDataflow::NumericExpression>
FunctionDataflow::checkedByteExpression(const core::Affine &value,
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
    if (!type &&
        (checkedTerminatorInputs.contains(*value.place) ||
         std::ranges::any_of(checkedCoordinates, [&](const auto &entry) {
           return entry.second == *value.place;
         })))
      type = bytes;
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

std::optional<core::Affine> FunctionDataflow::checkedByteSum(
    const core::Affine &lhs, const core::Affine &rhs,
    const core::AnalysisState &state, const Stmt &at) {
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
        !checkedAtMost(core::Affine::ofPlace(*cursor.place),
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
  const auto a = checkedByteExpression(lhs, state);
  const auto b = checkedByteExpression(rhs, state);
  if (!a || !b)
    return std::nullopt;
  const auto sum = NumericExpression::operation(core::IntegerOp::Add, *a, *b);
  if (!sum)
    return std::nullopt;
  const bool proved =
      operationDoesNotOverflow(core::IntegerOp::Add, *a, *b, a->type(), state);
  bool required = false;
  if (!proved && options.checkContracts) {
    const auto first = summaryAffineOf(lhs);
    const auto last = summaryAffineOf(rhs);
    required = first && last;
    if (required && recording())
      inferred.checked.require({.kind = core::CheckedRequirementKind::SumFits,
                                .path = {},
                                .other = {},
                                .begin = *first,
                                .end = *last,
                                .family = {}});
  }
  if (!proved && !required)
    return std::nullopt;
  safetyObligation(core::SafetyProperty::Arithmetic,
                   core::safetyOutcome(proved, required), at, "byte sum",
                   "byte interval sum must not overflow");
  auto saved = expressionPlaces.find(*sum);
  if (saved == expressionPlaces.end()) {
    const auto place = places.create("checked byte sum");
    saved = expressionPlaces.emplace(*sum, place).first;
    numericExpressions.emplace(place, *sum);
  }
  return core::Affine::ofPlace(saved->second);
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::checkedMemoryAt(core::PlaceId holder,
                                  const core::Affine &begin,
                                  const core::Affine &end,
                                  const core::AnalysisState &state) {
  if (state.safety->invalidatedPointers.contains(holder))
    return CheckedMemory{.storage = places.deref(holder),
                         .begin = begin,
                         .end = end,
                         .extent = {},
                         .input = {},
                         .pointer = nullptr,
                         .holder = holder};
  if (const auto found = state.safety->positions.find(holder);
      found != state.safety->positions.end()) {
    const auto &position = found->second;
    const auto offset = foldAffine(position.offset, state);
    auto first = sumOf(position.offset, begin);
    auto last = sumOf(position.offset, end);
    if (!first)
      first = sumOf(offset, begin);
    if (!last)
      last = sumOf(offset, end);
    if (!first)
      first =
          checkedByteSum(position.offset, begin, state, *function.getBody());
    if (!last)
      last = checkedByteSum(position.offset, end, state, *function.getBody());
    if (!first || !last)
      return std::nullopt;
    auto extent = position.extent;
    if (!extent)
      if (const auto accessible =
              state.safety->accessible.find(position.storage);
          accessible != state.safety->accessible.end())
        extent = accessible->second;
    return CheckedMemory{.storage = position.storage,
                         .begin = *first,
                         .end = *last,
                         .extent = extent,
                         .input = position.input
                                      ? builder.summaryPathOf(*position.input)
                                      : std::nullopt,
                         .pointer = nullptr,
                         .holder = holder,
                         .inputPlace = position.input};
  }
  const auto inputPath = stableSummaryPathOf(holder);
  CheckedMemory result{.storage = holder,
                       .begin = begin,
                       .end = end,
                       .extent = {},
                       .input = inputPath,
                       .pointer = nullptr,
                       .holder = holder,
                       .inputPlace =
                           inputPath ? std::optional(holder) : std::nullopt};
  const auto input = places.deref(holder);
  checkedInputObjects[holder] = input;
  result.storage = input;
  if (const auto object = state.safety->objects.find(holder);
      object != state.safety->objects.end()) {
    result.storage = object->second;
  } else {
    const auto loans = state.loans.heldBy(holder);
    if (loans.size() == 1)
      result.storage = loans.front().place;
    else if (!loans.empty())
      return std::nullopt;
  }
  if (const auto spatial = spatialRecordAt(holder, state)) {
    result.extent = spatial->extent;
    const auto offset = spatial->boundsOffset.value_or(spatial->offset);
    if (!offset.isZero()) {
      const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(holder));
      const auto unit =
          decl && decl->getType()->isPointerType()
              ? byteSizeOf(decl->getType()->getPointeeType(), context)
              : std::nullopt;
      std::int64_t bytes = 0;
      if (!unit || !offset.isElements() ||
          __builtin_mul_overflow(offset.elements, *unit, &bytes))
        return std::nullopt;
      const auto first = begin.shifted(bytes);
      const auto last = end.shifted(bytes);
      if (!first || !last)
        return std::nullopt;
      result.begin = *first;
      result.end = *last;
    }
  }
  if (result.storage != input ||
      state.safety->replacedPointers.contains(holder)) {
    result.input.reset();
    result.inputPlace.reset();
  }
  if (!result.extent)
    if (const auto accessible = state.safety->accessible.find(result.storage);
        accessible != state.safety->accessible.end())
      result.extent = accessible->second;
  return result;
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::checkedPathMemory(const core::SummaryPath &path,
                                    const CallExpr &call,
                                    const core::Affine &begin,
                                    const core::Affine &end,
                                    const core::AnalysisState &state) {
  if (path.isParam() && path.isRoot() && path.index < call.getNumArgs())
    return checkedMemory(*call.getArg(path.index), begin, end, state);
  const auto place = builder.resolveSummaryPath(path, call, true);
  return place && place->element.isWhole()
             ? checkedMemoryAt(place->place, begin, end, state)
             : std::nullopt;
}

bool FunctionDataflow::checkedValid(const CheckedMemory &memory,
                                    const core::AnalysisState &state) {
  if (!memory.holder)
    return (memory.pointer == nullptr) ||
           builder.classifyValue(*memory.pointer).kind ==
               ValueOrigin::Kind::Borrow;
  const auto place = *memory.holder;
  const auto nullness = nullnessAt(place, state);
  return state.safety->pointers.contains(place) &&
         !state.safety->invalidatedPointers.contains(place) &&
         !state.moves.recordOf(place) && !state.resources.isEscaped(place) &&
         nullness && nullness->state == core::Nullness::NonNull;
}

void FunctionDataflow::checkedPointerFormation(
    const Expr &at, const Expr &pointer,
    const std::optional<core::Affine> &shift, core::AnalysisState &state) {
  const auto memory =
      shift ? checkedMemory(pointer, *shift, *shift, state) : std::nullopt;
  if (memory) {
    checkedStringBound(*memory, at, state);
    checkedStringUse(*memory, at, state);
  }
  const bool bounds =
      memory && memory->extent &&
      checkedInterval(memory->begin, memory->end, *memory->extent, state);
  const bool required =
      memory && (!bounds || memory->input) &&
      checkedRequire(core::CheckedRequirementKind::Extent, *memory, at, state);
  safetyObligation(
      core::SafetyProperty::Bounds, core::safetyOutcome(bounds, required), at,
      "pointer formation",
      "formed pointer must remain within its object or one past it");
  const bool valid = memory && checkedValid(*memory, state);
  const bool input =
      memory && memory->input &&
      checkedRequire(core::CheckedRequirementKind::Valid, *memory, at, state);
  safetyObligation(core::SafetyProperty::Validity,
                   core::safetyOutcome(valid, input), at, "pointer formation",
                   "pointer arithmetic requires live non-null storage");
}

bool FunctionDataflow::checkedInterval(const core::Affine &begin,
                                       const core::Affine &end,
                                       const core::Affine &extent,
                                       const core::AnalysisState &state) {
  const auto first = foldAffine(begin, state);
  const auto last = foldAffine(end, state);
  const auto size = foldAffine(extent, state);
  if (first.isConstant() && last.isConstant() && size.isConstant())
    return first.constant >= 0 && first.constant <= last.constant &&
           last.constant <= size.constant;
  const auto lower = [&](const core::Affine &value) {
    return value.place ? integerBounds(*value.place, state).first
                       : std::optional<std::int64_t>{};
  };
  const auto upper = [&](const core::Affine &value) {
    return value.place ? integerBounds(*value.place, state).second
                       : std::optional<std::int64_t>{};
  };
  const auto relation = last.place && size.place
                            ? state.relations.between(*last.place, *size.place)
                            : std::nullopt;
  const bool affine = core::checkSpatialBounds(first, last, size, relation,
                                               {.needAtMost = upper(last),
                                                .haveAtMost = upper(size),
                                                .needAtLeast = lower(last),
                                                .haveAtLeast = lower(size),
                                                .needBoundaryWitness = false},
                                               lower(first))
                          .outcome == core::SpatialOutcome::Proven;
  return affine || (checkedAtMost({}, first, state) &&
                    checkedAtMost(first, last, state) &&
                    checkedAtMost(last, size, state));
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::checkedMemory(const Expr &pointer, const core::Affine &begin,
                                const core::Affine &end,
                                const core::AnalysisState &state) {
  const Expr *value = pointer.IgnoreParenImpCasts();
  if (const auto *address = dyn_cast<UnaryOperator>(value);
      address && address->getOpcode() == UO_AddrOf) {
    const auto *target = address->getSubExpr()->IgnoreParenImpCasts();
    const bool indirect =
        isa<ArraySubscriptExpr>(target) ||
        (isa<UnaryOperator>(target) &&
         cast<UnaryOperator>(target)->getOpcode() == UO_Deref);
    if (indirect) {
      auto location = checkedLvalue(*target, state);
      if (!location)
        return std::nullopt;
      const auto first = checkedByteSum(location->begin, begin, state, pointer);
      const auto last = checkedByteSum(location->begin, end, state, pointer);
      if (!first || !last)
        return std::nullopt;
      location->begin = *first;
      location->end = *last;
      location->pointer = &pointer;
      return location;
    }
  }
  if (const auto saved = checkedPointerResults.find(value);
      saved != checkedPointerResults.end() &&
      state.safety->positions.contains(saved->second)) {
    auto result = checkedMemoryAt(saved->second, begin, end, state);
    if (result)
      result->pointer = &pointer;
    return result;
  }
  // A represented holder already denotes its current value. Classification
  // can also carry an ordinary derived offset for an indirect holder; adding
  // it again would double-count a previous pointer-to-pointer increment.
  if (value->getType()->isPointerType() && PlaceBuilder::isPlaceExpr(*value))
    if (const auto holder = builder.resolvePointerValue(*value);
        holder && state.safety->positions.contains(holder->place)) {
      auto result = checkedMemoryAt(holder->place, begin, end, state);
      if (result)
        result->pointer = &pointer;
      return result;
    }
  // Preserve the evaluated C index, then scale in mathematical byte units.
  if (const auto *binary = dyn_cast<BinaryOperator>(value);
      binary &&
      (binary->getOpcode() == BO_Add || binary->getOpcode() == BO_Sub)) {
    const Expr *base = binary->getLHS();
    const Expr *index = binary->getRHS();
    if (binary->getOpcode() == BO_Add && index->getType()->isPointerType())
      std::swap(base, index);
    if (base->getType()->isPointerType() && index->getType()->isIntegerType()) {
      const auto unit = byteSizeOf(base->getType()->getPointeeType(), context);
      const auto count = builder.affineOf(*index);
      const auto shift =
          unit && count
              ? count->times(binary->getOpcode() == BO_Sub ? -*unit : *unit)
              : std::nullopt;
      const auto first =
          shift
              ? checkedByteSum(begin, foldAffine(*shift, state), state, pointer)
              : std::nullopt;
      const auto last =
          shift ? checkedByteSum(end, foldAffine(*shift, state), state, pointer)
                : std::nullopt;
      return first && last ? checkedMemory(*base, *first, *last, state)
                           : std::nullopt;
    }
  }
  auto origin = builder.classifyValue(pointer);
  if (origin.kind == ValueOrigin::Kind::Opaque &&
      pointer.getType()->isPointerType() && PlaceBuilder::isPlaceExpr(pointer))
    if (const auto ref = builder.resolvePointerValue(pointer)) {
      origin.kind = ValueOrigin::Kind::Copy;
      origin.place = *ref;
    }
  CheckedMemory result{.storage = {},
                       .begin = begin,
                       .end = end,
                       .extent = {},
                       .input = {},
                       .pointer = &pointer};
  if (!origin.place)
    return std::nullopt;
  result.storage = origin.place->place;
  if (origin.kind == ValueOrigin::Kind::Copy) {
    auto memory = checkedMemoryAt(result.storage, begin, end, state);
    if (!memory || !origin.offset.isZero())
      return std::nullopt;
    memory->pointer = &pointer;
    return memory;
  }
  if (origin.kind == ValueOrigin::Kind::Borrow &&
      places.step(result.storage) == core::PathStep::Index)
    if (const auto parent = places.parent(result.storage))
      if (const auto *var = builder.varForPlace(*parent);
          var && var->getType()->isArrayType())
        result.storage = *parent;
  if (origin.kind != ValueOrigin::Kind::Copy &&
      origin.kind != ValueOrigin::Kind::Borrow)
    return std::nullopt;
  Access access{.base = &pointer,
                .storage = nullptr,
                .start = begin,
                .end = end,
                .index = nullptr};
  const auto known = knownExtentOf(access, state);
  result.extent = origin.extent;
  core::PointerOffset offset = origin.offset;
  if (known) {
    result.extent = known->have;
    offset = known->offset.plus(origin.offset);
  }
  if (origin.kind == ValueOrigin::Kind::Borrow)
    if (const auto *var = builder.varForPlace(result.storage))
      if (const auto size = byteSizeOf(var->getType(), context))
        result.extent = core::Affine::ofConstant(*size);
  if (!offset.isZero()) {
    const auto unit = byteSizeOf(pointer.getType()->getPointeeType(), context);
    std::int64_t shift = 0;
    if (!offset.isElements() || !unit ||
        __builtin_mul_overflow(offset.elements, *unit, &shift))
      return std::nullopt;
    const auto first = begin.shifted(shift);
    const auto last = end.shifted(shift);
    if (!first || !last)
      return std::nullopt;
    result.begin = *first;
    result.end = *last;
  }
  if (origin.kind == ValueOrigin::Kind::Copy) {
    const auto loans = state.loans.heldBy(result.storage);
    if (loans.size() == 1)
      result.storage = loans.front().place;
    else if (!loans.empty())
      return std::nullopt;
    else
      result.input = stableSummaryPathOf(result.storage);
  }
  if (result.input && !result.input->isParam() && !result.input->isGlobal())
    result.input.reset();
  return result;
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::checkedLvalue(const Expr &expr,
                                const core::AnalysisState &state) {
  auto access = accessOf(expr);
  if (access) {
    const auto size = byteSizeOf(expr.getType(), context);
    const auto end = size ? access->start.shifted(*size) : std::nullopt;
    if (!end)
      return std::nullopt;
    access->end = *end;
  }
  if (!access) {
    const auto ref = builder.resolve(expr);
    const auto bytes = byteSizeOf(expr.getType(), context);
    if (!ref || !ref->derefs.empty() || !ref->element.isWhole() || !bytes)
      return std::nullopt;
    return CheckedMemory{.storage = ref->place,
                         .begin = core::Affine::ofConstant(0),
                         .end = core::Affine::ofConstant(*bytes),
                         .extent = core::Affine::ofConstant(*bytes),
                         .input = {},
                         .pointer = nullptr};
  }
  if (access->storage) {
    const auto bytes = byteSizeOf(access->storage->getType(), context);
    return CheckedMemory{
        .storage = builder.placeForVar(*access->storage),
        .begin = access->start,
        .end = access->end,
        .extent = bytes ? std::optional(core::Affine::ofConstant(*bytes))
                        : std::nullopt,
        .input = {},
        .pointer = nullptr};
  }
  if (!access->base)
    return std::nullopt;
  return checkedMemory(*access->base, access->start, access->end, state);
}

std::optional<bool>
FunctionDataflow::checkedWritePermission(const CheckedMemory &memory,
                                         const core::AnalysisState &state) {
  if (foldAffine(memory.begin, state) == foldAffine(memory.end, state))
    return true;
  if (memory.input)
    return std::nullopt;
  if (builder.isLiteralPlace(places.root(memory.storage)))
    return false;
  bool indirect = false;
  if (memory.pointer) {
    const auto origin = builder.classifyValue(*memory.pointer);
    indirect = origin.kind == ValueOrigin::Kind::Copy && origin.place &&
               origin.place->place == memory.storage;
  }
  if (!indirect) {
    auto place = memory.storage;
    bool mutableObject = false;
    while (true) {
      if (const auto *decl =
              dyn_cast_or_null<ValueDecl>(builder.declFor(place))) {
        auto type = decl->getType();
        while (const auto *array = context.getAsArrayType(type))
          type = array->getElementType();
        if (type.isConstQualified())
          return false;
        if (isa<VarDecl>(decl) && !type->isPointerType())
          mutableObject = true;
        if (place == memory.storage && memory.pointer && !memory.holder &&
            builder.classifyValue(*memory.pointer).kind ==
                ValueOrigin::Kind::Borrow &&
            !decl->getType().isConstQualified())
          mutableObject = true;
      }
      const auto parent = places.parent(place);
      if (!parent)
        break;
      place = *parent;
    }
    if (mutableObject)
      return true;
  }
  // A const pointer holder does not make its malloc allocation const. Other
  // external allocation families do not imply write permission.
  const auto resource =
      state.resources.recordOf(memory.holder.value_or(memory.storage));
  if (resource && resource->origin == core::ResourceOrigin::Allocated &&
      resource->family == "free")
    return true;
  return std::nullopt;
}

bool FunctionDataflow::checkedWrite(const CheckedMemory &memory, const Stmt &at,
                                    core::AnalysisState &state) {
  // RFC 0024: byte writes do not implement a variadic lifecycle operation.
  // Retain the outstanding va_end obligation while retiring cursor evidence.
  if (foldAffine(memory.begin, state) != foldAffine(memory.end, state))
    for (auto &[place, list] : state.safety->argumentLists)
      if (places.root(place) == places.root(memory.storage))
        list.phase = core::ArgumentListPhase::Unknown;
  const auto permission = checkedWritePermission(memory, state);
  const bool required =
      !permission &&
      checkedRequire(core::CheckedRequirementKind::Writable, memory, at, state);
  auto outcome = core::safetyOutcome(permission.value_or(false), required);
  if (permission && !*permission)
    outcome = core::SafetyOutcome::Violation;
  safetyObligation(core::SafetyProperty::Validity, outcome, at, "write",
                   permission && !*permission
                       ? "cannot write to read-only storage"
                       : "write interval must be writable");
  return permission.value_or(false) || required;
}

bool FunctionDataflow::checkedInitialized(const CheckedMemory &memory,
                                          const core::AnalysisState &state) {
  if (foldAffine(memory.begin, state) == foldAffine(memory.end, state))
    return true;
  if (builder.isLiteralPlace(memory.storage) && memory.extent)
    return checkedInterval(memory.begin, memory.end, *memory.extent, state);
  // RFC 0022: C initializes static scalar cells, including function pointers
  // inside hook records. This does not initialize anything they point to.
  if (const auto *global = builder.varForPlace(places.root(memory.storage));
      global && global->hasGlobalStorage()) {
    bool cell = true;
    for (auto place = memory.storage; !places.isBase(place);) {
      if (places.step(place) == core::PathStep::Deref) {
        cell = false;
        break;
      }
      place = *places.parent(place);
    }
    if (cell && memory.extent &&
        checkedInterval(memory.begin, memory.end, *memory.extent, state))
      return true;
  }
  // This is the object's own value (including a by-value parameter), not
  // the referent of an initialized pointer holder.
  if (state.safety->initialized.contains(memory.storage) &&
      isLocalStorage(memory.storage))
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(memory.storage));
        decl && !decl->getType()->isRecordType())
      if (const auto bytes = byteSizeOf(decl->getType(), context))
        if (checkedInterval(memory.begin, memory.end,
                            core::Affine::ofConstant(*bytes), state))
          return true;
  if (const auto witnesses = state.safety->termination.find(memory.storage);
      witnesses != state.safety->termination.end())
    for (const auto &witness : witnesses->second) {
      auto when = witness.when;
      const auto through = witness.zero.shifted(1);
      if (through && pruneGuard(when, state) && when.trivial() &&
          checkedAtMost(witness.begin, memory.begin, state) &&
          checkedAtMost(memory.begin, memory.end, state) &&
          checkedAtMost(memory.end, *through, state))
        return true;
    }
  const auto found = state.safety->memory.find(memory.storage);
  if (found == state.safety->memory.end())
    return false;
  return std::ranges::any_of(found->second, [&](const auto &range) {
    if (range.source)
      return false;
    auto condition = range.when;
    if (!pruneGuard(condition, state) || !condition.trivial())
      return false;
    if (range.begin == memory.begin && range.end == memory.end)
      return true;
    // Offset the interval against its represented lower bound. An affine
    // difference involving two independent places is deliberately unresolved.
    const auto negative = range.begin.times(-1);
    const auto start = negative ? sumOf(memory.begin, *negative) : std::nullopt;
    const auto end = negative ? sumOf(memory.end, *negative) : std::nullopt;
    const auto have = negative ? sumOf(range.end, *negative) : std::nullopt;
    return start && end && have && checkedInterval(*start, *end, *have, state);
  });
}

bool FunctionDataflow::checkedTerminated(const CheckedMemory &memory,
                                         const core::AnalysisState &state) {
  if (!memory.extent || !checkedValid(memory, state))
    return false;
  if (checkedWitness(memory, state))
    return true;
  if (const auto bounded =
          state.safety->boundedTermination.find(memory.storage);
      bounded != state.safety->boundedTermination.end())
    for (const auto &fact : bounded->second) {
      auto when = fact.when;
      if (pruneGuard(when, state) && when.trivial() &&
          fact.begin == memory.begin &&
          checkedInterval(fact.begin, fact.end, *memory.extent, state))
        return true;
    }
  const auto found = state.safety->memory.find(memory.storage);
  if (found == state.safety->memory.end())
    return false;
  for (const auto &range : found->second) {
    if (!range.zeroed || range.source)
      continue;
    auto when = range.when;
    if (!pruneGuard(when, state) || !when.trivial())
      continue;
    auto point = foldAffine(range.begin, state);
    const auto first = foldAffine(memory.begin, state);
    if (point.isConstant() && first.isConstant())
      point.constant = std::max(point.constant, first.constant);
    const auto through = point.shifted(1);
    if (!through || !checkedInterval(point, *through, range.end, state) ||
        !checkedInterval(first, *through, *memory.extent, state))
      continue;
    auto prefix = memory;
    prefix.end = *through;
    if (checkedInitialized(prefix, state))
      return true;
  }
  return false;
}

bool FunctionDataflow::checkedRequire(core::CheckedRequirementKind kind,
                                      const CheckedMemory &memory,
                                      const Stmt &at,
                                      core::AnalysisState &state,
                                      std::string family) {
  if (options.deferCheckedCalls &&
      (state.safety->deferred.contains(memory.storage) ||
       (memory.holder && state.safety->deferred.contains(*memory.holder)))) {
    if (recording())
      inferred.checked.deferred = true;
    return true;
  }
  if (!memory.input)
    return false;
  core::PathGuard condition;
  if (recording()) {
    if (checkedRequirementGuard) {
      if (!*checkedRequirementGuard)
        *checkedRequirementGuard = summaryGuardOf(guardHere(state));
      condition = **checkedRequirementGuard;
    } else {
      condition = summaryGuardOf(guardHere(state));
    }
  }
  auto begin = summaryAffineOf(memory.begin);
  auto end = summaryAffineOf(memory.end);
  if (kind == core::CheckedRequirementKind::Valid ||
      kind == core::CheckedRequirementKind::Release) {
    begin = core::PathAffine::ofConstant(0);
    end = core::PathAffine::ofConstant(0);
  } else if (kind == core::CheckedRequirementKind::Terminated) {
    // RFC 0021: begin is the minimum entry terminator index, not an
    // interval endpoint. The end field is reserved and remains zero even
    // when a builtin or nested call consumes an advanced input pointer.
    // An interval envelope that resets begin to zero would weaken this
    // requirement, so an unrepresentable minimum must remain unresolved.
    // Entry witnesses initialize bytes starting at the input pointer. They
    // cannot justify a scan that may start before it.
    if (!checkedAtMost({}, memory.begin, state))
      return false;
    end = core::PathAffine::ofConstant(0);
  } else if (!end) {
    end = checkedRequirementEnvelope(memory.end, state);
    if (!end)
      end = checkedLoopRequirement(memory.end, at, state);
    if (end && checkedAtMost({}, memory.begin, state))
      begin = core::PathAffine::ofConstant(0);
    else
      return false;
  }
  if (!begin || !end)
    return false;
  if (kind == core::CheckedRequirementKind::Extent &&
      checkedAtMost({}, memory.begin, state)) {
    auto &accessible = state.safety->accessible;
    const auto found = accessible.find(memory.storage);
    if (found == accessible.end() ||
        checkedAtMost(found->second, memory.end, state))
      accessible[memory.storage] = memory.end;
  }
  if (kind == core::CheckedRequirementKind::Valid && memory.holder) {
    state.safety->pointers.insert(*memory.holder);
    if (state.nulls.stateOf(*memory.holder) != core::Nullness::NonNull)
      state.nulls.set(*memory.holder, {.state = core::Nullness::NonNull,
                                       .location = {},
                                       .reason = core::NullReason::Declared});
  }
  if (recording())
    inferred.checked.require({.kind = kind,
                              .path = *memory.input,
                              .other = {},
                              .begin = *begin,
                              .end = *end,
                              .family = std::move(family),
                              .when = condition});
  return true;
}

void FunctionDataflow::checkedAccess(const Expr &expr, const PlaceRef &ref,
                                     Role role, core::AnalysisState &state) {
  // RFC 0022: a function designator is not an object memory access. Its
  // pointer operand is still evaluated and checked in the ordinary walk.
  if (expr.getType()->isFunctionType())
    return;
  if (checkedContainerAccess(expr,
                             role == Role::Read || role == Role::ReadWrite ||
                                 role == Role::Consume,
                             role == Role::Write || role == Role::ReadWrite,
                             state))
    return;
  const bool reads =
      role == Role::Read || role == Role::ReadWrite || role == Role::Consume;
  if (const auto *decl = dyn_cast<DeclRefExpr>(expr.IgnoreParenImpCasts())) {
    const auto *var = dyn_cast<VarDecl>(decl->getDecl());
    if (!var || var->getType()->isArrayType() || role == Role::AddressOf ||
        runtimeListType(var->getType()))
      return;
    const auto storage = checkedLvalue(expr, state);
    if (reads) {
      const bool deferred = options.deferCheckedCalls &&
                            state.safety->deferred.contains(ref.place);
      if (deferred && recording())
        inferred.checked.deferred = true;
      safetyObligation(core::SafetyProperty::Initialization,
                       var->hasGlobalStorage() ||
                               state.safety->initialized.contains(ref.place) ||
                               (storage && checkedInitialized(*storage, state))
                           ? core::SafetyOutcome::Proven
                           : core::safetyOutcome(false, deferred),
                       expr, "value", "local value must be initialized");
    }
    return;
  }
  const auto memory = checkedLvalue(expr, state);
  if (!memory) {
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Unresolved, expr, "memory",
                     "memory location is not represented");
    return;
  }
  checkedStringBound(*memory, expr, state);
  checkedStringUse(*memory, expr, state);
  if (role == Role::Write || role == Role::ReadWrite)
    checkedWrite(*memory, expr, state);
  if (memory->pointer || memory->holder) {
    const bool valid = checkedValid(*memory, state);
    const bool required = (!valid || memory->input) &&
                          checkedRequire(core::CheckedRequirementKind::Valid,
                                         *memory, expr, state);
    safetyObligation(core::SafetyProperty::Validity,
                     core::safetyOutcome(valid, required), expr, "pointer",
                     "pointer must identify live non-null storage");
  }
  auto end = role == Role::AddressOf ? memory->begin : memory->end;
  const bool bounds = memory->extent && checkedInterval(memory->begin, end,
                                                        *memory->extent, state);
  const bool required = (!bounds || memory->input) &&
                        checkedRequire(core::CheckedRequirementKind::Extent,
                                       *memory, expr, state);
  safetyObligation(core::SafetyProperty::Bounds,
                   core::safetyOutcome(bounds, required), expr, "access",
                   "access interval must fit its object");
  if (reads) {
    const bool initialized = (ref.element.isWhole() &&
                              state.safety->initialized.contains(ref.place)) ||
                             checkedInitialized(*memory, state);
    const bool input = !initialized &&
                       checkedRequire(core::CheckedRequirementKind::Initialized,
                                      *memory, expr, state);
    safetyObligation(core::SafetyProperty::Initialization,
                     core::safetyOutcome(initialized, input), expr, "access",
                     "read interval must be initialized");
  }
}

} // namespace weavec::analysis
