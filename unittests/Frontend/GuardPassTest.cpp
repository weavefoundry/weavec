//===- GuardPassTest.cpp - The guard passes on IR -------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035: GuardInsert, GuardPrune and GuardExpand on IR text, each rule of
// §6 alone, and the ledger rows they leave.
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/GuardPass.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace weavec::frontend {
namespace {

constexpr const char *Layout =
    "target datalayout = \"e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-"
    "i128:128-n32:64-S128-Fn32\"\ntarget triple = "
    "\"arm64-apple-macosx15.0.0\"\n";

struct PassRun {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  std::shared_ptr<EnforcementLedger> ledger =
      std::make_shared<EnforcementLedger>();
  GuardOptions options;

  explicit PassRun(const std::string &body,
                   GuardOptions::Mode mode = GuardOptions::Mode::Trap) {
    llvm::SMDiagnostic error;
    module =
        llvm::parseAssemblyString(std::string(Layout) + body, error, context);
    if (!module) {
      std::string text;
      llvm::raw_string_ostream os(text);
      error.print("GuardPassTest", os);
      ADD_FAILURE() << text;
    }
    options.mode = mode;
    options.ledger = ledger;
  }

  template <typename Pass>
  void runModule(Pass pass) {
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager sccs;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder builder;
    builder.registerModuleAnalyses(modules);
    builder.registerCGSCCAnalyses(sccs);
    builder.registerFunctionAnalyses(functions);
    builder.registerLoopAnalyses(loops);
    builder.crossRegisterProxies(loops, functions, sccs, modules);
    llvm::ModulePassManager passes;
    passes.addPass(std::move(pass));
    passes.run(*module, modules);
  }

  void insert() { runModule(GuardInsertPass(options)); }
  void prune() {
    llvm::FunctionAnalysisManager functions;
    llvm::PassBuilder builder;
    builder.registerFunctionAnalyses(functions);
    GuardPrunePass pass(options);
    for (llvm::Function &function : *module)
      if (!function.isDeclaration())
        (void)pass.run(function, functions);
  }
  void expand() { runModule(GuardExpandPass(options)); }

  [[nodiscard]] unsigned calls(llvm::StringRef callee) const {
    unsigned count = 0;
    for (const llvm::Function &function : *module)
      for (const llvm::BasicBlock &block : function)
        for (const llvm::Instruction &instruction : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
            if (const llvm::Function *target = call->getCalledFunction())
              if (target->getName() == callee)
                ++count;
    return count;
  }

  [[nodiscard]] bool valid() const {
    std::string text;
    llvm::raw_string_ostream os(text);
    const bool broken = llvm::verifyModule(*module, &os);
    if (broken)
      ADD_FAILURE() << text;
    return !broken;
  }
};

TEST(GuardInsertTest, GuardsAccessesThroughPointers) {
  PassRun run(R"(
define i32 @get(ptr %p, i64 %i) {
  %a = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load i32, ptr %a, align 4
  store i32 0, ptr %p, align 4
  ret i32 %v
}
)");
  run.insert();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec.guard"), 2U);
  ASSERT_EQ(run.ledger->rows.size(), 2U);
  EXPECT_EQ(run.ledger->rows[0].operation, "load");
  EXPECT_EQ(run.ledger->rows[1].operation, "store");
}

TEST(GuardInsertTest, LeavesNamedObjectsAtConstantOffsets) {
  PassRun run(R"(
@g = global [4 x i32] zeroinitializer
define i32 @local() {
  %a = alloca [4 x i32], align 4
  %e = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 3
  store i32 1, ptr %e, align 4
  %v = load i32, ptr %e, align 4
  %w = load i32, ptr getelementptr inbounds ([4 x i32], ptr @g, i64 0, i64 1), align 4
  %s = add i32 %v, %w
  ret i32 %s
}
)");
  run.insert();
  EXPECT_EQ(run.calls("__weavec.guard"), 0U);
  EXPECT_TRUE(run.ledger->rows.empty());
}

TEST(GuardInsertTest, RangesOfMemoryIntrinsics) {
  PassRun run(R"(
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
define void @copy(ptr %d, ptr %s, i64 %n) {
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 24, i1 false)
  ret void
}
)");
  run.insert();
  EXPECT_EQ(run.calls("__weavec.range"), 2U);
  EXPECT_EQ(run.calls("__weavec.guard"), 2U);
}

TEST(GuardPruneTest, RemovesGuardsInliningMadeNamed) {
  PassRun run(R"(
declare void @__weavec.guard(ptr, i64, i32, ptr, i32)
define i32 @inlined() {
  %x = alloca i32, align 4
  call void @__weavec.guard(ptr %x, i64 4, i32 512, ptr null, i32 0)
  store i32 1, ptr %x, align 4
  %v = load i32, ptr %x, align 4
  ret i32 %v
}
)");
  run.ledger->add(LedgerRow{});
  run.prune();
  EXPECT_EQ(run.calls("__weavec.guard"), 0U);
  run.ledger->finish();
  EXPECT_EQ(run.ledger->rows[0].outcome, LedgerRow::Outcome::Proven);
  EXPECT_EQ(run.ledger->rows[0].reason, "in-bounds");
}

TEST(GuardPruneTest, VerifyKeepsAMonitor) {
  PassRun run(R"(
declare void @__weavec.guard(ptr, i64, i32, ptr, i32)
define void @inlined() {
  %x = alloca i32, align 4
  call void @__weavec.guard(ptr %x, i64 4, i32 512, ptr null, i32 0)
  store i32 1, ptr %x, align 4
  ret void
}
)",
              GuardOptions::Mode::Verify);
  run.prune();
  EXPECT_EQ(run.calls("__weavec.guard"), 1U);
}

TEST(GuardExpandTest, ExpandsIntoInlineChecks) {
  PassRun run(R"(
define i32 @get(ptr %p, i64 %i) {
  %a = getelementptr inbounds i32, ptr %p, i64 %i
  %v = load i32, ptr %a, align 4
  ret i32 %v
}
)");
  run.insert();
  run.expand();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec.guard"), 0U);
  EXPECT_EQ(run.calls("__weavec_rt_guard"), 1U);
  EXPECT_EQ(run.calls("__weavec_rt_null"), 1U);
  run.ledger->finish();
  EXPECT_EQ(run.ledger->guarded, 1U);
}

TEST(GuardExpandTest, ProvesIndexesInsideALocal) {
  PassRun run(R"(
define i32 @local(i32 %i) {
  %a = alloca [4 x i32], align 4
  %m = and i32 %i, 3
  %x = zext i32 %m to i64
  %e = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %x
  %v = load i32, ptr %e, align 4
  ret i32 %v
}
)");
  run.insert();
  EXPECT_EQ(run.calls("__weavec.guard"), 1U);
  run.expand();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec_rt_guard"), 0U);
  run.ledger->finish();
  EXPECT_EQ(run.ledger->proven, 1U);
  EXPECT_EQ(run.ledger->rows[0].reason, "in-bounds");
}

TEST(GuardExpandTest, RemovesDominatedGuards) {
  PassRun run(R"(
declare void @nofree() nofree
define i32 @twice(ptr %p, i1 %c) {
  %v = load i32, ptr %p, align 8
  call void @nofree()
  br i1 %c, label %then, label %done
then:
  %w = load i16, ptr %p, align 8
  %z = zext i16 %w to i32
  br label %done
done:
  %r = phi i32 [ %v, %0 ], [ %z, %then ]
  ret i32 %r
}
)");
  run.insert();
  run.expand();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec_rt_guard"), 1U);
  run.ledger->finish();
  EXPECT_EQ(run.ledger->rows[1].reason, "dominated");
}

TEST(GuardExpandTest, AReleaseBetweenKeepsTheGuard) {
  PassRun run(R"(
declare void @free(ptr)
define i32 @released(ptr %p, ptr %q) {
  %v = load i32, ptr %p, align 4
  call void @free(ptr %q)
  %w = load i32, ptr %p, align 4
  %s = add i32 %v, %w
  ret i32 %s
}
)");
  run.insert();
  run.expand();
  EXPECT_EQ(run.calls("__weavec_rt_guard"), 2U);
}

TEST(GuardExpandTest, MergesTheFieldsOfABlock) {
  PassRun run(R"(
define i64 @fields(ptr %p) {
  %a = load i32, ptr %p, align 8
  %bp = getelementptr inbounds i8, ptr %p, i64 4
  %b = load i32, ptr %bp, align 4
  %cp = getelementptr inbounds i8, ptr %p, i64 8
  %c = load i64, ptr %cp, align 8
  %a64 = zext i32 %a to i64
  %b64 = zext i32 %b to i64
  %s = add i64 %a64, %b64
  %t = add i64 %s, %c
  ret i64 %t
}
)");
  run.insert();
  run.expand();
  EXPECT_TRUE(run.valid());
  // One inline check of the hull; its slow path checks each field, so that
  // a failure names the access that fails.
  EXPECT_EQ(run.calls("__weavec_rt_guard"), 3U);
  run.ledger->finish();
  EXPECT_EQ(run.ledger->guarded, 1U);
  EXPECT_EQ(run.ledger->proven, 2U);
}

TEST(GuardExpandTest, LaysOutEscapingLocals) {
  PassRun run(R"(
declare void @use(ptr)
define void @escapes() {
  %buf = alloca [10 x i8], align 1
  call void @use(ptr %buf)
  ret void
}
)");
  run.insert();
  run.expand();
  EXPECT_TRUE(run.valid());
  const llvm::Function *function = run.module->getFunction("escapes");
  ASSERT_NE(function, nullptr);
  const auto *frame =
      llvm::dyn_cast<llvm::AllocaInst>(&function->getEntryBlock().front());
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->getAlign().value(), 32U);
  // A left zone of 64 bytes, the object's granule and a redzone of 32.
  EXPECT_EQ(*frame->getAllocationSize(run.module->getDataLayout()), 128U);
}

