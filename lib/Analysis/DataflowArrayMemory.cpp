//===- DataflowArrayMemory.cpp - Simultaneous array storage copies --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "weavec/Analysis/Allocators.h"
#include "weavec/Core/Array.h"

#include <utility>

using namespace clang;

namespace weavec::analysis {

std::optional<FunctionDataflow::ArrayBuffer>
FunctionDataflow::arrayBuffer(const Expr &expr, core::AnalysisState &state) {
  const Expr &value = *expr.IgnoreParenImpCasts();
  const auto zero = core::Affine::ofConstant(0);
  if (const auto *array = value.getType()->getAsArrayTypeUnsafe()) {
    const auto ref = builder.resolve(value);
    if (!ref)
      return std::nullopt;
    return ArrayBuffer{.storage = places.index(ref->place),
                       .element = array->getElementType(),
                       .start = zero,
                       .explicitArray = true};
  }
  if (const auto *address = dyn_cast<UnaryOperator>(&value);
      address && address->getOpcode() == UO_AddrOf) {
    const auto &operand = *address->getSubExpr()->IgnoreParenImpCasts();
    if (operand.getType()->isArrayType())
      return arrayBuffer(operand, state);
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(&operand)) {
      auto buffer = arrayBuffer(*subscript->getBase(), state);
      const auto index =
          foldAffine(builder.affineOf(*subscript->getIdx()), state);
      if (!buffer || !index)
        return std::nullopt;
      const auto start = sumOf(buffer->start, *index);
      if (!start)
        return std::nullopt;
      buffer->start = *start;
      buffer->explicitArray = true;
      return buffer;
    }
    // Individual pointer/record objects stay RFC 0014 complete copies.
    return std::nullopt;
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(&value)) {
    const auto *pointer = PlaceBuilder::pointerOperandOfArithmetic(value);
    if (!pointer)
      return std::nullopt;
    auto buffer = arrayBuffer(*pointer, state);
    auto index = foldAffine(
        builder.affineOf(*(pointer == binary->getLHS() ? binary->getRHS()
                                                       : binary->getLHS())),
        state);
    if (binary->getOpcode() == BO_Sub && index)
      index = index->times(-1);
    if (!buffer || !index)
      return std::nullopt;
    const auto start = sumOf(buffer->start, *index);
    if (!start)
      return std::nullopt;
    buffer->start = *start;
    buffer->explicitArray = true;
    return buffer;
  }
  if (!value.getType()->isPointerType())
    return std::nullopt;
  const auto ref = builder.resolvePointerValue(value);
  if (!ref)
    return std::nullopt;
  auto storage = places.deref(ref->place);
  auto start = zero;
  for (const auto &loan : state.loans.heldBy(ref->place)) {
    if (places.step(loan.place) != core::PathStep::Index ||
        !places.fieldName(loan.place).empty())
      continue;
    const auto spatial = state.spatial.recordOf(ref->place);
    if (spatial && !spatial->offset.isZero() && !spatial->offset.isElements())
      return std::nullopt;
    storage = loan.place;
    if (spatial)
      start = core::Affine::ofConstant(spatial->offset.elements);
    return ArrayBuffer{.storage = storage,
                       .element = value.getType()->getPointeeType(),
                       .start = start,
                       .explicitArray = true};
  }
  for (const auto &[alias, edge] :
       state.definiteAliases.edgesFrom(ref->place)) {
    if (alias >= ref->place ||
        (!edge.offset.isZero() && !edge.offset.isElements()))
      continue;
    if (const auto offset = state.definiteAliases.offsetOf(alias, ref->place)) {
      storage = places.deref(alias);
      start = core::Affine::ofConstant(offset->elements);
      break;
    }
  }
  return ArrayBuffer{.storage = storage,
                     .element = value.getType()->getPointeeType(),
                     .start = start,
                     .explicitArray = false};
}

