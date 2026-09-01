//===- Allocators.cpp - Recognised allocation and release functions -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Allocators.h"

#include "weavec/Analysis/Annotations.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <array>

using namespace clang;

namespace weavec::analysis {

bool CallEffects::consumes(unsigned arg) const noexcept {
  return llvm::is_contained(consumedArgs, arg);
}

static llvm::StringRef globalName(const FunctionDecl &function) {
  // Only global functions count: a static helper called `free` in some file
  // is the user's business, not libc's.
  if (!function.isGlobal())
    return {};
  const IdentifierInfo *ident = function.getIdentifier();
  return ident == nullptr ? llvm::StringRef() : ident->getName();
}

bool isKnownAllocator(const FunctionDecl &function) {
  static constexpr std::array<llvm::StringLiteral, 6> Names = {
      "malloc", "calloc", "realloc", "strdup", "strndup", "aligned_alloc"};
  return llvm::is_contained(Names, globalName(function));
}

bool isKnownReleaser(const FunctionDecl &function) {
  return globalName(function) == "free";
}

std::optional<CallEffects> classifyCall(const CallExpr &call) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr)
    return std::nullopt;

  CallEffects effects;
  bool any = false;

  if (isKnownAllocator(*callee)) {
    effects.producesOwned = true;
    any = true;
    if (globalName(*callee) == "realloc" && call.getNumArgs() == 2) {
      effects.isRealloc = true;
      effects.consumedArgs.push_back(0);
    }
  }
  if (isKnownReleaser(*callee) && call.getNumArgs() == 1) {
    effects.consumedArgs.push_back(0);
    effects.releasesArgs = true;
    any = true;
  }

  // Annotations on the declaration are authoritative (RFC 0001).
  if (getAnnotations(*callee).owned &&
      callee->getReturnType()->isPointerType()) {
    effects.producesOwned = true;
    any = true;
  }
  const unsigned params = callee->getNumParams();
  for (unsigned i = 0; i < params && i < call.getNumArgs(); ++i) {
    const ParmVarDecl *param = callee->getParamDecl(i);
    if (!param->getType()->isPointerType())
      continue;
    const AnnotationSet annotations = getAnnotations(*param);
    if (annotations.owned && !effects.consumes(i)) {
      effects.consumedArgs.push_back(i);
      any = true;
    } else if (annotations.mutBorrowed) {
      effects.borrowedArgs.emplace_back(i, core::BorrowKind::Mutable);
      any = true;
    } else if (annotations.borrowed) {
      effects.borrowedArgs.emplace_back(i, core::BorrowKind::Shared);
      any = true;
    }
  }

  if (!any)
    return std::nullopt;
  return effects;
}

} // namespace weavec::analysis
