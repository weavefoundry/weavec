//===- GuardPass.cpp - Every memory access guarded ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035, RFC 0036: the guard passes' entry points and registration, and
// what they share: the module's context (types, markers, runtime entry points,
// site records, the ledger) and the inline checks of the shadow.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "clang/Basic/CodeGenOptions.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

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
  return size && offset >= 0 && std::cmp_less_equal(offset, *size) &&
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
      if (const auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
          instruction && (markerFor(*instruction, alloca) < 0))
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

std::optional<std::uint64_t> fieldLimit(const llvm::Value *pointer,
                                        const llvm::DataLayout &layout) {
  // (Not stripPointerCasts: that strips a first field's all-zero indices.)
  const auto *gep = llvm::dyn_cast<llvm::GEPOperator>(pointer);
  // An array's decay to its first element.
  if (gep != nullptr && gep->getSourceElementType()->isArrayTy() &&
      gep->hasAllZeroIndices())
    gep = llvm::dyn_cast<llvm::GEPOperator>(gep->getPointerOperand());
  if (gep == nullptr || gep->getNumIndices() < 2 ||
      !gep->getSourceElementType()->isStructTy())
    return std::nullopt;
  const auto *first = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
  if (first == nullptr || !first->isZero())
    return std::nullopt;
  llvm::Type *type = gep->getSourceElementType();
  const llvm::StructType *parent = nullptr;
  std::uint64_t field = 0;
  for (unsigned i = 2; i < gep->getNumOperands(); ++i) {
    const auto *index = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(i));
    if (index == nullptr)
      return std::nullopt;
    if (const auto *structure = llvm::dyn_cast<llvm::StructType>(type)) {
      parent = structure;
      field = index->getZExtValue();
      type = structure->getElementType(static_cast<unsigned>(field));
    } else if (const auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      parent = nullptr;
      type = array->getElementType();
    } else {
      return std::nullopt;
    }
  }
  if (parent == nullptr || !type->isArrayTy() ||
      field + 1 >= parent->getNumElements())
    return std::nullopt;
  return layout.getTypeAllocSize(type).getFixedValue();
}

bool skips(const llvm::Function &function) {
  // An available_externally body (a C99 inline definition) is guarded too:
  // the inliner copies it into this unit.
  // A library thunk (RFC 0037 §5) is guarded: its call is the one checked.
  return function.isDeclaration() ||
         function.hasFnAttribute(llvm::Attribute::Naked) ||
         (ModuleContext::isRuntimeName(function.getName()) &&
          !function.getName().starts_with(ThunkPrefix));
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
static void keep(llvm::FunctionCallee callee, llvm::MemoryEffects effects,
                 const GuardOptions &options) {
  auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee());
  if (function == nullptr)
    return;
  function->setDoesNotThrow();
  function->setMemoryEffects(effects);
  // Two guards of different accesses stay two, each with its location.
  function->addFnAttr(llvm::Attribute::NoMerge);
  // In C++ the inliner prices a marker as one instruction, not as a call
  // (RFC 0039, amendment 13): priced as a call, markers keep small members
  // (an iterator's operator*, vector::push_back) out of line. C keeps the
  // call's price, and so v0.19.0's inlining and build time.
  if (options.cheapMarkers)
    function->addFnAttr("call-inline-cost", "5");
  for (llvm::Argument &argument : function->args())
    if (argument.getType()->isPointerTy())
      argument.addAttr(llvm::Attribute::getWithCaptureInfo(
          function->getContext(), llvm::CaptureInfo::none()));
}

llvm::FunctionCallee ModuleContext::markerGuard() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerGuard, llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                           {ptr, i64, i32, ptr, i32}, false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref),
       options);
  return callee;
}

llvm::FunctionCallee ModuleContext::markerRange() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerRange, llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                           {ptr, i64, i32, i32}, false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref),
       options);
  return callee;
}

llvm::FunctionCallee ModuleContext::markerString() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerString, llvm::FunctionType::get(i64, {ptr, i64, i32, i32}, false));
  keep(callee,
       llvm::MemoryEffects::inaccessibleOrArgMemOnly(llvm::ModRefInfo::Ref),
       options);
  return callee;
}

