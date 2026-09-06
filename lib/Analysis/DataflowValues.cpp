//===- DataflowValues.cpp - Allocation-time scalar values -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

namespace weavec::analysis {

std::optional<core::Affine>
FunctionDataflow::foldAffine(std::optional<core::Affine> value,
                             const core::AnalysisState &state) {
  if (!value || !value->place)
    return value;
  std::optional<std::int64_t> constant;
  const auto read = [&](core::PlaceId place) {
    if (const auto fact = state.scalars.factOf(place); fact && fact->constant)
      constant = fact->constant;
  };
  read(*value->place);
  if (!constant) {
    for (const core::PlaceId image : borrowedImages(*value->place, state))
      read(image);
  }
  if (!constant) {
    const auto upper = state.relations.atMost(*value->place);
    const auto lower = state.relations.atLeast(*value->place);
    if (upper && lower && upper == lower)
      constant = upper;
  }
  if (!constant)
    return value;
  const auto scaled = core::Affine::ofConstant(*constant).times(value->scale);
  return scaled ? scaled->shifted(value->constant) : std::nullopt;
}

void FunctionDataflow::snapshotScalar(core::PlaceId place,
                                      const clang::Expr *at,
                                      core::AnalysisState &state) {
  std::vector<std::pair<core::PlaceId, core::SpatialRecord>> affected;
  for (const auto &[holder, record] : state.spatial.all()) {
    if ((record.extent && record.extent->place == place) ||
        (record.string && record.string->length &&
         record.string->length->place == place))
      affected.emplace_back(holder, record);
  }
  if (affected.empty())
    return;

  // Fold constants before allocating a symbolic name. A snapshot is interned
  // by write site, and its previous generation is forgotten before reuse.
  // Thus an older loop iteration can lose a bound but cannot acquire the
  // size of a newer allocation (RFC 0013, Allocation-time scalar values).
  std::optional<core::PlaceId> snapshot;
  const auto capture = [&](std::optional<core::Affine> &value) {
    if (!value || value->place != place)
      return;
    value = foldAffine(value, state);
    if (!value || !value->place)
      return;
    if (!snapshot) {
      const auto key = std::pair{place, at};
      auto it = valueSnapshots.find(key);
      if (it == valueSnapshots.end()) {
        const core::PlaceId id =
            places.create("allocation-time(" + nameOf(place) + ")");
        it = valueSnapshots.emplace(key, id).first;
        snapshotPlaces.insert(id);
      }
      snapshot = it->second;
      state.spatial.dropExtentsOn(*snapshot);
      for (auto &[holder, record] : affected) {
        if (record.extent && record.extent->place == snapshot)
          record.extent.reset();
        if (record.string && record.string->length &&
            record.string->length->place == snapshot)
          record.string->length.reset();
      }
      state.scalars.forget(*snapshot);
      state.relations.forget(*snapshot);
      if (const auto fact = state.scalars.factOf(place))
        state.scalars.set(*snapshot, *fact);
      const auto relations = state.relations.all();
      for (const auto &[pair, edge] : relations) {
        if (pair.first == place)
          state.relations.learn(*snapshot, edge.relation, pair.second,
                                edge.offset);
        else if (pair.second == place)
          state.relations.learn(pair.first, edge.relation, *snapshot,
                                edge.offset);
      }
      if (const auto bound = state.relations.atMost(place))
        state.relations.learnAtMost(*snapshot, *bound);
      if (const auto bound = state.relations.atLeast(place))
        state.relations.learnAtLeast(*snapshot, *bound);
    }
    value->place = *snapshot;
  };
  for (auto &[holder, record] : affected) {
    capture(record.extent);
    if (record.string)
      capture(record.string->length);
    state.spatial.set(holder, std::move(record));
  }
}

} // namespace weavec::analysis
