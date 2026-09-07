//===- DataflowNumericOutputs.cpp - Numeric interfaces (RFC 0017) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

#include <algorithm>

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::recordNumericOutputs(const Expr *value,
                                            const core::AnalysisState &state) {
  if (!recording())
    return;
  const auto guard = guardHere(state);
  const auto projectedGuard = summaryGuardOf(guard);
  const bool guardComplete = integerGuardComplete(guard, state) &&
                             summaryGuardComplete(guard, projectedGuard);
  const auto record = [&](const core::SummaryPath &path,
                          const std::optional<NumericExpression> &expression) {
    core::NumericOutput output;
    if (guardComplete)
      output.when = projectedGuard;
    if (expression && guardComplete)
      output.value = summaryIntegerExpression(*expression);
    if (expression && !output.value)
      inferred.incomplete.insert("unsupported numeric output projection");
    inferred.addNumericOutput(path, std::move(output));
  };
  if (value && value->getType()->isIntegerType()) {
    auto expression = integerExpressionOf(*value, state);
    if (const auto fact = integerRangeOf(*value, state);
        fact && !fact->mayBeInvalid)
      if (const auto exact = fact->values.constant())
        expression = NumericExpression::constant(*exact);
    record(core::SummaryPath::result(), expression);
  }
  const auto count = places.size();
  for (std::size_t index = 0; index < count; ++index) {
    const core::PlaceId place{static_cast<std::uint32_t>(index)};
    const auto path = callerVisiblePath(place);
    if (!path || !writtenScalarPaths.contains(*path) || !tracksScalar(place))
      continue;
    const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(place));
    const auto type = decl ? integerTypeOf(*decl, context) : std::nullopt;
    std::optional<NumericExpression> expression;
    if (const auto stored = state.numericValues.find(place);
        stored != state.numericValues.end())
      expression = stored->second;
    if (type)
      if (const auto fact = state.scalars.factOf(place))
        if (const auto exact = fact->inType(*type).constant())
          expression = NumericExpression::constant(*exact);
    record(*path, expression);
  }
}

std::optional<core::PlaceId>
FunctionDataflow::numericCallResult(const CallExpr &call) const {
  const auto site = numericCallOutputs.find(&call);
  if (site == numericCallOutputs.end())
    return std::nullopt;
  const auto result = site->second.find(core::SummaryPath::result());
  return result == site->second.end() ? std::nullopt
                                      : std::optional(result->second);
}

void FunctionDataflow::prepareNumericCall(const CallExpr &call,
                                          const core::FunctionSummary &summary,
                                          core::AnalysisState &state) {
  captureNumericInputs(call, summary, state);
  if (const auto old = numericCallOutputs.find(&call);
      old != numericCallOutputs.end())
    for (const auto &[path, place] : old->second) {
      snapshotIntegerDependencies(place, &call, state);
      snapshotScalar(place, &call, state);
      state.dropGuardsOn(place);
      state.scalars.forget(place);
      state.numericValues.erase(place);
    }
  if (integerTypeOf(call.getType(), context)) {
    auto &outputs = numericCallOutputs[&call];
    if (!outputs.contains(core::SummaryPath::result())) {
      const auto result =
          places.create("integer-call@" + std::to_string(locate(call).line));
      outputs.emplace(core::SummaryPath::result(), result);
      snapshotPlaces.insert(result);
    }
  }
  for (const auto &[path, outputs] : summary.numericOutputs) {
    if (path.isResult() && !path.isRoot())
      continue;
    std::optional<core::IntegerType> type;
    if (path.isResult()) {
      type = integerTypeOf(call.getType(), context);
    } else if (const auto ref = builder.resolveSummaryPath(path, call)) {
      const auto *decl =
          dyn_cast_or_null<ValueDecl>(builder.declFor(ref->place));
      if (decl)
        type = integerTypeOf(*decl, context);
    }
    if (!type)
      for (const auto &output : outputs)
        if (output.value) {
          type = output.value->type();
          break;
        }
    if (!type)
      continue;
    auto &destinations = numericCallOutputs[&call];
    auto saved = destinations.find(path);
    if (saved == destinations.end()) {
      const auto place =
          places.create("numeric-output@" + std::to_string(locate(call).line) +
                        ":" + path.toString("value"));
      saved = destinations.emplace(path, place).first;
      snapshotPlaces.insert(place);
    }
    bool any = false;
    bool unknown = false;
    bool allSame = true;
    std::optional<NumericExpression> same;
    core::IntegerRange range(*type);
    for (const auto &output : outputs) {
      auto guard = builder.translateGuard(output.when, call);
      if (!guard || !pruneGuard(*guard, state))
        continue;
      any = true;
      if (!output.value) {
        unknown = true;
        continue;
      }
      const auto expression = output.value->substitute<core::PlaceId>(
          [&](const core::SummaryPath &input,
              core::IntegerType inputType) -> std::optional<NumericExpression> {
            return numericInput(call, input, inputType, state);
          });
      if (!expression) {
        unknown = true;
        continue;
      }
      const auto actual = evaluateNumericExpression(*expression, state);
      if (actual.mayBeInvalid)
        unknown = true;
      else
        range = range.united(actual.values.converted(*type));
      if (same && *same != *expression)
        allSame = false;
      if (!same)
        same = expression;
    }
    if (!any || unknown) {
      state.scalars.forget(saved->second);
      state.numericValues.erase(saved->second);
      continue;
    }
    state.scalars.set(saved->second, core::ValueFact::ofInteger(range));
    state.numericValues.erase(saved->second);
    if (allSame && same)
      state.numericValues.insert_or_assign(saved->second, *same);
  }
}

