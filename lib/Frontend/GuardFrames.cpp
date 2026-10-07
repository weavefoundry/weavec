//===- GuardFrames.cpp - Stack objects and non-local exits ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §3. The tracked static allocas of a function become offsets into
// one frame alloca with redzones between them. The frame's shadow is written
// at entry (redzones poisoned, objects whose scope has not begun poisoned as
// out of scope) and cleared before each return; lifetime markers write the
// scope of their object. A tracked dynamic alloca gets a redzone after it,
// which the runtime clears at the stack restore or return that frees it.
// Before a call that does not return, the runtime clears the stack's shadow
// below the caller, so that a `longjmp` leaves no poison behind.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"

#include <algorithm>

namespace weavec::frontend::guard {

/// Shadow values of a frame (runtime/weavec_rt.h).
static constexpr std::uint8_t ShadowLeft = 0xF1;
static constexpr std::uint8_t ShadowMid = 0xF2;
static constexpr std::uint8_t ShadowRight = 0xF3;
static constexpr std::uint8_t ShadowScope = 0xF8;

/// The alignment of every object in a frame, and its left redzone: wider
/// than the redzones between objects, for the negative indexes that reach
/// below a frame's first array.
static constexpr std::uint64_t FrameAlign = 32;
static constexpr std::uint64_t LeftZone = 64;

namespace {

struct FrameObject {
  llvm::AllocaInst *alloca = nullptr;
  std::uint64_t size = 0;
  std::uint64_t offset = 0;
  /// The lifetime markers of the object.
  llvm::SmallVector<llvm::IntrinsicInst *, 2> starts;
  llvm::SmallVector<llvm::IntrinsicInst *, 2> ends;
};

} // namespace

/// The shadow bytes of `size` addressable bytes: whole granules 0, a
/// partial last granule its byte count.
static void objectBytes(std::uint64_t size,
                        llvm::SmallVectorImpl<std::uint8_t> &bytes) {
  for (std::uint64_t at = 0; at < size; at += 16)
    bytes.push_back(size - at >= 16 ? 0 : static_cast<std::uint8_t>(size - at));
}

/// Whether `function` can have its frame laid out: no `musttail` call (the
/// frame's shadow could not be cleared before it) and no escaped locals
/// for `localescape`.
static bool canLayOut(const llvm::Function &function) {
  for (const llvm::BasicBlock &block : function)
    for (const llvm::Instruction &instruction : block) {
      if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
        if (call->isMustTailCall())
          return false;
        if (const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call))
          if (intrinsic->getIntrinsicID() == llvm::Intrinsic::localescape)
            return false;
      }
    }
  return true;
}

/// The lifetime markers that name `alloca` directly.
static void markersOf(FrameObject &object) {
  for (llvm::User *user : object.alloca->users())
    if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user)) {
      if (intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start)
        object.starts.push_back(intrinsic);
      else if (intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end)
        object.ends.push_back(intrinsic);
    }
}

/// The function's exits through which its frame dies.
static llvm::SmallVector<llvm::Instruction *, 4>
exitsOf(llvm::Function &function) {
  llvm::SmallVector<llvm::Instruction *, 4> exits;
  for (llvm::BasicBlock &block : function) {
    llvm::Instruction *terminator = block.getTerminator();
    if (llvm::isa<llvm::ReturnInst, llvm::ResumeInst>(terminator))
      exits.push_back(terminator);
  }
  return exits;
}

