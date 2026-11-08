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

// The passes' own colorRoot (RFC 0038 §3), not part of the library's
// interface.
#include "../../lib/Frontend/GuardPassImpl.h"

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
  void prune() const {
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
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
              call != nullptr && call->getCalledFunction() != nullptr &&
              call->getCalledFunction()->getName() == callee)
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

TEST(GuardInsertTest, CxxMarkersCostTheInlinerOneInstruction) {
  // RFC 0039, amendment 13: the inliner prices a C++ unit's markers as one
  // instruction and a C unit's as calls.
  const char *body = R"(
define i32 @get(ptr %p) {
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
)";
  for (const bool cxx : {false, true}) {
    PassRun run(body);
    run.options.cheapMarkers = cxx;
    run.insert();
    const llvm::Function *marker = run.module->getFunction("__weavec.guard");
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->getFnAttribute("call-inline-cost").getValueAsString(),
              cxx ? "5" : "");
  }
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

// RFC 0039 §2: an invoke of a library function is guarded before it, and a
// search's check after it goes on its normal path, whose edge is split when
// the destination has another predecessor; never on the unwind path.
TEST(GuardInsertTest, GuardsInvokesOnTheirNormalPath) {
  PassRun run(R"(
declare i64 @read(i32, ptr, i64)
declare ptr @memchr(ptr, i32, i64)
declare i32 @__gxx_personality_v0(...)
define i64 @f(i32 %fd, ptr %b, i64 %n, i1 %c) personality ptr @__gxx_personality_v0 {
entry:
  br i1 %c, label %search, label %join
search:
  %got = invoke i64 @read(i32 %fd, ptr %b, i64 %n) to label %found unwind label %pad
found:
  %at = invoke ptr @memchr(ptr %b, i32 120, i64 %n) to label %join unwind label %pad
join:
  %r = phi i64 [ 0, %entry ], [ %got, %found ]
  ret i64 %r
pad:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}
)");
  run.insert();
  EXPECT_TRUE(run.valid());
  // read's buffer before the call; memchr's first byte before it, and after
  // it what it read, when it found the byte and when it did not.
  EXPECT_EQ(run.calls("__weavec.range"), 4U);
  const llvm::Function *f = run.module->getFunction("f");
  for (const llvm::BasicBlock &block : *f)
    if (block.isLandingPad())
      for (const llvm::Instruction &instruction : block)
        if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
          EXPECT_FALSE(
              call->getCalledFunction() != nullptr &&
              call->getCalledFunction()->getName().starts_with("__weavec."));
  run.expand();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec_rt_landed"), 1U);
}

