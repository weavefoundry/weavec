//===- GuardFrames.cpp - Stack objects and non-local exits ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §3. The tracked static allocas of a function become offsets into
// one frame alloca with redzones between them; two whose lifetimes never
// overlap may share bytes (RFC 0036 §2), as Clang's stack coloring lets
// them share stack space. The frame's shadow is written
// at entry (redzones poisoned, objects whose scope has not begun poisoned as
// out of scope) and written as returned before each return (RFC 0037 §9,
// the runtime clears what is stale); lifetime markers write the
// scope of their object. A tracked dynamic alloca gets a redzone after it,
// which the runtime clears at the stack restore or return that frees it.
// Before a call that does not return, the runtime clears the stack's shadow
// below the caller, so that a `longjmp` leaves no poison behind; RFC 0039
// §3: not before one that ends the process or unwinds (the library table
// says which), whose unwound frames the runtime marks returned when a
// landing pad calls it.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"
#include "weavec/Core/LibrarySpec.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/StackLifetime.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>

namespace weavec::frontend::guard {

/// Shadow values of a frame (runtime/weavec_rt.h).
static constexpr std::uint8_t ShadowLeft = 0xF1;
static constexpr std::uint8_t ShadowMid = 0xF2;
static constexpr std::uint8_t ShadowRight = 0xF3;
static constexpr std::uint8_t ShadowScope = 0xF8;
/// RFC 0037 §9: a frame that returned.
static constexpr std::uint8_t ShadowReturned = 0xF5;

/// The alignment of every object in a frame, and its left redzone: wider
/// than the redzones between objects, for the negative indexes that reach
/// below a frame's first array.
static constexpr std::uint64_t FrameAlign = 32;
static constexpr std::uint64_t LeftZone = 64;
/// RFC 0036 amendment 8: locals share bytes only in a frame that would be
/// larger than this without sharing. Below it the redzones cost little, and
/// keeping every local apart keeps a use after scope from landing in a
/// local whose scope began later.
static constexpr std::uint64_t ShareAbove = 512;

namespace {

struct FrameObject {
  llvm::AllocaInst *alloca = nullptr;
  std::uint64_t size = 0;
  std::uint64_t offset = 0;
  /// RFC 0037 §7.2: the object's color.
  std::uint8_t color = 0;
  /// The lifetime markers of the object.
  llvm::SmallVector<llvm::IntrinsicInst *, 2> starts;
  llvm::SmallVector<llvm::IntrinsicInst *, 2> ends;
};

} // namespace

/// The shadow byte of a partial granule of `bytes` (1 to 15) addressable
/// bytes of an object of `color` (RFC 0037 §7.1: `1ccc kkkk`).
static std::uint8_t partialOf(std::uint8_t color, std::uint64_t bytes) {
  return static_cast<std::uint8_t>(
      0x80U | ((static_cast<unsigned>(color) & 7U) << 4U) | bytes);
}

/// The shadow of `size` addressable bytes of an object of `color`: whole
/// granules the color, a partial last granule its byte count.
static void objectRuns(std::uint64_t size, std::uint8_t color,
                       ShadowRuns &runs) {
  appendRun(runs, size / 16, color);
  if (size % 16 != 0)
    appendRun(runs, 1, partialOf(color, size % 16));
}

/// The granules `size` bytes cover.
static std::uint64_t granules(std::uint64_t size) {
  return (size + 15) / 16;
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
        if (const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call);
            intrinsic &&
            (intrinsic->getIntrinsicID() == llvm::Intrinsic::localescape))
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
  const ModuleContext &module = context.module;
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

  // RFC 0036 §2: two objects may share bytes when their lifetimes never
  // overlap. An object without lifetime markers is live throughout.
  // (Amendment 8: only in a frame that would be large without sharing.)
  llvm::SmallVector<const llvm::AllocaInst *, 8> lifetimeAllocas;
  for (const FrameObject &object : objects)
    lifetimeAllocas.push_back(object.alloca);
  llvm::StackLifetime lifetimes(function, lifetimeAllocas,
                                llvm::StackLifetime::LivenessType::May);
  bool scoped = false;
  auto together = [&](const FrameObject &a, const FrameObject &b) {
    return !scoped || a.starts.empty() || b.starts.empty() ||
           lifetimes.getLiveRange(a.alloca).overlaps(
               lifetimes.getLiveRange(b.alloca));
  };
  auto extentEnd = [](const FrameObject &object) {
    return object.offset + paddedSize(object.size);
  };
  // Each object at the lowest aligned offset where its extent (the object
  // and its redzone) meets no extent of an object live with it; the frame's
  // size.
  auto place = [&] {
    std::uint64_t size = LeftZone;
    for (std::size_t i = 0; i < objects.size(); ++i) {
      FrameObject &object = objects[i];
      const std::uint64_t align = std::max<std::uint64_t>(
          FrameAlign, object.alloca->getAlign().value());
      const std::uint64_t extent = paddedSize(object.size);
      std::uint64_t offset = (LeftZone + align - 1) & ~(align - 1);
      for (bool clash = true; clash;) {
        clash = false;
        for (std::size_t j = 0; j < i; ++j)
          if (offset < extentEnd(objects[j]) &&
              objects[j].offset < offset + extent &&
              together(objects[j], object)) {
            offset = (extentEnd(objects[j]) + align - 1) & ~(align - 1);
            clash = true;
          }
      }
      object.offset = offset;
      size = std::max(size, offset + extent);
    }
    return size;
  };
  std::uint64_t frameSize = place();
  // RFC 0037 §7.2: the locals of a frame take consecutive colors, from a
  // hash of the function's name, so that any two of them differ.
  {
    std::uint64_t next = llvm::xxh3_64bits(function.getName());
    for (FrameObject &object : objects) {
      object.color = colorAt(next++);
      context.frameColors[object.alloca] = object.color;
    }
  }
  if (frameSize > ShareAbove &&
      llvm::any_of(objects, [](const FrameObject &object) {
        return !object.starts.empty();
      })) {
    scoped = true;
    lifetimes.run();
    frameSize = place();
  }