TEST(GuardExpandTest, GlobalsGetRedzonesAndARegistration) {
  PassRun run(R"(
@table = global [4 x i32] zeroinitializer, align 4
@.str = private unnamed_addr constant [3 x i8] c"hi\00", align 1
define ptr @name() {
  ret ptr @.str
}
)");
  run.expand();
  EXPECT_TRUE(run.valid());
  const llvm::GlobalVariable *table = run.module->getNamedGlobal("table");
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->getAlign()->value(), 32U);
  // 16 bytes and the minimum redzone of 32 bytes.
  EXPECT_EQ(run.module->getDataLayout().getTypeAllocSize(table->getValueType()),
            64U);
  EXPECT_EQ(run.calls("__weavec_rt_globals_register"), 1U);
  const llvm::GlobalVariable *literal = run.module->getNamedGlobal(".str");
  ASSERT_NE(literal, nullptr);
  EXPECT_TRUE(literal->getValueType()->isArrayTy());
}

TEST(GuardExpandTest, VerifyKeepsRemovedGuardsAsMonitors) {
  PassRun run(R"(
define i32 @local(i32 %i) {
  %a = alloca [4 x i32], align 4
  %m = and i32 %i, 3
  %x = zext i32 %m to i64
  %e = getelementptr inbounds [4 x i32], ptr %a, i64 0, i64 %x
  %v = load i32, ptr %e, align 4
  ret i32 %v
}
)",
              GuardOptions::Mode::Verify);
  run.insert();
  run.expand();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec_rt_guard"), 1U);
}

TEST(GuardExpandTest, StringsBecomeTheRuntimesScan) {
  PassRun run(R"(
declare i64 @strlen(ptr) nounwind
define i64 @length(ptr %s) {
  %n = call i64 @strlen(ptr %s)
  ret i64 %n
}
)");
  run.insert();
  EXPECT_EQ(run.calls("strlen"), 0U);
  EXPECT_EQ(run.calls("__weavec.strlen"), 1U);
  run.expand();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec_rt_strlen"), 1U);
}

} // namespace
} // namespace weavec::frontend
