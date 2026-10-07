//===- GuardPass.cpp - Every memory access guarded ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035: the three passes' entry points and registration, and what they
// share: the module's context (types, markers, runtime entry points, site
// records, the ledger) and the inline checks of the shadow.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "clang/Basic/CodeGenOptions.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>

namespace weavec::frontend {

namespace guard {

std::uint64_t paddedSize(std::uint64_t size) {
  const std::uint64_t zone = std::clamp<std::uint64_t>(size / 8, 32, 4096);
  return (size + zone + 31) & ~std::uint64_t{31};
}

std::optional<std::uint64_t> exactSize(const llvm::Value *object,
                                       const llvm::DataLayout &layout) {
  if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(object)) {
    if (!alloca->isStaticAlloca())
      return std::nullopt;
    const std::optional<llvm::TypeSize> size =
        alloca->getAllocationSize(layout);
    if (!size || size->isScalable())
      return std::nullopt;
    return size->getFixedValue();
  }
  if (const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(object)) {
    if (global->isDeclaration() || !global->hasExactDefinition() ||
        global->isInterposable() || global->hasCommonLinkage())
      return std::nullopt;
    const llvm::TypeSize size = layout.getTypeAllocSize(global->getValueType());
    if (size.isScalable())
      return std::nullopt;
    return size.getFixedValue();
  }
  return std::nullopt;
}

bool insideAt(const llvm::Value *object, std::int64_t offset,
              std::uint64_t width, const llvm::DataLayout &layout) {
  const std::optional<std::uint64_t> size = exactSize(object, layout);
  return size && offset >= 0 && static_cast<std::uint64_t>(offset) <= *size &&
         width <= *size - static_cast<std::uint64_t>(offset);
}

/// The lifetime marker `instruction` is of `alloca`: +1 a start, -1 an end,
/// 0 none.
static int markerFor(const llvm::Instruction &instruction,
                     const llvm::AllocaInst &alloca) {
  const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
  if (intrinsic == nullptr || !intrinsic->isLifetimeStartOrEnd() ||
      intrinsic->getArgOperand(intrinsic->arg_size() - 1) != &alloca)
    return 0;
  return intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start ? 1
                                                                        : -1;
}

bool Scopes::mayBeOutOfScope(const llvm::AllocaInst &alloca,
                             const llvm::Instruction &at) {
  auto found = deadAtEntry.find(&alloca);
  if (found == deadAtEntry.end()) {
    // Forward: a block is entered out of scope when a predecessor is left
    // so, that is after an end with no start after it.
    llvm::DenseSet<const llvm::BasicBlock *> dead;
    llvm::SmallVector<const llvm::BasicBlock *, 8> work;
    auto leavesDead = [&](const llvm::BasicBlock &block, bool enteredDead) {
      bool state = enteredDead;
      for (const llvm::Instruction &instruction : block)
        if (const int marker = markerFor(instruction, alloca))
          state = marker < 0;
      return state;
    };
    for (const llvm::User *user : alloca.users())
      if (const auto *instruction = llvm::dyn_cast<llvm::Instruction>(user))
        if (markerFor(*instruction, alloca) < 0)
          work.push_back(instruction->getParent());
    llvm::DenseSet<const llvm::BasicBlock *> leftDead;
    while (!work.empty()) {
      const llvm::BasicBlock *block = work.pop_back_val();
      if (!leavesDead(*block, dead.contains(block)) ||
          !leftDead.insert(block).second)
        continue;
      for (const llvm::BasicBlock *successor : llvm::successors(block))
        if (dead.insert(successor).second)
          work.push_back(successor);
    }
    found = deadAtEntry.try_emplace(&alloca, std::move(dead)).first;
  }
  bool state = found->second.contains(at.getParent());
  for (const llvm::Instruction &instruction : *at.getParent()) {
    if (&instruction == &at)
      return state;
    if (const int marker = markerFor(instruction, alloca))
      state = marker < 0;
  }
  return state;
}

bool skips(const llvm::Function &function) {
  // An available_externally body (a C99 inline definition) is guarded too:
  // the inliner copies it into this unit.
  return function.isDeclaration() ||
         function.hasFnAttribute(llvm::Attribute::Naked) ||
         ModuleContext::isRuntimeName(function.getName());
}

//===----------------------------------------------------------------------===//
// ModuleContext
//===----------------------------------------------------------------------===//

ModuleContext::ModuleContext(llvm::Module &module, const GuardOptions &options)
    : module(module), layout(module.getDataLayout()),
      context(module.getContext()), options(options),
      i8(llvm::Type::getInt8Ty(context)), i32(llvm::Type::getInt32Ty(context)),
      i64(llvm::Type::getInt64Ty(context)),
      ptr(llvm::PointerType::getUnqual(context)) {}

/// The attributes that keep a marker: no unwinding, no memory but what the
/// runtime owns (and the string a string marker scans), not `willreturn`.
static void keep(llvm::FunctionCallee callee, llvm::MemoryEffects effects) {
  auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee());
  if (function == nullptr)
    return;
  function->setDoesNotThrow();
  function->setMemoryEffects(effects);
  // Two guards of different accesses stay two, each with its location.
  function->addFnAttr(llvm::Attribute::NoMerge);
  for (llvm::Argument &argument : function->args())
    if (argument.getType()->isPointerTy())
      argument.addAttr(llvm::Attribute::getWithCaptureInfo(
          function->getContext(), llvm::CaptureInfo::none()));
}

