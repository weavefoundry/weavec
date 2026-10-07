//===- GuardLoops.cpp - Loop ranges
//----------------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §6.4. Before the vectoriser, a loop whose trip count is known
// when it is entered, and in which nothing can end an object's lifetime,
// gets a second version. Its preheader checks the whole range of bytes
// each guarded access whose address is affine in the loop (or invariant)
// covers over the trip; when every range is addressable, the version
// without those guards runs (and may be vectorised), and otherwise the
// guarded loop runs and stops at the access that fails. A range larger
// than what the loop reads only sends it to the guarded loop.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <optional>
#include <vector>

namespace weavec::frontend::guard {

namespace {

/// A guard whose bytes over the whole trip one range covers.
struct Ranged {
  llvm::CallInst *marker;
  const llvm::SCEV *low;
  const llvm::SCEV *high;
  /// The base the guard tests for null, defined before the loop, or null.
  llvm::Value *nullBase = nullptr;
};

/// The analyses a loop's versioning uses and keeps up to date.
struct LoopAnalyses {
  llvm::DominatorTree &dominators;
  llvm::LoopInfo &loops;
  llvm::ScalarEvolution &evolution;
  llvm::AssumptionCache &assumptions;
};

} // namespace

/// The most blocks and instructions a loop may have to be versioned: a
/// copy of each is the cost.
static constexpr unsigned MaxBlocks = 32;
static constexpr unsigned MaxInstructions = 1000;
/// The fewest iterations for which the versioned loop is worth the test
/// of its ranges; a shorter trip runs the guarded loop.
static constexpr unsigned MinTrip = 16;

/// The range of the bytes `marker` guards over the trip of `loop`, whose
/// backedge is taken `count` times (an i64); none when its address is
/// neither invariant nor affine with a constant step.
static std::optional<Ranged> rangeOf(llvm::CallInst &marker, llvm::Loop &loop,
                                     llvm::ScalarEvolution &evolution,
                                     const llvm::SCEV *count) {
  const std::optional<llvm::StringRef> name = ModuleContext::markerOf(marker);
  if (!name || (*name != MarkerGuard && *name != MarkerRange))
    return std::nullopt;
  const auto *flags =
      llvm::dyn_cast<llvm::ConstantInt>(marker.getArgOperand(2));
  if (flags == nullptr || (flags->getZExtValue() & MarkerKeep) != 0)
    return std::nullopt;
  // A guard that tests its base for null: the versioned loop runs only
  // when the base, defined before it, is not.
  llvm::Value *nullBase = nullptr;
  if (*name == MarkerGuard &&
      !llvm::isa<llvm::ConstantPointerNull>(marker.getArgOperand(3))) {
    nullBase = marker.getArgOperand(3);
    if (!loop.isLoopInvariant(nullBase))
      return std::nullopt;
  }
  llvm::Type *i64 = llvm::Type::getInt64Ty(marker.getContext());
  const llvm::SCEV *bytes = evolution.getTruncateOrZeroExtend(
      evolution.getSCEV(marker.getArgOperand(1)), i64);
  if (!evolution.isLoopInvariant(bytes, &loop))
    return std::nullopt;
  const llvm::SCEV *at = evolution.getSCEV(marker.getArgOperand(0));
  if (evolution.isLoopInvariant(at, &loop))
    return Ranged{&marker, at, evolution.getAddExpr(at, bytes), nullBase};
  const auto *recurrence = llvm::dyn_cast<llvm::SCEVAddRecExpr>(at);
  if (recurrence == nullptr || recurrence->getLoop() != &loop ||
      !recurrence->isAffine())
    return std::nullopt;
  const auto *step = llvm::dyn_cast<llvm::SCEVConstant>(
      recurrence->getStepRecurrence(evolution));
  if (step == nullptr)
    return std::nullopt;
  const llvm::SCEV *first = recurrence->getStart();
  const llvm::SCEV *last = evolution.getAddExpr(
      first, evolution.getMulExpr(evolution.getTruncateOrSignExtend(step, i64),
                                  count));
  if (step->getAPInt().isNegative())
    std::swap(first, last);
  return Ranged{&marker, first, evolution.getAddExpr(last, bytes), nullBase};
}

/// The most iterations, and the most instructions over all of them, of a
/// loop GuardLoop unrolls fully: what LLVM's own full unrolling does at
/// `-O2` for a loop without markers, which it does not unroll because a
/// marker is not `willreturn`.
static constexpr unsigned MaxUnrollTrip = 32;
static constexpr unsigned MaxUnrollSize = 300;

/// Unrolls `loop` fully when its trip count is a small constant and its
/// instructions other than the markers are few; whether it did.
static bool unrollSmall(llvm::Loop &loop, LoopAnalyses &analyses,
                        const llvm::TargetTransformInfo &costs) {
  const unsigned trip = analyses.evolution.getSmallConstantTripCount(&loop);
  if (trip < 2 || trip > MaxUnrollTrip || loop.getLoopLatch() == nullptr ||
      loop.getExitingBlock() == nullptr)
    return false;
  unsigned size = 0;
  bool guarded = false;
  for (llvm::BasicBlock *block : loop.blocks())
    for (llvm::Instruction &instruction : *block) {
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        if (ModuleContext::markerOf(*call)) {
          guarded = true;
          continue;
        }
        if (ModuleContext::isScope(*call) || call->cannotDuplicate() ||
            call->isConvergent())
          return false;
      }
      if (!instruction.isDebugOrPseudoInst())
        ++size;
    }
  if (!guarded || size * trip > MaxUnrollSize)
    return false;
  llvm::UnrollLoopOptions options{};
  options.Count = trip;
  options.Force = false;
  options.Runtime = false;
  options.AllowExpensiveTripCount = false;
  options.UnrollRemainder = false;
  options.ForgetAllSCEV = false;
  options.SCEVExpansionBudget = 0;
  return llvm::UnrollLoop(
             &loop, options, &analyses.loops, &analyses.evolution,
             &analyses.dominators, &analyses.assumptions, &costs, nullptr,
             /*PreserveLCSSA=*/true) == llvm::LoopUnrollResult::FullyUnrolled;
}

