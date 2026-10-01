//===- DispatchEdgesTest.cpp - RFC 0031 §9.2 edge split -------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/DispatchEdges.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace weavec::frontend {
namespace {

/// A function whose block `%dispatch` ends in an `indirectbr` and is
/// entered from `predecessors` conditional branches (critical edges).
std::string dispatchFunction(unsigned predecessors) {
  std::string ir = "define void @f(ptr %target, i1 %c) {\n"
                   "entry:\n  br label %p0\n";
  for (unsigned i = 0; i < predecessors; ++i) {
    std::string next = i + 1 < predecessors ? "%p" + std::to_string(i + 1)
                                            : std::string("%exit");
    ir += "p" + std::to_string(i) + ":\n  br i1 %c, label %dispatch, label " +
          next + "\n";
  }
  ir += "dispatch:\n  indirectbr ptr %target, [label %exit]\n"
        "exit:\n  ret void\n}\n";
  return ir;
}

unsigned criticalEdgesInto(llvm::Function &function, llvm::StringRef name) {
  unsigned count = 0;
  for (llvm::BasicBlock &block : function) {
    llvm::Instruction *terminator = block.getTerminator();
    for (unsigned i = 0; i < terminator->getNumSuccessors(); ++i)
      if (terminator->getSuccessor(i)->getName() == name &&
          llvm::isCriticalEdge(terminator, i))
        ++count;
  }
  return count;
}

std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &context,
                                    const std::string &ir) {
  llvm::SMDiagnostic error;
  auto module = llvm::parseAssemblyString(ir, error, context);
  EXPECT_NE(module, nullptr) << error.getMessage().str();
  return module;
}

TEST(DispatchEdgesTest, SplitsCriticalEdgesIntoABusyDispatch) {
  llvm::LLVMContext context;
  auto module = parse(context, dispatchFunction(DispatchPredecessors + 2));
  ASSERT_NE(module, nullptr);
  llvm::Function &function = *module->getFunction("f");
  EXPECT_EQ(criticalEdgesInto(function, "dispatch"), DispatchPredecessors + 2);
  llvm::FunctionAnalysisManager analyses;
  llvm::PreservedAnalyses preserved =
      SplitDispatchEdges().run(function, analyses);
  EXPECT_FALSE(preserved.areAllPreserved());
  EXPECT_EQ(criticalEdgesInto(function, "dispatch"), 0U);
  // Every predecessor of the dispatch now has a single successor.
  for (llvm::BasicBlock *predecessor : llvm::predecessors(&*std::find_if(
           function.begin(), function.end(),
           [](llvm::BasicBlock &b) { return b.getName() == "dispatch"; })))
    EXPECT_EQ(predecessor->getTerminator()->getNumSuccessors(), 1U);
}

TEST(DispatchEdgesTest, LeavesAQuietDispatchAlone) {
  llvm::LLVMContext context;
  auto module = parse(context, dispatchFunction(DispatchPredecessors));
  ASSERT_NE(module, nullptr);
  llvm::Function &function = *module->getFunction("f");
  llvm::FunctionAnalysisManager analyses;
  EXPECT_TRUE(SplitDispatchEdges().run(function, analyses).areAllPreserved());
  EXPECT_EQ(criticalEdgesInto(function, "dispatch"), DispatchPredecessors);
}

} // namespace
} // namespace weavec::frontend