llvm::FunctionCallee ModuleContext::markerDisjoint() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerDisjoint, llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                              {ptr, ptr, i64}, false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref),
       options);
  return callee;
}

llvm::FunctionCallee ModuleContext::markerScope() {
  llvm::FunctionCallee callee = module.getOrInsertFunction(
      MarkerScope,
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false));
  keep(callee, llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Mod),
       options);
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
  if (auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
    function->setDoesNotThrow();
    // RFC 0038 amendment 12: the runtime is a shared library, and a lazily
    // bound call's first trip through the dynamic linker's resolver keeps
    // only the argument registers, where a preserve_all slow path promises
    // its caller every register: every call goes through the GOT, bound at
    // load time.
    function->addFnAttr(llvm::Attribute::NonLazyBind);
  }
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
  const llvm::Triple &triple(module.getTargetTriple());
  if (triple.isAArch64() || triple.getArch() == llvm::Triple::x86_64)
    function->setCallingConv(llvm::CallingConv::PreserveAll);
}

llvm::CallInst *ModuleContext::callSlow(llvm::IRBuilder<> &builder,
                                        llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value *> args) {
  llvm::CallInst *call = builder.CreateCall(callee, args);
  if (const auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
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

llvm::FunctionCallee ModuleContext::jumpFunction() {
  llvm::FunctionCallee callee = runtime(
      "jump", llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                      {i64, i64, i64, i64, ptr}, false));
  slowPath(callee, module);
  return callee;
}

llvm::FunctionCallee ModuleContext::guardJumpFunction() {
  llvm::FunctionCallee callee = runtime(
      "guard_jump", llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                            {i64, i64, i64, i64, ptr}, false));
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

llvm::FunctionCallee ModuleContext::limitFailFunction() {
  llvm::FunctionCallee callee = runtime(
      "limit_fail", llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                            {i64, i64, i64, ptr}, false));
  slowPath(callee, module);
  return callee;
}

llvm::GlobalVariable *ModuleContext::shadowDescriptor() {
  if (descriptor != nullptr)
    return descriptor;
  auto *type = llvm::StructType::get(context, {i64, i64, i32});
  descriptor = module.getOrInsertGlobal("__weavec_rt_shadow", type);
  return descriptor;
}

/// Where a report names `location`: past the frames inlined from artificial
/// functions (`__attribute__((artificial))`, as the C library's fortified
/// wrappers are) and, RFC 0039, from the unit's system headers (the C++
/// library's), at their call site.
const llvm::DILocation *
ModuleContext::reportedAt(const llvm::DILocation *at) const {
  const UnsafeRegions *unit = options.unsafe.get();
  while (at->getInlinedAt() != nullptr) {
    const llvm::DISubprogram *function = at->getScope()->getSubprogram();
    const auto [known, added] =
        systemFiles.try_emplace(at->getScope()->getFile(), false);
    if (added && unit != nullptr)
      known->second = unit->isSystemFile(
          UnsafeRegions::normalize(at->getFilename(), at->getDirectory()));
    if (function == nullptr || (!function->isArtificial() && !known->second))
      break;
    at = at->getInlinedAt();
  }
  return at;
}

