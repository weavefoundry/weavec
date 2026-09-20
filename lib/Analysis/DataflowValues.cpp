//===- DataflowValues.cpp - Allocation-time scalar values -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

namespace weavec::analysis {

std::optional<core::Affine>
FunctionDataflow::foldAffine(std::optional<core::Affine> value,
                             const core::AnalysisState &state) {
  if (!value || !value->place)
    return value;
  if (const auto symbolic = numericExpressions.find(*value->place);
      symbolic != numericExpressions.end()) {
    if (const auto linear = linearIntegerExpression(symbolic->second, state)) {
      const auto scaled = linear->times(value->scale);
      return scaled ? scaled->shifted(value->constant) : std::nullopt;
    }
  }
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
  const bool valuesAffected =
      std::ranges::any_of(state.numericValues, [&](const auto &entry) {
        return entry.first != place && entry.second.dependsOn(place);
      });
  const bool conditionsAffected = std::ranges::any_of(
      state.numericConditions.integers, [place](const auto &predicate) {
        return predicate.lhs.dependsOn(place) || predicate.rhs.dependsOn(place);
      });
  if (affected.empty() && !valuesAffected && !conditionsAffected)
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
      numericSnapshotExpressions.erase(*snapshot);
      if (const auto expression = state.numericValues.find(place);
          expression != state.numericValues.end()) {
        if (const auto projected = summaryIntegerExpression(expression->second))
          numericSnapshotExpressions.emplace(*snapshot, *projected);
      } else if (!state.numericWrites.contains(place)) {
        const auto *decl =
            llvm::dyn_cast_or_null<clang::ValueDecl>(builder.declFor(place));
        auto type = decl ? integerTypeOf(*decl, context) : std::nullopt;
        // declFor(*p) can be p's pointer declaration. The captured input
        // nodes retain the actual integer type of the dereferenced cell.
        // Conflicting views cannot justify one exported storage type.
        if (!type) {
          bool conflict = false;
          const auto inspect = [&](const NumericExpression &dependent) {
            for (const auto &node : dependent.all()) {
              if (node.key != place)
                continue;
              if (type && *type != node.type)
                conflict = true;
              else
                type = node.type;
            }
          };
          for (const auto &[holder, dependent] : state.numericValues)
            inspect(dependent);
          for (const auto &[symbol, dependent] : numericExpressions)
            inspect(dependent);
          for (const auto &predicate : state.numericConditions.integers) {
            inspect(predicate.lhs);
            inspect(predicate.rhs);
          }
          if (conflict)
            type.reset();
        }
        if (type)
          if (const auto projected = summaryIntegerExpression(
                  NumericExpression::input(place, *type)))
            numericSnapshotExpressions.emplace(*snapshot, *projected);
      }
      state.spatial.dropExtentsOn(*snapshot);
      for (auto &[holder, record] : affected) {
        if (record.extent && record.extent->place == snapshot)
          record.extent.reset();
        if (record.string && record.string->length &&
            record.string->length->place == snapshot)
          record.string->length.reset();
      }
      std::erase_if(state.numericValues, [&](const auto &entry) {
        return entry.first == *snapshot || entry.second.dependsOn(*snapshot);
      });
      state.scalars.forget(*snapshot);
      state.relations.forget(*snapshot);
      state.dropGuardsOn(*snapshot);
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
  if (valuesAffected || conditionsAffected) {
    std::optional<core::Affine> value = core::Affine::ofPlace(place);
    capture(value);
    if (value) {
      for (auto &[holder, expression] : state.numericValues) {
        if (holder == place || !expression.dependsOn(place))
          continue;
        const auto frozen = expression.substitute<core::PlaceId>(
            [&](core::PlaceId leaf,
                core::IntegerType type) -> std::optional<NumericExpression> {
              if (leaf != place)
                return NumericExpression::input(leaf, type);
              if (value->isConstant())
                return NumericExpression::constant(core::IntegerValue::ofBits(
                    type, static_cast<std::uint64_t>(value->constant)));
              return NumericExpression::input(*value->place, type);
            });
        if (frozen)
          expression = *frozen;
      }
      // The branch tested the old value. Preserve that premise through a
      // write instead of making a later access requirement unconditional.
      auto conditions = state.numericConditions;
      conditions.integers.clear();
      for (const auto &predicate : state.numericConditions.integers) {
        const auto frozen = predicate.substitute<core::PlaceId>(
            [&](core::PlaceId leaf,
                core::IntegerType type) -> std::optional<NumericExpression> {
              if (leaf != place)
                return NumericExpression::input(leaf, type);
              if (value->isConstant())
                return NumericExpression::constant(core::IntegerValue::ofBits(
                    type, static_cast<std::uint64_t>(value->constant)));
              return NumericExpression::input(*value->place, type);
            });
        if (frozen)
          conditions.requireInteger(*frozen);
        else
          state.numericConditionsIncomplete = true;
      }
      state.numericConditions = std::move(conditions);
    } else if (conditionsAffected) {
      state.numericConditionsIncomplete = true;
    }
  }
  for (auto &[holder, record] : affected) {
    capture(record.extent);
    if (record.string)
      capture(record.string->length);
    state.spatial.set(holder, std::move(record));
  }
}

} // namespace weavec::analysis