// RFC 0039 §3: no clearing of the stack before a throw (`unwinds`) or the
// end of the process (`exits`); before any other call that does not
// return, as before.
TEST(GuardExpandTest, ThrowsKeepTheStacksShadow) {
  PassRun run(R"(
declare void @__cxa_throw(ptr, ptr, ptr) noreturn
declare void @exit(i32) noreturn
declare void @longjmp(ptr, i32) noreturn
define void @f(i32 %k, ptr %e, ptr %j) {
  switch i32 %k, label %a [ i32 1, label %b
                            i32 2, label %c ]
a:
  call void @__cxa_throw(ptr %e, ptr null, ptr null)
  unreachable
b:
  call void @exit(i32 1)
  unreachable
c:
  call void @longjmp(ptr %j, i32 1)
  unreachable
}
)");
  run.expand();
  EXPECT_TRUE(run.valid());
  EXPECT_EQ(run.calls("__weavec_rt_unpoison_stack"), 1U);
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
  // RFC 0037 §8.2: computed from %p, the access's cold call also decides
  // its color.
  EXPECT_EQ(run.calls("__weavec_rt_guard_jump"), 1U);
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

/// The value named `name` in the module's only function.
static llvm::Value *named(const PassRun &run, llvm::StringRef name) {
  for (llvm::Function &function : *run.module)
    for (llvm::BasicBlock &block : function)
      for (llvm::Instruction &instruction : block)
        if (instruction.getName() == name)
          return &instruction;
  return nullptr;
}

TEST(ColorRootTest, DeepArithmeticHasNoRoot) {
  // RFC 0038 amendment 25: `p + (q - p)` hidden under more than eight
  // operations may land in q's object: no root, rather than p.
  PassRun run(R"(
define ptr @moved(ptr %p, ptr %q, i64 %x) {
  %pi = ptrtoint ptr %p to i64
  %qi = ptrtoint ptr %q to i64
  %a0 = sub i64 %qi, %pi
  %a1 = add i64 %a0, %x
  %a2 = add i64 %a1, %x
  %a3 = add i64 %a2, %x
  %a4 = add i64 %a3, %x
  %a5 = add i64 %a4, %x
  %a6 = add i64 %a5, %x
  %a7 = add i64 %a6, %x
  %a8 = add i64 %a7, %x
  %a9 = add i64 %a8, %x
  %sum = add i64 %pi, %a9
  %deep = inttoptr i64 %sum to ptr
  %near = add i64 %pi, %x
  %shallow = inttoptr i64 %near to ptr
  ret ptr %deep
}
)");
  EXPECT_EQ(guard::colorRoot(named(run, "deep")), nullptr);
  EXPECT_EQ(guard::colorRoot(named(run, "shallow")),
            run.module->begin()->getArg(0));
}

TEST(ColorRootTest, SharedInputsAreWalkedOnce) {
  // Amendment 25: forty selects whose two inputs are the one before: 2^40
  // paths, one root.
  PassRun run(R"(
define ptr @chain(ptr %p, i1 %c, i64 %x) {
  %s0 = getelementptr i8, ptr %p, i64 %x
  %s1 = select i1 %c, ptr %s0, ptr %s0
  %s2 = select i1 %c, ptr %s1, ptr %s1
  %s3 = select i1 %c, ptr %s2, ptr %s2
  %s4 = select i1 %c, ptr %s3, ptr %s3
  %s5 = select i1 %c, ptr %s4, ptr %s4
  %s6 = select i1 %c, ptr %s5, ptr %s5
  %s7 = select i1 %c, ptr %s6, ptr %s6
  %s8 = select i1 %c, ptr %s7, ptr %s7
  %s9 = select i1 %c, ptr %s8, ptr %s8
  %s10 = select i1 %c, ptr %s9, ptr %s9
  %s11 = select i1 %c, ptr %s10, ptr %s10
  %s12 = select i1 %c, ptr %s11, ptr %s11
  %s13 = select i1 %c, ptr %s12, ptr %s12
  %s14 = select i1 %c, ptr %s13, ptr %s13
  %s15 = select i1 %c, ptr %s14, ptr %s14
  %s16 = select i1 %c, ptr %s15, ptr %s15
  %s17 = select i1 %c, ptr %s16, ptr %s16
  %s18 = select i1 %c, ptr %s17, ptr %s17
  %s19 = select i1 %c, ptr %s18, ptr %s18
  %s20 = select i1 %c, ptr %s19, ptr %s19
  %s21 = select i1 %c, ptr %s20, ptr %s20
  %s22 = select i1 %c, ptr %s21, ptr %s21
  %s23 = select i1 %c, ptr %s22, ptr %s22
  %s24 = select i1 %c, ptr %s23, ptr %s23
  %s25 = select i1 %c, ptr %s24, ptr %s24
  %s26 = select i1 %c, ptr %s25, ptr %s25
  %s27 = select i1 %c, ptr %s26, ptr %s26
  %s28 = select i1 %c, ptr %s27, ptr %s27
  %s29 = select i1 %c, ptr %s28, ptr %s28
  %s30 = select i1 %c, ptr %s29, ptr %s29
  %s31 = select i1 %c, ptr %s30, ptr %s30
  %s32 = select i1 %c, ptr %s31, ptr %s31
  %s33 = select i1 %c, ptr %s32, ptr %s32
  %s34 = select i1 %c, ptr %s33, ptr %s33
  %s35 = select i1 %c, ptr %s34, ptr %s34
  %s36 = select i1 %c, ptr %s35, ptr %s35
  %s37 = select i1 %c, ptr %s36, ptr %s36
  %s38 = select i1 %c, ptr %s37, ptr %s37
  %s39 = select i1 %c, ptr %s38, ptr %s38
  %s40 = select i1 %c, ptr %s39, ptr %s39
  %e = getelementptr i8, ptr %s40, i64 1
  ret ptr %e
}
)");
  guard::RootCache cache;
  EXPECT_EQ(guard::colorRoot(named(run, "e"), &cache),
            run.module->begin()->getArg(0));
  EXPECT_EQ(guard::colorRoot(named(run, "e")), run.module->begin()->getArg(0));
}

} // namespace
} // namespace weavec::frontend