ModuleContext::Position
ModuleContext::positionOf(const llvm::DebugLoc &location) const {
  Position position;
  if (!location)
    return position;
  const llvm::DILocation *at = reportedAt(location.get());
  position.file =
      UnsafeRegions::normalize(at->getFilename(), at->getDirectory());
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
    const llvm::DILocation *at = reportedAt(location.get());
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
  // RFC 0037 §11: the file as an offset from the record, so that the record
  // is read-only data the loader does not relocate.
  llvm::Type *i16 = llvm::Type::getInt16Ty(context);
  if (siteType == nullptr)
    siteType = llvm::StructType::get(context, {i64, i32, i16, i16});
  auto *global = new llvm::GlobalVariable(module, siteType, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          nullptr, "__weavec.site");
  llvm::Constant *offset =
      file->isNullValue() ? llvm::ConstantInt::get(i64, 0)
                          : llvm::ConstantExpr::getSub(
                                llvm::ConstantExpr::getPtrToInt(file, i64),
                                llvm::ConstantExpr::getPtrToInt(global, i64));
  global->setInitializer(llvm::ConstantStruct::get(
      siteType, {offset, llvm::ConstantInt::get(i32, line),
                 llvm::ConstantInt::get(i16, std::min(column, 0xFFFFU)),
                 llvm::ConstantInt::get(i16, flags)}));
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
  loaded = {.base = base, .mask = mask};
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
    ModuleContext::callSlow(builder, module.guardFunction(),
                            {address, width, site});
    return;
  }
  for (const Member &member : members)
    ModuleContext::callSlow(builder, module.guardFunction(),
                            {builder.CreatePtrToInt(member.pointer, module.i64),
                             llvm::ConstantInt::get(module.i64, member.width),
                             member.site});
}