llvm::FunctionCallee ModuleContext::markerGuard() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerGuard, llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                           {ptr, i64, i32, ptr, i32}, false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref));
  return callee;
}

llvm::FunctionCallee ModuleContext::markerRange() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerRange, llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                           {ptr, i64, i32, i32}, false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref));
  return callee;
}

llvm::FunctionCallee ModuleContext::markerString() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerString, llvm::FunctionType::get(i64, {ptr, i64, i32, i32}, false));
  keep(callee,
       llvm::MemoryEffects::inaccessibleOrArgMemOnly(llvm::ModRefInfo::Ref));
  return callee;
}

llvm::FunctionCallee ModuleContext::markerDisjoint() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerDisjoint, llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                              {ptr, ptr, i64}, false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref));
  return callee;
}

llvm::FunctionCallee ModuleContext::markerScope() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerScope,
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Mod));
  return callee;
}

bool ModuleContext::isScope(const llvm::CallBase &call) {
  const llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->getName() == MarkerScope;
}

std::optional<llvm::StringRef>
ModuleContext::markerOf(const llvm::CallBase &call) {
  const llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr)
    return std::nullopt;
  const llvm::StringRef name = callee->getName();
  if (name == MarkerGuard || name == MarkerRange || name == MarkerString ||
      name == MarkerDisjoint)
    return name;
  return std::nullopt;
}

void ModuleContext::insertDisjoint(llvm::Instruction *before,
                                   llvm::Value *destination,
                                   llvm::Value *source, llvm::Value *length) {
  llvm::IRBuilder<> builder(before);
  llvm::CallInst *call = builder.CreateCall(
      markerDisjoint(),
      {destination, source, builder.CreateZExtOrTrunc(length, i64)});
  call->setDebugLoc(before->getDebugLoc());
}

llvm::CallInst *
ModuleContext::insertGuard(llvm::Instruction *before, llvm::Value *pointer,
                           std::uint64_t width, llvm::Align alignment,
                           unsigned flags, llvm::Value *nullBase,
                           unsigned row) {
  llvm::IRBuilder<> builder(before);
  flags |= llvm::Log2(alignment) << MarkerAlignShift;
  llvm::CallInst *call = builder.CreateCall(
      markerGuard(),
      {pointer, llvm::ConstantInt::get(i64, width),
       llvm::ConstantInt::get(i32, flags),
       nullBase != nullptr ? nullBase : llvm::ConstantPointerNull::get(ptr),
       llvm::ConstantInt::get(i32, row)});
  call->setDebugLoc(before->getDebugLoc());
  return call;
}

llvm::CallInst *ModuleContext::insertRange(llvm::Instruction *before,
                                           llvm::Value *pointer,
                                           llvm::Value *length, unsigned flags,
                                           unsigned row) {
  llvm::IRBuilder<> builder(before);
  llvm::CallInst *call = builder.CreateCall(
      markerRange(),
      {pointer, builder.CreateZExtOrTrunc(length, i64),
       llvm::ConstantInt::get(i32, flags), llvm::ConstantInt::get(i32, row)});
  call->setDebugLoc(before->getDebugLoc());
  return call;
}

llvm::Value *ModuleContext::insertString(llvm::IRBuilder<> &builder,
                                         llvm::Value *pointer, llvm::Value *max,
                                         unsigned row, unsigned flags) {
  return builder.CreateCall(markerString(),
                            {pointer, max, llvm::ConstantInt::get(i32, flags),
                             llvm::ConstantInt::get(i32, row)});
}

