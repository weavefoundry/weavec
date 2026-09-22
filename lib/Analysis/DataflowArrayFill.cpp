//===- DataflowArrayFill.cpp - Proved contiguous initialization -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "llvm/ADT/ScopeExit.h"

#include <utility>

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::fillArrayRange(core::PlaceId storage, core::Affine count,
                                      std::optional<std::int64_t> bytes,
                                      const Expr &at,
                                      core::AnalysisState &state,
                                      std::size_t ordinal, bool definite) {
  if (count.isConstant() && count.constant <= 0)
    return;
  checkArrayTraversal(storage, count, at, state);
  if (count.place && count.scale != 1) {
    decideIncomplete("unsupported contiguous array fill", at);
    return;
  }
  if (recording()) {
    const auto path = stableSummaryPathOf(storage);
    const auto length = summaryAffineOf(count);
    if (path && length)
      inferred.arrayFills.insert({.storage = *path,
                                  .count = *length,
                                  .bytes = bytes,
                                  .when = summaryGuardOf(state.pathGuard()),
                                  .definite = definite});
  }
  auto site = arrayFillSites.find({&at, ordinal});
  if (site == arrayFillSites.end()) {
    if (arrayFillSites.size() >= core::MaxArrayRanges) {
      state.incompleteHeap.insert(storage);
      decideIncomplete("array fill range limit reached", at);
      return;
    }
    site = arrayFillSites
               .emplace(std::pair{&at, ordinal}, places.create("array-fill"))
               .first;
    arrayFillExpressions[site->second] = &at;
  }
  state.filledArrayRanges.insert_or_assign(
      site->second, core::FilledArrayRange{.storage = storage,
                                           .count = count,
                                           .bytes = bytes,
                                           .materialized = {},
                                           .definite = definite});
  // Bounded constant loops have a complete set of cells. Symbolic fills
  // remain sparse and are instantiated only at subsequent selections.
  if (count.isConstant() &&
      std::cmp_less_equal(count.constant, core::MaxArrayCells))
    for (std::int64_t i = 0; i < count.constant; ++i)
      (void)boundedArrayCell(storage, core::ArrayIndex::constant(i), at, state);
  const core::ArraySpan span{.begin = core::ArrayIndex::constant(0),
                             .count = count};
  for (const auto cell : places.descendants(storage)) {
    if (places.parent(cell) != storage || !places.isElement(cell))
      continue;
    const auto index = core::ArrayIndex::parse(places.fieldName(cell));
    if (index && span.contains(*index, state.scalars, state.relations) ==
                     core::ArrayRelation::Yes)
      materializeArrayFill(storage, cell, *index, at, state);
  }
}

void FunctionDataflow::materializeArrayFill(core::PlaceId storage,
                                            core::PlaceId cell,
                                            const core::ArrayIndex &index,
                                            const Expr &at,
                                            core::AnalysisState &state) {
  if (materializingArrayFill)
    return;
  materializingArrayFill = true;
  const auto reset = llvm::scope_exit([&] { materializingArrayFill = false; });
  for (auto &[key, range] : state.filledArrayRanges) {
    if (range.storage != storage || range.materialized.contains(index))
      continue;
    const core::ArraySpan span{.begin = core::ArrayIndex::constant(0),
                               .count = range.count};
    const auto membership =
        span.contains(index, state.scalars, state.relations);
    if (membership == core::ArrayRelation::No)
      continue;
    // RFC 0030 §7.4: the fill says what the cells held when it ran, and it
    // is materialized onto a cell the first time one is selected. A cell
    // this path already knows a value for holds that value, whether it
    // came from the fill or from a store since: putting the fill's value
    // back would forget the store. `for (i) a[i] = NULL;` then
    // `for (i) { a[i] = make(); use(a[i]); }` reads what `make` returned,
    // not the null the first loop left — the second store weakens every
    // represented cell (the index resolves to none of them), so the cells
    // are *may*-written, which the record already says.
    if (state.definiteHeapWrites.contains(cell) || state.nulls.recordOf(cell) ||
        state.resources.recordOf(cell)) {
      range.materialized.insert(index);
      continue;
    }
    const auto site = arrayFillExpressions.find(key);
    if (site == arrayFillExpressions.end() ||
        range.materialized.size() >= core::MaxArrayCells) {
      decideIncomplete("array fill selection limit reached", at);
      state.incompleteHeap.insert(storage);
      continue;
    }
    materializeArrayCell(storage, cell, index, arrayElementType(storage), at,
                         state);
    std::optional<core::AnalysisState> before;
    if (!range.definite || membership != core::ArrayRelation::Yes)
      before = state;
    if (index.symbol && range.count.isConstant() &&
        std::cmp_less_equal(range.count.constant, core::MaxArrayCells)) {
      // The concrete cells already own these allocations. A symbolic read
      // names one of those values, not a new allocation for this spelling.
      std::optional<core::AnalysisState> joined;
      for (std::int64_t i = 0; i < range.count.constant; ++i) {
        const auto selected = core::ArrayIndex::constant(i);
        if (core::arrayIndicesDisjoint(index, selected, state.scalars,
                                       state.relations))
          continue;
        const auto source = boundedArrayCell(storage, selected, at, state);
        if (!source)
          continue;
        auto branch = state;
        copyHeapValue(*source, cell, branch);
        if (joined)
          joined->join(branch, &places);
        else
          joined = std::move(branch);
      }
      const auto rangeKey = key;
      if (joined)
        state = std::move(*joined);
      state.filledArrayRanges.at(rangeKey).materialized.insert(index);
      if (before)
        state.join(*before, &places);
      return;
    }
    ValueOrigin origin;
    origin.kind =
        range.bytes ? ValueOrigin::Kind::Alloc : ValueOrigin::Kind::Null;
    if (range.bytes) {
      origin.family = "free";
      origin.extent = core::Affine::ofConstant(*range.bytes);
    }
    applyPointerAssign(cell, origin, *site->second, false, state);
    range.materialized.insert(index);
    if (before) {
      state.join(*before, &places);
      decideIncomplete("array fill membership is unresolved", at);
      state.incompleteHeap.insert(storage);
    }
  }
}

void FunctionDataflow::applyArrayFills(const CallExpr &call,
                                       const core::FunctionSummary &summary,
                                       core::AnalysisState &state) {
  std::size_t ordinal = 0;
  for (const auto &fill : summary.arrayFills) {
    auto when = builder.translateGuard(fill.when, call);
    if (!when || !pruneGuard(*when, state))
      continue;
    auto storage = builder.resolveSummaryPath(fill.storage, call, true);
    if (fill.storage.isResult() && call.getType()->isPointerType()) {
      auto &outputs = arrayResultOutputs[&call];
      auto found = outputs.find(fill.storage);
      if (found == outputs.end())
        found =
            outputs.emplace(fill.storage, places.create("array-result-storage"))
                .first;
      arrayTypes[found->second] = call.getType()->getPointeeType();
      pointerSnapshots.insert(found->second);
      storage = PlaceRef{.place = found->second, .derefs = {}, .element = {}};
    }
    const auto count =
        foldAffine(builder.affineFromPath(fill.count, call), state);
    if (!storage || !count) {
      decideIncomplete("unresolved array fill at call", call);
      continue;
    }
    fillArrayRange(storage->place, *count, fill.bytes, call, state, ordinal++,
                   fill.definite && when->trivial());
  }
}

} // namespace weavec::analysis