void FunctionContext::emitGuard(llvm::Instruction *before, llvm::Value *pointer,
                                std::uint64_t width, llvm::Align alignment,
                                llvm::Constant *site, llvm::Value *nullBase,
                                llvm::ArrayRef<Member> members,
                                RootCheck root) {
  llvm::LLVMContext &context = module.context;
  llvm::IRBuilder<> builder(before);
  llvm::Value *address = builder.CreatePtrToInt(pointer, module.i64);
  const bool little = module.layout.isLittleEndian();
  if (width == 0)
    return;
  // RFC 0037 §8.2: all but the one-granule form test the color apart.
  const bool oneGranule =
      width <= 16 && llvm::isPowerOf2_64(width) && alignment.value() >= width;
  if (root && !oneGranule) {
    colorCheck(before, address, llvm::ConstantInt::get(module.i64, width), site,
               root);
    builder.SetInsertPoint(before);
  }
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
  // A null root's shadow is poison, so the color test sends it to the
  // check block, where the null test can wait.
  const bool nullInCheck =
      single && root && nullBase != nullptr && root.root == nullBase;
  auto makeNullTest = [&](llvm::IRBuilder<> &at) {
    return at.CreateICmpEQ(
        nullBase, llvm::ConstantPointerNull::get(
                      llvm::cast<llvm::PointerType>(nullBase->getType())));
  };
  if (nullBase != nullptr && !nullInCheck)
    nullTest = makeNullTest(builder);
  llvm::Value *shadowAt = shadowAddress(builder, address);
  llvm::Value *offset = builder.CreateAnd(address, 15);
  if (single) {
    llvm::LoadInst *value = builder.CreateLoad(module.i8, shadowAt);
    llvm::Value *nonzero =
        root ? builder.CreateICmpNE(builder.CreateZExt(value, module.i32),
                                    root.shadow)
             : builder.CreateICmpSLT(value,
                                     llvm::ConstantInt::get(module.i8, 0));
    llvm::Value *slowish =
        nullTest != nullptr ? builder.CreateOr(nonzero, nullTest) : nonzero;
    llvm::Instruction *check = llvm::SplitBlockAndInsertIfThen(
        slowish, before, /*Unreachable=*/false, unlikely(context));
    builder.SetInsertPoint(check);
    if (nullInCheck)
      nullTest = makeNullTest(builder);
    if (nullTest != nullptr) {
      llvm::Instruction *null = llvm::SplitBlockAndInsertIfThen(
          nullTest, check, false, unlikely(context));
      llvm::IRBuilder<> atNull(null);
      ModuleContext::callSlow(atNull, module.nullFunction(), {address, site});
      builder.SetInsertPoint(check);
    }
    llvm::Value *last = builder.CreateTrunc(
        builder.CreateAdd(offset,
                          llvm::ConstantInt::get(module.i64, width - 1)),
        module.i8);
    // Poison, or a partial granule the access ends past. (Without a root
    // only a byte with its sign bit set comes here.)
    llvm::Value *bad = builder.CreateOr(
        builder.CreateICmpUGE(value, llvm::ConstantInt::get(module.i8, 0xF0)),
        builder.CreateICmpUGE(
            last,
            builder.CreateAnd(value, llvm::ConstantInt::get(module.i8, 15))));
    if (root)
      bad = builder.CreateAnd(
          builder.CreateICmpSLT(value, llvm::ConstantInt::get(module.i8, 0)),
          bad);
    // RFC 0037 §8.2: with a root, a byte of another color joins the cold
    // call, which decides the guard and the color at once; a partial
    // granule of the root's color class needs no call.
    if (root && root.monitorSite == nullptr && members.empty()) {
      llvm::Value *rootByte = builder.CreateTrunc(root.shadow, module.i8);
      auto classOf = [&](llvm::Value *byte) {
        return builder.CreateSelect(
            builder.CreateICmpSLT(byte, llvm::ConstantInt::get(module.i8, 0)),
            builder.CreateAnd(builder.CreateLShr(byte, 4), 7),
            builder.CreateAnd(byte, 7));
      };
      auto whole = [&](llvm::Value *byte) {
        return builder.CreateICmpSGE(byte,
                                     llvm::ConstantInt::get(module.i8, 0));
      };
      llvm::Value *differs = builder.CreateOr(
          builder.CreateAnd(builder.CreateAnd(whole(value), whole(rootByte)),
                            builder.CreateICmpNE(value, rootByte)),
          builder.CreateICmpNE(classOf(value), classOf(rootByte)));
      llvm::Instruction *slow = llvm::SplitBlockAndInsertIfThen(
          builder.CreateOr(bad, differs), check, false, unlikely(context));
      llvm::IRBuilder<> at(slow);
      ModuleContext::callSlow(at, module.guardJumpFunction(),
                              {address,
                               llvm::ConstantInt::get(module.i64, width),
                               at.CreatePtrToInt(root.root, module.i64),
                               at.CreateZExt(rootByte, module.i64), site});
      return;
    }
    llvm::Instruction *slow =
        llvm::SplitBlockAndInsertIfThen(bad, check, false, unlikely(context));
    llvm::IRBuilder<> at(slow);
    slowGuard(at, address, llvm::ConstantInt::get(module.i64, width), site,
              members);
    if (root) {
      builder.SetInsertPoint(check);
      llvm::Value *rootByte = builder.CreateTrunc(root.shadow, module.i8);
      llvm::Value *differs = builder.CreateICmpNE(value, rootByte);
      llvm::Instruction *jump = llvm::SplitBlockAndInsertIfThen(
          differs, check, false, unlikely(context));
      llvm::IRBuilder<> atJump(jump);
      ModuleContext::callSlow(
          atJump, module.jumpFunction(),
          {address, llvm::ConstantInt::get(module.i64, width),
           atJump.CreatePtrToInt(root.root, module.i64),
           atJump.CreateZExt(rootByte, module.i64),
           root.monitorSite != nullptr ? root.monitorSite : site});
    }
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
          builder.CreateAnd(pair, llvm::ConstantInt::get(
                                      llvm::Type::getInt16Ty(context), 0x8080)),
          llvm::ConstantInt::get(llvm::Type::getInt16Ty(context), 0));
    } else {
      first = builder.CreateLoad(module.i8, shadowAt);
      last = builder.CreateLoad(module.i8, shadowAddress(builder, lastAddress));
      nonzero = builder.CreateICmpSLT(builder.CreateOr(first, last),
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
      ModuleContext::callSlow(atNull, module.nullFunction(), {address, site});
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
            builder.CreateICmpSLT(first, llvm::ConstantInt::get(module.i8, 0))),
        builder.CreateAnd(
            builder.CreateICmpSLT(last, llvm::ConstantInt::get(module.i8, 0)),
            builder.CreateOr(
                builder.CreateICmpUGE(last,
                                      llvm::ConstantInt::get(module.i8, 0xF0)),
                builder.CreateICmpUGE(
                    lastByte, builder.CreateAnd(last, llvm::ConstantInt::get(
                                                          module.i8, 15))))));
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