void FunctionDataflow::copyArrayCell(core::PlaceId dest, core::PlaceId source,
                                     QualType type, const CallExpr &at,
                                     core::AnalysisState &state) {
  if (dest == source)
    return;
  if (places.isElement(dest))
    if (const auto index = core::ArrayIndex::parse(places.fieldName(dest)))
      for (auto &[id, range] : state.releasedArrayRanges) {
        (void)id;
        if (places.parent(dest) == range.storage)
          range.materialized.insert(*index);
      }
  recordAccess(dest, true, state);
  if (type->isPointerType()) {
    std::vector<core::PlaceId> shareHolders;
    if (const auto resource = state.resources.recordOf(source);
        resource && (resource->shares >= 2 ||
                     resource->origin == core::ResourceOrigin::Retained)) {
      // An input snapshot is not another owner. If assignment transfers
      // its surplus share, retire that obligation from the program holder
      // whose value was frozen as well (RFC 0010 / RFC 0015).
      for (const auto &[alias, edge] : state.aliases.edgesFrom(source))
        if (edge.exact() && edge.sameShare && alias != dest &&
            !pointerSnapshots.contains(places.root(alias)) &&
            state.resources.recordOf(alias) == resource)
          shareHolders.push_back(alias);
    }
    ValueOrigin origin;
    origin.kind = ValueOrigin::Kind::Copy;
    origin.place = PlaceRef{.place = source,
                            .derefs = {},
                            .derefExprs = {},
                            .derefElements = {},
                            .element = {}};
    noteRewritten(dest, state);
    noteOverwritten(dest, state);
    applyPointerAssign(dest, origin, at,
                       type->getPointeeType().isConstQualified(), state);
    for (const auto holder : shareHolders)
      state.resources.release(holder);
    applyHeapValue(dest, origin, state);
  } else {
    const auto storage = storageOf(dest);
    const std::set<core::PlaceId> overwritten(storage.begin(), storage.end());
    checkLeaks(
        storage,
        [&overwritten](core::PlaceId cell) {
          return overwritten.contains(cell);
        },
        LeakForm::Overwritten, locate(at), state);
    noteRewritten(dest, state);
    noteOverwritten(dest, state);
    copyRecordPlaces(dest, source, state);
    for (const auto field : storageOf(dest))
      if (field != dest)
        noteCalleeStore(field, at, state);
  }
}

bool FunctionDataflow::handleArrayCopy(const CallExpr &call,
                                       const CallEffects &effects,
                                       core::AnalysisState &state) {
  auto dest = arrayBuffer(*call.getArg(0), state);
  auto source = arrayBuffer(*call.getArg(1), state);
  if (!dest || !source ||
      (!source->element->isPointerType() && !source->element->isRecordType()))
    return false;
  auto bytes = foldAffine(builder.affineOf(*call.getArg(2)), state);
  const auto size = byteSizeOf(source->element, context);
  if (!size ||
      !ASTContext::hasSameUnqualifiedType(dest->element, source->element))
    return false;
  if (!bytes || bytes->constant % *size != 0 ||
      (bytes->place && bytes->scale % *size != 0))
    return false;
  core::Affine elements = *bytes;
  elements.constant /= *size;
  if (elements.place)
    elements.scale /= *size;
  if (!bytes->isConstant() || !source->start.isConstant() ||
      !dest->start.isConstant() ||
      std::cmp_greater(elements.constant, core::MaxArrayCells)) {
    checkRequiredArguments(call, *effects.summary, state);
    checkRequiredExtents(call, *effects.summary, state);
    return installArrayRange(*dest, *source, elements, call, state);
  }
  if (bytes->constant == 0)
    return true;
  if (bytes->constant < 0 || bytes->constant % *size != 0)
    return false;
  const auto count = bytes->constant / *size;
  if (std::cmp_greater(count, core::MaxArrayCells))
    return false;
  if (count == 1 && !source->explicitArray && !dest->explicitArray)
    return false;
  checkRequiredArguments(call, *effects.summary, state);
  checkRequiredExtents(call, *effects.summary, state);
  doMutationCheck(dest->storage, call, state);
  arrayTypes[source->storage] = source->element;
  arrayTypes[dest->storage] = dest->element;
  std::vector<std::pair<core::PlaceId, core::PlaceId>> cells;
  for (std::int64_t i = 0; i < count; ++i) {
    const auto srcIndex = source->start.shifted(i);
    const auto dstIndex = dest->start.shifted(i);
    if (!srcIndex || !dstIndex)
      return false;
    PlaceRef src{.place = source->storage,
                 .derefs = {},
                 .derefExprs = {},
                 .derefElements = {},
                 .element = {}};
    PlaceRef dst{.place = dest->storage,
                 .derefs = {},
                 .derefExprs = {},
                 .derefElements = {},
                 .element = {}};
    src = selectArrayElement(src, srcIndex, source->element, call);
    dst = selectArrayElement(dst, dstIndex, dest->element, call);
    if (!src.element.isWhole() || !dst.element.isWhole())
      return false;
    recordAccess(src.place, false, state);
    const auto key = std::pair{&call, i};
    auto snapshot = arrayCopySnapshots.find(key);
    if (snapshot == arrayCopySnapshots.end()) {
      snapshot =
          arrayCopySnapshots.emplace(key, places.create("array-copy-input"))
              .first;
      pointerSnapshots.insert(snapshot->second);
    }
    snapshotArrayCell(src.place, snapshot->second, source->element, state);
    cells.emplace_back(dst.place, snapshot->second);
  }
  for (const auto &[destination, snapshot] : cells)
    copyArrayCell(destination, snapshot, dest->element, call, state);
  for (const auto &[destination, snapshot] : cells) {
    (void)destination;
    for (const auto child : places.descendants(snapshot))
      state.forget(child);
    state.forget(snapshot);
  }
  return true;
}

