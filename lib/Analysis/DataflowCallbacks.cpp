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
      if (source.path && source.path->isParam() && recording())
        inferred.callbackInputs.insert(*source.path);
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
    return ResolvedSummary{.summary = &*cached->second,
                           .source = callSources.at(&call)};
  }
  const FunctionDecl *direct = call.getDirectCallee();
  if (!currentState)
    return direct ? summaries.lookup(*direct) : summaries.lookupIndirect(call);
  core::AnalysisState &state = *currentState;
  std::optional<core::FunctionSummary> result;
  SummarySource source = SummarySource::Inferred;
  if (direct) {
    if (const auto base = summaries.lookup(*direct)) {
      result = *base->summary;
      source = base->source;
      if (!result->callbackInputs.empty()) {
        core::CallbackBindings bindings;
        for (const auto &path : result->callbackInputs) {
          if (path.root == core::SummaryRoot::Param &&
              path.index < call.getNumArgs() && path.steps.empty()) {
            bindings[path] = functionTargets(*call.getArg(path.index), state);
          } else if (const auto place =
                         builder.resolveSummaryPath(path, call)) {
            const auto it = state.callTargets.find(place->place);
            bindings[path] = it == state.callTargets.end()
                                 ? core::CallTargets::any()
                                 : it->second;
          }
        }
        const bool known =
            !bindings.empty() &&
            std::ranges::any_of(bindings, [](const auto &binding) {
              return !binding.second.functions.empty() || binding.second.null;
            });
        if (known) {
          callbackContexts[&call] = bindings;
          if (const auto specialized = summaries.specialize(
                  *direct, bindings, options,
                  recording() && emitDiagnostics ? &sink : nullptr)) {
            result = *specialized->summary;
          } else {
            reportIncomplete("callback context unavailable or limit reached",
                             call);
          }
        }
      }
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
      result = *contract->summary;
      source = contract->source;
    } else {
      bool returns = false;
      for (const auto &symbol : targets.functions) {
        const auto target = summaries.lookupSymbol(symbol);
        if (!target) {
          targets.unknown = true;
          continue;
        }
        if (!result)
          result = *target->summary;
        else
          result->join(*target->summary);
        returns |= !target->summary->neverReturns;
      }
      if (result)
        result->neverReturns = !returns && !targets.unknown;
      if (targets.unknown || targets.null || targets.functions.empty()) {
        if (result) {
          if (recording())
            inferred.incomplete.insert(
                "indirect call has an unresolved target");
          // Retain known effects and the unresolved alternative's boundary.
          handleUncheckedCall(call, state);
        }
      }
    }
  }
  callSources[&call] = source;
  auto &cached = callSummaries[&call];
  cached = std::move(result);
  if (!cached)
    return std::nullopt;
  return ResolvedSummary{.summary = &*cached, .source = source};
}

} // namespace weavec::analysis