/// Whether `loop` can be versioned: one preheader and one exit block, a
/// computable trip count, nothing that ends a lifetime, and not too big.
static bool versionable(llvm::Loop &loop, llvm::ScalarEvolution &evolution) {
  if (loop.getLoopPreheader() == nullptr || loop.getExitBlock() == nullptr ||
      !loop.hasDedicatedExits() || loop.getNumBlocks() > MaxBlocks)
    return false;
  const llvm::SCEV *count = evolution.getBackedgeTakenCount(&loop);
  if (llvm::isa<llvm::SCEVCouldNotCompute>(count))
    return false;
  // A constant trip too short for the versioned loop ever to run.
  if (const auto *constant = llvm::dyn_cast<llvm::SCEVConstant>(count);
      constant != nullptr && constant->getAPInt().ult(MinTrip - 1))
    return false;
  unsigned instructions = 0;
  for (llvm::BasicBlock *block : loop.blocks())
    for (llvm::Instruction &instruction : *block) {
      if (++instructions > MaxInstructions || killsGuards(instruction))
        return false;
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
        if (ModuleContext::isScope(*call))
          return false;
      // A value the loop defines and its exit does not take through a phi
      // (not in LCSSA form).
      for (const llvm::User *user : instruction.users())
        if (const auto *used = llvm::dyn_cast<llvm::Instruction>(user);
            used != nullptr && !loop.contains(used) &&
            !llvm::isa<llvm::PHINode>(used))
          return false;
    }
  return true;
}

