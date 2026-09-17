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

#include <array>

using namespace clang;

namespace weavec::analysis {

std::optional<core::SummaryPath>
FunctionDataflow::checkedSeparationInput(const CheckedMemory &memory,
                                         const core::AnalysisState &state) {
  // An immutable backing identity supports separation after a cursor update,
  // but supplies neither current validity nor current capacity (RFC 0029).
  if (memory.holder) {
    if (state.moves.recordOf(*memory.holder) ||
        state.safety->invalidatedPointers.contains(*memory.holder))
      return std::nullopt;
    if (const auto *buffer = bufferFact(*memory.holder, state);
        buffer && buffer->entryBacking)
      return builder.summaryPathOf(*buffer->entryBacking);
    // Changing the logical prefix retires the full buffer predicate while
    // preserving the backing identity. Storage facts are independently
    // invalidated if that pointer, header or capacity is replaced.
    if (const auto backing = state.safety->buffers.storage.find(*memory.holder);
        backing != state.safety->buffers.storage.end() &&
        backing->second.entryBacking &&
        memory.storage == places.deref(*backing->second.entryBacking))
      return builder.summaryPathOf(*backing->second.entryBacking);
  }
  if (memory.input)
    return memory.input;
  return memory.inputPlace ? stableSummaryPathOf(*memory.inputPlace)
                           : std::nullopt;
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::checkedScalarMemory(core::PlaceId place,
                                      const core::AnalysisState &state) {
  if (!state.safety || state.safety->havoc)
    return std::nullopt;
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(place));
  QualType type = decl ? decl->getType() : QualType{};
  if (!places.isBase(place) && places.step(place) == core::PathStep::Deref) {
    const auto *pointer =
        dyn_cast_or_null<ValueDecl>(builder.declFor(*places.parent(place)));
    type = pointer && pointer->getType()->isPointerType()
               ? pointer->getType()->getPointeeType()
               : QualType{};
  } else if (places.isElement(place)) {
    type = arrayElementType(*places.parent(place));
  }
  if (type.isNull() || !type->isIntegerType() || type.isVolatileQualified() ||
      type->isAtomicType())
    return std::nullopt;
  const auto bytes = byteSizeOf(type, context);
  if (!bytes)
    return std::nullopt;
  auto storage = place;
  std::int64_t offset = 0;
  bool selected = false;
  unsigned depth = 0;
  while (!places.isBase(storage)) {
    if (++depth > core::MaxHeapPathDepth)
      return std::nullopt;
    const auto parent = places.parent(storage);
    if (!parent)
      return std::nullopt;
    std::int64_t shift = 0;
    if (places.step(storage) == core::PathStep::Deref) {
      std::int64_t end = 0;
      if (__builtin_add_overflow(offset, *bytes, &end))
        return std::nullopt;
      return checkedMemoryAt(*parent, core::Affine::ofConstant(offset),
                             core::Affine::ofConstant(end), state);
    }
    if (places.step(storage) == core::PathStep::Field) {
      const auto *field = dyn_cast_or_null<FieldDecl>(builder.declFor(storage));
      if (!field || field->isBitField() || field->getParent()->isUnion())
        return std::nullopt;
      shift = static_cast<std::int64_t>(context.getFieldOffset(field) /
                                        context.getCharWidth());
    } else if (places.isElement(storage)) {
      const auto index = core::ArrayIndex::parse(places.fieldName(storage));
      const auto element = arrayElementType(*parent);
      const auto unit =
          element.isNull() ? std::nullopt : byteSizeOf(element, context);
      if (!index || index->symbol || !unit ||
          __builtin_mul_overflow(index->offset, *unit, &shift))
        return std::nullopt;
      selected = true;
    } else if (places.step(storage) == core::PathStep::Index && selected) {
      selected = false;
    } else {
      return std::nullopt;
    }
    if (__builtin_add_overflow(offset, shift, &offset))
      return std::nullopt;
    storage = *parent;
  }
  std::int64_t end = 0;
  if (__builtin_add_overflow(offset, *bytes, &end))
    return std::nullopt;
  return CheckedMemory{.storage = storage,
                       .begin = core::Affine::ofConstant(offset),
                       .end = core::Affine::ofConstant(end),
                       .extent = {},
                       .input = {},
                       .pointer = nullptr};
}

bool FunctionDataflow::checkedZeroInteger(core::PlaceId place,
                                          const core::AnalysisState &state) {
  if (!state.safety || state.safety->havoc || state.safety->memory.empty())
    return false;
  const auto root = places.root(place);
  const auto ranges = state.safety->memory.find(root);
  if (ranges == state.safety->memory.end() ||
      std::ranges::none_of(ranges->second, [](const auto &range) {
        return range.zeroed && !range.source && range.when.trivial();
      }))
    return false;
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(place));
  if (!decl || !decl->getType()->isIntegerType() ||
      decl->getType().isVolatileQualified() || decl->getType()->isAtomicType())
    return false;
  const auto bytes = byteSizeOf(decl->getType(), context);
  if (!bytes)
    return false;
  // RFC 0029: use current byte evidence, never a remembered memset event.
  // This bounded layout walk covers concrete automatic cells only. It does
  // not dereference a pointer or assume that a wildcard denotes element zero.
  auto storage = place;
  std::int64_t offset = 0;
  bool selected = false;
  unsigned depth = 0;
  while (!places.isBase(storage)) {
    if (++depth > core::MaxHeapPathDepth)
      return false;
    const auto parent = places.parent(storage);
    if (!parent)
      return false;
    std::int64_t shift = 0;
    if (places.step(storage) == core::PathStep::Field) {
      const auto *field = dyn_cast_or_null<FieldDecl>(builder.declFor(storage));
      if (!field || field->isBitField() || field->getParent()->isUnion() ||
          field->getType().isVolatileQualified() ||
          field->getType()->isAtomicType())
        return false;
      shift = static_cast<std::int64_t>(context.getFieldOffset(field) /
                                        context.getCharWidth());
    } else if (places.isElement(storage)) {
      const auto index = core::ArrayIndex::parse(places.fieldName(storage));
      const auto type = arrayElementType(*parent);
      const auto unit =
          type.isNull() ? std::nullopt : byteSizeOf(type, context);
      if (!index || index->symbol || index->offset < 0 || !unit ||
          __builtin_mul_overflow(index->offset, *unit, &shift))
        return false;
      selected = true;
    } else if (places.step(storage) == core::PathStep::Index && selected) {
      selected = false;
    } else {
      return false;
    }
    if (__builtin_add_overflow(offset, shift, &offset))
      return false;
    storage = *parent;
  }
  const auto *var = builder.varForPlace(storage);
  if (!var || !var->hasLocalStorage() || var->getType().isVolatileQualified() ||
      var->getType()->isAtomicType())
    return false;
  const auto extent = byteSizeOf(var->getType(), context);
  std::int64_t end = 0;
  if (!extent || __builtin_add_overflow(offset, *bytes, &end) || offset < 0 ||
      end > *extent)
    return false;
  return std::ranges::any_of(ranges->second, [&](const auto &range) {
    return range.zeroed && !range.source && range.when.trivial() &&
           range.begin.isConstant() && range.end.isConstant() &&
           range.begin.constant <= offset && end <= range.end.constant;
  });
}

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
         std::ranges::any_of(
             checkedSpans,
             [&](const auto &entry) { return entry.second == *value.place; }) ||
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
  const auto a = checkedByteExpression(left, state);
  const auto b = checkedByteExpression(right, state);
  if (!a || !b)
    return std::nullopt;
  const auto sum = NumericExpression::operation(core::IntegerOp::Add, *a, *b);
  if (!sum)
    return std::nullopt;
  const bool proved =
      operationDoesNotOverflow(core::IntegerOp::Add, *a, *b, a->type(), state);
  bool required = false;
  if (!proved && options.checkContracts) {
    const auto first = summaryAffineOf(left);
    const auto last = summaryAffineOf(right);
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
  return core::Affine::ofPlace(saved->second, 1, displacement);
}