llvm::FunctionCallee ModuleContext::runtime(llvm::StringRef name,
                                            llvm::FunctionType *type) {
  const std::string full = ("__weavec_rt_" + name).str();
  llvm::FunctionCallee callee = module.getOrInsertFunction(full, type);
  if (auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
    function->setDoesNotThrow();
  return callee;
}

/// A slow path: cold, and on AArch64 and x86-64 called with preserve_all
/// (the runtime declares it so), so that the inline guard costs the code
/// around it no spills and the register allocator no splits.
static void slowPath(llvm::FunctionCallee callee, const llvm::Module &module) {
  auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee());
  if (function == nullptr)
    return;
  function->addFnAttr(llvm::Attribute::Cold);
  const llvm::Triple triple(module.getTargetTriple());
  if (triple.isAArch64() || triple.getArch() == llvm::Triple::x86_64)
    function->setCallingConv(llvm::CallingConv::PreserveAll);
}

llvm::CallInst *ModuleContext::callSlow(llvm::IRBuilder<> &builder,
                                        llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value *> args) {
  llvm::CallInst *call = builder.CreateCall(callee, args);
  if (auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
    call->setCallingConv(function->getCallingConv());
  return call;
}

llvm::FunctionCallee ModuleContext::guardFunction() {
  llvm::FunctionCallee callee =
      runtime("guard", llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                               {i64, i64, ptr}, false));
  slowPath(callee, module);
  return callee;
}

llvm::FunctionCallee ModuleContext::nullFunction() {
  llvm::FunctionCallee callee =
      runtime("null", llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                              {i64, ptr}, false));
  slowPath(callee, module);
  return callee;
}

llvm::FunctionCallee ModuleContext::rangeFunction() {
  return runtime("range",
                 llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                         {i64, i64, ptr}, false));
}

llvm::FunctionCallee ModuleContext::rangeOkFunction() {
  return runtime("range_ok", llvm::FunctionType::get(i32, {i64, i64}, false));
}

llvm::FunctionCallee ModuleContext::strlenFunction() {
  return runtime("strlen",
                 llvm::FunctionType::get(i64, {ptr, i64, ptr}, false));
}

llvm::GlobalVariable *ModuleContext::shadowDescriptor() {
  if (descriptor != nullptr)
    return descriptor;
  auto *type = llvm::StructType::get(context, {i64, i64, i32});
  descriptor = llvm::dyn_cast<llvm::GlobalVariable>(
      module.getOrInsertGlobal("__weavec_rt_shadow", type));
  return descriptor;
}

ModuleContext::Position
ModuleContext::positionOf(const llvm::DebugLoc &location) {
  Position position;
  if (!location)
    return position;
  const llvm::DILocation *at = location.get();
  position.file =
      UnsafeRegions::normalise(at->getFilename(), at->getDirectory());
  position.line = at->getLine();
  position.column = at->getColumn();
  return position;
}

bool ModuleContext::isUnsafe(const llvm::DebugLoc &location) const {
  if (!options.unsafe || options.unsafe->empty() || !location)
    return false;
  const Position position = positionOf(location);
  return options.unsafe->contains(position.file, position.line,
                                  position.column);
}

llvm::Constant *ModuleContext::site(const llvm::DebugLoc &location,
                                    unsigned flags) {
  if (options.mode == GuardOptions::Mode::Report)
    flags |= SiteReport;
  llvm::Constant *file = llvm::ConstantPointerNull::get(ptr);
  unsigned line = 0;
  unsigned column = 0;
  if (location) {
    const llvm::DILocation *at = location.get();
    line = at->getLine();
    column = at->getColumn();
    llvm::StringRef name = at->getFilename();
    llvm::Constant *&text = files[name];
    if (text == nullptr) {
      auto *array = llvm::ConstantDataArray::getString(context, name);
      auto *global = new llvm::GlobalVariable(
          module, array->getType(), /*isConstant=*/true,
          llvm::GlobalValue::PrivateLinkage, array, "__weavec.file");
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      global->setAlignment(llvm::Align(1));
      text = global;
    }
    file = text;
  }
  llvm::Constant *&record = sites[{file, line, column, flags}];
  if (record != nullptr)
    return record;
  if (siteType == nullptr)
    siteType = llvm::StructType::get(context, {ptr, i32, i32, i32});
  auto *initializer = llvm::ConstantStruct::get(
      siteType, {file, llvm::ConstantInt::get(i32, line),
                 llvm::ConstantInt::get(i32, column),
                 llvm::ConstantInt::get(i32, flags)});
  auto *global = new llvm::GlobalVariable(module, siteType, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          initializer, "__weavec.site");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  record = global;
  return record;
}

