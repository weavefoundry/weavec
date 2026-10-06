//===- GuardPasses.cpp - Guards the backend lowers ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/GuardPasses.h"

#include "clang/Basic/CodeGenOptions.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/KnownBits.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace weavec::frontend {

namespace {

/// The kind bits of `__weavec_rt_guard` (runtime/weavec_rt.h).
constexpr std::uint64_t KindObject = 0;
constexpr std::uint64_t KindLive = 1;
constexpr std::uint64_t KindMask = 3;
constexpr std::uint64_t KindOverflow = 4;
constexpr std::uint64_t KindProven = 8;

/// What the name of a declared guard says.
struct DeclaredGuard {
  bool live = false;
  bool proven = false;
  bool report = false;
  /// A call argument's (RFC 0032 §3): `object_n(p, need)` returns `p`,
  /// `object_l(p, need)` returns `need`.
  bool bytes = false;
  bool length = false;
};

/// Guards above which a function loads the descriptor at each guard.
constexpr std::size_t MaxHoistedGuards = 1024;

/// The offsets of `base`, `bytes`, `shadow` and `mask` in
/// `__weavec_rt_heap`.
constexpr unsigned HeapBase = 0;
constexpr unsigned HeapBytes = 8;
constexpr unsigned HeapShadow = 16;
constexpr unsigned HeapMask = 24;

} // namespace

static std::optional<DeclaredGuard> declaredGuard(llvm::StringRef name) {
  DeclaredGuard guard;
  if (name.consume_front("__weavec_prv_"))
    guard.proven = true;
  else if (!name.consume_front("__weavec_chk_"))
    return std::nullopt;
  guard.report = name.consume_back("_report");
  guard.bytes = name.consume_back("_n");
  guard.length = !guard.bytes && name.consume_back("_l");
  if (name == "live" && !guard.bytes && !guard.length)
    guard.live = true;
  else if (name != "object")
    return std::nullopt;
  return guard;
}

/// `__weavec_rt_guard`, declared with the attributes of §1.2.
static llvm::Function *slowPath(llvm::Module &module) {
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *ptr = llvm::PointerType::getUnqual(context);
  auto *type =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {ptr, ptr, llvm::Type::getInt64Ty(context),
                               llvm::Type::getInt32Ty(context), ptr},
                              false);
  llvm::FunctionCallee callee = module.getOrInsertFunction(GuardSlowPath, type);
  auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee());
  if (function == nullptr || function->getFunctionType() != type)
    return nullptr;
  function->setDoesNotThrow();
  function->setMemoryEffects(
      llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref));
  return function;
}

static bool isSlowPathCall(const llvm::Instruction &instruction) {
  const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
  const llvm::Function *callee =
      call != nullptr ? call->getCalledFunction() : nullptr;
  return callee != nullptr && callee->getName() == GuardSlowPath &&
         call->arg_size() == 5;
}

/// Report mode's site record `{file, line, column}` for constant operands.
static llvm::Constant *siteRecord(llvm::Module &module, llvm::Value *file,
                                  llvm::Value *line, llvm::Value *column) {
  auto *fileConstant = llvm::dyn_cast<llvm::Constant>(file);
  const auto *lineConstant = llvm::dyn_cast<llvm::ConstantInt>(line);
  const auto *columnConstant = llvm::dyn_cast<llvm::ConstantInt>(column);
  if (fileConstant == nullptr || lineConstant == nullptr ||
      columnConstant == nullptr)
    return nullptr;
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  auto *type = llvm::StructType::get(
      context, {llvm::PointerType::getUnqual(context), i32, i32});
  auto *initializer = llvm::ConstantStruct::get(
      type,
      {fileConstant, llvm::ConstantInt::get(i32, lineConstant->getZExtValue()),
       llvm::ConstantInt::get(i32, columnConstant->getZExtValue())});
  auto *record = new llvm::GlobalVariable(module, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          initializer, "__weavec.site");
  record->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  return record;
}

//===----------------------------------------------------------------------===//
// GuardCanonicalize
//===----------------------------------------------------------------------===//

