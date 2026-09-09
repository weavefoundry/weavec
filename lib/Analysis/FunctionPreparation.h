//===- FunctionPreparation.h - Immutable AST preparation ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_LIB_ANALYSIS_FUNCTIONPREPARATION_H
#define WEAVEC_LIB_ANALYSIS_FUNCTIONPREPARATION_H

#include "weavec/Core/Lifetime.h"
#include "weavec/Core/SourceLocation.h"

#include "clang/AST/Decl.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/CFG.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <map>
#include <memory>
#include <vector>

namespace weavec::analysis {

/// RFC 0020: all handles belong to one AST; no mutable flow/place state.
struct FunctionPreparation {
  std::shared_ptr<clang::CFG> cfg;
  std::unique_ptr<clang::PostOrderCFGView> order;
  core::LifetimeConstraints lifetimes;
  llvm::DenseMap<const clang::VarDecl *, core::LifetimeId> varLifetimes;
  std::map<std::uint32_t, core::SourceLocation> scopeEnds;
  llvm::DenseMap<const clang::VarDecl *, unsigned> liveIndex;
  llvm::DenseSet<const clang::VarDecl *> addressTaken;
  std::vector<std::vector<llvm::BitVector>> liveBefore;
  std::vector<llvm::BitVector> liveOut;
  std::vector<llvm::BitVector> liveIn;
  std::vector<bool> noReturnBlocks;
  bool hasLiveness = false;
};

} // namespace weavec::analysis
#endif // WEAVEC_LIB_ANALYSIS_FUNCTIONPREPARATION_H