unsigned ModuleContext::row(const llvm::Function &function,
                            const llvm::DebugLoc &location,
                            llvm::StringRef operation, std::uint64_t bytes,
                            LedgerRow::Outcome outcome,
                            llvm::StringRef reason) {
  EnforcementLedger *ledger = options.ledger.get();
  if (ledger == nullptr)
    return 0;
  const Position position = positionOf(location);
  LedgerRow row;
  row.function = function.getName().str();
  row.file = position.file;
  row.line = position.line;
  row.column = position.column;
  row.operation = operation.str();
  row.bytes = bytes;
  row.outcome = outcome;
  row.reason = reason.str();
  return ledger->add(std::move(row));
}

void ModuleContext::copy(unsigned row, bool stays, llvm::StringRef reason) {
  if (EnforcementLedger *ledger = options.ledger.get())
    ledger->copy(row, stays, reason);
}

bool ModuleContext::isRuntimeName(llvm::StringRef name) {
  return name.starts_with("__weavec_") || name.starts_with("__weavec.") ||
         name.starts_with("__ubsan_handle_");
}

//===----------------------------------------------------------------------===//
// FunctionContext
//===----------------------------------------------------------------------===//

FunctionContext::FunctionContext(ModuleContext &module,
                                 llvm::Function &function)
    : module(module), function(function) {
  // The descriptor's loads come first, after the allocas; code that must run
  // first at entry goes right after them, which no pass removes.
  llvm::BasicBlock &entry = function.getEntryBlock();
  auto at = entry.getFirstInsertionPt();
  while (at != entry.end() && llvm::isa<llvm::AllocaInst>(*at))
    ++at;
  llvm::IRBuilder<> builder(&entry, at);
  llvm::GlobalVariable *descriptor = module.shadowDescriptor();
  llvm::Type *type = descriptor->getValueType();
  auto *base = builder.CreateLoad(module.ptr,
                                  builder.CreateStructGEP(type, descriptor, 0),
                                  "weavec.shadow.base");
  auto *mask = builder.CreateLoad(module.i64,
                                  builder.CreateStructGEP(type, descriptor, 1),
                                  "weavec.shadow.mask");
  loaded = {base, mask};
  anchor = mask;
}

FunctionContext::~FunctionContext() {
  // Unused when the function guards nothing.
  for (llvm::Value *value : {loaded.mask, loaded.base})
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
        load != nullptr && load->use_empty())
      load->eraseFromParent();
}

llvm::Instruction *FunctionContext::entry() const {
  return anchor->getNextNode();
}

ShadowValues FunctionContext::shadow() {
  return loaded;
}

llvm::Value *FunctionContext::shadowAddress(llvm::IRBuilder<> &builder,
                                            llvm::Value *address) {
  const ShadowValues values = shadow();
  llvm::Value *index =
      builder.CreateAnd(builder.CreateLShr(address, 4), values.mask);
  return builder.CreateGEP(module.i8, values.base, index);
}

/// Weights that make a branch's true edge the cold one.
static llvm::MDNode *unlikely(llvm::LLVMContext &context) {
  return llvm::MDBuilder(context).createUnlikelyBranchWeights();
}

void FunctionContext::slowGuard(llvm::IRBuilder<> &builder,
                                llvm::Value *address, llvm::Value *width,
                                llvm::Constant *site,
                                llvm::ArrayRef<Member> members) {
  if (members.empty()) {
    module.callSlow(builder, module.guardFunction(), {address, width, site});
    return;
  }
  for (const Member &member : members)
    module.callSlow(builder, module.guardFunction(),
                    {builder.CreatePtrToInt(member.pointer, module.i64),
                     llvm::ConstantInt::get(module.i64, member.width),
                     member.site});
}

