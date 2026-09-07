//===- DataflowArrayRanges.cpp - Sparse symbolic container copies --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"

#include "llvm/ADT/ScopeExit.h"

using namespace clang;

namespace weavec::analysis {

static std::optional<core::ArrayIndex>
arrayIndexOf(const core::Affine &affine) {
  if (!affine.place)
    return core::ArrayIndex::constant(affine.constant);
  if (affine.scale == 1)
    return core::ArrayIndex::variable(affine.place->value, affine.constant);
  return std::nullopt;
}

QualType FunctionDataflow::arrayElementType(core::PlaceId storage) {
  if (const auto found = arrayTypes.find(storage); found != arrayTypes.end())
    return found->second;
  const auto root = places.root(storage);
  const auto *decl = builder.varForPlace(root);
  if (!decl)
    return {};
  QualType type = decl->getType();
  auto chain = places.ancestors(storage);
  chain.insert(chain.begin(), storage);
  for (const auto node : llvm::reverse(chain)) {
    if (node == root || type.isNull())
      continue;
    if (places.isElement(node))
      continue;
    if (places.step(node) == core::PathStep::Field) {
      const auto *record = type->getAsRecordDecl();
      type = {};
      if (record)
        for (const auto *field : record->fields())
          if (field->getName() == llvm::StringRef(places.fieldName(node))) {
            type = field->getType();
            break;
          }
    } else if (const auto *array = type->getAsArrayTypeUnsafe()) {
      type = array->getElementType();
    } else if (type->isPointerType()) {
      type = type->getPointeeType();
    } else {
      type = {};
    }
  }
  return type;
}

void FunctionDataflow::snapshotArrayCell(core::PlaceId source,
                                         core::PlaceId target, QualType type,
                                         core::AnalysisState &state) {
  // Materialize the record's immediate pointer fields even when no earlier
  // expression named them: the bytes contain their entry values too.
  if (!type.isNull() && type->isRecordType()) {
    if (const auto *record = type->getAsRecordDecl())
      for (const auto *field : record->fields()) {
        const auto cell = builder.fieldPlace(source, *field);
        if (field->getType()->isPointerType() && !state.incoming.contains(cell))
          if (const auto path = stableSummaryPathOf(cell))
            state.incoming[cell] = core::ValueSource::copy(*path);
      }
    copyRecordPlaces(target, source, state);
    return;
  }
  if (!state.incoming.contains(source) &&
      !state.definiteHeapWrites.contains(source))
    if (const auto path = stableSummaryPathOf(source))
      state.incoming[source] = core::ValueSource::copy(*path);
  copyHeapValue(source, target, state);
}

void FunctionDataflow::materializeArrayCell(core::PlaceId storage,
                                            core::PlaceId cell,
                                            const core::ArrayIndex &index,
                                            QualType type, const Expr &at,
                                            core::AnalysisState &state) {
  if (materializingArray || state.arrayRanges.empty())
    return;
  materializingArray = true;
  const auto reset = llvm::scope_exit([&] { materializingArray = false; });
  if (type.isNull())
    type = arrayElementType(storage);

  // Destination contents are loaded before source snapshots are captured:
  // a later range may copy a value produced by an earlier one.
  for (auto &[key, range] : state.arrayRanges) {
    if (range.destination != storage || range.materialized.contains(index))
      continue;
    const auto membership =
        range.span.contains(index, state.scalars, state.relations);
    if (membership == core::ArrayRelation::No)
      continue;
    const auto sourceIndex =
        core::translateArrayIndex(index, range.span.begin, range.sourceBegin);
    if (!sourceIndex || type.isNull() ||
        range.materialized.size() >= core::MaxArrayCells) {
      state.incompleteHeap.insert(storage);
      reportIncomplete("unsupported array range selection", at);
      continue;
    }
    const auto input = places.element(range.snapshot, sourceIndex->toString());
    if (range.source == storage && !range.captured.contains(index)) {
      // memmove may overwrite an element before it is later needed as a
      // source. Freeze that element before applying this range's write.
      if (range.captured.size() >= core::MaxArrayCells) {
        range.sourceLive = false;
        state.incompleteHeap.insert(storage);
        reportIncomplete("array source snapshot limit reached", at);
        continue;
      }
      snapshotArrayCell(cell, places.element(range.snapshot, index.toString()),
                        type, state);
      range.captured.insert(index);
    }
    if (!range.captured.contains(*sourceIndex)) {
      if (!range.sourceLive || range.captured.size() >= core::MaxArrayCells) {
        state.incompleteHeap.insert(storage);
        reportIncomplete("array source snapshot is incomplete", at);
        continue;
      }
      const auto source =
          boundedArrayCell(range.source, *sourceIndex, at, state);
      if (!source) {
        range.sourceLive = false;
        state.incompleteHeap.insert(storage);
        continue;
      }
      snapshotArrayCell(*source, input, type, state);
      range.captured.insert(*sourceIndex);
    }
    const bool strong =
        membership == core::ArrayRelation::Yes && range.definite;
    std::optional<core::AnalysisState> before;
    if (!strong)
      before = state;
    const auto site = arrayRangeSites.find(key);
    if (site == arrayRangeSites.end())
      continue;
    copyArrayCell(cell, input, type, *site->second, state);
    range.materialized.insert(index);
    if (before) {
      // RFC 0017: a supported actual numeric count describes both the
      // copied and untouched alternatives. The join covers both; unknown
      // membership alone is not a missing transfer in that representation.
      const bool numericCount =
          range.span.count.place &&
          (numericExpressions.contains(*range.span.count.place) ||
           numericSnapshotExpressions.contains(*range.span.count.place));
      state.join(*before, &places);
      if (!numericCount) {
        state.incompleteHeap.insert(storage);
        reportIncomplete("array range membership is unresolved", at);
      }
      // join may invalidate references into a map when future domains grow;
      // no access through `range` follows the join.
    }
  }

  // Copy on first observation is enough: every supported write resolves
  // its destination before changing it. The frozen input survives later
  // replacement of the source cell, as an ordinary saved pointer would.
  for (auto &[key, range] : state.arrayRanges) {
    (void)key;
    if (range.source != storage || !range.sourceLive ||
        range.captured.contains(index))
      continue;
    if (range.captured.size() >= core::MaxArrayCells) {
      range.sourceLive = false;
      state.incompleteHeap.insert(range.destination);
      reportIncomplete("array source snapshot limit reached", at);
      continue;
    }
    const auto input = places.element(range.snapshot, index.toString());
    snapshotArrayCell(cell, input, type, state);
    range.captured.insert(index);
  }
}

bool FunctionDataflow::installArrayRange(const ArrayBuffer &dest,
                                         const ArrayBuffer &source,
                                         core::Affine count,
                                         const CallExpr &call,
                                         core::AnalysisState &state,
                                         std::size_t ordinal, bool definite) {
  const auto destBegin = arrayIndexOf(dest.start);
  const auto sourceBegin = arrayIndexOf(source.start);
  if (!destBegin || !sourceBegin || (count.place && count.scale != 1) ||
      (!count.place && count.constant < 0))
    return false;
  if (!count.place && count.constant == 0)
    return true;
  if (std::ranges::count_if(state.arrayRanges, [&](const auto &entry) {
        return entry.second.destination == dest.storage;
      }) >= static_cast<std::ptrdiff_t>(core::MaxArrayRanges)) {
    reportIncomplete("array range limit reached", call);
    state.incompleteHeap.insert(dest.storage);
    return false;
  }
  auto entry = arrayRangeSnapshots.find({&call, ordinal});
  if (entry == arrayRangeSnapshots.end())
    entry = arrayRangeSnapshots
                .emplace(std::pair{&call, ordinal},
                         places.create("array-range-input"))
                .first;
  const auto snapshot = entry->second;
  arrayRangeSites[snapshot] = &call;
  pointerSnapshots.insert(snapshot);
  arrayTypes[source.storage] = source.element;
  arrayTypes[dest.storage] = dest.element;
  arrayTypes[snapshot] = source.element;
  if (state.arrayRanges.contains(snapshot)) {
    state.incompleteHeap.insert(dest.storage);
    reportIncomplete("array range snapshot generation is ambiguous", call);
    definite = false;
  }
  core::ArrayRange range{.destination = dest.storage,
                         .source = source.storage,
                         .snapshot = snapshot,
                         .span = {.begin = *destBegin, .count = count},
                         .sourceBegin = *sourceBegin,
                         .captured = {},
                         .materialized = {},
                         .exported = std::nullopt,
                         .definite = definite,
                         .sourceLive = true};
  const bool composedSource =
      std::ranges::any_of(state.arrayRanges, [&](const auto &entry) {
        return entry.second.destination == source.storage;
      });
  if (composedSource) {
    range.sourceLive = false;
    state.incompleteHeap.insert(dest.storage);
    reportIncomplete("symbolic array copy composition is incomplete", call);
  }
  const auto output = stableSummaryPathOf(dest.storage);
  const auto input = stableSummaryPathOf(source.storage);
  const auto outBegin = summaryAffineOf(dest.start);
  const auto inBegin = summaryAffineOf(source.start);
  const auto length = summaryAffineOf(count);
  const auto size = byteSizeOf(source.element, context);
  if (!composedSource && input && outBegin && inBegin && length && size) {
    // Local storage may become a returned container. A result placeholder
    // is published only if recordArrayResult proves that correspondence.
    range.exported = core::ArrayCopy{
        .dest = output.value_or(core::SummaryPath::result().deref()),
        .source = *input,
        .destBegin = *outBegin,
        .sourceBegin = *inBegin,
        .count = *length,
        .elementBytes = *size,
        .view = std::string(summaries.objectView(source.element)),
        .when = summaryGuardOf(state.pathGuard()),
        .definite = definite};
  }
  // Freeze every represented source before touching a destination, even
  // when its eventual membership depends on a count not known here.
  for (const auto cell : places.descendants(source.storage)) {
    if (places.parent(cell) != source.storage || !places.isElement(cell))
      continue;
    const auto selector = core::ArrayIndex::parse(places.fieldName(cell));
    if (!selector)
      continue;
    materializeArrayCell(source.storage, cell, *selector, source.element, call,
                         state);
    if (range.exported && state.definiteHeapWrites.contains(cell)) {
      // A symbolic final range cannot claim that a rewritten source cell
      // still holds its entry value. Concrete copied cells remain useful.
      range.exported.reset();
      state.incompleteHeap.insert(dest.storage);
      reportIncomplete("array source was changed before the copied range",
                       call);
    }
    snapshotArrayCell(cell, places.element(snapshot, selector->toString()),
                      source.element, state);
    range.captured.insert(*selector);
    if (range.captured.size() >= core::MaxArrayCells)
      break;
  }
  // Composition that cannot retain the untouched remainder is explicit.
  for (auto it = state.arrayRanges.begin(); it != state.arrayRanges.end();) {
    if (it->second.destination == dest.storage) {
      state.incompleteHeap.insert(dest.storage);
      reportIncomplete(
          "overlapping symbolic array ranges require a wider relation", call);
      it = state.arrayRanges.erase(it);
    } else {
      ++it;
    }
  }
  state.arrayRanges.insert_or_assign(snapshot, std::move(range));
  for (const auto cell : places.descendants(dest.storage)) {
    if (places.parent(cell) != dest.storage || !places.isElement(cell))
      continue;
    if (const auto selector = core::ArrayIndex::parse(places.fieldName(cell));
        selector && state.arrayRanges.at(snapshot).span.contains(
                        *selector, state.scalars, state.relations) ==
                        core::ArrayRelation::Yes)
      materializeArrayCell(dest.storage, cell, *selector, dest.element, call,
                           state);
  }
  return true;
}

void FunctionDataflow::applyArrayRanges(const CallExpr &call,
                                        const core::FunctionSummary &summary,
                                        core::AnalysisState &state) {
  std::size_t ordinal = 0;
  for (const auto &copy : summary.arrayCopies) {
    const auto guard = builder.translateGuard(copy.when, call);
    if (!guard)
      continue;
    auto when = *guard;
    if (!pruneGuard(when, state))
      continue;
    auto dest = builder.resolveSummaryPath(copy.dest, call, true);
    const auto source = builder.resolveSummaryPath(copy.source, call, true);
    const auto destBegin =
        foldAffine(builder.affineFromPath(copy.destBegin, call), state);
    const auto sourceBegin =
        foldAffine(builder.affineFromPath(copy.sourceBegin, call), state);
    const auto count =
        foldAffine(builder.affineFromPath(copy.count, call), state);
    if (copy.dest.isResult() && source) {
      auto &outputs = arrayResultOutputs[&call];
      auto found = outputs.find(copy.dest);
      if (found == outputs.end())
        found =
            outputs.emplace(copy.dest, places.create("array-result-storage"))
                .first;
      arrayTypes[found->second] = arrayElementType(source->place);
      pointerSnapshots.insert(found->second);
      dest = PlaceRef{.place = found->second,
                      .derefs = {},
                      .derefExprs = {},
                      .derefElements = {},
                      .element = {}};
    }
    if (!dest || !source || !destBegin || !sourceBegin || !count) {
      reportIncomplete("unresolved array range at call", call);
      continue;
    }
    const auto destType = arrayElementType(dest->place);
    const auto sourceType = arrayElementType(source->place);
    if (destType.isNull() || sourceType.isNull() ||
        !ASTContext::hasSameUnqualifiedType(destType, sourceType) ||
        byteSizeOf(sourceType, context) != copy.elementBytes ||
        summaries.objectView(sourceType) != copy.view ||
        !installArrayRange({.storage = dest->place,
                            .element = destType,
                            .start = *destBegin,
                            .explicitArray = true},
                           {.storage = source->place,
                            .element = sourceType,
                            .start = *sourceBegin,
                            .explicitArray = true},
                           *count, call, state, ordinal++,
                           copy.definite && when.trivial()))
      reportIncomplete("incompatible or unsupported array range at call", call);
  }
}

void FunctionDataflow::recordArrayOutputs(const core::AnalysisState &state) {
  for (const auto &[key, range] : state.arrayRanges) {
    (void)key;
    if (!range.exported || range.exported->dest.isResult())
      continue;
    auto copy = *range.exported;
    copy.definite &= range.definite;
    if (!range.sourceLive)
      inferred.incomplete.insert("array source snapshot is incomplete");
    inferred.arrayCopies.insert(std::move(copy));
  }
}

void FunctionDataflow::recordArrayResult(core::PlaceId result,
                                         const core::AnalysisState &state) {
  const auto storage = places.deref(result);
  for (const auto &[key, range] : state.filledArrayRanges) {
    (void)key;
    if (range.storage != storage)
      continue;
    if (const auto count = summaryAffineOf(range.count))
      inferred.arrayFills.insert(
          {.storage = core::SummaryPath::result().deref(),
           .count = *count,
           .bytes = range.bytes,
           .when = summaryGuardOf(state.pathGuard()),
           .definite = range.definite});
    else
      inferred.incomplete.insert("unresolved returned array fill");
  }
  for (const auto &[key, range] : state.arrayRanges) {
    (void)key;
    if (!range.exported || range.destination != storage)
      continue;
    auto copy = *range.exported;
    copy.dest = core::SummaryPath::result().deref();
    copy.definite &= range.definite;
    inferred.arrayCopies.insert(std::move(copy));
    if (!range.sourceLive)
      inferred.incomplete.insert("array source snapshot is incomplete");
  }
}

void FunctionDataflow::applyArrayResult(core::PlaceId result,
                                        const CallExpr &call,
                                        core::AnalysisState &state) {
  const auto outputs = arrayResultOutputs.find(&call);
  if (outputs == arrayResultOutputs.end())
    return;
  for (const auto &[path, placeholder] : outputs->second) {
    const auto storage = builder.resolveBelow(result, path, &call);
    if (!storage)
      continue;
    arrayTypes[*storage] = arrayTypes.at(placeholder);
    for (auto &[key, range] : state.filledArrayRanges) {
      (void)key;
      if (range.storage != placeholder)
        continue;
      range.storage = *storage;
      for (const auto cell : places.descendants(placeholder)) {
        if (places.parent(cell) != placeholder || !places.isElement(cell))
          continue;
        const auto index = core::ArrayIndex::parse(places.fieldName(cell));
        const auto target =
            index ? boundedArrayCell(*storage, *index, call, state)
                  : std::nullopt;
        if (target && !state.definiteHeapWrites.contains(*target))
          copyHeapValue(cell, *target, state);
      }
      for (const auto cell : places.descendants(*storage)) {
        if (places.parent(cell) != *storage || !places.isElement(cell) ||
            !state.definiteHeapWrites.contains(cell))
          continue;
        if (const auto index = core::ArrayIndex::parse(places.fieldName(cell)))
          range.materialized.insert(*index);
      }
    }
    for (auto &[key, range] : state.arrayRanges) {
      (void)key;
      if (range.destination != placeholder)
        continue;
      range.destination = *storage;
      range.materialized.clear();
      // The caller may return this allocation in turn.
      if (range.exported)
        range.exported->dest = core::SummaryPath::result().deref();
    }
  }
}

} // namespace weavec::analysis
