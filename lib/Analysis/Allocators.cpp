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
  if (callee == nullptr)
    return std::nullopt;
  const auto resolved = summaries.lookup(*callee);
  if (!resolved)
    return std::nullopt;

  CallEffects effects;
  effects.summary = resolved->summary;
  effects.source = resolved->source;
  effects.isRealloc = effects.summary->reallocLike;
  effects.producesOwned =
      effects.summary->returns.contains(core::ValueSource::fresh());

  const unsigned params = callee->getNumParams();
  for (unsigned i = 0; i < params && i < call.getNumArgs(); ++i) {
    if (!callee->getParamDecl(i)->getType()->isPointerType())
      continue;
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
  return builtin != nullptr &&
         builtin->returns.contains(core::ValueSource::fresh());
}

bool isKnownReleaser(const FunctionDecl &function) {
  const core::FunctionSummary *builtin = builtinSummary(function);
  return builtin != nullptr && builtin->frees(0);
}

} // namespace weavec::analysis
