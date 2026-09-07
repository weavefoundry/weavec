//===- DataflowArrays.cpp - Array cells and container operations ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "weavec/Core/Array.h"

#include "clang/AST/Type.h"

using namespace clang;

namespace weavec::analysis {

static bool hasPointerCells(QualType type, unsigned depth = 0) {
  if (type.isNull())
    return true; // A serialized selected path already denotes array storage.
  if (depth > core::MaxHeapPathDepth)
    return false;
  if (type->isPointerType())
    return true;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return hasPointerCells(array->getElementType(), depth + 1);
  if (const auto *record = type->getAsRecordDecl())
    for (const auto *field : record->fields())
      if (hasPointerCells(field->getType(), depth + 1))
        return true;
  return false;
}

std::optional<std::string>
FunctionDataflow::summaryArrayIndex(std::string_view selector) {
  auto index = core::ArrayIndex::parse(selector);
  if (!index)
    return std::nullopt;
  if (!index->symbol)
    return index->toString();
  const core::PlaceId place{*index->symbol};
  std::optional<core::SummaryPath> path;
  if (const auto snapshot = snapshotInputPaths.find(place);
      snapshot != snapshotInputPaths.end())
    path = snapshot->second;
  else if (const auto *param =
               dyn_cast_or_null<ParmVarDecl>(builder.varForPlace(place));
           param && !paramReassigned[param->getFunctionScopeIndex()])
    path = core::SummaryPath::param(param->getFunctionScopeIndex());
  if (!path || !path->isParam() || !path->isRoot())
    return std::nullopt;
  index->symbol = path->index;
  return index->toString();
}

std::optional<core::PlaceId>
FunctionDataflow::boundedArrayCell(core::PlaceId storage,
                                   const core::ArrayIndex &index,
                                   const Expr &at, core::AnalysisState &state) {
  const std::string key = index.toString();
  if (const auto existing = places.child(storage, core::PathStep::Index, key))
    return existing;
  std::size_t cells = 0;
  for (const auto cell : places.descendants(storage))
    if (places.parent(cell) == storage && places.isElement(cell))
      ++cells;
  if (cells >= core::MaxArrayCells) {
    state.incompleteHeap.insert(storage);
    reportIncomplete("array element limit reached", at);
    return std::nullopt;
  }
  return places.element(storage, key);
}

PlaceRef FunctionDataflow::selectArrayElement(PlaceRef storage,
                                              std::optional<core::Affine> index,
                                              QualType type, const Expr &at) {
  if (!hasPointerCells(type))
    return storage;
  // A helper called with &a[k] starts selection at k. The address already
  // denotes a cell, whereas a decayed array denotes its storage summary.
  if (places.isElement(storage.place)) {
    const auto base = core::ArrayIndex::parse(places.fieldName(storage.place));
    if (base && index) {
      const auto start =
          base->symbol ? core::Affine::ofPlace(core::PlaceId{*base->symbol}, 1,
                                               base->offset)
                       : core::Affine::ofConstant(base->offset);
      index = sumOf(start, *index);
      storage.place = *places.parent(storage.place);
    }
  }
  // A plain dereference of a pointer-to-pointer is also the conventional
  // out-parameter spelling. Keep that scalar cell until its storage is
  // actually used as an array; explicit selection opts into RFC 0015.
  bool derivedArray = false;
  if (currentState && places.step(storage.place) == core::PathStep::Deref) {
    const auto pointer = *places.parent(storage.place);
    derivedArray = std::ranges::any_of(
        currentState->loans.heldBy(pointer), [this](const core::Loan &loan) {
          return places.step(loan.place) == core::PathStep::Index &&
                 places.fieldName(loan.place).empty();
        });
    if (const auto spatial = currentState->spatial.recordOf(pointer))
      derivedArray |= spatial->offset.isElements() && !spatial->offset.isZero();
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(&at);
      unary && unary->getOpcode() == UO_Deref &&
      !unary->getSubExpr()->IgnoreParenImpCasts()->getType()->isArrayType() &&
      !PlaceBuilder::pointerOperandOfArithmetic(
          *unary->getSubExpr()->IgnoreParenImpCasts()) &&
      !arrayTypes.contains(storage.place) && !derivedArray)
    return storage;
  if (!type.isNull())
    arrayTypes[storage.place] = type;
  if (!currentState)
    return storage;
  auto &state = *currentState;
  if (places.step(storage.place) == core::PathStep::Deref) {
    const auto pointer = *places.parent(storage.place);
    bool resolvedLoan = false;
    for (const auto &loan : state.loans.heldBy(pointer)) {
      if (places.step(loan.place) != core::PathStep::Index ||
          !places.fieldName(loan.place).empty())
        continue;
      const auto spatial = state.spatial.recordOf(pointer);
      if (spatial &&
          (!spatial->offset.isZero() && !spatial->offset.isElements()))
        index = std::nullopt;
      else if (spatial && index)
        index = index->shifted(spatial->offset.elements);
      storage.place = loan.place;
      resolvedLoan = true;
      break;
    }
    if (!resolvedLoan)
      for (const auto &[alias, edge] :
           state.definiteAliases.edgesFrom(pointer)) {
        if (alias >= pointer ||
            (!edge.offset.isZero() && !edge.offset.isElements()))
          continue;
        const auto offset = state.definiteAliases.offsetOf(alias, pointer);
        if (index && offset)
          index = index->shifted(offset->elements);
        storage.place = places.deref(alias);
        break;
      }
  }
  index = foldAffine(index, state);
  // Prefer a stable equal value, including a saved index, to the spelling
  // of the current local. Only exact affine equalities justify this choice.
  if (index && index->place && index->scale == 1) {
    auto best = *index;
    for (const auto &[pair, edge] : state.relations.all()) {
      if (edge.relation != core::Relation::Equal)
        continue;
      const auto other = pair.first == *index->place ? pair.second : pair.first;
      if (pair.first != *index->place && pair.second != *index->place)
        continue;
      if (other >= *best.place)
        continue;
      const auto relation = state.relations.edgeBetween(*index->place, other);
      if (relation) {
        auto translated =
            core::Affine::ofPlace(other).shifted(relation->offset);
        if (translated)
          translated = translated->shifted(index->constant);
        if (translated)
          best = *translated;
      }
    }
    index = foldAffine(best, state);
  }
  if (!index || (index->place && index->scale != 1)) {
    state.incompleteHeap.insert(storage.place);
    reportIncomplete("unresolved array element selection", at);
    storage.element = core::ElementWitness::unknown();
    return storage;
  }
  const auto selector =
      index->place
          ? core::ArrayIndex::variable(index->place->value, index->constant)
          : core::ArrayIndex::constant(index->constant);
  // A saved scalar and its immutable snapshot can have different place
  // numbers while denoting the same entry value. Reuse a represented cell
  // only under an established equality, never from a may-alias.
  if (selector.symbol)
    for (const auto cell : places.descendants(storage.place)) {
      if (places.parent(cell) != storage.place || !places.isElement(cell))
        continue;
      const auto known = core::ArrayIndex::parse(places.fieldName(cell));
      if (!known || !known->symbol || !selector.symbol ||
          known->symbol == selector.symbol)
        continue;
      const auto equal = state.relations.edgeBetween(
          core::PlaceId{*selector.symbol}, core::PlaceId{*known->symbol});
      const auto shifted =
          equal ? selector.shifted(equal->offset) : std::nullopt;
      if (equal && equal->relation == core::Relation::Equal && shifted &&
          shifted->offset == known->offset) {
        storage.place = cell;
        storage.element = core::ElementWitness::whole();
        materializeArrayFill(*places.parent(cell), cell, *known, at, state);
        materializeArrayCell(*places.parent(cell), cell, *known, type, at,
                             state);
        materializeArrayRelease(*places.parent(cell), cell, *known, at, state);
        return storage;
      }
    }
  const auto selected = boundedArrayCell(storage.place, selector, at, state);
  if (!selected) {
    storage.element = core::ElementWitness::unknown();
    return storage;
  }
  const auto array = storage.place;
  storage.place = *selected;
  storage.element = core::ElementWitness::whole();
  materializeArrayFill(array, storage.place, selector, at, state);
  materializeArrayCell(array, storage.place, selector, type, at, state);
  materializeArrayRelease(array, storage.place, selector, at, state);
  return storage;
}

void FunctionDataflow::snapshotArrayIndex(core::PlaceId place, const Expr *at,
                                          core::AnalysisState &state) {
  std::vector<std::pair<core::PlaceId, core::ArrayIndex>> cells;
  const auto tracked = [&state](core::PlaceId cell) {
    return state.moves.recordOf(cell) || state.resources.holds(cell) ||
           state.incoming.contains(cell) || state.nulls.recordOf(cell) ||
           state.callTargets.contains(cell);
  };
  for (std::uint32_t id = 0; id < places.size(); ++id) {
    const core::PlaceId cell{id};
    if (!places.isElement(cell))
      continue;
    const auto index = core::ArrayIndex::parse(places.fieldName(cell));
    if (index && index->symbol == place.value &&
        (tracked(cell) ||
         std::ranges::any_of(places.descendants(cell), tracked)))
      cells.emplace_back(cell, *index);
  }
  const bool usedByRange =
      std::ranges::any_of(state.arrayRanges, [place](const auto &entry) {
        const auto &range = entry.second;
        return range.span.count.place == place ||
               range.span.begin.symbol == place.value ||
               range.sourceBegin.symbol == place.value;
      });
  const bool usedByRelease = std::ranges::any_of(
      state.releasedArrayRanges, [place](const auto &entry) {
        return entry.second.span.count.place == place ||
               entry.second.span.begin.symbol == place.value;
      });
  const bool usedByFill =
      std::ranges::any_of(state.filledArrayRanges, [place](const auto &entry) {
        return entry.second.count.place == place;
      });
  if (cells.empty() && !usedByRange && !usedByRelease && !usedByFill)
    return;
  const auto key = std::pair{place, at};
  auto it = arrayIndexSnapshots.find(key);
  if (it == arrayIndexSnapshots.end()) {
    const auto snapshot = places.create("array-index(" + nameOf(place) + ")");
    it = arrayIndexSnapshots.emplace(key, snapshot).first;
    snapshotPlaces.insert(snapshot);
    if (const auto *param =
            dyn_cast_or_null<ParmVarDecl>(builder.varForPlace(place));
        param && !paramReassigned[param->getFunctionScopeIndex()])
      snapshotInputPaths[snapshot] =
          core::SummaryPath::param(param->getFunctionScopeIndex());
  }
  const auto snapshot = it->second;
  state.relations.forget(snapshot);
  if (const auto fact = state.scalars.factOf(place))
    state.scalars.set(snapshot, *fact);
  const auto relations = state.relations.all();
  for (const auto &[pair, edge] : relations) {
    if (pair.first == place)
      state.relations.learn(snapshot, edge.relation, pair.second, edge.offset);
    else if (pair.second == place)
      state.relations.learn(pair.first, edge.relation, snapshot, edge.offset);
  }
  for (auto &[id, range] : state.arrayRanges) {
    (void)id;
    if (range.span.count.place == place)
      range.span.count.place = snapshot;
    if (range.span.begin.symbol == place.value)
      range.span.begin.symbol = snapshot.value;
    if (range.sourceBegin.symbol == place.value)
      range.sourceBegin.symbol = snapshot.value;
  }
  for (auto &[id, range] : state.releasedArrayRanges) {
    (void)id;
    if (range.span.count.place == place)
      range.span.count.place = snapshot;
    if (range.span.begin.symbol == place.value)
      range.span.begin.symbol = snapshot.value;
  }
  for (auto &[id, range] : state.filledArrayRanges) {
    (void)id;
    if (range.count.place == place)
      range.count.place = snapshot;
  }
  for (auto [cell, index] : cells) {
    index.symbol = snapshot.value;
    const auto array = *places.parent(cell);
    const auto selectorKey = index.toString();
    const auto existing =
        places.child(array, core::PathStep::Index, selectorKey);
    const auto children = places.descendants(array);
    const auto count =
        std::ranges::count_if(children, [&](core::PlaceId child) {
          return places.parent(child) == array && places.isElement(child);
        });
    if (!existing && std::cmp_greater_equal(count, core::MaxArrayCells)) {
      state.incompleteHeap.insert(array);
      if (at)
        reportIncomplete("array index snapshot element limit reached", *at);
      // The old value must remain possibly consumed even if a later write
      // reinitializes the same syntactic selector with the new index value.
      auto moved = state.moves.recordOf(cell);
      if (!moved)
        for (const auto child : places.descendants(cell))
          if (const auto record = state.moves.recordOf(child)) {
            moved = record;
            break;
          }
      if (moved)
        state.moves.markMoved(array, moved->reason, moved->location, moved->via,
                              core::ElementWitness::unknown(), moved->family,
                              moved->ownValue, moved->guard);
      continue;
    }
    const auto old = existing.value_or(places.element(array, selectorKey));
    const auto previous = state.moves.recordOf(old);
    copyHeapValue(cell, old, state);
    if (previous) {
      state.moves.markMoved(old, previous->reason, previous->location,
                            previous->via, previous->element, previous->family,
                            previous->ownValue, previous->guard);
      if (at)
        reportIncomplete("array index snapshot generation is ambiguous", *at);
    }
    state.forget(cell);
    for (const auto child : places.descendants(cell))
      state.forget(child);
  }
}

void FunctionDataflow::initializeArray(core::PlaceId storage, QualType type,
                                       const Expr *init, const VarDecl &decl,
                                       core::AnalysisState &state,
                                       bool zeroInitialize) {
  const auto *array = context.getAsConstantArrayType(type);
  if (!array || !hasPointerCells(array->getElementType()))
    return;
  const auto count = array->getSize().getLimitedValue(core::MaxArrayCells + 1);
  const auto summary = places.index(storage);
  arrayTypes[summary] = array->getElementType();
  const auto *list =
      init ? dyn_cast<InitListExpr>(init->IgnoreParenImpCasts()) : nullptr;
  const auto limit =
      std::min(count, static_cast<std::uint64_t>(core::MaxArrayCells));
  for (std::uint64_t i = 0; i < limit; ++i) {
    const auto cell = places.element(summary, std::to_string(i));
    const Expr *value = list && i < list->getNumInits()
                            ? list->getInit(static_cast<unsigned>(i))
                            : nullptr;
    initializeArrayValue(cell, array->getElementType(), value, decl, state,
                         zeroInitialize || (init != nullptr) ||
                             decl.hasGlobalStorage());
  }
  if (count > core::MaxArrayCells && init)
    reportIncomplete("array initializer exceeds element limit", *init);
}

void FunctionDataflow::initializeArrayValue(core::PlaceId cell, QualType type,
                                            const Expr *value,
                                            const VarDecl &decl,
                                            core::AnalysisState &state,
                                            bool zeroInitialize) {
  if (places.depth(cell) >= PlaceBuilder::MaxPlaceDepth) {
    state.incompleteHeap.insert(cell);
    if (value)
      reportIncomplete("array initializer exceeds path limit", *value);
    return;
  }
  if (type->isArrayType()) {
    initializeArray(cell, type, value, decl, state, zeroInitialize);
    return;
  }
  if (const auto *record = type->getAsRecordDecl()) {
    const auto *list =
        value ? dyn_cast<InitListExpr>(value->IgnoreParenImpCasts()) : nullptr;
    if (value && !list && !isa<ImplicitValueInitExpr>(value)) {
      copyRecord(cell, *value, state);
      return;
    }
    if (list && !list->isSemanticForm())
      list = list->getSemanticForm();
    unsigned next = 0;
    for (const auto *field : record->fields()) {
      if (field->isUnnamedBitField())
        continue;
      if (record->isUnion() && list &&
          field != list->getInitializedFieldInUnion())
        continue;
      const Expr *fieldValue =
          list && next < list->getNumInits() ? list->getInit(next) : nullptr;
      ++next;
      initializeArrayValue(builder.fieldPlace(cell, *field), field->getType(),
                           fieldValue, decl, state, zeroInitialize);
      if (record->isUnion())
        break;
    }
    return;
  }
  if (type->isPointerType()) {
    if (value && !isa<ImplicitValueInitExpr>(value)) {
      const auto origin = builder.classifyValue(*value);
      applyPointerAssign(cell, origin, *value,
                         type->getPointeeType().isConstQualified(), state);
      applyHeapValue(cell, origin, state);
    } else if (zeroInitialize) {
      state.moves.reinitialize(cell);
      state.resources.markNull(cell);
      state.nulls.set(cell, {.state = core::Nullness::Null,
                             .location = locate(decl.getLocation()),
                             .reason = core::NullReason::AssignedNull,
                             .detail = {}});
      if (type->isFunctionPointerType())
        state.callTargets[cell] = {
            .functions = {}, .unknown = false, .null = true};
    } else {
      state.moves.markMoved(cell, core::MoveReason::Uninitialized,
                            locate(decl.getLocation()));
    }
  } else if (type->isIntegerType()) {
    if (value && !isa<ImplicitValueInitExpr>(value))
      assignScalar(cell, value, state);
    else if (zeroInitialize)
      state.scalars.set(cell, core::ValueFact::ofConstant(0));
  }
}

void FunctionDataflow::weakenOverlappingArrayWrites(
    core::PlaceId dest, const ValueOrigin &origin, const Expr &at,
    bool constPointee, core::AnalysisState &state) {
  auto selected = std::optional(dest);
  while (selected && !places.isElement(*selected))
    selected = places.parent(*selected);
  if (!selected)
    return;
  const auto array = *places.parent(*selected);
  const auto index = core::ArrayIndex::parse(places.fieldName(*selected));
  if (!index)
    return;
  for (const auto other : places.descendants(array)) {
    if (places.parent(other) != array || !places.isElement(other) ||
        other == *selected)
      continue;
    const auto otherIndex = core::ArrayIndex::parse(places.fieldName(other));
    if (!otherIndex || core::arrayIndicesDisjoint(
                           *index, *otherIndex, state.scalars, state.relations))
      continue;
    const auto target = places.lookupTranslated(dest, *selected, other);
    if (!target)
      continue;
    auto alternative = state;
    auto *previousState = currentState;
    currentState = &alternative;
    applyPointerAssign(*target, origin, at, constPointee, alternative);
    currentState = previousState;
    state.join(alternative, &places);
  }
}

void FunctionDataflow::forgetArrayStorage(core::PlaceId place,
                                          core::AnalysisState &state) {
  const auto within = [this, place](core::PlaceId storage) {
    return storage == place || places.isDescendantOf(storage, place);
  };
  for (auto it = state.arrayRanges.begin(); it != state.arrayRanges.end();) {
    auto &range = it->second;
    if (within(range.destination)) {
      it = state.arrayRanges.erase(it);
      continue;
    }
    if (within(range.source)) {
      range.sourceLive = false;
      state.incompleteHeap.insert(range.destination);
    }
    ++it;
  }
  std::erase_if(state.releasedArrayRanges, [&](const auto &entry) {
    return within(entry.second.storage);
  });
  std::erase_if(state.filledArrayRanges, [&](const auto &entry) {
    return within(entry.second.storage);
  });
}

void FunctionDataflow::checkArrayTraversal(core::PlaceId storage,
                                           const core::Affine &count,
                                           const Expr &at,
                                           core::AnalysisState &state) {
  const auto parent = places.parent(storage);
  if (!parent)
    return;
  const auto type = arrayElementType(storage);
  if (type.isNull())
    return;
  const auto size = byteSizeOf(type, context);
  const auto bytes = size ? count.times(*size) : std::nullopt;
  if (!bytes)
    return;
  const core::ArraySpan span{.begin = core::ArrayIndex::constant(0),
                             .count = count};
  std::optional<KnownExtent> known;
  if (places.step(storage) == core::PathStep::Deref) {
    if (span.contains(core::ArrayIndex::constant(0), state.scalars,
                      state.relations) == core::ArrayRelation::Yes) {
      PlaceRef ref{.place = *parent,
                   .derefs = {},
                   .derefExprs = {},
                   .derefElements = {},
                   .element = {}};
      doRead(ref, at, state, true);
      checkDereference(*parent, at, state);
    }
    noteExtentRequirement(*parent, *bytes, state);
    if (const auto spatial = spatialRecordAt(*parent, state);
        spatial && spatial->extent)
      known = KnownExtent{.have = *spatial->extent,
                          .origin = spatial->location,
                          .pointer = *parent,
                          .offset = spatial->offset,
                          .unit = size,
                          .declared = spatial->declared};
  } else if (const auto *decl = builder.varForPlace(*parent);
             decl && decl->getType()->isArrayType()) {
    if (const auto extent = byteSizeOf(decl->getType(), context))
      known = KnownExtent{.have = core::Affine::ofConstant(*extent),
                          .origin = locate(decl->getLocation()),
                          .pointer = std::nullopt,
                          .offset = {},
                          .unit = size,
                          .declared = true};
  }
  if (known)
    (void)reportBounds(*bytes, *known, at, "array traversal", "elements",
                       nullptr, nullptr, state);
}

} // namespace weavec::analysis