/// Lays out the tracked static allocas (section 3.2, 3.3).
static void layoutStatic(FunctionContext &context,
                         llvm::ArrayRef<llvm::AllocaInst *> allocas,
                         llvm::DenseMap<llvm::Value *, llvm::Value *> &moved) {
  ModuleContext &module = context.module;
  llvm::Function &function = context.function;
  std::vector<FrameObject> objects;
  std::uint64_t alignment = FrameAlign;
  for (llvm::AllocaInst *alloca : allocas) {
    const std::optional<llvm::TypeSize> size =
        alloca->getAllocationSize(module.layout);
    if (!size || size->isScalable() || alloca->isSwiftError() ||
        alloca->isUsedWithInAlloca())
      continue;
    FrameObject object;
    object.alloca = alloca;
    object.size = std::max<std::uint64_t>(size->getFixedValue(), 1);
    markersOf(object);
    alignment = std::max<std::uint64_t>(alignment, alloca->getAlign().value());
    objects.push_back(std::move(object));
  }
  if (objects.empty())
    return;
  // Larger alignments first, so that padding stays small; then in source
  // order.
  std::ranges::stable_sort(objects,
                           [](const FrameObject &a, const FrameObject &b) {
                             return a.alloca->getAlign() > b.alloca->getAlign();
                           });
  std::uint64_t offset = LeftZone;
  for (FrameObject &object : objects) {
    const std::uint64_t align =
        std::max<std::uint64_t>(FrameAlign, object.alloca->getAlign().value());
    offset = (offset + align - 1) & ~(align - 1);
    object.offset = offset;
    offset += paddedSize(object.size);
  }
  const std::uint64_t frameSize = offset;

  // The shadow at entry: redzones, objects in scope or not yet.
  llvm::SmallVector<std::uint8_t, 64> entry(frameSize / 16, ShadowMid);
  for (std::uint64_t g = 0; g < LeftZone / 16; ++g)
    entry[g] = ShadowLeft;
  for (const FrameObject &object : objects) {
    llvm::SmallVector<std::uint8_t, 16> bytes;
    objectBytes(object.size, bytes);
    const bool scoped = !object.starts.empty();
    for (std::size_t i = 0; i < bytes.size(); ++i)
      entry[object.offset / 16 + i] = scoped ? ShadowScope : bytes[i];
  }
  {
    const FrameObject &last = *std::ranges::max_element(
        objects, {}, [](const FrameObject &object) { return object.offset; });
    for (std::uint64_t g = (last.offset + last.size + 15) / 16;
         g < frameSize / 16; ++g)
      entry[g] = ShadowRight;
  }

  // The frame, at the top of the entry block.
  llvm::BasicBlock &first = function.getEntryBlock();
  llvm::IRBuilder<> builder(&first, first.getFirstInsertionPt());
  auto *frame = builder.CreateAlloca(llvm::ArrayType::get(module.i8, frameSize),
                                     nullptr, "weavec.frame");
  frame->setAlignment(llvm::Align(alignment));
  // Every object's address first: writing a scope's shadow splits blocks.
  std::vector<llvm::Value *> addresses;
  for (FrameObject &object : objects) {
    llvm::Value *at = builder.CreateConstInBoundsGEP1_64(
        module.i8, frame, object.offset, object.alloca->getName() + ".weavec");
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(at))
      instruction->setDebugLoc(object.alloca->getDebugLoc());
    addresses.push_back(at);
  }
  for (std::size_t i = 0; i < objects.size(); ++i) {
    FrameObject &object = objects[i];
    llvm::Value *at = addresses[i];
    // Scopes: the markers write the object's shadow, then go (they name an
    // alloca, which the object no longer is).
    llvm::SmallVector<std::uint8_t, 16> live;
    objectBytes(object.size, live);
    const llvm::SmallVector<std::uint8_t, 16> dead(live.size(), ShadowScope);
    for (llvm::IntrinsicInst *start : object.starts) {
      context.writeShadow(start, at, live);
      start->eraseFromParent();
    }
    for (llvm::IntrinsicInst *end : object.ends) {
      context.writeShadow(end, at, dead);
      end->eraseFromParent();
    }
    object.alloca->replaceAllUsesWith(at);
    at->takeName(object.alloca);
    moved[object.alloca] = at;
    object.alloca->eraseFromParent();
  }
  // The entry's shadow first; the frame's cleared at each exit.
  const llvm::SmallVector<std::uint8_t, 64> clear(frameSize / 16, 0);
  for (llvm::Instruction *exit : exitsOf(function))
    context.writeShadow(exit, frame, clear);
  context.writeShadow(context.entry(), frame, entry);
}