void FunctionDataflow::captureArrayReallocation(const CallExpr &call,
                                                const CallEffects &effects,
                                                core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  if (!callee || effects.source != SummarySource::Builtin ||
      call.getNumArgs() < 2)
    return;
  const auto name = callee->getName();
  if (name != "realloc" && name != "reallocarray")
    return;
  const auto source = builder.resolvePointerValue(*call.getArg(0));
  if (!source)
    return;
  const auto storage = places.deref(source->place);
  const auto type = arrayElementType(storage);
  if (type.isNull() || (!type->isPointerType() && !type->isRecordType()))
    return;
  auto it = arrayReallocInputs.find(&call);
  if (it == arrayReallocInputs.end())
    it = arrayReallocInputs.emplace(&call, places.create("array-realloc-input"))
             .first;
  pointerSnapshots.insert(it->second);
  arrayTypes[it->second] = type;
  arrayTypes[storage] = type;
  for (const auto cell : places.descendants(storage)) {
    if (places.parent(cell) != storage || !places.isElement(cell))
      continue;
    const auto selector = core::ArrayIndex::parse(places.fieldName(cell));
    if (!selector)
      continue;
    materializeArrayCell(storage, cell, *selector, type, call, state);
    snapshotArrayCell(cell, places.element(it->second, selector->toString()),
                      type, state);
  }
}

void FunctionDataflow::applyArrayReallocation(core::PlaceId dest,
                                              const CallExpr &call,
                                              core::AnalysisState &state) {
  const auto input = arrayReallocInputs.find(&call);
  if (input == arrayReallocInputs.end())
    return;
  const auto type = arrayTypes.at(input->second);
  const auto storage = places.deref(dest);
  arrayTypes[storage] = type;
  const auto size = byteSizeOf(type, context);
  const auto spatial = state.spatial.recordOf(dest);
  const auto extent =
      spatial ? foldAffine(spatial->extent, state) : std::nullopt;
  const auto source = builder.resolvePointerValue(*call.getArg(0));
  const auto oldStorage =
      source ? std::optional(places.deref(source->place)) : std::nullopt;
  for (const auto cell : places.descendants(input->second)) {
    if (places.parent(cell) != input->second || !places.isElement(cell))
      continue;
    const auto index = core::ArrayIndex::parse(places.fieldName(cell));
    // A known truncation does not publish discarded cells into new storage.
    if (index && !index->symbol && size && extent && extent->isConstant() &&
        index->offset >= extent->constant / *size) {
      if (oldStorage) {
        const auto dying = [&](core::PlaceId holder) {
          return places.isDescendantOf(holder, *oldStorage) ||
                 places.isDescendantOf(holder, input->second);
        };
        for (const auto value : storageOf(cell)) {
          const auto resource = state.resources.recordOf(value);
          if (!resource || !resourceLost(value, *resource, dying, state))
            continue;
          const auto original =
              places.translate(value, input->second, *oldStorage);
          reportLeak(original, *resource,
                     "'" + nameOf(original) +
                         "' is leaked when reallocation discards the element",
                     locate(call));
        }
      }
      continue;
    }
    const auto target =
        index ? boundedArrayCell(storage, *index, call, state) : std::nullopt;
    if (!target)
      continue;
    if (type->isRecordType())
      copyRecordPlaces(*target, cell, state);
    else
      copyHeapValue(cell, *target, state);
  }
}

} // namespace weavec::analysis