void FunctionDataflow::finishNumericCall(const CallExpr &call,
                                         core::AnalysisState &state) {
  const auto site = numericCallOutputs.find(&call);
  if (site == numericCallOutputs.end())
    return;
  struct Output {
    std::optional<core::ValueFact> fact;
    std::optional<NumericExpression> value;
    bool conflict = false;
  };
  std::map<core::PlaceId, Output> outputs;
  // Several interface paths can designate the same cell, including through
  // may-alias mirrors. Read all saved outputs before writing any destination;
  // interface path order is not the callee's execution order (RFCs 0016/17).
  for (const auto &[path, saved] : site->second) {
    if (path.isResult())
      continue;
    const auto dest = builder.resolveSummaryPath(path, call);
    if (!dest)
      continue;
    const auto fact = state.scalars.factOf(saved);
    const auto expression = state.numericValues.find(saved);
    const auto value = expression == state.numericValues.end()
                           ? std::nullopt
                           : std::optional(expression->second);
    auto cells = mirrors(dest->place, state);
    if (!llvm::is_contained(cells, dest->place))
      cells.push_back(dest->place);
    for (const auto cell : borrowedImages(dest->place, state))
      if (!llvm::is_contained(cells, cell))
        cells.push_back(cell);
    for (const auto cell : cells) {
      auto [entry, inserted] =
          outputs.try_emplace(cell, Output{.fact = fact, .value = value});
      if (inserted)
        continue;
      auto &combined = entry->second;
      // A contextual summary executes the body under these aliases and
      // exports agreeing final values. If it is unavailable, retain all
      // possible outputs, never whichever summary path sorts last.
      const bool sameValue = combined.value && combined.value == value;
      const bool sameConstant = combined.fact && fact &&
                                combined.fact->constant &&
                                combined.fact->constant == fact->constant;
      if (!sameValue && !sameConstant &&
          (combined.fact != fact || combined.value != value))
        combined.conflict = true;
      if (combined.fact && fact)
        combined.fact->join(*fact);
      else
        combined.fact.reset();
      if (combined.value != value)
        combined.value.reset();
    }
  }
  bool conflict = false;
  for (const auto &[cell, output] : outputs) {
    conflict |= output.conflict;
    state.numericWrites.insert(cell);
    state.scalars.forget(cell);
    state.numericValues.erase(cell);
    if (output.fact)
      state.scalars.set(cell, *output.fact);
    if (output.value && !output.value->dependsOn(cell))
      state.numericValues.insert_or_assign(cell, *output.value);
    if (const auto path = callerVisiblePath(cell))
      writtenScalarPaths.insert(*path);
  }
  if (conflict)
    reportIncomplete("conflicting numeric outputs for aliased storage", call);
}

} // namespace weavec::analysis