/// Gives each tracked dynamic alloca a redzone (section 3.4).
static void layoutDynamic(FunctionContext &context,
                          llvm::ArrayRef<llvm::AllocaInst *> allocas,
                          llvm::DenseMap<llvm::Value *, llvm::Value *> &moved) {
  ModuleContext &module = context.module;
  llvm::Function &function = context.function;
  if (allocas.empty())
    return;
  llvm::FunctionCallee poison = module.runtime(
      "alloca_poison",
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.context),
                              {module.i64, module.i64}, false));
  llvm::FunctionCallee unpoison = module.runtime(
      "alloca_unpoison",
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.context),
                              {module.i64, module.i64}, false));
  for (llvm::AllocaInst *alloca : allocas) {
    if (alloca->isSwiftError() || alloca->isUsedWithInAlloca())
      continue;
    llvm::IRBuilder<> builder(alloca);
    const llvm::TypeSize element =
        module.layout.getTypeAllocSize(alloca->getAllocatedType());
    if (element.isScalable())
      continue;
    llvm::Value *count =
        builder.CreateZExtOrTrunc(alloca->getArraySize(), module.i64);
    llvm::Value *bytes = builder.CreateMul(
        count, llvm::ConstantInt::get(module.i64, element.getFixedValue()));
    // Rounded to 32 and 32 more: the redzone.
    llvm::Value *padded = builder.CreateAdd(
        builder.CreateAnd(
            builder.CreateAdd(
                bytes, llvm::ConstantInt::get(module.i64, FrameAlign - 1)),
            llvm::ConstantInt::get(module.i64, ~(FrameAlign - 1))),
        llvm::ConstantInt::get(module.i64, FrameAlign));
    auto *replacement =
        builder.CreateAlloca(module.i8, padded, alloca->getName() + ".weavec");
    replacement->setAlignment(llvm::Align(
        std::max<std::uint64_t>(FrameAlign, alloca->getAlign().value())));
    replacement->setDebugLoc(alloca->getDebugLoc());
    builder.SetInsertPoint(alloca);
    builder.CreateCall(
        poison, {builder.CreatePtrToInt(replacement, module.i64), bytes});
    alloca->replaceAllUsesWith(replacement);
    replacement->takeName(alloca);
    moved[alloca] = replacement;
    alloca->eraseFromParent();
  }
  // The stack below the entry's stack pointer, cleared at each restore and
  // each exit.
  llvm::IRBuilder<> builder(context.entry());
  llvm::Value *top = builder.CreatePtrToInt(
      builder.CreateStackSave("weavec.stack.top"), module.i64);
  auto clearTo = [&](llvm::Instruction *before, llvm::Value *high) {
    llvm::IRBuilder<> at(before);
    llvm::Value *low = at.CreatePtrToInt(at.CreateStackSave(), module.i64);
    at.CreateCall(unpoison, {low, high});
  };
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : llvm::make_early_inc_range(block))
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
        if (intrinsic->getIntrinsicID() == llvm::Intrinsic::stackrestore) {
          llvm::IRBuilder<> at(intrinsic);
          clearTo(intrinsic,
                  at.CreatePtrToInt(intrinsic->getArgOperand(0), module.i64));
        }
  for (llvm::Instruction *exit : exitsOf(function))
    clearTo(exit, top);
}

llvm::DenseMap<llvm::Value *, llvm::Value *>
layoutFrame(FunctionContext &context,
            llvm::ArrayRef<llvm::AllocaInst *> tracked) {
  llvm::DenseMap<llvm::Value *, llvm::Value *> moved;
  if (tracked.empty() || !canLayOut(context.function))
    return moved;
  llvm::BasicBlock &first = context.function.getEntryBlock();
  llvm::SmallVector<llvm::AllocaInst *, 8> fixed;
  llvm::SmallVector<llvm::AllocaInst *, 4> dynamic;
  for (llvm::AllocaInst *alloca : tracked) {
    if (alloca->isStaticAlloca() && alloca->getParent() == &first)
      fixed.push_back(alloca);
    else
      dynamic.push_back(alloca);
  }
  layoutStatic(context, fixed, moved);
  layoutDynamic(context, dynamic, moved);
  return moved;
}

/// Functions that end the process, after which no stack is used again.
static bool endsProcess(llvm::StringRef name) {
  return llvm::is_contained({"abort", "exit", "_exit", "_Exit", "quick_exit",
                             "__assert_fail", "__assert_rtn", "__assert",
                             "__stack_chk_fail", "__chk_fail", "err", "errx",
                             "verr", "verrx", "__fortify_fail"},
                            name);
}

void unpoisonBeforeNoReturn(FunctionContext &context) {
  ModuleContext &module = context.module;
  llvm::SmallVector<llvm::CallBase *, 4> calls;
  for (llvm::BasicBlock &block : context.function)
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr || !call->doesNotReturn() ||
          llvm::isa<llvm::IntrinsicInst>(call))
        continue;
      if (const llvm::Function *callee = call->getCalledFunction())
        if (ModuleContext::isRuntimeName(callee->getName()) ||
            endsProcess(callee->getName()))
          continue;
      calls.push_back(call);
    }
  if (calls.empty())
    return;
  llvm::FunctionCallee unpoison = module.runtime(
      "unpoison_stack",
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false));
  for (llvm::CallBase *call : calls)
    llvm::IRBuilder<>(call).CreateCall(unpoison);
}

} // namespace weavec::frontend::guard
