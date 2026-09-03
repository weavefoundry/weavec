//===- Allocators.cpp - Ownership effects of a call -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Allocators.h"

#include "llvm/ADT/STLExtras.h"

using namespace clang;

namespace weavec::analysis {

bool CallEffects::consumes(unsigned arg) const noexcept {
  return llvm::is_contained(consumedArgs, arg);
}

bool CallEffects::frees(unsigned arg) const noexcept {
  return summary != nullptr && summary->frees(arg);
}

std::optional<CallEffects> classifyCall(const CallExpr &call,
                                        SummaryStore &summaries) {
  const FunctionDecl *callee = call.getDirectCallee();
  std::optional<ResolvedSummary> resolved;
  std::vector<bool> pointerParams;
  if (callee != nullptr) {
    resolved = summaries.lookup(*callee);
    for (const ParmVarDecl *param : callee->parameters())
      pointerParams.push_back(param->getType()->isPointerType());
  } else {
    // A call through a function pointer (RFC 0004, *Boundaries*).
    resolved = summaries.lookupIndirect(call);
    if (const FunctionProtoType *type = indirectCalleeType(call)) {
      for (const QualType param : type->getParamTypes())
        pointerParams.push_back(param->isPointerType());
    }
  }
  if (!resolved)
    return std::nullopt;

  CallEffects effects;
  effects.summary = resolved->summary;
  effects.source = resolved->source;
  effects.producesOwned = effects.summary->returnsFresh();

  const auto params = static_cast<unsigned>(pointerParams.size());
  effects.declaredParams = params;
  for (unsigned i = 0; i < params && i < call.getNumArgs(); ++i) {
    if (!pointerParams[i])
      continue;
    effects.pointerArgs.push_back(i);
    if (effects.summary->consumes(i)) {
      effects.consumedArgs.push_back(i);
      continue;
    }
    if (const auto kind = effects.summary->borrowKind(i))
      effects.borrowedArgs.emplace_back(i, *kind);
  }
  return effects;
}

bool isKnownAllocator(const FunctionDecl &function) {
  const core::FunctionSummary *builtin = builtinSummary(function);
  return builtin != nullptr && builtin->returnsFresh();
}

bool isKnownReleaser(const FunctionDecl &function) {
  const core::FunctionSummary *builtin = builtinSummary(function);
  return builtin != nullptr && builtin->frees(0);
}

} // namespace weavec::analysis