void FunctionContext::colorCheck(llvm::Instruction *before,
                                 llvm::Value *address, llvm::Value *width,
                                 llvm::Constant *site, RootCheck root) {
  llvm::IRBuilder<> builder(before);
  llvm::LoadInst *value =
      builder.CreateLoad(module.i8, shadowAddress(builder, address));
  llvm::Value *rootByte = builder.CreateTrunc(root.shadow, module.i8);
  llvm::Value *differs = builder.CreateAnd(
      builder.CreateICmpNE(builder.CreateZExt(value, module.i32), root.shadow),
      builder.CreateICmpNE(value, rootByte));
  llvm::Instruction *jump = llvm::SplitBlockAndInsertIfThen(
      differs, before, false, unlikely(module.context));
  llvm::IRBuilder<> at(jump);
  ModuleContext::callSlow(
      at, module.jumpFunction(),
      {address, at.CreateZExtOrTrunc(width, module.i64),
       at.CreatePtrToInt(root.root, module.i64),
       at.CreateZExt(rootByte, module.i64),
       root.monitorSite != nullptr ? root.monitorSite : site});
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
      llvm::Type *type =
          llvm::Type::getIntNTy(context, 8 * static_cast<unsigned>(reach));
      fast = builder.CreateICmpNE(
          builder.CreateAnd(
              builder.CreateAlignedLoad(type, shadowAt, llvm::Align(1)),
              llvm::ConstantInt::get(type, 0x8080808080808080ULL)),
          llvm::ConstantInt::get(type, 0));
    } else {
      const std::uint64_t mask = reach >= 8
                                     ? ~std::uint64_t{0}
                                     : (std::uint64_t{1} << (8 * reach)) - 1;
      fast = builder.CreateICmpNE(
          builder.CreateAnd(
              builder.CreateAlignedLoad(module.i64, shadowAt, llvm::Align(1)),
              mask & 0x8080808080808080ULL),
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
  llvm::Value *rest = builder.CreateAnd(
      builder.CreateAnd(chunk, restMask),
      llvm::ConstantInt::get(module.i64, 0x8080808080808080ULL));
  llvm::Value *lastValue =
      builder.CreateTrunc(builder.CreateLShr(chunk, bits), module.i8);
  llvm::Value *nonzero = builder.CreateOr(
      builder.CreateICmpNE(rest, llvm::ConstantInt::get(module.i64, 0)),
      builder.CreateICmpSLT(lastValue, llvm::ConstantInt::get(module.i8, 0)));
  llvm::Value *slowish =
      nullTest != nullptr ? builder.CreateOr(nonzero, nullTest) : nonzero;
  llvm::Instruction *check = llvm::SplitBlockAndInsertIfThen(
      slowish, exactBefore, false, unlikely(context));
  builder.SetInsertPoint(check);
  if (nullTest != nullptr) {
    llvm::Instruction *null = llvm::SplitBlockAndInsertIfThen(
        nullTest, check, false, unlikely(context));
    llvm::IRBuilder<> atNull(null);
    ModuleContext::callSlow(atNull, module.nullFunction(), {address, site});
    builder.SetInsertPoint(check);
  }
  llvm::Value *lastByte =
      builder.CreateTrunc(builder.CreateAnd(lastOffset, 15), module.i8);
  llvm::Value *bad = builder.CreateOr(
      builder.CreateICmpNE(rest, llvm::ConstantInt::get(module.i64, 0)),
      builder.CreateAnd(
          builder.CreateICmpSLT(lastValue,
                                llvm::ConstantInt::get(module.i8, 0)),
          builder.CreateOr(
              builder.CreateICmpUGE(lastValue,
                                    llvm::ConstantInt::get(module.i8, 0xF0)),
              builder.CreateICmpUGE(
                  lastByte,
                  builder.CreateAnd(lastValue,
                                    llvm::ConstantInt::get(module.i8, 15))))));
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
                                  llvm::ArrayRef<ShadowRun> runs) {
  if (runs.empty())
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
  // Short runs are stored a word at a time; a run of 64 granules or more
  // is a memset, and one of RuntimeRun granules or more goes to the
  // runtime, which writes only what lies on the thread's stack.
  constexpr std::uint64_t MemsetRun = 64;
  constexpr std::uint64_t RuntimeRun = 4096;
  llvm::SmallVector<std::uint8_t, 64> pending;
  std::uint64_t pendingAt = 0;
  auto flush = [&] {
    std::size_t at = 0;
    while (at < pending.size()) {
      // The widest store that fits, little-endian.
      unsigned width = 1;
      if (module.layout.isLittleEndian())
        for (const unsigned candidate : {8U, 4U, 2U})
          if (at + candidate <= pending.size()) {
            width = candidate;
            break;
          }
      std::uint64_t word = 0;
      for (unsigned i = 0; i < width; ++i)
        word |= static_cast<std::uint64_t>(pending[at + i]) << (8 * i);
      builder.CreateAlignedStore(
          llvm::ConstantInt::get(
              llvm::IntegerType::get(module.context, 8 * width), word),
          builder.CreateConstInBoundsGEP1_64(module.i8, start, pendingAt + at),
          llvm::Align(1));
      at += width;
    }
    pending.clear();
  };
  std::uint64_t at = 0;
  for (const ShadowRun &run : runs) {
    if (run.count < MemsetRun) {
      if (pending.empty())
        pendingAt = at;
      pending.append(run.count, run.value);
      at += run.count;
      continue;
    }
    flush();
    if (run.count >= RuntimeRun) {
      builder.CreateCall(
          module.runtime("frame_shadow",
                         llvm::FunctionType::get(
                             llvm::Type::getVoidTy(module.context),
                             {module.i64, module.i64, module.i64}, false)),
          {builder.CreateAdd(address,
                             llvm::ConstantInt::get(module.i64, at * 16)),
           llvm::ConstantInt::get(module.i64, run.count * 16),
           llvm::ConstantInt::get(module.i64, run.value)});
    } else {
      builder.CreateMemSet(
          builder.CreateConstInBoundsGEP1_64(module.i8, start, at),
          llvm::ConstantInt::get(module.i8, run.value), run.count,
          llvm::MaybeAlign(1));
    }
    at += run.count;
  }
  flush();
}

/// In a build with assertions, the function each pass leaves is valid IR:
/// a broken one fails at the pass that broke it, not in the back end.
static void verified([[maybe_unused]] const llvm::Function &function) {
#ifndef NDEBUG
  if (llvm::verifyFunction(function, &llvm::errs()))
    llvm::report_fatal_error(llvm::Twine("weavec: a guard pass broke ") +
                             function.getName());
#endif
}

} // namespace guard

