//===- DataflowSafetyMemory.cpp - Positive memory evidence (RFC 0018) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"

using namespace clang;

namespace weavec::analysis {

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
  const auto origin = builder.classifyValue(pointer);
  bool valid = origin.kind == ValueOrigin::Kind::Borrow;
  if (origin.kind == ValueOrigin::Kind::Copy && origin.place) {
    const auto place = origin.place->place;
    const auto nullness = nullnessAt(place, state);
    valid = state.safety->pointers.contains(place) &&
            !state.moves.recordOf(place) && !state.resources.isEscaped(place) &&
            nullness && nullness->state == core::Nullness::NonNull;
  }
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
  auto origin = builder.classifyValue(pointer);
  CheckedMemory result{.storage = {},
                       .begin = begin,
                       .end = end,
                       .extent = {},
                       .input = {},
                       .pointer = &pointer};
  if (!origin.place)
    return std::nullopt;
  result.storage = origin.place->place;
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
  if (result.input && (!result.input->isParam() || !result.input->isRoot()))
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
  const auto resource = state.resources.recordOf(memory.storage);
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
  const auto found = state.safety->memory.find(memory.storage);
  if (found == state.safety->memory.end())
    return false;
  return std::ranges::any_of(found->second, [&](const auto &range) {
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

bool FunctionDataflow::checkedRequire(core::CheckedRequirementKind kind,
                                      const CheckedMemory &memory,
                                      const Stmt &at,
                                      core::AnalysisState &state,
                                      std::string family) {
  if (options.deferCheckedCalls &&
      state.safety->deferred.contains(memory.storage)) {
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
    if (reads)
      safetyObligation(core::SafetyProperty::Initialization,
                       var->hasGlobalStorage() ||
                               state.safety->initialized.contains(ref.place) ||
                               (storage && checkedInitialized(*storage, state))
                           ? core::SafetyOutcome::Proven
                           : core::SafetyOutcome::Unresolved,
                       expr, "value", "local value must be initialized");
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
  if (memory->pointer) {
    const auto origin = builder.classifyValue(*memory->pointer);
    bool valid = origin.kind == ValueOrigin::Kind::Borrow;
    if (origin.kind == ValueOrigin::Kind::Copy && origin.place) {
      const auto place = origin.place->place;
      const auto nullness = nullnessAt(place, state);
      valid = state.safety->pointers.contains(place) &&
              !state.moves.recordOf(place) &&
              !state.resources.isEscaped(place) && nullness &&
              nullness->state == core::Nullness::NonNull;
    }
    const bool required =
        (memory->input || state.safety->deferred.contains(memory->storage)) &&
        checkedRequire(core::CheckedRequirementKind::Valid, *memory, expr,
                       state);
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
