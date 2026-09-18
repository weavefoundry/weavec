//===- DataflowCallbackContracts.cpp - Callback premises (RFC 0029) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include <algorithm>

using namespace clang;
namespace weavec::analysis {

static std::optional<core::CheckedRequirementKind>
callbackProtocol(QualType type, const ASTContext &context) {
  if (type->isPointerType())
    type = type->getPointeeType();
  const auto *prototype = type->getAs<FunctionProtoType>();
  if (!prototype || prototype->isVariadic() || prototype->getNumParams() != 1)
    return std::nullopt;
  if (ASTContext::hasSameType(prototype->getReturnType(), context.VoidPtrTy) &&
      ASTContext::hasSameUnqualifiedType(prototype->getParamType(0),
                                         context.getSizeType()))
    return core::CheckedRequirementKind::CallbackAllocate;
  if (prototype->getReturnType()->isVoidType() &&
      ASTContext::hasSameUnqualifiedType(prototype->getParamType(0),
                                         context.VoidPtrTy))
    return core::CheckedRequirementKind::CallbackRelease;
  return std::nullopt;
}

std::optional<core::SummaryPath>
FunctionDataflow::callbackEntry(core::PlaceId holder,
                                const core::AnalysisState &state) {
  if (state.safety->havoc || state.safety->invalidatedPointers.contains(holder))
    return std::nullopt;
  ValueOrigin input;
  input.kind = ValueOrigin::Kind::Copy;
  input.place = PlaceRef{.place = holder, .derefs = {}, .element = {}};
  const auto source = sourceValueOf(input, state, true);
  if (!source.path || !source.path->isParam() ||
      state.isOverwritten(*source.path))
    return std::nullopt;
  // A symbolic copy must still denote its entry version. In particular a
  // function-wide spelling cannot recover a callback after replacement.
  const auto direct = builder.summaryPathOf(holder);
  if (direct == source.path && state.safety->replacedPointers.contains(holder))
    return std::nullopt;
  return source.path;
}

std::shared_ptr<const core::FunctionSummary>
FunctionDataflow::requiredCallback(const CallExpr &call,
                                   core::AnalysisState &state) {
  if (!options.checkContracts || call.getNumArgs() != 1)
    return {};
  // Do not erase an explicit function pointer cast before checking identity.
  const auto *expression = call.getCallee()->IgnoreParens();
  while (const auto *cast = dyn_cast<ImplicitCastExpr>(expression))
    expression = cast->getSubExpr()->IgnoreParens();
  if (isa<ExplicitCastExpr>(expression))
    return {};
  const auto kind = callbackProtocol(expression->getType(), context);
  const auto ref = builder.resolvePointerValue(*expression);
  const auto path = ref ? callbackEntry(ref->place, state) : std::nullopt;
  if (!kind || !path)
    return {};
  if (recording()) {
    inferred.callbackInputs.insert(*path);
    inferred.checked.require({.kind = *kind,
                              .path = *path,
                              .other = {},
                              .family = "free",
                              .when = summaryGuardOf(guardHere(state))});
  }
  safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Required,
                   call, "callback", "entry callback behavior is required");
  auto summary = std::make_shared<core::FunctionSummary>();
  summary->checked.computed = true;
  summary->checked.signature =
      functionTypeKey(call.getCallee()->getType(), context);
  if (*kind == core::CheckedRequirementKind::CallbackAllocate) {
    summary->addReturn(core::ValueSource::freshAt(
        "free", core::PointerOffset::zero(),
        core::PathAffine::ofPath(core::SummaryPath::param(0))));
    summary->addReturn(core::ValueSource::null());
  } else {
    summary->addEffect(core::SummaryPath::param(0),
                       core::PlaceEffect{.freed = true, .family = "free"});
    summary->checked.require({.kind = core::CheckedRequirementKind::Release,
                              .path = core::SummaryPath::param(0),
                              .other = {},
                              .family = "free"});
    summary->checked.establish(
        {.kind = core::CheckedRequirementKind::AllocationConsumed,
         .path = core::SummaryPath::param(0),
         .other = {},
         .family = "free"});
  }
  return summary;
}