// NOLINTBEGIN(readability-convert-member-functions-to-static): the pass
// manager calls `run` on an instance.
llvm::PreservedAnalyses
GuardCanonicalize::run(llvm::Function &function,
                       llvm::FunctionAnalysisManager & /*analyses*/) {
  // NOLINTEND(readability-convert-member-functions-to-static)
  llvm::Module &module = *function.getParent();
  if (module.getDataLayout().getPointerSizeInBits() != 64)
    return llvm::PreservedAnalyses::all();
  std::vector<std::pair<llvm::CallInst *, DeclaredGuard>> calls;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (const llvm::Function *callee = call->getCalledFunction())
          if (std::optional<DeclaredGuard> guard =
                  declaredGuard(callee->getName()))
            calls.emplace_back(call, *guard);
  if (calls.empty())
    return llvm::PreservedAnalyses::all();
  llvm::Function *slow = slowPath(module);
  if (slow == nullptr)
    return llvm::PreservedAnalyses::all();
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *i8 = llvm::Type::getInt8Ty(context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  bool changed = false;
  for (auto [call, guard] : calls) {
    const bool argument = guard.bytes || guard.length;
    unsigned operands = argument ? 2 : 5;
    if (guard.live)
      operands = 1;
    if (call->arg_size() != operands + (guard.report ? 3 : 0) ||
        !(guard.length ? call->getType()->isIntegerTy(64)
                       : call->getType()->isPointerTy()))
      continue;
    // A call argument's guard of a constant need is the guard of that many
    // bytes from the pointer; another stays the runtime's call.
    auto *need = argument
                     ? llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1))
                     : nullptr;
    if (argument && need == nullptr)
      continue;
    llvm::Value *site =
        llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context));
    if (guard.report) {
      llvm::Constant *record = siteRecord(module, call->getArgOperand(operands),
                                          call->getArgOperand(operands + 1),
                                          call->getArgOperand(operands + 2));
      if (record == nullptr)
        continue;
      site = record;
    }
    llvm::IRBuilder<> builder(call);
    llvm::Value *pointer = call->getArgOperand(0);
    const std::uint64_t bits = guard.proven ? KindProven : 0;
    if (argument) {
      if (!need->isZero())
        builder.CreateCall(
            slow, {pointer, pointer, need,
                   llvm::ConstantInt::get(i32, KindObject | bits), site});
      call->replaceAllUsesWith(guard.length ? need : pointer);
      call->eraseFromParent();
      changed = true;
      continue;
    }
    if (guard.live) {
      builder.CreateCall(slow,
                         {pointer, pointer, llvm::ConstantInt::get(i64, 0),
                          llvm::ConstantInt::get(i32, KindLive | bits), site});
      call->replaceAllUsesWith(pointer);
      call->eraseFromParent();
      changed = true;
      continue;
    }
    llvm::Value *index = call->getArgOperand(1);
    llvm::Value *step = call->getArgOperand(2);
    llvm::Value *offset = call->getArgOperand(3);
    llvm::Value *width = call->getArgOperand(4);
    llvm::Value *delta = nullptr;
    llvm::Value *kind = llvm::ConstantInt::get(i32, KindObject | bits);
    const auto *constantIndex = llvm::dyn_cast<llvm::ConstantInt>(index);
    const auto *constantStep = llvm::dyn_cast<llvm::ConstantInt>(step);
    if (constantIndex != nullptr && constantStep != nullptr) {
      bool overflow = false;
      const llvm::APInt product =
          constantIndex->getValue().smul_ov(constantStep->getValue(), overflow);
      // (The helper fails such a guard; leave it to do so.)
      if (overflow)
        continue;
      delta = llvm::ConstantInt::get(i64, product);
    } else {
      llvm::Value *product = builder.CreateBinaryIntrinsic(
          llvm::Intrinsic::smul_with_overflow, index, step);
      delta = builder.CreateExtractValue(product, 0);
      llvm::Value *overflowed = builder.CreateExtractValue(product, 1);
      kind = builder.CreateOr(
          kind, builder.CreateShl(builder.CreateZExt(overflowed, i32), 2));
    }
    llvm::Value *element = builder.CreateGEP(i8, pointer, delta);
    llvm::Value *at = builder.CreateGEP(i8, element, offset);
    builder.CreateCall(slow, {pointer, at, width, kind, site});
    call->replaceAllUsesWith(element);
    call->eraseFromParent();
    changed = true;
  }
  return changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

//===----------------------------------------------------------------------===//
// GuardMerge
//===----------------------------------------------------------------------===//

namespace {

/// A canonical guard with constant width and kind, as bytes of a base.
struct GuardFacts {
  llvm::CallInst *call = nullptr;
  const llvm::Value *from = nullptr;
  llvm::Value *base = nullptr;
  std::int64_t offset = 0;
  std::int64_t width = 0;
  bool live = false;
  std::uint64_t kind = 0;
};

} // namespace