/// The run-time test that every range is addressable, at `before`.
static llvm::Value *rangesAddressable(ModuleContext &module,
                                      llvm::ArrayRef<Ranged> ranges,
                                      llvm::ScalarEvolution &evolution,
                                      llvm::Instruction *before) {
  llvm::SCEVExpander expander(evolution, "weavec.range");
  llvm::IRBuilder<> builder(before);
  llvm::FunctionCallee test = module.runtime(
      "range_ok",
      llvm::FunctionType::get(module.i32, {module.i64, module.i64}, false));
  if (auto *function = llvm::dyn_cast<llvm::Function>(test.getCallee())) {
    function->setOnlyReadsMemory();
    function->setWillReturn();
  }
  llvm::Value *all = nullptr;
  for (const Ranged &range : ranges) {
    llvm::Value *low = expander.expandCodeFor(range.low, nullptr, before);
    llvm::Value *high = expander.expandCodeFor(range.high, nullptr, before);
    builder.SetInsertPoint(before);
    llvm::Value *lowAddress = builder.CreatePtrToInt(low, module.i64);
    llvm::Value *highAddress = builder.CreatePtrToInt(high, module.i64);
    // A range that wraps is not addressable.
    llvm::Value *ok = builder.CreateAnd(
        builder.CreateICmpULE(lowAddress, highAddress),
        builder.CreateICmpNE(
            builder.CreateCall(
                test, {lowAddress, builder.CreateSub(highAddress, lowAddress)}),
            llvm::ConstantInt::get(module.i32, 0)));
    if (range.nullBase != nullptr)
      ok = builder.CreateAnd(ok, builder.CreateIsNotNull(range.nullBase));
    all = all == nullptr ? ok : builder.CreateAnd(all, ok);
  }
  return all;
}

/// Versions `loop` on `ranges`: the original loop loses their guards and
/// runs when the ranges are addressable; a copy that keeps them runs
/// otherwise.
static void versionLoop(ModuleContext &module, llvm::Loop &loop,
                        llvm::ArrayRef<Ranged> ranges, const llvm::SCEV *count,
                        LoopAnalyses &analyses) {
  llvm::BasicBlock *check = loop.getLoopPreheader();
  llvm::BasicBlock *exit = loop.getExitBlock();
  llvm::Function &function = *check->getParent();
  // Every expansion comes first, while the loop is the only one: the
  // expander keeps LCSSA form, and would reach into a copy not yet linked.
  // A short trip runs the guarded loop without the ranges' cost; a longer
  // one checks them.
  llvm::Value *enough = nullptr;
  {
    llvm::SCEVExpander expander(analyses.evolution, "weavec.trip");
    llvm::Instruction *before = check->getTerminator();
    llvm::Value *taken = expander.expandCodeFor(count, nullptr, before);
    enough = llvm::IRBuilder<>(before).CreateICmpUGE(
        taken, llvm::ConstantInt::get(taken->getType(), MinTrip - 1));
  }
  llvm::BasicBlock *test =
      llvm::SplitBlock(check, check->getTerminator(), &analyses.dominators,
                       &analyses.loops, nullptr, "weavec.ranges");
  llvm::Value *ok = rangesAddressable(module, ranges, analyses.evolution,
                                      test->getTerminator());
  llvm::BasicBlock *preheader =
      llvm::SplitBlock(test, test->getTerminator(), &analyses.dominators,
                       &analyses.loops, nullptr, "weavec.ranged");
  llvm::ValueToValueMapTy map;
  llvm::SmallVector<llvm::BasicBlock *, 16> blocks;
  llvm::Loop *guarded = llvm::cloneLoopWithPreheader(
      preheader, test, &loop, map, ".guarded", &analyses.loops,
      &analyses.dominators, blocks);
  llvm::remapInstructionsInBlocks(blocks, map);
  llvm::BasicBlock *guardedPreheader = guarded->getLoopPreheader();
  auto branch = [](llvm::BasicBlock *block, llvm::Value *condition,
                   llvm::BasicBlock *taken, llvm::BasicBlock *other) {
    llvm::Instruction *old = block->getTerminator();
    llvm::IRBuilder<>(old).CreateCondBr(condition, taken, other);
    old->eraseFromParent();
  };
  branch(test, ok, preheader, guardedPreheader);
  branch(check, enough, test, guardedPreheader);
  analyses.dominators.recalculate(function);
  // The exit's phis take each value from either loop.
  for (llvm::PHINode &phi : exit->phis()) {
    const unsigned incoming = phi.getNumIncomingValues();
    for (unsigned at = 0; at < incoming; ++at) {
      llvm::BasicBlock *from = phi.getIncomingBlock(at);
      if (!loop.contains(from))
        continue;
      llvm::Value *value = phi.getIncomingValue(at);
      if (llvm::Value *copy = map.lookup(value))
        value = copy;
      phi.addIncoming(value, llvm::cast<llvm::BasicBlock>(map[from]));
    }
  }
  // The copy's guards stay where they are; the original's ranged ones go.
  for (llvm::BasicBlock *block : blocks)
    for (llvm::Instruction &instruction : *block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (const std::optional<llvm::StringRef> name =
                ModuleContext::markerOf(*call);
            name && (*name == MarkerGuard || *name == MarkerRange))
          if (const auto *flags =
                  llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2)))
            call->setArgOperand(
                2, llvm::ConstantInt::get(module.i32,
                                          flags->getZExtValue() | MarkerKeep));
  for (const Ranged &range : ranges)
    range.marker->eraseFromParent();
  analyses.evolution.forgetLoop(&loop);
}