static bool callbackSatisfies(const core::FunctionSummary &summary,
                              core::CheckedRequirementKind kind) {
  if (!summary.checked.complete() || !summary.incomplete.empty() ||
      summary.neverReturns || !summary.stores.empty() ||
      !summary.heap.empty() || !summary.callbackInputs.empty() ||
      !summary.arrayCopies.empty() || !summary.arrayReleases.empty() ||
      !summary.arrayFills.empty() || !summary.numericOutputs.empty() ||
      !summary.increments.empty() || !summary.decrements.empty() ||
      !summary.requiresNonNull.empty() || !summary.requiresExtent.empty())
    return false;
  const auto input = core::SummaryPath::param(0);
  if (kind == core::CheckedRequirementKind::CallbackAllocate) {
    if (!summary.effects.empty() || !summary.checked.requirements.empty() ||
        summary.returns.empty())
      return false;
    return std::ranges::all_of(summary.returns, [&](const auto &value) {
      return value.kind == core::ValueSource::Kind::Null ||
             (value.isFresh() && value.family == "free" &&
              value.offset.isZero() &&
              value.extent == core::PathAffine::ofPath(input));
    });
  }
  if (std::ranges::any_of(summary.effects, [&](const auto &entry) {
        const auto &effect = entry.second;
        return entry.first != input || effect.read || effect.written ||
               effect.moved || effect.escaped || effect.replaced ||
               effect.element || effect.share || !effect.freed ||
               effect.family != "free" || !effect.at.isZero();
      }))
    return false;
  if (std::ranges::any_of(summary.checked.requirements, [&](const auto &pre) {
        return pre.kind != core::CheckedRequirementKind::Release ||
               pre.path != input || pre.family != "free" ||
               pre.begin != core::PathAffine::ofConstant(0) ||
               pre.end != core::PathAffine::ofConstant(0);
      }))
    return false;
  return std::ranges::any_of(
      summary.checked.establishes, [&](const auto &post) {
        return post.kind == core::CheckedRequirementKind::AllocationConsumed &&
               post.path == input && post.family == "free" &&
               post.when.trivial() && !post.on && !post.ifNonNull;
      });
}

void FunctionDataflow::checkedCallbackRequirement(
    const core::CheckedRequirement &requirement, const CallExpr &call,
    core::AnalysisState &state) {
  core::CallTargets targets = core::CallTargets::any();
  const auto ref = builder.resolveSummaryPath(requirement.path, call);
  if (requirement.path.isParam() && requirement.path.isRoot() &&
      requirement.path.index < call.getNumArgs())
    targets = functionTargets(*call.getArg(requirement.path.index), state);
  else if (ref)
    if (const auto found = state.callTargets.find(ref->place);
        found != state.callTargets.end())
      targets = found->second;
  if (ref && state.nulls.isNonNull(ref->place))
    targets.null = false;
  bool satisfied =
      !targets.unknown && !targets.null && !targets.functions.empty();
  for (const auto &symbol : targets.functions) {
    const auto actual = summaries.lookupSymbol(symbol);
    const auto *declaration = summaries.callable(symbol);
    const auto protocol =
        declaration ? callbackProtocol(declaration->getType(), context)
                    : std::nullopt;
    bool matches = actual && protocol == requirement.kind;
    if (matches && actual->source == SummarySource::Builtin) {
      matches =
          symbol ==
          (requirement.kind == core::CheckedRequirementKind::CallbackAllocate
               ? "malloc"
               : "free");
    } else if (matches) {
      matches = actual->summary->checked.signature ==
                    functionTypeKey(declaration->getType(), context) &&
                callbackSatisfies(*actual->summary, requirement.kind);
    }
    if (matches && actual->source == SummarySource::Builtin)
      safetyObligation(core::SafetyProperty::Call, core::SafetyOutcome::Trusted,
                       call, symbol, "modeled C library contract");
    else if (matches && recording())
      inferred.checked.obligations.addCalls(
          actual->summary->checked.obligations, true, locate(call),
          function.getNameAsString(), symbol, inUnsafe);
    satisfied &= matches;
  }
  bool required = false;
  if (!satisfied && targets.functions.empty() && targets.unknown &&
      !targets.null && ref)
    if (const auto path = callbackEntry(ref->place, state)) {
      required = true;
      if (recording()) {
        auto forwarded = requirement;
        forwarded.path = *path;
        forwarded.when = summaryGuardOf(guardHere(state));
        inferred.checked.require(std::move(forwarded));
        inferred.callbackInputs.insert(*path);
      }
    }
  safetyObligation(core::SafetyProperty::Call,
                   core::safetyOutcome(satisfied, required), call, "callback",
                   "callee requires compatible synchronous callback behavior");
}

} // namespace weavec::analysis
