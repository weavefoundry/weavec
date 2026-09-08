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
  const auto *returnedCall =
      value ? dyn_cast<CallExpr>(value->IgnoreParenImpCasts()) : nullptr;
  if (returnedCall &&
      !ASTContext::hasSameType(value->getType(), returnedCall->getType()))
    returnedCall = nullptr;
  std::set<std::optional<core::Outcome>> classes{std::nullopt};
  if (options.checkContracts && value && value->getType()->isIntegerType()) {
    classes.clear();
    if (const auto fact = scalarFactOf(*value, state))
      for (const auto outcome : fact->classes)
        classes.insert(outcome);
    if (classes.empty()) {
      classes = {core::Outcome::Zero, core::Outcome::Positive};
      if (value->getType()->isSignedIntegerType())
        classes.insert(core::Outcome::Negative);
    }
  }
  const auto record = [&](const core::SummaryPath &path,
                          const std::optional<NumericExpression> &expression) {
    core::NumericOutput output;
    if (guardComplete)
      output.when = projectedGuard;
    // RFC 0019: forgetting an unexportable guard may add return cases. A
    // constant remains a sound possible value on the widened cases; an
    // expression whose evaluation depends on that guard does not.
    const bool total =
        options.checkContracts && expression &&
        !expression
             ->evaluate([](core::PlaceId, core::IntegerType type) {
               return core::IntegerRange::full(type);
             })
             .mayBeInvalid;
    if (expression && (guardComplete || total))
      output.value = summaryIntegerExpression(*expression);
    if (total)
      for (const auto &store : inferred.stores)
        output.when.drop(store.dest);
    if (expression && !output.value)
      inferred.incomplete.insert("unsupported numeric output projection");
    for (const auto outcome : classes) {
      auto selected = output;
      selected.on = outcome;
      std::optional<core::ValueFact> fact;
      if (outcome && returnedCall && lastCall &&
          lastCall->call == returnedCall) {
        if (path.isResult()) {
          if (const auto site = numericCallOutcomeFacts.find(returnedCall);
              site != numericCallOutcomeFacts.end())
            if (const auto result =
                    site->second.find(core::SummaryPath::result());
                result != site->second.end())
              if (const auto entry = result->second.find(*outcome);
                  entry != result->second.end())
                fact = entry->second;
        } else {
          auto narrowed = lastCall->pending;
          narrowed.select({*outcome});
          for (const auto &[place, established] : narrowed.factsInAll())
            if (callerVisiblePath(place) == path)
              fact = established;
        }
      }
      if (fact && fact->constant) {
        std::optional<core::IntegerType> type;
        if (path.isResult() && value)
          type = integerTypeOf(value->getType(), context);
        else if (const auto target = contextPlace(path, state))
          type = integerTypeOf(target->second, context);
        if (type) {
          selected.value = core::IntegerExpression<core::SummaryPath>::constant(
              core::IntegerValue::ofBits(
                  *type, static_cast<std::uint64_t>(*fact->constant)));
          selected.when = {};
        }
      }
      if (fact && fact->integer)
        if (const auto exact = fact->integer->constant()) {
          selected.value =
              core::IntegerExpression<core::SummaryPath>::constant(*exact);
          selected.when = {};
        }
      inferred.addNumericOutput(path, std::move(selected));
    }
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
    auto type = decl ? integerTypeOf(*decl, context) : std::nullopt;
    std::optional<NumericExpression> expression;
    if (const auto stored = state.numericValues.find(place);
        stored != state.numericValues.end())
      expression = stored->second;
    if (!type && expression)
      type = expression->type();
    if (!type)
      if (const auto fact = state.scalars.factOf(place); fact && fact->integer)
        type = fact->integer->type;
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
  numericCallOutcomeFacts.erase(&call);
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
    std::map<core::Outcome, core::IntegerRange> byOutcome;
    std::set<core::Outcome> unknownOutcomes;
    std::set<core::Outcome> outcomes;
    if (options.checkContracts)
      for (const auto &[outcome, effects] : summary.outcomes) {
        (void)effects;
        outcomes.insert(outcome);
      }
    for (const auto &output : outputs)
      if (output.on)
        outcomes.insert(*output.on);
    for (const auto &output : outputs) {
      auto guard = builder.translateGuard(output.when, call);
      if (!guard || !pruneGuard(*guard, state))
        continue;
      any = true;
      const auto unknownHere = [&] {
        if (output.on)
          unknownOutcomes.insert(*output.on);
        else
          unknownOutcomes.insert(outcomes.begin(), outcomes.end());
      };
      if (!output.value) {
        unknown = true;
        unknownHere();
        continue;
      }
      const auto expression = output.value->substitute<core::PlaceId>(
          [&](const core::SummaryPath &input,
              core::IntegerType inputType) -> std::optional<NumericExpression> {
            return numericInput(call, input, inputType, state);
          });
      if (!expression) {
        unknown = true;
        unknownHere();
        continue;
      }
      const auto actual = evaluateNumericExpression(*expression, state);
      if (actual.mayBeInvalid) {
        unknown = true;
        unknownHere();
      } else {
        range = range.united(actual.values.converted(*type));
        for (const auto outcome : outcomes) {
          if (output.on && output.on != outcome)
            continue;
          auto [entry, inserted] = byOutcome.try_emplace(outcome, *type);
          (void)inserted;
          entry->second = entry->second.united(actual.values.converted(*type));
        }
      }
      if (same && *same != *expression)
        allSame = false;
      if (!same)
        same = expression;
    }
    for (const auto &[outcome, values] : byOutcome)
      if (!unknownOutcomes.contains(outcome) && !values.empty())
        numericCallOutcomeFacts[&call][path].emplace(
            outcome, core::ValueFact::ofInteger(values));
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
    snapshotIntegerDependencies(cell, &call, state);
    snapshotScalar(cell, &call, state);
    state.dropGuardsOn(cell);
    state.relations.forget(cell);
    state.spatial.dropExtentsOn(cell);
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
  const auto conditional = numericCallOutcomeFacts.find(&call);
  if (!conflict && conditional != numericCallOutcomeFacts.end()) {
    if (!lastCall || lastCall->call != &call) {
      core::PendingOutcome callOutcome;
      callOutcome.callee = calleeName(call);
      callOutcome.location = locate(call);
      lastCall = CallOutcome{.call = &call, .pending = std::move(callOutcome)};
    }
    const auto type = integerTypeOf(call.getType(), context);
    if (type && lastCall->pending.consumedBy.empty()) {
      lastCall->pending.consumedBy.try_emplace(core::Outcome::Zero);
      lastCall->pending.consumedBy.try_emplace(core::Outcome::Positive);
      if (type->isSigned)
        lastCall->pending.consumedBy.try_emplace(core::Outcome::Negative);
    }
    for (const auto &[path, facts] : conditional->second) {
      if (path.isResult())
        continue;
      const auto dest = builder.resolveSummaryPath(path, call);
      if (!dest)
        continue;
      for (const auto &[outcome, fact] : facts) {
        lastCall->pending.consumedBy.try_emplace(outcome);
        auto &established = lastCall->pending.factOn[outcome];
        std::erase_if(established, [&](const auto &entry) {
          return entry.first == dest->place;
        });
        established.emplace_back(dest->place, fact);
      }
    }
  }
}

} // namespace weavec::analysis