static std::optional<GuardFacts> factsOf(llvm::CallInst &call,
                                         const llvm::DataLayout &layout) {
  if (!isSlowPathCall(call))
    return std::nullopt;
  const auto *width = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(2));
  const auto *kind = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(3));
  if (width == nullptr || kind == nullptr ||
      (kind->getZExtValue() & KindOverflow) != 0 ||
      !llvm::isa<llvm::ConstantPointerNull>(call.getArgOperand(4)))
    return std::nullopt;
  GuardFacts facts;
  facts.call = &call;
  facts.kind = kind->getZExtValue();
  facts.live = (facts.kind & KindMask) == KindLive;
  facts.width = static_cast<std::int64_t>(width->getZExtValue());
  facts.from = call.getArgOperand(0)->stripPointerCasts();
  llvm::APInt offset(64, 0);
  facts.base = call.getArgOperand(1)->stripAndAccumulateConstantOffsets(
      layout, offset, /*AllowNonInbounds=*/true);
  if (offset.getSignificantBits() > 62 || facts.width < 0 ||
      facts.width > std::int64_t{1048576})
    return std::nullopt;
  facts.offset = offset.getSExtValue();
  return facts;
}

/// Whether passing `earlier` means `later` passes (same family).
static bool covers(const GuardFacts &earlier, const GuardFacts &later) {
  if (earlier.base != later.base ||
      (earlier.kind & KindProven) != (later.kind & KindProven))
    return false;
  if (later.live) {
    if (earlier.live)
      return earlier.offset == later.offset;
    // The byte at the pointer is a live object's.
    return earlier.width >= 1 && earlier.offset <= later.offset &&
           later.offset < earlier.offset + earlier.width;
  }
  // The slow path looks at `from` only for stack and global objects; the
  // same `from` gives the same answer for the same bytes.
  return !earlier.live && earlier.from == later.from &&
         earlier.offset <= later.offset &&
         later.offset + later.width <= earlier.offset + earlier.width;
}

/// Whether nothing between `earlier` and `later` (which it dominates) may
/// release or register an object.
static bool unclobbered(llvm::MemorySSA &memory, const GuardFacts &earlier,
                        const GuardFacts &later) {
  llvm::MemoryAccess *laterAccess = memory.getMemoryAccess(later.call);
  const llvm::MemoryAccess *earlierAccess =
      memory.getMemoryAccess(earlier.call);
  if (laterAccess == nullptr || earlierAccess == nullptr)
    return false;
  const llvm::MemoryAccess *clobber =
      memory.getWalker()->getClobberingMemoryAccess(laterAccess);
  return clobber != nullptr && memory.dominates(clobber, earlierAccess);
}

/// Whether control always reaches `last` once `first` (in the same block,
/// before it) returns.
static bool reachedTogether(const llvm::Instruction &first,
                            const llvm::Instruction &last) {
  for (const llvm::Instruction *at = first.getNextNode();
       at != nullptr && at != &last; at = at->getNextNode())
    if (!isSlowPathCall(*at) &&
        !llvm::isGuaranteedToTransferExecutionToSuccessor(at))
      return false;
  return true;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static): the pass