void FunctionContext::emitGuard(llvm::Instruction *before, llvm::Value *pointer,
                                std::uint64_t width, llvm::Align alignment,
                                llvm::Constant *site, llvm::Value *nullBase,
                                llvm::ArrayRef<Member> members) {
  llvm::LLVMContext &context = module.context;
  llvm::IRBuilder<> builder(before);
  llvm::Value *address = builder.CreatePtrToInt(pointer, module.i64);
  const bool little = module.layout.isLittleEndian();
  if (width == 0)
    return;
  if (width > 113 || (!little && width > 16)) {
    if (members.empty()) {
      builder.CreateCall(
          module.rangeFunction(),
          {address, llvm::ConstantInt::get(module.i64, width), site});
      return;
    }
    slowGuard(builder, address, nullptr, site, members);
    return;
  }
  // One granule: a power of two at most 16 whose alignment keeps it in one.
  const bool single =
      width <= 16 && llvm::isPowerOf2_64(width) && alignment.value() >= width;
  llvm::Value *nullTest = nullptr;
  if (nullBase != nullptr)
    nullTest = builder.CreateICmpEQ(
        nullBase, llvm::ConstantPointerNull::get(
                      llvm::cast<llvm::PointerType>(nullBase->getType())));
  llvm::Value *shadowAt = shadowAddress(builder, address);
  llvm::Value *offset = builder.CreateAnd(address, 15);
  if (single) {
    llvm::LoadInst *value = builder.CreateLoad(module.i8, shadowAt);
    llvm::Value *nonzero =
        builder.CreateICmpNE(value, llvm::ConstantInt::get(module.i8, 0));
    llvm::Value *slowish =
        nullTest != nullptr ? builder.CreateOr(nonzero, nullTest) : nonzero;
    llvm::Instruction *check = llvm::SplitBlockAndInsertIfThen(
        slowish, before, /*Unreachable=*/false, unlikely(context));
    builder.SetInsertPoint(check);
    if (nullTest != nullptr) {
      llvm::Instruction *null = llvm::SplitBlockAndInsertIfThen(
          nullTest, check, false, unlikely(context));
      llvm::IRBuilder<> atNull(null);
      module.callSlow(atNull, module.nullFunction(), {address, site});
      builder.SetInsertPoint(check);
    }
    llvm::Value *last = builder.CreateTrunc(
        builder.CreateAdd(offset,
                          llvm::ConstantInt::get(module.i64, width - 1)),
        module.i8);
    llvm::Value *bad = builder.CreateICmpSGE(last, value);
    llvm::Instruction *slow =
        llvm::SplitBlockAndInsertIfThen(bad, check, false, unlikely(context));
    llvm::IRBuilder<> at(slow);
    slowGuard(at, address, llvm::ConstantInt::get(module.i64, width), site,
              members);
    return;
  }
  if (width <= 16) {
    // At most two granules: the first byte's and the last byte's. Both 0,
    // the access passes; otherwise the first must be 0 when the access
    // leaves its granule, and the last must hold the last byte.
    // The fast path loads the two shadow bytes from the first one: both 0,
    // the access passes (the second may be a granule the access does not
    // reach, which can only send it to the exact check).
    llvm::Value *lastAddress = builder.CreateAdd(
        address, llvm::ConstantInt::get(module.i64, width - 1));
    llvm::Value *nonzero = nullptr;
    llvm::Value *first = nullptr;
    llvm::Value *last = nullptr;
    if (little) {
      llvm::Value *pair = builder.CreateAlignedLoad(
          llvm::Type::getInt16Ty(context), shadowAt, llvm::Align(1));
      nonzero = builder.CreateICmpNE(
          pair, llvm::ConstantInt::get(llvm::Type::getInt16Ty(context), 0));
    } else {
      first = builder.CreateLoad(module.i8, shadowAt);
      last = builder.CreateLoad(module.i8, shadowAddress(builder, lastAddress));
      nonzero = builder.CreateICmpNE(builder.CreateOr(first, last),
                                     llvm::ConstantInt::get(module.i8, 0));
    }
    llvm::Value *slowish =
        nullTest != nullptr ? builder.CreateOr(nonzero, nullTest) : nonzero;
    llvm::Instruction *check = llvm::SplitBlockAndInsertIfThen(
        slowish, before, false, unlikely(context));
    builder.SetInsertPoint(check);
    if (little) {
      first = builder.CreateLoad(module.i8, shadowAt);
      last = builder.CreateLoad(module.i8, shadowAddress(builder, lastAddress));
    }
    if (nullTest != nullptr) {
      llvm::Instruction *null = llvm::SplitBlockAndInsertIfThen(
          nullTest, check, false, unlikely(context));
      llvm::IRBuilder<> atNull(null);
      module.callSlow(atNull, module.nullFunction(), {address, site});
      builder.SetInsertPoint(check);
    }
    llvm::Value *crosses = builder.CreateICmpNE(
        builder.CreateLShr(builder.CreateXor(address, lastAddress), 4),
        llvm::ConstantInt::get(module.i64, 0));
    llvm::Value *lastByte =
        builder.CreateTrunc(builder.CreateAnd(lastAddress, 15), module.i8);
    llvm::Value *bad = builder.CreateOr(
        builder.CreateAnd(
            crosses,
            builder.CreateICmpNE(first, llvm::ConstantInt::get(module.i8, 0))),
        builder.CreateAnd(
            builder.CreateICmpNE(last, llvm::ConstantInt::get(module.i8, 0)),
            builder.CreateICmpSGE(lastByte, last)));
    llvm::Instruction *slow =
        llvm::SplitBlockAndInsertIfThen(bad, check, false, unlikely(context));
    llvm::IRBuilder<> at(slow);
    slowGuard(at, address, llvm::ConstantInt::get(module.i64, width), site,
              members);
    return;
  }
  chunkCheck(before, address, llvm::ConstantInt::get(module.i64, width), site,
             nullTest, shadowAt, {}, members);
}

