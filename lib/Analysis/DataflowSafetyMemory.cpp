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
  if (value.scale < 0 || value.constant < 0)
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
    const auto shift = NumericExpression::constant(core::IntegerValue::ofBits(
        bytes, static_cast<std::uint64_t>(value.constant)));
    if (!operationDoesNotOverflow(core::IntegerOp::Add, *expression, shift,
                                  bytes, state))
      return std::nullopt;
    expression =
        NumericExpression::operation(core::IntegerOp::Add, *expression, shift);
  }
  return expression;
}

std::optional<core::Affine> FunctionDataflow::checkedByteSum(
    const core::Affine &lhs, const core::Affine &rhs,
    const core::AnalysisState &state, const Stmt &at) {
  if (const auto linear = sumOf(lhs, rhs))
    return linear;
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
  CheckedMemory result{.storage = holder,
                       .begin = begin,
                       .end = end,
                       .extent = {},
                       .input = stableSummaryPathOf(holder),
                       .pointer = nullptr,
                       .holder = holder};
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
  if (result.storage != input)
    result.input.reset();
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
         !state.moves.recordOf(place) && !state.resources.isEscaped(place) &&
         nullness && nullness->state == core::Nullness::NonNull;
}

void FunctionDataflow::checkedPointerFormation(
    const Expr &at, const Expr &pointer,
    const std::optional<core::Affine> &shift, core::AnalysisState &state) {
  const auto memory =
      shift ? checkedMemory(pointer, *shift, *shift, state) : std::nullopt;
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
  return core::checkSpatialBounds(first, last, size, relation,
                                  {.needAtMost = upper(last),
                                   .haveAtMost = upper(size),
                                   .needAtLeast = lower(last),
                                   .haveAtLeast = lower(size),
                                   .needBoundaryWitness = false},
                                  lower(first))
             .outcome == core::SpatialOutcome::Proven;
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::checkedMemory(const Expr &pointer, const core::Affine &begin,
                                const core::Affine &end,
                                const core::AnalysisState &state) {
  const Expr *value = pointer.IgnoreParenImpCasts();
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
  auto begin = summaryAffineOf(memory.begin);
  auto end = summaryAffineOf(memory.end);
  if (kind == core::CheckedRequirementKind::Valid ||
      kind == core::CheckedRequirementKind::Release) {
    begin = core::PathAffine::ofConstant(0);
    end = core::PathAffine::ofConstant(0);
  } else if (!end) {
    end = checkedLoopRequirement(memory.end, at, state);
    if (end && memory.begin.scale >= 0 && memory.begin.constant >= 0)
      begin = core::PathAffine::ofConstant(0);
    else
      return false;
  }
  if (!begin || !end)
    return false;
  if (recording())
    inferred.checked.require({.kind = kind,
                              .path = *memory.input,
                              .other = {},
                              .begin = *begin,
                              .end = *end,
                              .family = std::move(family)});
  return true;
}

void FunctionDataflow::checkedAccess(const Expr &expr, const PlaceRef &ref,
                                     Role role, core::AnalysisState &state) {
  const bool reads =
      role == Role::Read || role == Role::ReadWrite || role == Role::Consume;
  if (const auto *decl = dyn_cast<DeclRefExpr>(expr.IgnoreParenImpCasts())) {
    const auto *var = dyn_cast<VarDecl>(decl->getDecl());
    if (!var || var->getType()->isArrayType() || role == Role::AddressOf)
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