// manager calls `run` on an instance.
llvm::PreservedAnalyses
GuardMerge::run(llvm::Function &function,
                llvm::FunctionAnalysisManager &analyses) {
  // NOLINTEND(readability-convert-member-functions-to-static)
  const llvm::DataLayout &layout = function.getParent()->getDataLayout();
  std::vector<GuardFacts> guards;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (std::optional<GuardFacts> facts = factsOf(*call, layout))
          guards.push_back(*facts);
  if (guards.size() < 2)
    return llvm::PreservedAnalyses::all();
  const auto &tree = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
  auto &memory =
      analyses.getResult<llvm::MemorySSAAnalysis>(function).getMSSA();

  // Hulls first: consecutive guards of one block on one base, as one guard
  // of at most 16 bytes (the inline check's span).
  bool changed = false;
  std::vector<bool> erased(guards.size(), false);
  for (std::size_t i = 0; i < guards.size(); ++i) {
    if (erased[i] || guards[i].live)
      continue;
    for (std::size_t j = i + 1; j < guards.size(); ++j) {
      if (erased[j] || guards[j].live)
        continue;
      GuardFacts &first = guards[i];
      const GuardFacts &next = guards[j];
      if (next.call->getParent() != first.call->getParent())
        break;
      if (next.base != first.base || next.from != first.from ||
          next.kind != first.kind)
        continue;
      const std::int64_t low = std::min(first.offset, next.offset);
      const std::int64_t high =
          std::max(first.offset + first.width, next.offset + next.width);
      if (high - low > 16 || !reachedTogether(*first.call, *next.call) ||
          !unclobbered(memory, first, next))
        continue;
      if (low != first.offset) {
        llvm::IRBuilder<> builder(first.call);
        first.call->setArgOperand(
            1, builder.CreateGEP(
                   builder.getInt8Ty(), first.base,
                   builder.getInt64(static_cast<std::uint64_t>(low))));
      }
      first.call->setArgOperand(
          2, llvm::ConstantInt::get(first.call->getArgOperand(2)->getType(),
                                    static_cast<std::uint64_t>(high - low)));
      first.offset = low;
      first.width = high - low;
      erased[j] = true;
      changed = true;
    }
  }
  // Then whatever a dominating guard covers.
  for (std::size_t j = 0; j < guards.size(); ++j) {
    if (erased[j])
      continue;
    for (std::size_t i = 0; i < guards.size(); ++i) {
      if (i == j || erased[i] || !covers(guards[i], guards[j]) ||
          !tree.dominates(guards[i].call, guards[j].call) ||
          !unclobbered(memory, guards[i], guards[j]))
        continue;
      erased[j] = true;
      changed = true;
      break;
    }
  }
  for (std::size_t i = 0; i < guards.size(); ++i)
    if (erased[i]) {
      if (llvm::MemoryAccess *access = memory.getMemoryAccess(guards[i].call))
        llvm::MemorySSAUpdater(&memory).removeMemoryAccess(access);
      guards[i].call->eraseFromParent();
    }
  if (!changed)
    return llvm::PreservedAnalyses::all();
  llvm::PreservedAnalyses kept;
  kept.preserveSet<llvm::CFGAnalyses>();
  kept.preserve<llvm::MemorySSAAnalysis>();
  return kept;
}

//===----------------------------------------------------------------------===//
// GuardExpand
//===----------------------------------------------------------------------===//

/// The alignment of the access the guard `call` is for: a load or store of
/// its bytes later in its block, or 1.
static std::uint64_t accessAlignment(const llvm::CallInst &call,
                                     const llvm::DataLayout &layout) {
  llvm::APInt offset(64, 0);
  const llvm::Value *base =
      call.getArgOperand(1)->stripAndAccumulateConstantOffsets(
          layout, offset, /*AllowNonInbounds=*/true);
  for (const llvm::Instruction *at = call.getNextNode(); at != nullptr;
       at = at->getNextNode()) {
    const llvm::Value *pointer = nullptr;
    llvm::Align align;
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(at)) {
      pointer = load->getPointerOperand();
      align = load->getAlign();
    } else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(at)) {
      pointer = store->getPointerOperand();
      align = store->getAlign();
    } else if (llvm::isa<llvm::CallBase>(at) && !isSlowPathCall(*at)) {
      break;
    }
    if (pointer == nullptr)
      continue;
    llvm::APInt accessOffset(64, 0);
    if (pointer->stripAndAccumulateConstantOffsets(
            layout, accessOffset, /*AllowNonInbounds=*/true) == base &&
        accessOffset == offset)
      return align.value();
  }
  return 1;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static): the pass