void FunctionContext::chunkCheckString(llvm::Instruction *before,
                                       llvm::Value *pointer,
                                       std::uint64_t bound,
                                       llvm::Constant *site) {
  llvm::IRBuilder<> builder(before);
  llvm::Value *address = builder.CreatePtrToInt(pointer, module.i64);
  // The slow path takes the pointer, the bound and the site, as the
  // runtime's string guard does.
  chunkCheck(before, address, llvm::ConstantInt::get(module.i64, bound), site,
             nullptr, nullptr, module.strlenFunction());
}

void FunctionContext::chunkCheck(llvm::Instruction *before,
                                 llvm::Value *address, llvm::Value *width,
                                 llvm::Constant *site, llvm::Value *nullTest,
                                 llvm::Value *shadowAt,
                                 llvm::FunctionCallee slow,
                                 llvm::ArrayRef<Member> members) {
  // Up to eight granules: the shadow bytes of every granule the access
  // touches but the last must be 0, and the last must be 0 or hold more
  // bytes than the access uses of it.
  llvm::LLVMContext &context = module.context;
  llvm::IRBuilder<> builder(before);
  if (shadowAt == nullptr)
    shadowAt = shadowAddress(builder, address);
  // A constant width: the fast path tests every granule the access could
  // touch, whatever its offset in the first, with one load and a constant
  // mask; a granule it does not reach can only send it to the exact check
  // below.
  llvm::Instruction *exactBefore = before;
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(width);
      constant != nullptr && !constant->isZero() &&
      constant->getZExtValue() <= 113) {
    const std::uint64_t reach = (15 + constant->getZExtValue() + 15) / 16;
    llvm::Value *fast = nullptr;
    if (reach <= 2) {
      llvm::Type *type = llvm::Type::getIntNTy(context, 8 * unsigned(reach));
      fast = builder.CreateICmpNE(
          builder.CreateAlignedLoad(type, shadowAt, llvm::Align(1)),
          llvm::ConstantInt::get(type, 0));
    } else {
      const std::uint64_t mask = reach >= 8
                                     ? ~std::uint64_t{0}
                                     : (std::uint64_t{1} << (8 * reach)) - 1;
      fast = builder.CreateICmpNE(
          builder.CreateAnd(
              builder.CreateAlignedLoad(module.i64, shadowAt, llvm::Align(1)),
              mask),
          llvm::ConstantInt::get(module.i64, 0));
    }
    if (nullTest != nullptr)
      fast = builder.CreateOr(fast, nullTest);
    llvm::Instruction *exact =
        llvm::SplitBlockAndInsertIfThen(fast, before, false, unlikely(context));
    builder.SetInsertPoint(exact);
    exactBefore = exact;
  }
  llvm::Value *offset = builder.CreateAnd(address, 15);
  llvm::Value *lastOffset = builder.CreateAdd(
      offset, builder.CreateSub(width, llvm::ConstantInt::get(module.i64, 1)));
  llvm::Value *granules = builder.CreateLShr(lastOffset, 4);
  llvm::LoadInst *chunk =
      builder.CreateAlignedLoad(module.i64, shadowAt, llvm::Align(1));
  llvm::Value *bits = builder.CreateShl(granules, 3);
  llvm::Value *restMask = builder.CreateSub(
      builder.CreateShl(llvm::ConstantInt::get(module.i64, 1), bits),
      llvm::ConstantInt::get(module.i64, 1));
  llvm::Value *rest = builder.CreateAnd(chunk, restMask);
  llvm::Value *lastValue =
      builder.CreateTrunc(builder.CreateLShr(chunk, bits), module.i8);
  llvm::Value *nonzero = builder.CreateOr(
      builder.CreateICmpNE(rest, llvm::ConstantInt::get(module.i64, 0)),
      builder.CreateICmpNE(lastValue, llvm::ConstantInt::get(module.i8, 0)));
  llvm::Value *slowish =
      nullTest != nullptr ? builder.CreateOr(nonzero, nullTest) : nonzero;
  llvm::Instruction *check = llvm::SplitBlockAndInsertIfThen(
      slowish, exactBefore, false, unlikely(context));
  builder.SetInsertPoint(check);
  if (nullTest != nullptr) {
    llvm::Instruction *null = llvm::SplitBlockAndInsertIfThen(
        nullTest, check, false, unlikely(context));
    llvm::IRBuilder<> atNull(null);
    module.callSlow(atNull, module.nullFunction(), {address, site});
    builder.SetInsertPoint(check);
  }
  llvm::Value *lastByte =
      builder.CreateTrunc(builder.CreateAnd(lastOffset, 15), module.i8);
  llvm::Value *bad = builder.CreateOr(
      builder.CreateICmpNE(rest, llvm::ConstantInt::get(module.i64, 0)),
      builder.CreateICmpSGE(lastByte, lastValue));
  llvm::Instruction *slowPath =
      llvm::SplitBlockAndInsertIfThen(bad, check, false, unlikely(context));
  llvm::IRBuilder<> at(slowPath);
  if (slow.getCallee() == nullptr)
    slowGuard(at, address, width, site, members);
  else
    at.CreateCall(slow, {at.CreateIntToPtr(address, module.ptr), width, site});
}