std::optional<FunctionDataflow::CheckedMemory>
FunctionDataflow::checkedMemoryAt(core::PlaceId holder,
                                  const core::Affine &begin,
                                  const core::Affine &end,
                                  const core::AnalysisState &state) {
  // RFC 0029: *slot can name a local pointer cell, not a second cursor.
  // Resolve only an exact whole-cell access with independently proved
  // validity, initialization and the same ordinary pointer representation.
  if (places.step(holder) == core::PathStep::Deref)
    if (const auto parent = places.parent(holder))
      if (const auto *pointer =
              dyn_cast_or_null<ValueDecl>(builder.declFor(*parent));
          pointer && pointer->getType()->isPointerType()) {
        const auto type = pointer->getType()->getPointeeType();
        const auto bytes = byteSizeOf(type, context);
        if (type->isPointerType() && !type.isVolatileQualified() && bytes) {
          const auto cell = checkedMemoryAt(
              *parent, {}, core::Affine::ofConstant(*bytes), state);
          const auto *variable =
              cell ? dyn_cast_or_null<VarDecl>(builder.declFor(cell->storage))
                   : nullptr;
          if (variable && variable->hasLocalStorage() &&
              places.isBase(cell->storage) &&
              !variable->getType().isVolatileQualified() &&
              ASTContext::hasSameUnqualifiedType(type, variable->getType()) &&
              foldAffine(cell->begin, state) == core::Affine::ofConstant(0) &&
              cell->extent && checkedValid(*cell, state) &&
              checkedInitialized(*cell, state) &&
              checkedInterval(cell->begin, cell->end, *cell->extent, state))
            holder = cell->storage;
        }
      }
  // A borrowed union object's member is the same holder under either name.
  // Canonicalize only definite, bounded storage images, never may-alias values.
  if (!state.safety->unions.members.empty()) {
    std::array<core::PlaceId, core::MaxHeapPathDepth> visited{};
    std::size_t count = 0;
    while (count < visited.size()) {
      if (std::find(visited.begin(),
                    visited.begin() + static_cast<std::ptrdiff_t>(count),
                    holder) !=
          visited.begin() + static_cast<std::ptrdiff_t>(count))
        break;
      visited.at(count++) = holder;
      const auto images = borrowedImages(holder, state);
      if (images.size() != 1 || images.front() == holder)
        break;
      const auto *field =
          dyn_cast_or_null<FieldDecl>(builder.declFor(images.front()));
      if (!field || !field->getParent()->isUnion() ||
          !field->getType()->isPointerType())
        break;
      holder = images.front();
    }
  }
  if (state.safety->invalidatedPointers.contains(holder))
    return CheckedMemory{.storage = places.deref(holder),
                         .begin = begin,
                         .end = end,
                         .extent = {},
                         .input = {},
                         .pointer = nullptr,
                         .holder = holder};
  if (const auto *fact = bufferFact(holder, state)) {
    const auto object = state.safety->objects.find(holder);
    const auto storage = object != state.safety->objects.end()
                             ? object->second
                             : places.deref(holder);
    auto extent = core::Affine::ofPlace(
        fact->capacity, static_cast<std::int64_t>(fact->shape.elementBytes));
    // A container may advertise less than its physical allocation. Folding a
    // logical capacity must not discard a separately proved larger extent.
    if (const auto physical = spatialRecordAt(holder, state);
        physical && physical->extent && physical->offset.isZero() &&
        checkedAtMost(extent, *physical->extent, state))
      extent = *physical->extent;
    const auto input = storage == places.deref(holder) &&
                               !state.safety->replacedPointers.contains(holder)
                           ? stableSummaryPathOf(holder)
                           : std::nullopt;
    const auto entry = input ? std::optional(holder) : std::nullopt;
    return CheckedMemory{
        .storage = storage,
        .begin = begin,
        .end = end,
        .extent = extent,
        // The relational premise describes current storage. Its retained
        // entry identity is generally usable for separation alone.
        .input = {},
        .pointer = nullptr,
        .holder = holder,
        .inputPlace = fact->entryBacking ? fact->entryBacking : entry,
        .validWhenNonempty =
            fact->nonNull ||
            extent == core::Affine::ofPlace(fact->capacity,
                                            static_cast<std::int64_t>(
                                                fact->shape.elementBytes)) ||
            checkedAtMost(core::Affine::ofConstant(1),
                          core::Affine::ofPlace(fact->capacity), state)};
  }
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
                         .inputPlace = position.input,
                         .validWhenNonempty = position.validWhenNonempty};
  }
  auto inputPath = stableSummaryPathOf(holder);
  // RFC 0029: a later parameter assignment does not change the value at this
  // operation. Recover the entry root only while the flow-sensitive domain
  // proves that it has not been replaced on any incoming edge.
  if (!inputPath && !state.safety->replacedPointers.contains(holder)) {
    const auto path = builder.summaryPathOf(holder);
    if (path && path->isParam() && path->isRoot())
      inputPath = path;
  }
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
    if (const auto *field =
            dyn_cast_or_null<FieldDecl>(builder.declFor(result.storage));
        field && field->getParent()->isUnion())
      if (const auto bytes = byteSizeOf(field->getType(), context))
        result.extent = core::Affine::ofConstant(*bytes);
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
  if (memory.validWhenNonempty && memory.extent &&
      !state.safety->invalidatedPointers.contains(place) &&
      !state.moves.recordOf(place) && !state.resources.isEscaped(place) &&
      checkedAtMost(core::Affine::ofConstant(1), *memory.extent, state))
    return true;
  if (const auto *fact = bufferFact(place, state);
      fact && (fact->nonNull ||
               checkedAtMost(core::Affine::ofConstant(1),
                             core::Affine::ofPlace(fact->capacity), state)))
    return true;
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
  auto validity = memory;
  if (!valid && validity && !validity->input && validity->holder) {
    const auto holder = *validity->holder;
    // An empty buffer need not promise a live backing. Pointer arithmetic
    // still needs one, even for an offset of zero. Export that extra premise
    // only for the exact unchanged entry pointer; a replaced backing cannot
    // recover validity from its old allocation's identity.
    if (const auto *fact = bufferFact(holder, state);
        fact && fact->entryBacking == holder &&
        validity->storage == places.deref(holder) &&
        !state.safety->replacedPointers.contains(holder))
      validity->input = stableSummaryPathOf(holder);
  }
  const bool input =
      validity && validity->input &&
      checkedRequire(core::CheckedRequirementKind::Valid, *validity, at, state);
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
  // Pointer-to-pointer casts preserve storage identity (RFC 0004). Inspect
  // arithmetic inside an explicit cast too, retaining the operand's original
  // element size before expressing its offset in mathematical byte units.
  const Expr *value = &PlaceBuilder::stripTransparent(pointer);
  if (const auto *address = dyn_cast<UnaryOperator>(value);
      address && address->getOpcode() == UO_AddrOf) {
    const auto *target = address->getSubExpr()->IgnoreParenImpCasts();
    const bool indirect =
        isa<ArraySubscriptExpr>(target) || isa<MemberExpr>(target) ||
        (isa<UnaryOperator>(target) &&
         cast<UnaryOperator>(target)->getOpcode() == UO_Deref);
    // Keep a const subobject's own identity so a cast cannot turn its mutable
    // enclosing record into write permission for that member (RFC 0018).
    if (indirect && !target->getType().isConstQualified()) {
      // A member address retains its enclosing storage identity, but typed
      // pointer arithmetic is still confined to that member subobject. Its
      // layout alone supplies no evidence that the enclosing storage is live
      // or large enough; checkedLvalue retains those separate obligations.
      if (isa<MemberExpr>(target)) {
        const auto bytes = byteSizeOf(target->getType(), context);
        if (!bytes || !checkedAtMost({}, begin, state) ||
            !checkedAtMost(begin, end, state) ||
            !checkedAtMost(end, core::Affine::ofConstant(*bytes), state))
          return std::nullopt;
      }
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
    if (const auto holder = builder.resolvePointerValue(*value)) {
      auto result = checkedMemoryAt(holder->place, begin, end, state);
      if (result && result->holder &&
          state.safety->positions.contains(*result->holder)) {
        result->pointer = &pointer;
        return result;
      }
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
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(result.storage))) {
      const auto *field = dyn_cast<FieldDecl>(decl);
      if (isa<VarDecl>(decl) || (field && field->getParent()->isUnion()))
        if (const auto size = byteSizeOf(decl->getType(), context))
          result.extent = core::Affine::ofConstant(*size);
    }
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
  if (memory.holder)
    if (const auto *fact = bufferFact(*memory.holder, state);
        fact && !fact->shape.reader)
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
        // RFC 0018: an identified pointer variable is a writable cell too.
        // Its pointee still needs separate permission. A direct lvalue or a
        // different holder identifying this object denotes the cell itself.
        if (isa<VarDecl>(decl) &&
            (!type->isPointerType() ||
             (place == memory.storage &&
              (!memory.holder || *memory.holder != memory.storage) &&
              !places.innermostDeref(place))))
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
  for (const auto &[holder, storage] : state.safety->objects) {
    if (storage != memory.storage)
      continue;
    const auto owner = state.resources.recordOf(holder);
    if (!owner || owner->origin != core::ResourceOrigin::Allocated ||
        owner->family != "free" || owner->escaped)
      continue;
    auto guard = owner->guard;
    const auto allocation = checkedMemoryAt(holder, {}, {}, state);
    if (pruneGuard(guard, state) && guard.trivial() && allocation &&
        allocation->storage == memory.storage &&
        checkedValid(*allocation, state))
      return true;
  }
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
  if (memory.holder)
    if (const auto *fact = bufferFact(*memory.holder, state);
        fact && fact->initialized &&
        checkedInterval(
            memory.begin, memory.end,
            core::Affine::ofPlace(
                fact->shape.reader ? fact->capacity : fact->length,
                static_cast<std::int64_t>(fact->shape.elementBytes)),
            state))
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
  if (const auto witness = checkedWitness(memory, state))
    if (const auto through = witness->zero.shifted(1);
        through &&
        checkedInterval(memory.begin, *through, *memory.extent, state))
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
  auto input = memory.input;
  if (!input && memory.holder &&
      (kind == core::CheckedRequirementKind::Extent ||
       kind == core::CheckedRequirementKind::Initialized) &&
      bufferFact(*memory.holder, state) &&
      memory.storage == places.deref(*memory.holder) &&
      !state.safety->replacedPointers.contains(*memory.holder))
    input = stableSummaryPathOf(*memory.holder);
  if (!input)
    return false;
  if (kind == core::CheckedRequirementKind::Extent && memory.inputPlace &&
      bufferFact(*memory.inputPlace, state)) {
    const auto backing = checkedMemoryAt(*memory.inputPlace, {}, {}, state);
    if (backing && backing->storage == memory.storage && backing->extent &&
        checkedInterval(memory.begin, memory.end, *backing->extent, state))
      return true;
  }
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
  } else if (kind == core::CheckedRequirementKind::TerminatedWithin) {
    // Both bounds name entry values. Widening or resetting the start of a
    // terminated prefix could include an earlier, uninitialized interval.
    if (!begin || !end || !checkedAtMost({}, memory.begin, state))
      return false;
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
                              .path = *input,
                              .other = {},
                              .begin = *begin,
                              .end = *end,
                              .family = std::move(family),
                              .when = condition});
  return true;
}

void FunctionDataflow::checkedAccess(const Expr &expr, const PlaceRef &ref,
                                     Role role, core::AnalysisState &state) {
  // Intermediate member operands have Role::Ignore in the ordinary walk.
  // Loading a union pointer to reach a pointee still reads that member.
  for (const auto &deref : ref.derefs)
    if (deref.expression)
      checkedUnionAccess(*deref.expression, Role::Read, state);
  checkedUnionAccess(expr, role, state);
  // RFC 0022: the function designator itself is not object memory.
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