// manager calls `run` on an instance.
llvm::PreservedAnalyses
GuardExpand::run(llvm::Function &function,
                 llvm::FunctionAnalysisManager & /*analyses*/) {
  // NOLINTEND(readability-convert-member-functions-to-static)
  llvm::Module &module = *function.getParent();
  const llvm::DataLayout &layout = module.getDataLayout();
  if (layout.getPointerSizeInBits() != 64)
    return llvm::PreservedAnalyses::all();
  std::vector<llvm::CallInst *> calls;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (isSlowPathCall(instruction))
        calls.push_back(llvm::cast<llvm::CallInst>(&instruction));
  if (calls.empty())
    return llvm::PreservedAnalyses::all();
  llvm::LLVMContext &context = module.getContext();
  llvm::Type *i8 = llvm::Type::getInt8Ty(context);
  llvm::Type *i64 = llvm::Type::getInt64Ty(context);
  llvm::Constant *heap = module.getOrInsertGlobal("__weavec_rt_heap",
                                                  llvm::ArrayType::get(i64, 4));
  llvm::MDNode *invariant = llvm::MDNode::get(context, {});
  llvm::MDBuilder metadata(context);
  llvm::MDNode *likely = metadata.createLikelyBranchWeights();
  bool changed = false;
  // §1.4: the descriptor, loaded once in the entry block (a function that
  // starts before the runtime does sees an empty arena and a zero mask, and
  // asks the runtime, which is correct for every address).
  // A function with thousands of guards loads the arena's bounds at each
  // guard instead, as plain loads: thousands of uses of them made the code
  // generator's common-subexpression pass quadratic. The shadow's base and
  // mask, which every guard reads first, are loaded once at the entry.
  const bool hoisted = calls.size() <= MaxHoistedGuards;
  // The descriptor's fields, loaded once at the entry when hoisted.
  struct Field {
    unsigned offset;
    llvm::Value *value;
  };
  std::array<Field, 4> descriptor = {
      Field{.offset = HeapBase, .value = nullptr},
      Field{.offset = HeapBytes, .value = nullptr},
      Field{.offset = HeapShadow, .value = nullptr},
      Field{.offset = HeapMask, .value = nullptr}};
  {
    llvm::IRBuilder<> entry(&*function.getEntryBlock().getFirstInsertionPt());
    for (Field &field : descriptor) {
      if (!hoisted && field.offset != HeapShadow && field.offset != HeapMask)
        continue;
      llvm::LoadInst *load = entry.CreateAlignedLoad(
          i64, entry.CreateConstGEP1_64(i8, heap, field.offset),
          llvm::Align(8));
      load->setMetadata(llvm::LLVMContext::MD_invariant_load, invariant);
      field.value = load;
    }
  }
  for (llvm::CallInst *call : calls) {
    const auto *width =
        llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
    llvm::Value *kind = call->getArgOperand(3);
    const auto *kindConstant = llvm::dyn_cast<llvm::ConstantInt>(kind);
    // The kind is constant but for its overflow bit (§1.2).
    std::uint64_t staticKind = 0;
    if (kindConstant != nullptr) {
      staticKind = kindConstant->getZExtValue();
      if ((staticKind & KindOverflow) != 0)
        continue;
    } else {
      const llvm::KnownBits known = llvm::computeKnownBits(kind, layout);
      const llvm::APInt fixed = known.Zero | known.One;
      if ((~fixed).getZExtValue() != KindOverflow)
        continue;
      staticKind = known.One.getZExtValue();
    }
    const bool live = (staticKind & KindMask) == KindLive;
    const std::uint64_t bytes = width != nullptr ? width->getZExtValue() : 0;
    if (!live && (bytes == 0 || bytes > 16))
      continue;
    // An access whose alignment is a multiple of its width stays in one
    // granule.
    const bool oneGranule =
        live || bytes == 1 ||
        (llvm::isPowerOf2_64(bytes) && accessAlignment(*call, layout) >= bytes);

    // head: the shadow byte; check: a partial granule or a crossing access;
    // slow: the call; after: the rest of the block.
    llvm::BasicBlock *head = call->getParent();
    llvm::BasicBlock *slow =
        head->splitBasicBlock(call->getIterator(), "weavec.guard.slow");
    llvm::BasicBlock *after = slow->splitBasicBlock(
        std::next(call->getIterator()), head->getName() + ".guarded");
    head->getTerminator()->eraseFromParent();
    llvm::IRBuilder<> builder(head);
    const auto field = [&](unsigned offset) -> llvm::Value * {
      for (const Field &each : descriptor)
        if (each.offset == offset && each.value != nullptr)
          return each.value;
      return builder.CreateAlignedLoad(
          i64, builder.CreateConstGEP1_64(i8, heap, offset), llvm::Align(8));
    };
    llvm::Value *address =
        builder.CreatePtrToInt(call->getArgOperand(live ? 0 : 1), i64);
    // §2.3: the granule's byte, at shadow + ((a >> 4) & mask).
    llvm::Value *byteAt = builder.CreateGEP(
        i8, builder.CreateIntToPtr(field(HeapShadow), builder.getPtrTy()),
        builder.CreateAnd(builder.CreateLShr(address, 4), field(HeapMask)));
    llvm::Value *byte = builder.CreateLoad(i8, byteAt);
    llvm::Value *overflowFree = nullptr;
    if (kindConstant == nullptr)
      overflowFree = builder.CreateICmpEQ(
          builder.CreateAnd(kind, builder.getInt32(KindOverflow)),
          builder.getInt32(0));
    const auto withOverflow = [&](llvm::Value *condition) {
      return overflowFree != nullptr
                 ? builder.CreateAnd(condition, overflowFree)
                 : condition;
    };
    // An address outside the arena whose byte is 0 is untracked (every
    // tracked object outside the arena has a non-zero byte).
    const auto outsideArena = [&] {
      return builder.CreateICmpUGE(builder.CreateSub(address, field(HeapBase)),
                                   field(HeapBytes));
    };
    // 0x41-0x50: the last granule, with v - 0x40 bytes (r is 0); 0x81-0xBF:
    // at least r granules follow in the object (`run`). Granules d apart are
    // of one object when d is at most the earlier one's r. The
    // bytes at `end` (in the granule's terms) are in one stack or global
    // object with the byte `from` points to.
    const auto exactObject = [&](llvm::Value *count, llvm::Value *fromCount,
                                 llvm::Value *fromAddress, llvm::Value *end) {
      const auto exact = [&](llvm::Value *value) {
        return builder.CreateOr(
            builder.CreateAnd(
                builder.CreateICmpUGT(value, builder.getInt64(0x40)),
                builder.CreateICmpULE(value, builder.getInt64(0x50))),
            builder.CreateAnd(
                builder.CreateICmpUGT(value, builder.getInt64(0x80)),
                builder.CreateICmpULE(value, builder.getInt64(0xBF))));
      };
      // The least r a run byte says: v - 0x80 up to 0xB0, 2^(v - 0xAC)
      // above (runtime/weavec_rt.h).
      const auto run = [&](llvm::Value *value) {
        return builder.CreateSelect(
            builder.CreateICmpUGT(value, builder.getInt64(0x80)),
            builder.CreateSelect(
                builder.CreateICmpULE(value, builder.getInt64(0xB0)),
                builder.CreateSub(value, builder.getInt64(0x80)),
                builder.CreateShl(
                    builder.getInt64(1),
                    builder.CreateSub(value, builder.getInt64(0xAC)))),
            builder.getInt64(0));
      };
      // (Two or more granules after it: the next one is whole, and holds
      // the rest of an access that crosses into it.)
      llvm::Value *bytesThere = builder.CreateSelect(
          builder.CreateICmpULE(count, builder.getInt64(0x50)),
          builder.CreateSub(count, builder.getInt64(0x40)),
          builder.CreateSelect(
              builder.CreateICmpUGE(count, builder.getInt64(0x82)),
              builder.getInt64(32), builder.getInt64(16)));
      llvm::Value *distance = builder.CreateSub(
          builder.CreateLShr(address, 4), builder.CreateLShr(fromAddress, 4));
      llvm::Value *sameObject = builder.CreateSelect(
          builder.CreateICmpSGE(distance, builder.getInt64(0)),
          builder.CreateICmpULE(distance, run(fromCount)),
          builder.CreateICmpULE(builder.CreateNeg(distance), run(count)));
      return builder.CreateAnd(
          builder.CreateAnd(exact(count), exact(fromCount)),
          builder.CreateAnd(sameObject,
                            builder.CreateICmpULE(end, bytesThere)));
    };
    // A function with thousands of guards gets the compact form: two
    // blocks, the runtime for anything else (the full form's blocks kept
    // the code generator on such a function for seconds).
    if (!hoisted) {
      llvm::Value *inGranule =
          oneGranule ? builder.getTrue()
                     : builder.CreateICmpULE(
                           builder.CreateAdd(
                               builder.CreateAnd(address, builder.getInt64(15)),
                               builder.getInt64(bytes)),
                           builder.getInt64(16));
      llvm::Value *passes =
          live
              ? builder.CreateICmpNE(byte, builder.getInt8(0))
              : builder.CreateAnd(
                    builder.CreateICmpEQ(byte, builder.getInt8(16)), inGranule);
      if (live) {
        builder.CreateCondBr(withOverflow(passes), after, slow, likely);
        changed = true;
        continue;
      }
      // Untracked bytes (a byte of 0 outside the arena) reached from
      // untracked memory pass too, a frame's `alloca` buffers, and so do a
      // stack or global object's bytes reached from inside it.
      llvm::BasicBlock *untracked = llvm::BasicBlock::Create(
          context, "weavec.guard.untracked", &function, slow);
      builder.CreateCondBr(withOverflow(passes), after, untracked, likely);
      builder.SetInsertPoint(untracked);
      llvm::Value *from = call->getArgOperand(0);
      llvm::Value *fromAddress = builder.CreatePtrToInt(from, i64);
      llvm::Value *fromByte = byte;
      if (from->stripPointerCasts() !=
          call->getArgOperand(1)->stripPointerCasts())
        fromByte = builder.CreateLoad(
            i8,
            builder.CreateGEP(
                i8,
                builder.CreateIntToPtr(field(HeapShadow), builder.getPtrTy()),
                builder.CreateAnd(builder.CreateLShr(fromAddress, 4),
                                  field(HeapMask))));
      llvm::Value *passesUntracked = builder.CreateAnd(
          builder.CreateAnd(builder.CreateICmpEQ(byte, builder.getInt8(0)),
                            builder.CreateICmpEQ(fromByte, builder.getInt8(0))),
          builder.CreateAnd(inGranule, outsideArena()));
      llvm::Value *end =
          builder.CreateAdd(builder.CreateAnd(address, builder.getInt64(15)),
                            builder.getInt64(bytes));
      llvm::Value *count = builder.CreateZExt(byte, i64);
      llvm::Value *object = exactObject(
          count, builder.CreateZExt(fromByte, i64), fromAddress, end);
      // A whole heap granule followed by one that holds the rest.
      llvm::Value *crossing = builder.getFalse();
      if (!oneGranule) {
        llvm::Value *next = builder.CreateZExt(
            builder.CreateLoad(i8, builder.CreateConstGEP1_64(i8, byteAt, 1)),
            i64);
        crossing = builder.CreateAnd(
            builder.CreateICmpEQ(count, builder.getInt64(16)),
            builder.CreateAnd(
                builder.CreateICmpULE(next, builder.getInt64(16)),
                builder.CreateICmpULE(
                    builder.CreateSub(end, builder.getInt64(16)), next)));
      }
      builder.CreateCondBr(
          withOverflow(builder.CreateOr(
              builder.CreateOr(passesUntracked, object), crossing)),
          after, slow);
      changed = true;
      continue;
    }
    llvm::BasicBlock *check = llvm::BasicBlock::Create(
        context, "weavec.guard.check", &function, slow);
    if (live) {
      builder.CreateCondBr(
          withOverflow(builder.CreateICmpNE(byte, builder.getInt8(0))), after,
          check, likely);
      builder.SetInsertPoint(check);
      builder.CreateCondBr(withOverflow(outsideArena()), after, slow);
      changed = true;
      continue;
    }
    // e = (a & 15) + w: the end of the bytes in the granule's terms. A whole
    // granule (16) holds an access that stays in it.
    llvm::Value *end =
        builder.CreateAdd(builder.CreateAnd(address, builder.getInt64(15)),
                          builder.getInt64(bytes));
    llvm::Value *whole = builder.CreateICmpEQ(byte, builder.getInt8(16));
    if (!oneGranule)
      whole = builder.CreateAnd(
          whole, builder.CreateICmpULE(end, builder.getInt64(16)));
    builder.CreateCondBr(withOverflow(whole), after, check, likely);
    // A partial granule (1-15) holds the bytes when its count covers e; a
    // whole granule followed by one that covers e - 16 holds a crossing
    // access; untracked bytes in one granule pass when they were reached
    // from no live tracked object.
    builder.SetInsertPoint(check);
    llvm::Value *count = builder.CreateZExt(byte, i64);
    llvm::Value *partial =
        builder.CreateAnd(builder.CreateICmpULT(count, builder.getInt64(16)),
                          builder.CreateICmpULE(end, count));
    llvm::Value *next = builder.CreateZExt(
        builder.CreateLoad(i8, builder.CreateConstGEP1_64(i8, byteAt, 1)), i64);
    llvm::Value *crossing = builder.CreateAnd(
        builder.CreateAnd(builder.CreateICmpUGT(end, builder.getInt64(16)),
                          builder.CreateICmpEQ(count, builder.getInt64(16))),
        builder.CreateAnd(
            builder.CreateICmpULE(next, builder.getInt64(16)),
            builder.CreateICmpULE(builder.CreateSub(end, builder.getInt64(16)),
                                  next)));
    // The byte of the granule `from` points into: untracked bytes pass when
    // it is 0; a stack or global object's bytes when it is in the same
    // object (§2.1: the distance between the granules equals the difference
    // of their r).
    llvm::Value *from = call->getArgOperand(0);
    llvm::Value *fromAddress = builder.CreatePtrToInt(from, i64);
    llvm::Value *fromByte = byte;
    if (from->stripPointerCasts() !=
        call->getArgOperand(1)->stripPointerCasts())
      fromByte = builder.CreateLoad(
          i8,
          builder.CreateGEP(
              i8, builder.CreateIntToPtr(field(HeapShadow), builder.getPtrTy()),
              builder.CreateAnd(builder.CreateLShr(fromAddress, 4),
                                field(HeapMask))));
    llvm::Value *oneGranuleEnd =
        builder.CreateICmpULE(end, builder.getInt64(16));
    llvm::Value *untracked = builder.CreateAnd(
        builder.CreateAnd(builder.CreateICmpEQ(count, builder.getInt64(0)),
                          oneGranuleEnd),
        builder.CreateAnd(outsideArena(),
                          builder.CreateICmpEQ(fromByte, builder.getInt8(0))));
    llvm::Value *object =
        exactObject(count, builder.CreateZExt(fromByte, i64), fromAddress, end);
    builder.CreateCondBr(
        withOverflow(builder.CreateOr(builder.CreateOr(partial, crossing),
                                      builder.CreateOr(untracked, object))),
        after, slow);
    changed = true;
  }
  for (llvm::CallInst *call : calls)
    call->addFnAttr(llvm::Attribute::Cold);
  return changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

//===----------------------------------------------------------------------===//
// GlobalPadding
//===----------------------------------------------------------------------===//

// NOLINTBEGIN(readability-convert-member-functions-to-static): the pass
// manager calls `run` on an instance.
llvm::PreservedAnalyses
GlobalPadding::run(llvm::Module &module,
                   llvm::ModuleAnalysisManager & /*analyses*/) {
  // NOLINTEND(readability-convert-member-functions-to-static)
  // The globals the unit's descriptors register (`{&g, sizeof g}`).
  llvm::SetVector<llvm::GlobalVariable *> registered;
  for (llvm::GlobalVariable &descriptor : module.globals()) {
    const llvm::StringRef section = descriptor.getSection();
    const auto *pair =
        descriptor.hasInitializer()
            ? llvm::dyn_cast<llvm::ConstantArray>(descriptor.getInitializer())
            : nullptr;
    if ((section != "__DATA,__weavec_glob" && section != "weavec_globals") ||
        pair == nullptr || pair->getNumOperands() != 2)
      continue;
    auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
        pair->getOperand(0)->stripPointerCasts());
    if (global != nullptr && global->hasInitializer() &&
        !global->hasCommonLinkage() && !global->hasAvailableExternallyLinkage())
      registered.insert(global);
  }
  // Each is padded to whole granules, and by one more when it fills its
  // last: the linker may place anything right after it (a string literal in
  // the bytes its last granule leaves, or at its one-past address), and the
  // shadow would give those bytes to it.
  const llvm::DataLayout &layout = module.getDataLayout();
  llvm::LLVMContext &context = module.getContext();
  for (llvm::GlobalVariable *global : registered) {
    llvm::Type *type = global->getValueType();
    const std::uint64_t bytes = layout.getTypeAllocSize(type);
    const std::uint64_t padding = 16 - (bytes % 16);
    llvm::ArrayType *tail =
        llvm::ArrayType::get(llvm::Type::getInt8Ty(context), padding);
    llvm::StructType *paddedType =
        llvm::StructType::get(context, {type, tail}, /*isPacked=*/true);
    auto *padded = new llvm::GlobalVariable(
        module, paddedType, global->isConstant(), global->getLinkage(),
        llvm::ConstantStruct::get(
            paddedType,
            {global->getInitializer(), llvm::ConstantAggregateZero::get(tail)}),
        "", global, global->getThreadLocalMode(), global->getAddressSpace(),
        global->isExternallyInitialized());
    padded->copyAttributesFrom(global);
    padded->setAlignment(
        std::max(global->getAlign().valueOrOne(), llvm::Align(16)));
    padded->setComdat(global->getComdat());
    padded->copyMetadata(global, 0);
    padded->takeName(global);
    global->replaceAllUsesWith(padded);
    global->eraseFromParent();
  }
  return registered.empty() ? llvm::PreservedAnalyses::all()
                            : llvm::PreservedAnalyses::none();
}

void registerGuardPasses(clang::CodeGenOptions &options) {
  options.PassBuilderCallbacks.emplace_back([](llvm::PassBuilder &builder) {
    builder.registerPipelineStartEPCallback(
        [](llvm::ModulePassManager &passes, llvm::OptimizationLevel /*level*/) {
          passes.addPass(GlobalPadding());
          passes.addPass(
              llvm::createModuleToFunctionPassAdaptor(GuardCanonicalize()));
        });
    builder.registerOptimizerLastEPCallback(
        [](llvm::ModulePassManager &passes, llvm::OptimizationLevel level,
           llvm::ThinOrFullLTOPhase /*phase*/) {
          llvm::FunctionPassManager guards;
          if (level != llvm::OptimizationLevel::O0)
            guards.addPass(GuardMerge());
          guards.addPass(GuardExpand());
          passes.addPass(
              llvm::createModuleToFunctionPassAdaptor(std::move(guards)));
        });
  });
}

} // namespace weavec::frontend
