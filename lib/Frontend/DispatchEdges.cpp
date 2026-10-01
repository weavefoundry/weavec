//===- DispatchEdges.cpp - Split critical edges into dispatches -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/DispatchEdges.h"

#include "clang/Basic/CodeGenOptions.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <set>
#include <utility>
#include <vector>

namespace weavec::frontend {

// NOLINTBEGIN(readability-convert-member-functions-to-static): the pass
// manager calls `run` on an instance.
llvm::PreservedAnalyses
SplitDispatchEdges::run(llvm::Function &function,
                        llvm::FunctionAnalysisManager & /*analyses*/) {
  // NOLINTEND(readability-convert-member-functions-to-static)
  // The edges first: splitting changes the predecessor lists walked here.
  std::vector<std::pair<llvm::Instruction *, unsigned>> edges;
  for (llvm::BasicBlock &block : function) {
    if (!llvm::isa<llvm::IndirectBrInst>(block.getTerminator()) ||
        !block.hasNPredecessorsOrMore(DispatchPredecessors + 1))
      continue;
    std::set<llvm::BasicBlock *> seen;
    for (llvm::BasicBlock *predecessor : llvm::predecessors(&block)) {
      if (!seen.insert(predecessor).second)
        continue;
      llvm::Instruction *terminator = predecessor->getTerminator();
      for (unsigned i = 0; i < terminator->getNumSuccessors(); ++i)
        if (terminator->getSuccessor(i) == &block &&
            llvm::isCriticalEdge(terminator, i))
          edges.emplace_back(terminator, i);
    }
  }
  bool changed = false;
  for (auto [terminator, successor] : edges)
    // An edge out of an `indirectbr` or a `callbr` cannot be split; that is
    // not this pass's shape and is left alone.
    changed =
        llvm::SplitCriticalEdge(terminator, successor) != nullptr || changed;
  return changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

void registerDispatchEdgeSplit(clang::CodeGenOptions &options) {
  options.PassBuilderCallbacks.emplace_back([](llvm::PassBuilder &builder) {
    builder.registerOptimizerLastEPCallback(
        [](llvm::ModulePassManager &passes, llvm::OptimizationLevel level,
           llvm::ThinOrFullLTOPhase /*phase*/) {
          if (level == llvm::OptimizationLevel::O0)
            return;
          passes.addPass(
              llvm::createModuleToFunctionPassAdaptor(SplitDispatchEdges()));
        });
  });
}

} // namespace weavec::frontend
