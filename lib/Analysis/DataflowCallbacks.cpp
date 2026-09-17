//===- DataflowCallbacks.cpp - Function pointer value flow ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "weavec/Analysis/Summaries.h"

using namespace clang;

namespace weavec::analysis {

std::string FunctionDataflow::resolvedLibraryName(const CallExpr &call) const {
  const auto source = callSources.find(&call);
  if (source == callSources.end() || source->second != SummarySource::Builtin)
    return {};
  if (const auto *direct = call.getDirectCallee())
    return direct->getNameAsString();
  const auto targets = callTargetsSeen.find(&call);
  if (targets == callTargetsSeen.end() || targets->second.unknown ||
      targets->second.null || targets->second.functions.size() != 1)
    return {};
  return *targets->second.functions.begin();
}

core::CallTargets
FunctionDataflow::originTargets(const ValueOrigin &origin,
                                const core::AnalysisState &state) {
  if (!origin.targets.empty())
    return origin.targets;
  if (origin.kind == ValueOrigin::Kind::Conditional) {
    bool hasFunction = false;
    for (const auto &arm : origin.alternatives)
      hasFunction |= !originTargets(arm, state).empty();
    if (!hasFunction)
      return {};
    core::CallTargets result;
    for (const auto &arm : origin.alternatives) {
      auto targets = originTargets(arm, state);
      if (targets.empty()) {
        if (arm.kind == ValueOrigin::Kind::Null)
          targets.null = true;
        else
          targets.unknown = true;
      }
      result.join(targets);
    }
    return result;
  }
  if (origin.place) {
    if (const auto it = state.callTargets.find(origin.place->place);
        it != state.callTargets.end())
      return it->second;
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(origin.place->place));
        decl && decl->getType()->isFunctionPointerType()) {
      if (const auto path = builder.summaryPathOf(origin.place->place);
          path && path->isGlobal())
        return summaries.targetsForGlobal(*path);
    }
  }
  return {};
}

core::CallTargets FunctionDataflow::functionTargets(const Expr &expr,
                                                    core::AnalysisState &state,
                                                    unsigned depth) {
  if (depth > core::MaxHeapPathDepth)
    return core::CallTargets::any();
  const Expr *e = expr.IgnoreParens();
  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    if (cast->getCastKind() == CK_NullToPointer)
      return {.functions = {}, .unknown = false, .null = true};
    if (cast->getCastKind() == CK_IntegralToPointer ||
        cast->getCastKind() == CK_PointerToIntegral)
      return core::CallTargets::any();
    if (cast->getCastKind() == CK_BitCast &&
        !ASTContext::hasSameUnqualifiedType(cast->getType(),
                                            cast->getSubExpr()->getType()))
      return core::CallTargets::any();
    return functionTargets(*cast->getSubExpr(), state, depth + 1);
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(e);
      unary &&
      (unary->getOpcode() == UO_AddrOf || unary->getOpcode() == UO_Deref))
    return functionTargets(*unary->getSubExpr(), state, depth + 1);
  if (const auto *conditional = dyn_cast<ConditionalOperator>(e)) {
    auto result =
        functionTargets(*conditional->getTrueExpr(), state, depth + 1);
    result.join(
        functionTargets(*conditional->getFalseExpr(), state, depth + 1));
    return result;
  }
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    if (const auto *fn = dyn_cast<FunctionDecl>(ref->getDecl())) {
      summaries.registerCallable(*fn);
      return core::CallTargets::function(callableSymbol(*fn));
    }
  }
  if (auto place = builder.resolve(*e)) {
    const auto found = state.callTargets.find(place->place);
    if (found == state.callTargets.end() || found->second.unknown) {
      ValueOrigin input;
      input.kind = ValueOrigin::Kind::Copy;
      input.place = *place;
      const auto source = sourceValueOf(input, state, true);
      auto path = source.path;
      if (!path && options.checkContracts)
        if (const auto global = builder.summaryPathOf(place->place);
            global && global->isGlobal() && !state.isOverwritten(*global))
          path = global;
      if (path &&
          (path->isParam() || (options.checkContracts && path->isGlobal())) &&
          recording())
        inferred.callbackInputs.insert(*path);
    }
    if (found != state.callTargets.end())
      return found->second;
  }
  auto staticValue = summaries.staticTargets(*e);
  if (!staticValue.empty())
    return staticValue;
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
        var && var->hasGlobalStorage() && var->hasInit())
      return functionTargets(*var->getInit(), state, depth + 1);
  }
  if (const auto *member = dyn_cast<MemberExpr>(e)) {
    const Expr *base = member->getBase()->IgnoreParenImpCasts();
    if (const auto *address = dyn_cast<UnaryOperator>(base);
        address && address->getOpcode() == UO_AddrOf)
      base = address->getSubExpr()->IgnoreParenImpCasts();
    if (const auto *ref = dyn_cast<DeclRefExpr>(base)) {
      if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
          var && var->hasGlobalStorage() && var->hasInit()) {
        if (const auto *init =
                dyn_cast<InitListExpr>(var->getInit()->IgnoreParenImpCasts())) {
          const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
          if (field && field->getFieldIndex() < init->getNumInits())
            return functionTargets(*init->getInit(field->getFieldIndex()),
                                   state, depth + 1);
        }
      }
    }
  }
  const auto origin = builder.classifyValue(*e);
  auto result = originTargets(origin, state);
  return result.empty() ? core::CallTargets::any() : result;
}

