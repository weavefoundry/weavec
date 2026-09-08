//===- DataflowNumericInputs.cpp - Call-entry integers (RFC 0017) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

using namespace clang;

namespace weavec::analysis {

std::optional<FunctionDataflow::NumericExpression>
FunctionDataflow::numericInput(const CallExpr &call,
                               const core::SummaryPath &path,
                               core::IntegerType type,
                               const core::AnalysisState &state) {
  if (numericInputsReady.contains(&call)) {
    const auto site = numericInputs.find(&call);
    if (site == numericInputs.end())
      return std::nullopt;
    const auto saved = site->second.find({path, type});
    if (saved == site->second.end())
      return std::nullopt;
    if (const auto fact = state.scalars.factOf(saved->second))
      if (const auto value = fact->inType(type).constant())
        return NumericExpression::constant(*value);
    // Ordinary writes freeze the expression's operands through numericValues.
    // An opaque aggregate replacement may discard that expression; the slot's
    // independently captured range still describes the old value in that case.
    if (const auto value = state.numericValues.find(saved->second);
        value != state.numericValues.end())
      return value->second.converted(type);
    return NumericExpression::input(saved->second, type);
  }
  // Summary/context resolution runs before capture. It must inspect this
  // iteration's entry state, never a slot left by the preceding invocation.
  if (path.isParam() && path.isRoot()) {
    if (path.index >= call.getNumArgs())
      return std::nullopt;
    const auto value = integerExpressionOf(*call.getArg(path.index), state);
    return value ? value->converted(type) : std::nullopt;
  }
  const auto ref = builder.resolveSummaryPath(path, call);
  if (!ref || !ref->element.isWhole())
    return std::nullopt;
  if (const auto value = state.numericValues.find(ref->place);
      value != state.numericValues.end())
    return value->second.converted(type);
  return NumericExpression::input(ref->place, type);
}

void FunctionDataflow::captureNumericInputs(
    const CallExpr &call, const core::FunctionSummary &summary,
    core::AnalysisState &state) {
  numericInputsReady.erase(&call);
  std::set<NumericInputKey> dependencies;
  const auto expression = [&](const auto &value) {
    for (const auto &node : value.all())
      if (node.key)
        dependencies.emplace(*node.key, node.type);
  };
  const auto guard = [&](const core::PathGuard &when) {
    // translateGuard may lower a scalar argument condition to a typed
    // predicate when the actual argument is a cast or compound expression.
    for (const auto &[path, fact] : when.conditions) {
      if (fact.isPointer())
        continue;
      if (path.isParam() && path.isRoot() && path.index < call.getNumArgs()) {
        if (const auto type =
                integerTypeOf(call.getArg(path.index)->getType(), context))
          dependencies.emplace(path, *type);
      } else if (fact.integer) {
        dependencies.emplace(path, fact.integer->type);
      }
    }
    for (const auto &predicate : when.integers) {
      expression(predicate.lhs);
      expression(predicate.rhs);
    }
  };
  const auto affine = [&](const core::PathAffine &value) {
    if (value.expression)
      expression(*value.expression);
  };
  const auto source = [&](const core::ValueSource &value) {
    guard(value.when);
    if (value.extent)
      affine(*value.extent);
    if (value.stringLength)
      affine(*value.stringLength);
  };
  for (const auto &[path, effect] : summary.effects)
    guard(effect.when);
  for (const auto &[outcome, effects] : summary.outcomes)
    for (const auto &[path, effect] : effects)
      guard(effect.when);
  for (const auto &value : summary.returns)
    source(value);
  for (const auto &store : summary.stores)
    source(store.value);
  for (const auto &[root, graph] : summary.heap)
    for (const auto &field : graph.fields)
      source(field.value);
  for (const auto &[path, outputs] : summary.numericOutputs)
    for (const auto &output : outputs) {
      guard(output.when);
      if (output.value)
        expression(*output.value);
    }
  for (const auto &[param, requirements] : summary.requiresExtent)
    for (const auto &requirement : requirements) {
      guard(requirement.when);
      affine(requirement.need);
      if (requirement.start)
        affine(*requirement.start);
    }
  for (const auto &copy : summary.arrayCopies) {
    guard(copy.when);
    affine(copy.destBegin);
    affine(copy.sourceBegin);
    affine(copy.count);
  }
  for (const auto &fill : summary.arrayFills) {
    guard(fill.when);
    affine(fill.count);
  }
  for (const auto &release : summary.arrayReleases) {
    guard(release.when);
    affine(release.begin);
    affine(release.count);
  }
  if (options.checkContracts) {
    for (const auto &requirement : summary.checked.requirements) {
      guard(requirement.when);
      affine(requirement.begin);
      affine(requirement.end);
    }
    for (const auto &post : summary.checked.establishes) {
      guard(post.when);
      affine(post.begin);
      affine(post.end);
    }
  }

  auto &inputs = numericInputs[&call];
  // Retire every previous slot, including dependencies that a contextual
  // summary no longer mentions. Older allocations retain snapshots of the
  // preceding value instead of acquiring this invocation's input.
  for (const auto &[key, saved] : inputs) {
    snapshotIntegerDependencies(saved, &call, state);
    snapshotScalar(saved, &call, state);
    state.dropGuardsOn(saved);
    state.scalars.forget(saved);
    state.relations.forget(saved);
    numericSnapshotExpressions.erase(saved);
  }
  for (const auto &[path, type] : dependencies) {
    auto saved = inputs.find({path, type});
    if (saved == inputs.end()) {
      const auto place =
          places.create("numeric-input@" + std::to_string(locate(call).line) +
                        ":" + path.toString("value"));
      saved = inputs.emplace(NumericInputKey{path, type}, place).first;
      snapshotPlaces.insert(place);
    }
    const auto value = numericInput(call, path, type, state);
    // A summary may mention an inaccessible input on a path the caller will
    // never take (including a null argument). Capture unknown here; eagerly
    // diagnosing every possible dependency would add warnings even when its
    // guarded consumer is refuted or the null access is already diagnosed.
    if (!value)
      continue;
    const auto evaluated = evaluateNumericExpression(*value, state);
    if (!evaluated.mayBeInvalid)
      state.scalars.set(saved->second,
                        core::ValueFact::ofInteger(evaluated.values));
    if (!value->dependsOn(saved->second))
      state.numericValues.insert_or_assign(saved->second, *value);
    if (const auto projected = summaryIntegerExpression(*value))
      numericSnapshotExpressions.emplace(saved->second, *projected);
    if (const auto input = value->inputKey())
      state.relations.learn(saved->second, core::Relation::Equal, *input);
  }
  numericInputsReady.insert(&call);
}

} // namespace weavec::analysis