void FunctionContext::emitRange(llvm::Instruction *before, llvm::Value *pointer,
                                llvm::Value *length, llvm::Constant *site) {
  llvm::IRBuilder<> builder(before);
  llvm::Value *address = builder.CreatePtrToInt(pointer, module.i64);
  llvm::Value *bytes = builder.CreateZExtOrTrunc(length, module.i64);
  if (!module.layout.isLittleEndian()) {
    builder.CreateCall(module.rangeFunction(), {address, bytes, site});
    return;
  }
  // From 1 to 113 bytes, inline; the rest, and 0, through the runtime.
  llvm::Value *small = builder.CreateICmpULT(
      builder.CreateSub(bytes, llvm::ConstantInt::get(module.i64, 1)),
      llvm::ConstantInt::get(module.i64, 113));
  llvm::Instruction *inlineTerm = nullptr;
  llvm::Instruction *callTerm = nullptr;
  llvm::SplitBlockAndInsertIfThenElse(small, before, &inlineTerm, &callTerm);
  llvm::IRBuilder<>(callTerm).CreateCall(module.rangeFunction(),
                                         {address, bytes, site});
  chunkCheck(inlineTerm, address, bytes, site, nullptr, nullptr);
}

void FunctionContext::writeShadow(llvm::Instruction *before,
                                  llvm::Value *pointer,
                                  llvm::ArrayRef<std::uint8_t> bytes) {
  if (bytes.empty())
    return;
  llvm::IRBuilder<> builder(before);
  const ShadowValues values = shadow();
  // Before the shadow exists `mask` is 0 and every index is 0: nothing is
  // written.
  llvm::Value *ready =
      builder.CreateICmpNE(values.mask, llvm::ConstantInt::get(module.i64, 0));
  llvm::Instruction *write =
      llvm::SplitBlockAndInsertIfThen(ready, before, false);
  builder.SetInsertPoint(write);
  llvm::Value *address = builder.CreatePtrToInt(pointer, module.i64);
  llvm::Value *start = shadowAddress(builder, address);
  std::size_t at = 0;
  while (at < bytes.size()) {
    // A long run of one value is a memset; eight bytes a store.
    std::size_t run = 1;
    while (at + run < bytes.size() && bytes[at + run] == bytes[at])
      ++run;
    llvm::Value *to = builder.CreateConstInBoundsGEP1_64(module.i8, start, at);
    if (run >= 64) {
      builder.CreateMemSet(to, llvm::ConstantInt::get(module.i8, bytes[at]),
                           run, llvm::MaybeAlign(1));
      at += run;
      continue;
    }
    // The widest store that fits, little-endian.
    unsigned width = 1;
    if (module.layout.isLittleEndian())
      for (const unsigned candidate : {8U, 4U, 2U})
        if (at + candidate <= bytes.size()) {
          width = candidate;
          break;
        }
    std::uint64_t word = 0;
    for (unsigned i = 0; i < width; ++i)
      word |= static_cast<std::uint64_t>(bytes[at + i]) << (8 * i);
    builder.CreateAlignedStore(
        llvm::ConstantInt::get(
            llvm::IntegerType::get(module.context, 8 * width), word),
        to, llvm::Align(1));
    at += width;
  }
}

} // namespace guard