bool versionLoops(ModuleContext &module, llvm::Function &function,
                  llvm::FunctionAnalysisManager &manager) {
  LoopAnalyses analyses{
      manager.getResult<llvm::DominatorTreeAnalysis>(function),
      manager.getResult<llvm::LoopAnalysis>(function),
      manager.getResult<llvm::ScalarEvolutionAnalysis>(function),
      manager.getResult<llvm::AssumptionAnalysis>(function)};
  std::vector<llvm::Loop *> innermost;
  for (llvm::Loop *loop : analyses.loops.getLoopsInPreorder())
    if (loop->isInnermost())
      innermost.push_back(loop);
  bool changed = false;
  llvm::Type *i64 = llvm::Type::getInt64Ty(function.getContext());
  const llvm::TargetTransformInfo &costs =
      manager.getResult<llvm::TargetIRAnalysis>(function);
  for (llvm::Loop *loop : innermost) {
    // Dedicated exits and LCSSA form, which the copy's exit phis need.
    changed |= llvm::simplifyLoop(loop, &analyses.dominators, &analyses.loops,
                                  &analyses.evolution, &analyses.assumptions,
                                  nullptr, /*PreserveLCSSA=*/false);
    changed |= llvm::formLCSSA(*loop, analyses.dominators, &analyses.loops,
                               &analyses.evolution);
    if (unrollSmall(*loop, analyses, costs)) {
      changed = true;
      continue;
    }
    if (!versionable(*loop, analyses.evolution))
      continue;
    const llvm::SCEV *count = analyses.evolution.getTruncateOrZeroExtend(
        analyses.evolution.getBackedgeTakenCount(loop), i64);
    llvm::SCEVExpander expander(analyses.evolution, "");
    std::vector<Ranged> ranges;
    for (llvm::BasicBlock *block : loop->blocks())
      for (llvm::Instruction &instruction : *block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
          if (std::optional<Ranged> range =
                  rangeOf(*call, *loop, analyses.evolution, count);
              range && expander.isSafeToExpand(range->low) &&
              expander.isSafeToExpand(range->high))
            ranges.push_back(*range);
    if (ranges.empty())
      continue;
    versionLoop(module, *loop, ranges, count, analyses);
    changed = true;
  }
  return changed;
}

} // namespace weavec::frontend::guard