  // The shadow at entry: redzones, objects in scope or not yet. An object
  // live at entry shares bytes with none. Each stretch between two of the
  // objects' boundaries takes one value, decided by the objects that cover
  // it in layout order.
  std::uint64_t objectsEnd = 0;
  for (const FrameObject &object : objects)
    objectsEnd = std::max(objectsEnd, object.offset + object.size);
  llvm::SmallVector<std::uint64_t, 32> cuts = {
      0, LeftZone / 16, granules(objectsEnd), frameSize / 16};
  for (const FrameObject &object : objects) {
    cuts.push_back(object.offset / 16);
    cuts.push_back((object.offset / 16) + (object.size / 16));
    cuts.push_back((object.offset / 16) + granules(object.size));
  }
  llvm::sort(cuts);
  cuts.erase(std::ranges::unique(cuts).begin(), cuts.end());
  ShadowRuns entry;
  for (std::size_t c = 0; c + 1 < cuts.size() && cuts[c] < frameSize / 16;
       ++c) {
    const std::uint64_t g = cuts[c];
    std::uint8_t value = g < LeftZone / 16 ? ShadowLeft : ShadowMid;
    if (g >= granules(objectsEnd)) {
      value = ShadowRight;
    } else {
      for (const FrameObject &object : objects) {
        const std::uint64_t first = object.offset / 16;
        if (g < first || g >= first + granules(object.size))
          continue;
        const std::uint8_t own =
            g - first < object.size / 16
                ? object.color
                : partialOf(object.color, object.size % 16);
        const bool inScope = object.starts.empty();
        if (inScope || value == ShadowMid)
          value = inScope ? own : ShadowScope;
      }
    }
    appendRun(entry, std::min(cuts[c + 1], frameSize / 16) - g, value);
  }

  // The frame, at the top of the entry block.
  llvm::BasicBlock &first = function.getEntryBlock();
  llvm::IRBuilder<> builder(&first, first.getFirstInsertionPt());
  auto *frame = builder.CreateAlloca(llvm::ArrayType::get(module.i8, frameSize),
                                     nullptr, "weavec.frame");
  frame->setAlignment(llvm::Align(alignment));
  // Every object's address first: writing a scope's shadow splits blocks.
  std::vector<llvm::Value *> addresses;
  for (const FrameObject &object : objects) {
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
    ShadowRuns live;
    objectRuns(object.size, object.color, live);
    ShadowRuns dead;
    appendRun(dead, granules(object.size), ShadowScope);
    // A redzone that shares bytes with another object's says so again at
    // the start, so that an overflow into it is reported as one.
    const bool sharesRedzone =
        llvm::any_of(objects, [&](const FrameObject &other) {
          return &other != &object && other.offset < extentEnd(object) &&
                 object.offset + object.size < other.offset + other.size;
        });
    if (sharesRedzone)
      appendRun(live, (paddedSize(object.size) / 16) - granules(object.size),
                ShadowMid);
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
  // The entry's shadow first; at each exit the frame says it returned
  // (RFC 0037 §9).
  ShadowRuns clear;
  appendRun(clear, frameSize / 16, ShadowReturned);
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
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
          intrinsic &&
          (intrinsic->getIntrinsicID() == llvm::Intrinsic::stackrestore)) {
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

void unpoisonBeforeNoReturn(FunctionContext &context) {
  ModuleContext &module = context.module;
  const core::LibrarySpec &spec = core::LibrarySpec::shipped();
  llvm::SmallVector<llvm::CallBase *, 4> calls;
  llvm::SmallVector<llvm::LandingPadInst *, 4> pads;
  for (llvm::BasicBlock &block : context.function)
    for (llvm::Instruction &instruction : block) {
      if (auto *pad = llvm::dyn_cast<llvm::LandingPadInst>(&instruction))
        pads.push_back(pad);
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr || !call->doesNotReturn() ||
          llvm::isa<llvm::IntrinsicInst>(call))
        continue;
      // RFC 0039 §3: after the end of the process no stack is used again,
      // and the frames an exception unwinds are marked at its landing pad.
      if (const llvm::Function *callee = call->getCalledFunction()) {
        if (ModuleContext::isRuntimeName(callee->getName()))
          continue;
        const std::optional<core::LibraryMatch> match =
            spec.lookup(libraryName(callee->getName()), call->arg_size());
        if (match && match->entry != nullptr &&
            (match->entry->exits || match->entry->unwinds))
          continue;
      }
      calls.push_back(call);
    }
  if (!calls.empty()) {
    llvm::FunctionCallee unpoison = module.runtime(
        "unpoison_stack",
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false));
    for (llvm::CallBase *call : calls)
      llvm::IRBuilder<>(call).CreateCall(unpoison);
  }
  if (pads.empty())
    return;
  llvm::FunctionCallee landed = module.runtime(
      "landed",
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false));
  for (llvm::LandingPadInst *pad : pads)
    llvm::IRBuilder<>(pad->getNextNode()).CreateCall(landed);
}

} // namespace weavec::frontend::guard