llvm::PreservedAnalyses GuardInsertPass::run(llvm::Module &module,
                                             llvm::ModuleAnalysisManager &) {
  if (module.getDataLayout().getPointerSizeInBits() != 64)
    return llvm::PreservedAnalyses::all();
  guard::ModuleContext context(module, options);
  for (llvm::Function &function : module)
    if (!guard::skips(function))
      guard::insertGuards(context, function);
  return llvm::PreservedAnalyses::none();
}

llvm::PreservedAnalyses GuardPrunePass::run(llvm::Function &function,
                                            llvm::FunctionAnalysisManager &) {
  if (guard::skips(function))
    return llvm::PreservedAnalyses::all();
  guard::ModuleContext context(*function.getParent(), options);
  guard::pruneGuards(context, function);
  return llvm::PreservedAnalyses::none();
}

llvm::PreservedAnalyses
GuardLoopPass::run(llvm::Function &function,
                   llvm::FunctionAnalysisManager &analyses) {
  // Verify mode keeps every guard as the monitor of its rule.
  if (guard::skips(function) || options.mode == GuardOptions::Mode::Verify ||
      function.getParent()->getDataLayout().getPointerSizeInBits() != 64)
    return llvm::PreservedAnalyses::all();
  guard::ModuleContext context(*function.getParent(), options);
  return guard::versionLoops(context, function, analyses)
             ? llvm::PreservedAnalyses::none()
             : llvm::PreservedAnalyses::all();
}

llvm::PreservedAnalyses
GuardExpandPass::run(llvm::Module &module,
                     llvm::ModuleAnalysisManager &analyses) {
  if (module.getDataLayout().getPointerSizeInBits() != 64)
    return llvm::PreservedAnalyses::all();
  guard::ModuleContext context(module, options);
  auto &functions =
      analyses.getResult<llvm::FunctionAnalysisManagerModuleProxy>(module)
          .getManager();
  std::vector<llvm::Function *> work;
  for (llvm::Function &function : module)
    if (!guard::skips(function))
      work.push_back(&function);
  for (llvm::Function *function : work) {
    guard::expandGuards(context, *function, functions);
    functions.invalidate(*function, llvm::PreservedAnalyses::none());
  }
  guard::instrumentGlobals(context);
  if (options.ledger)
    options.ledger->finish();
  return llvm::PreservedAnalyses::none();
}

void registerGuardPasses(clang::CodeGenOptions &codegen, GuardOptions options) {
  codegen.PassBuilderCallbacks.emplace_back(
      [options](llvm::PassBuilder &builder) {
        builder.registerPipelineStartEPCallback(
            [options](llvm::ModulePassManager &passes,
                      llvm::OptimizationLevel /*level*/) {
              passes.addPass(GuardInsertPass(options));
            });
        builder.registerPeepholeEPCallback(
            [options](llvm::FunctionPassManager &passes,
                      llvm::OptimizationLevel /*level*/) {
              passes.addPass(GuardPrunePass(options));
            });
        builder.registerVectorizerStartEPCallback(
            [options](llvm::FunctionPassManager &passes,
                      llvm::OptimizationLevel /*level*/) {
              passes.addPass(GuardLoopPass(options));
            });
        builder.registerOptimizerLastEPCallback(
            [options](llvm::ModulePassManager &passes,
                      llvm::OptimizationLevel /*level*/,
                      llvm::ThinOrFullLTOPhase /*phase*/) {
              passes.addPass(GuardExpandPass(options));
            });
      });
}

} // namespace weavec::frontend