llvm::PreservedAnalyses GuardInsertPass::run(llvm::Module &module,
                                             llvm::ModuleAnalysisManager &) {
  if (module.getDataLayout().getPointerSizeInBits() != 64)
    return llvm::PreservedAnalyses::all();
  guard::ModuleContext context(module, options);
  guard::thunkLibraryAddresses(context, module);
  for (llvm::Function &function : module)
    if (!guard::skips(function)) {
      guard::insertGuards(context, function);
      guard::verified(function);
    }
  return llvm::PreservedAnalyses::none();
}

llvm::PreservedAnalyses GuardPrunePass::run(llvm::Function &function,
                                            llvm::FunctionAnalysisManager &) {
  if (guard::skips(function))
    return llvm::PreservedAnalyses::all();
  guard::ModuleContext context(*function.getParent(), options);
  guard::pruneGuards(context, function);
  guard::verified(function);
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
  const bool changed = guard::versionLoops(context, function, analyses);
  guard::verified(function);
  return changed ? llvm::PreservedAnalyses::none()
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
    guard::verified(*function);
    functions.invalidate(*function, llvm::PreservedAnalyses::none());
  }
  guard::instrumentGlobals(context);
  if (options.ledger)
    options.ledger->finish();
  return llvm::PreservedAnalyses::none();
}

void registerGuardPasses(clang::CodeGenOptions &codegen,
                         const GuardOptions &options) {
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