std::optional<ResolvedSummary>
FunctionDataflow::resolveCall(const CallExpr &call) {
  if (const auto cached = callSummaries.find(&call);
      cached != callSummaries.end()) {
    if (!cached->second)
      return std::nullopt;
    return ResolvedSummary{.summary = cached->second,
                           .source = callSources.at(&call)};
  }
  checkedCallAlternatives.erase(&call);
  const FunctionDecl *direct = call.getDirectCallee();
  if (!currentState)
    return direct ? summaries.lookup(*direct) : summaries.lookupIndirect(call);
  core::AnalysisState &state = *currentState;
  if (auto hypothesis = recursiveInputCall(call, state)) {
    callSummaries[&call] = hypothesis;
    callSources[&call] = SummarySource::Inferred;
    return ResolvedSummary{.summary = std::move(hypothesis),
                           .source = SummarySource::Inferred};
  }
  std::shared_ptr<const core::FunctionSummary> result;
  SummarySource source = SummarySource::Inferred;
  const auto captureCallbacks = [&](const core::FunctionSummary &summary) {
    core::CallbackBindings bindings;
    for (const auto &path : summary.callbackInputs) {
      if (path.root == core::SummaryRoot::Param &&
          path.index < call.getNumArgs() && path.steps.empty()) {
        bindings[path] = functionTargets(*call.getArg(path.index), state);
      } else if (const auto place = builder.resolveSummaryPath(path, call)) {
        const auto actualPath = stableSummaryPathOf(place->place);
        const auto it = state.callTargets.find(place->place);
        if (it != state.callTargets.end())
          bindings[path] = it->second;
        else if (actualPath && actualPath->isGlobal())
          bindings[path] = summaries.targetsForGlobal(*actualPath);
        else if (path.isGlobal())
          bindings[path] = summaries.targetsForGlobal(path);
        else
          bindings[path] = core::CallTargets::any();
        if (bindings[path].empty())
          bindings[path] = core::CallTargets::any();
        if (actualPath && bindings[path].unknown && recording() &&
            (actualPath->isGlobal() || actualPath->isParam()))
          inferred.callbackInputs.insert(*actualPath);
      }
    }
    // RFC 0022: replaying the generic unresolved global set adds no
    // target or nullness premise. Keep its dependency, not a duplicate
    // specialization whose entry state would resolve the same set.
    std::erase_if(bindings, [&](const auto &binding) {
      return binding.first.isGlobal() && binding.second.unknown &&
             binding.second == summaries.targetsForGlobal(binding.first);
    });
    return bindings;
  };
  const auto contextualize =
      [&](std::string_view symbol,
          std::shared_ptr<const core::FunctionSummary> base,
          std::optional<core::CallContext> prepared = std::nullopt) {
        // Path resolution validates this target's object views before using
        // its footprint. The final contextual result replaces this below.
        auto &snapshot = callSummaries[&call];
        snapshot = std::move(base);
        auto bindings = prepared ? std::move(prepared)
                                 : captureCallContext(call, *snapshot, state);
        if (!bindings)
          return snapshot;
        if (const auto callbacks = callbackContexts.find(&call);
            callbacks != callbackContexts.end())
          bindings->callbacks = callbacks->second;
        memoryContexts[&call] = *bindings;
        core::DiagnosticCollector collected;
        const auto specialized = summaries.specializeMemory(
            symbol, *bindings, options,
            recording() && emitDiagnostics && !inUnsafe ? &collected : nullptr);
        // RFC 0029: a failed precision attempt cannot replace a complete
        // generic proof. Its unchanged requirements still apply at this call.
        if (options.checkContracts && snapshot->checked.complete() &&
            (!specialized || !specialized->summary->checked.complete())) {
          memoryContexts.erase(&call);
          return snapshot;
        }
        for (auto diagnostic : collected.diagnostics()) {
          diagnostic.addNote("called here with related pointer arguments",
                             locate(call));
          report(std::move(diagnostic));
        }
        if (!specialized) {
          reportIncomplete("call context unavailable or limit reached", call);
          return snapshot;
        }
        return summaries.retainSummary(*specialized);
      };
  if (direct) {
    if (const auto base = summaries.lookup(*direct)) {
      result = summaries.retainSummary(*base);
      source = base->source;
      std::optional<core::CallContext> combinedInputs;
      auto bindings = captureCallbacks(*result);
      const bool behavioral =
          result->checked.complete() && !result->callbackInputs.empty() &&
          std::ranges::all_of(result->callbackInputs, [&](const auto &path) {
            if (!path.isParam() || !path.isRoot())
              return false;
            const auto binding = bindings.find(path);
            if (binding == bindings.end() || binding->second.null)
              return false;
            return std::ranges::any_of(
                result->checked.requirements, [&](const auto &requirement) {
                  if (requirement.path != path ||
                      !(requirement.kind ==
                            core::CheckedRequirementKind::CallbackAllocate ||
                        requirement.kind ==
                            core::CheckedRequirementKind::CallbackRelease))
                    return false;
                  return std::ranges::all_of(
                      binding->second.functions, [&](const auto &symbol) {
                        const auto actual = summaries.lookupSymbol(symbol);
                        const auto expected =
                            requirement.kind == core::CheckedRequirementKind::
                                                    CallbackAllocate
                                ? "malloc"
                                : "free";
                        return actual &&
                               actual->source == SummarySource::Builtin &&
                               symbol == expected;
                      });
                });
          });
      if (!result->callbackInputs.empty() && !behavioral) {
        // A behavioral contract is one sufficient interface. A known target
        // with another protocol (e.g. a void(void*) writer) still uses its
        // actual body and memory effects, rather than acquiring release
        // semantics from its C prototype.
        const bool known =
            !bindings.empty() &&
            std::ranges::any_of(bindings, [](const auto &binding) {
              return !binding.second.functions.empty() || binding.second.null;
            });
        if (known) {
          callbackContexts[&call] = bindings;
          if (options.checkContracts) {
            // RFC 0029: both sets of actual entry premises belong to one
            // body check. A callback-only preliminary run can otherwise
            // populate nested cases for selectors the caller already knows.
            callSummaries[&call] = result;
            combinedInputs = captureCallContext(call, *result, state);
            if (combinedInputs) {
              combinedInputs->callbacks = bindings;
              if (options.stats)
                options.stats->add("combined_callback_case_requests");
            }
          }
          if (!combinedInputs) {
            if (const auto specialized =
                    summaries.specialize(*direct, bindings, options, nullptr)) {
              result = summaries.retainSummary(*specialized);
            } else {
              reportIncomplete("callback context unavailable or limit reached",
                               call);
            }
          }
        }
      }
      const bool knownBody =
          direct->getDefinition() != nullptr ||
          (summaries.programDatabase() != nullptr &&
           summaries.programDatabase()->defines(direct->getName()));
      if (!behavioral && (source == SummarySource::Inferred ||
                          source == SummarySource::Program ||
                          (source == SummarySource::Annotation && knownBody))) {
        summaries.registerCallable(*direct);
        result = contextualize(callableSymbol(*direct), std::move(result),
                               std::move(combinedInputs));
      }
      if (!memoryContexts.contains(&call) && callbackContexts.contains(&call) &&
          recording() && emitDiagnostics && !inUnsafe &&
          memoryContext.reportDiagnostics)
        (void)summaries.specialize(*direct, callbackContexts.at(&call), options,
                                   &sink);
    }
  } else {
    auto targets = functionTargets(*call.getCallee(), state);
    if (const auto place = builder.resolvePointerValue(*call.getCallee());
        place && state.nulls.isNonNull(place->place))
      targets.null = false;
    callTargetsSeen[&call] = targets;
    // An explicit type contract can cover an unresolved target. Known
    // targets still supply their actual effects when there is no contract.
    if (const auto contract = summaries.lookupIndirect(call)) {
      result = summaries.retainSummary(*contract);
      source = contract->source;
    } else if (const auto required =
                   targets.functions.empty() && targets.unknown && !targets.null
                       ? requiredCallback(call, state)
                       : nullptr;
               required) {
      result = required;
      // RFC 0029: this is a sufficient entry requirement, never a trusted
      // target or a replacement for checking a known callback implementation.
    } else {
      bool returns = false;
      std::optional<SummarySource> singleSource;
      std::shared_ptr<core::FunctionSummary> joined;
      for (const auto &symbol : targets.functions) {
        const auto target = summaries.lookupSymbol(symbol);
        if (!target) {
          targets.unknown = true;
          continue;
        }
        if (targets.functions.size() == 1 && !targets.unknown && !targets.null)
          singleSource = target->source;
        auto targetSummary = summaries.retainSummary(*target);
        callbackContexts.erase(&call);
        if (target->source != SummarySource::Builtin &&
            !targetSummary->callbackInputs.empty()) {
          auto bindings = captureCallbacks(*targetSummary);
          const bool known =
              std::ranges::any_of(bindings, [](const auto &binding) {
                return !binding.second.functions.empty() || binding.second.null;
              });
          if (known) {
            callbackContexts[&call] = bindings;
            if (const auto *definition = summaries.callable(symbol)) {
              if (const auto specialized = summaries.specialize(
                      *definition, bindings, options, nullptr))
                targetSummary = summaries.retainSummary(*specialized);
              else
                reportIncomplete(
                    "callback context unavailable or limit reached", call);
            } else {
              core::CallContext callbackContext;
              callbackContext.callbacks = bindings;
              if (const auto specialized = summaries.specializeMemory(
                      symbol, callbackContext, options, nullptr))
                targetSummary = summaries.retainSummary(*specialized);
              else
                reportIncomplete(
                    "callback context unavailable or limit reached", call);
            }
          }
        }
        auto actual = target->source == SummarySource::Builtin
                          ? std::move(targetSummary)
                          : contextualize(symbol, std::move(targetSummary));
        if (options.checkContracts && targets.functions.size() > 1)
          checkedCallAlternatives[&call].emplace(
              symbol,
              ResolvedSummary{.summary = actual, .source = target->source});
        returns |= !actual->neverReturns;
        if (!result) {
          result = std::move(actual);
        } else {
          if (!joined)
            joined = std::make_shared<core::FunctionSummary>(*result);
          joined->join(*actual);
          result = joined;
        }
      }
      if (result && result->neverReturns != (!returns && !targets.unknown)) {
        if (!joined)
          joined = std::make_shared<core::FunctionSummary>(*result);
        joined->neverReturns = !returns && !targets.unknown;
        result = joined;
      }
      if (targets.unknown || targets.null || targets.functions.empty()) {
        if (result) {
          if (recording())
            inferred.incomplete.insert(
                "indirect call has an unresolved target");
          // Retain known effects and the unresolved alternative's boundary.
          handleUncheckedCall(call, state);
        }
      }
      if (singleSource && !targets.unknown && !targets.null)
        source = *singleSource;
      const bool separateChecking =
          options.checkContracts &&
          std::ranges::any_of(
              checkedCallAlternatives[&call], [](const auto &entry) {
                return entry.second.source == SummarySource::Builtin ||
                       !entry.second.summary->checked.computed ||
                       entry.second.summary->neverReturns;
              });
      if (!separateChecking)
        checkedCallAlternatives.erase(&call);
      if (separateChecking && result && targets.functions.size() > 1 &&
          !targets.unknown && !targets.null &&
          checkedCallAlternatives[&call].size() == targets.functions.size()) {
        if (!joined)
          joined = std::make_shared<core::FunctionSummary>(*result);
        // Every actual target is checked at this call. The generic join must
        // not preserve a user target's must-output across an unchecked builtin.
        joined->checked.computed = true;
        joined->checked.signature =
            functionTypeKey(call.getCallee()->getType(), context);
        joined->checked.establishes.clear();
        result = joined;
      }
    }
  }
  if (result && source == SummarySource::Builtin) {
    auto specialized = std::make_shared<core::FunctionSummary>(*result);
    specializeIntegerBuiltin(call, *specialized, state);
    result = std::move(specialized);
  }
  callSources[&call] = source;
  auto &cached = callSummaries[&call];
  cached = std::move(result);
  if (!cached)
    return std::nullopt;
  return ResolvedSummary{.summary = cached, .source = source};
}

} // namespace weavec::analysis
