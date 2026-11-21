//===- GuardDefine.cpp - Nothing undefined in safe functions -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0051 §2, §3. Before GuardInsert, in every safe function of the unit,
// what the language leaves undefined is given one result or made a stop:
//
//   - signed `add`, `sub`, `mul` and `shl` lose `nsw`: they wrap;
//   - a shift whose count is not a constant below the width takes it modulo
//     the width (any width: `_BitInt(24)`'s too);
//   - `sdiv`, `udiv`, `lshr` and `ashr` lose `exact`: a pointer difference
//     that is not a multiple of the element's size is the quotient;
//   - `fptosi` and `fptoui` saturate (`llvm.fpto[su]i.sat`), NaN to 0;
//   - `llvm.assume` is removed, and `llvm.abs`, `llvm.ctlz` and `llvm.cttz`
//     have a result for the least value and for zero;
//   - `llvm.memcpy` and `llvm.memcpy.inline` (an assignment of a struct, a
//     copy Clang emits) are a move: an object assigned to one it overlaps is
//     copied as it was;
//   - the function and its loops lose `mustprogress`;
//   - a division or remainder whose divisor may be zero stops
//     (`division-by-zero`), and a signed one of the least value by -1
//     (`division-overflow`);
//   - an `unreachable` the source reaches stops (`unreachable`), and so does
//     a call of a function whose code is only `unreachable` (`std::unreachable`
//     inlines to nothing else); one after a call that does not return, or
//     after a stop Clang placed, stays;
//   - in C, the end of a function that must return a value, reached without
//     a `return`, stops (`missing-return`); C++'s is Clang's own check
//     (SanitizerKind::Return), which the driver asks for in a safe unit;
//   - a frame larger than a page is probed page by page (`probe-stack`), so
//     that the guard page below a stack cannot be stepped over;
//   - in a unit not compiled with -fweavec-safe, a local larger than the
//     4,096 bytes Clang zeroes is zeroed where its scope starts;
//   - in a unit with a safe function, no call of `malloc`, `new` and the
//     like says that its block's bytes are undefined (its `allockind`
//     says neither `uninitialized` nor `zeroed`, and it is not the
//     library's builtin): LLVM would otherwise fold a read of a fresh
//     block to any value. A read loads the block's bytes, which are the
//     runtime's zeroes (or what a program's own allocator gave).
//
// A unit compiled with -fweavec-safe gets the first, third and sixth from
// its options too (Driver.cpp), so that the library code it instantiates
// is defined as well; this pass is what makes a WEAVEC_SAFE function of any
// unit defined in its own code. No guard is removed here, so verify mode
// has nothing to monitor.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"

#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace weavec::frontend::guard {

/// RFC 0035 §1's limit on the locals Clang zeroes (`ZeroInitMaxSize` in
/// Driver.cpp), above which this pass zeroes a safe function's own.
static constexpr std::uint64_t ClangZeroedLimit = 4096;

bool definesSafe(const ModuleContext &module, const llvm::Function &function) {
  const SafeUnit *safe = module.options.safe.get();
  if (safe == nullptr || function.isDeclaration())
    return false;
  const llvm::StringRef name = function.getName();
  return safe->isSafe(name) || safe->definedSafe.contains(name);
}

/// A runtime entry that stops the program: `(site)` or `(site, bits)`.
static llvm::FunctionCallee stopFunction(ModuleContext &module,
                                         llvm::StringRef name, bool bits) {
  llvm::Type *none = llvm::Type::getVoidTy(module.context);
  llvm::FunctionType *type =
      bits ? llvm::FunctionType::get(none, {module.ptr, module.i32}, false)
           : llvm::FunctionType::get(none, {module.ptr}, false);
  llvm::FunctionCallee callee = module.runtime(name, type);
  if (auto *function = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
    function->setDoesNotReturn();
    function->addFnAttr(llvm::Attribute::Cold);
  }
  return callee;
}

/// Where a stop at `instruction` reports: its location, or the function's.
static llvm::DebugLoc placeOf(llvm::Instruction &instruction) {
  if (instruction.getDebugLoc())
    return instruction.getDebugLoc();
  llvm::DISubprogram *program = instruction.getFunction()->getSubprogram();
  if (program == nullptr)
    return {};
  return llvm::DILocation::get(program->getContext(), program->getLine(), 0,
                               program);
}

/// Inserts a call of the stop `name` before `before`.
static void stopAt(ModuleContext &module, llvm::Instruction *before,
                   llvm::StringRef name, const llvm::DebugLoc &at,
                   std::optional<unsigned> bits = std::nullopt) {
  llvm::IRBuilder<> builder(before);
  builder.SetCurrentDebugLocation(at);
  llvm::SmallVector<llvm::Value *, 2> args{module.site(at, 0)};
  if (bits)
    args.push_back(llvm::ConstantInt::get(module.i32, *bits));
  llvm::CallInst *call =
      builder.CreateCall(stopFunction(module, name, bits.has_value()), args);
  call->setDoesNotReturn();
}

/// Inserts, before `before`, a branch to a stop when `condition` holds.
static void stopIf(ModuleContext &module, llvm::Value *condition,
                   llvm::Instruction *before, llvm::StringRef name,
                   std::optional<unsigned> bits = std::nullopt) {
  const llvm::DebugLoc at = placeOf(*before);
  llvm::Instruction *then = llvm::SplitBlockAndInsertIfThen(
      condition, before->getIterator(), /*Unreachable=*/true);
  stopAt(module, then, name, at, bits);
}

/// Whether `function`'s code is only `unreachable`, directly or through
/// calls of functions that are.
static bool onlyUnreachable(const llvm::Function &function, unsigned depth) {
  if (function.isDeclaration() || depth > 4)
    return false;
  const llvm::Instruction &first = function.getEntryBlock().front();
  if (llvm::isa<llvm::UnreachableInst>(first))
    return true;
  const auto *call = llvm::dyn_cast<llvm::CallInst>(&first);
  const llvm::Function *callee =
      call == nullptr ? nullptr : call->getCalledFunction();
  return callee != nullptr && onlyUnreachable(*callee, depth + 1);
}

/// §3.1: an `unreachable` the program reaches stops, unless it follows a
/// call that does not return (a stop Clang placed, `abort`, a throw).
static void defineUnreachable(ModuleContext &module, llvm::UnreachableInst &end,
                              LedgerDefined &counts) {
  if (auto *call = llvm::dyn_cast_or_null<llvm::CallInst>(end.getPrevNode())) {
    const llvm::Function *callee = call->getCalledFunction();
    if (callee != nullptr && !callee->isIntrinsic() &&
        !ModuleContext::isRuntimeName(callee->getName()) &&
        onlyUnreachable(*callee, 0)) {
      // `std::unreachable()`: the call is the stop.
      stopAt(module, call, "unreachable", placeOf(*call));
      if (call->use_empty())
        call->eraseFromParent();
      ++counts.unreachable;
      return;
    }
    if (call->doesNotReturn() ||
        (callee != nullptr &&
         (callee->doesNotReturn() ||
          ModuleContext::isRuntimeName(callee->getName()))))
      return;
  }
  stopAt(module, &end, "unreachable", placeOf(end));
  ++counts.unreachable;
}

/// §3.1 in C: the return block reads a `retval` slot that only `return`
/// statements write; a path that writes none ends the function without its
/// value. A flag set beside each write says so, and the optimizer folds it
/// away where every path returns.
static void defineReturnsC(ModuleContext &module, llvm::Function &function,
                           LedgerDefined &counts) {
  if (function.getReturnType()->isVoidTy())
    return;
  std::vector<llvm::ReturnInst *> returns;
  for (llvm::BasicBlock &block : function)
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator()))
      returns.push_back(ret);
  llvm::BasicBlock &entry = function.getEntryBlock();
  for (llvm::ReturnInst *ret : returns) {
    llvm::Value *value = ret->getReturnValue();
    if (value == nullptr)
      continue;
    if (llvm::isa<llvm::UndefValue>(value)) {
      stopAt(module, ret, "missing_return", placeOf(*ret));
      ++counts.missingReturn;
      continue;
    }
    auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
    auto *slot =
        load == nullptr
            ? nullptr
            : llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand());
    if (slot == nullptr || slot->getParent() != &entry)
      continue;
    std::vector<llvm::StoreInst *> writes;
    bool onlyReturns = true;
    for (llvm::User *user : slot->users()) {
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
          store != nullptr && store->getPointerOperand() == slot &&
          !llvm::isa<llvm::Argument>(store->getValueOperand()))
        writes.push_back(store);
      else if (user != load)
        onlyReturns = false;
    }
    if (!onlyReturns || writes.empty())
      continue;
    llvm::IRBuilder<> builder(&*entry.getFirstNonPHIOrDbgOrAlloca());
    llvm::Type *flag = llvm::Type::getInt1Ty(module.context);
    auto *set = new llvm::AllocaInst(flag, 0, "weavec.returned",
                                     entry.getFirstInsertionPt());
    builder.CreateStore(llvm::ConstantInt::getFalse(module.context), set);
    for (llvm::StoreInst *write : writes) {
      llvm::IRBuilder<> after(write->getNextNode());
      after.CreateStore(llvm::ConstantInt::getTrue(module.context), set);
    }
    llvm::IRBuilder<> before(load);
    llvm::Value *returned = before.CreateLoad(flag, set);
    stopIf(module, before.CreateNot(returned), load, "missing_return");
    ++counts.missingReturn;
  }
}

/// §3.2: a division whose divisor may be zero, or a signed one that may
/// divide the least value by -1.
static void defineDivision(ModuleContext &module, llvm::BinaryOperator &divide,
                           LedgerDefined &counts) {
  llvm::Value *dividend = divide.getOperand(0);
  llvm::Value *divisor = divide.getOperand(1);
  auto *type = llvm::dyn_cast<llvm::IntegerType>(divide.getType());
  if (type == nullptr)
    return;
  const bool isSigned = divide.getOpcode() == llvm::Instruction::SDiv ||
                        divide.getOpcode() == llvm::Instruction::SRem;
  const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(divisor);
  const auto *numerator = llvm::dyn_cast<llvm::ConstantInt>(dividend);
  llvm::IRBuilder<> builder(&divide);
  if (constant == nullptr || constant->isZero()) {
    stopIf(module,
           builder.CreateICmpEQ(divisor, llvm::ConstantInt::get(type, 0)),
           &divide, "division", 0U);
    ++counts.division;
  }
  if (!isSigned || (constant != nullptr && !constant->isMinusOne()) ||
      (numerator != nullptr && !numerator->isMinValue(true)))
    return;
  builder.SetInsertPoint(&divide);
  llvm::Value *least = builder.CreateICmpEQ(
      dividend, llvm::ConstantInt::get(
                    type, llvm::APInt::getSignedMinValue(type->getBitWidth())));
  llvm::Value *minusOne =
      builder.CreateICmpEQ(divisor, llvm::ConstantInt::getSigned(type, -1));
  stopIf(module, builder.CreateAnd(least, minusOne), &divide, "division",
         type->getBitWidth());
  ++counts.division;
}

/// §2: a shift's count modulo the width: a mask for a width that is a power
/// of two, a remainder for any other (`_BitInt(24)`).
static void defineShift(llvm::BinaryOperator &shift, LedgerDefined &counts) {
  llvm::Type *type = shift.getType();
  const unsigned width = type->getScalarSizeInBits();
  if (width == 0)
    return;
  llvm::Value *count = shift.getOperand(1);
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(count);
      constant != nullptr && constant->getValue().ult(width))
    return;
  if (const auto *constant = llvm::dyn_cast<llvm::Constant>(count);
      constant != nullptr && type->isVectorTy())
    if (const llvm::Constant *splat = constant->getSplatValue())
      if (const auto *value = llvm::dyn_cast<llvm::ConstantInt>(splat);
          value != nullptr && value->getValue().ult(width))
        return;
  llvm::IRBuilder<> builder(&shift);
  shift.setOperand(
      1, llvm::isPowerOf2_32(width)
             ? builder.CreateAnd(count, llvm::ConstantInt::get(type, width - 1))
             : builder.CreateURem(count, llvm::ConstantInt::get(type, width)));
  ++counts.shifts;
}

/// §2: a conversion from floating point to an integer saturates.
static void defineConversion(ModuleContext &module, llvm::CastInst &cast,
                             LedgerDefined &counts) {
  const llvm::Intrinsic::ID id = cast.getOpcode() == llvm::Instruction::FPToSI
                                     ? llvm::Intrinsic::fptosi_sat
                                     : llvm::Intrinsic::fptoui_sat;
  llvm::Function *saturating = llvm::Intrinsic::getOrInsertDeclaration(
      &module.module, id, {cast.getDestTy(), cast.getSrcTy()});
  llvm::IRBuilder<> builder(&cast);
  llvm::CallInst *call = builder.CreateCall(saturating, {cast.getOperand(0)});
  call->setDebugLoc(cast.getDebugLoc());
  cast.replaceAllUsesWith(call);
  cast.eraseFromParent();
  ++counts.conversions;
}

/// §2: a loop's `llvm.loop` without `llvm.loop.mustprogress`.
static void forgetProgress(llvm::Instruction &terminator) {
  const llvm::MDNode *loop = terminator.getMetadata(llvm::LLVMContext::MD_loop);
  if (loop == nullptr || loop->getNumOperands() == 0)
    return;
  llvm::SmallVector<llvm::Metadata *, 4> kept{nullptr};
  bool dropped = false;
  for (unsigned i = 1; i < loop->getNumOperands(); ++i) {
    const auto *node = llvm::dyn_cast<llvm::MDNode>(loop->getOperand(i));
    const auto *name =
        node == nullptr || node->getNumOperands() == 0
            ? nullptr
            : llvm::dyn_cast<llvm::MDString>(node->getOperand(0));
    if (name != nullptr && name->getString() == "llvm.loop.mustprogress") {
      dropped = true;
      continue;
    }
    kept.push_back(loop->getOperand(i));
  }
  if (!dropped)
    return;
  llvm::MDNode *fresh =
      llvm::MDNode::getDistinct(terminator.getContext(), kept);
  fresh->replaceOperandWith(0, fresh);
  terminator.setMetadata(llvm::LLVMContext::MD_loop, fresh);
}

/// §2.3: in a unit whose options zero only the locals up to Clang's limit,
/// a safe function's larger locals are zeroed where each scope starts.
static void zeroLarge(const ModuleContext &module, llvm::Function &function) {
  std::vector<llvm::AllocaInst *> large;
  for (llvm::Instruction &instruction : function.getEntryBlock())
    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction))
      if (const std::optional<llvm::TypeSize> size =
              alloca->getAllocationSize(module.layout);
          size && !size->isScalable() &&
          size->getFixedValue() > ClangZeroedLimit)
        large.push_back(alloca);
  for (llvm::AllocaInst *alloca : large) {
    const std::uint64_t bytes =
        alloca->getAllocationSize(module.layout)->getFixedValue();
    std::vector<llvm::Instruction *> starts;
    for (llvm::User *user : alloca->users())
      if (auto *start = llvm::dyn_cast<llvm::LifetimeIntrinsic>(user);
          start != nullptr &&
          start->getIntrinsicID() == llvm::Intrinsic::lifetime_start)
        starts.push_back(start->getNextNode());
    if (starts.empty())
      starts.push_back(
          &*function.getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    for (llvm::Instruction *at : starts) {
      llvm::IRBuilder<> builder(at);
      builder.CreateMemSet(alloca, builder.getInt8(0), bytes,
                           alloca->getAlign());
    }
  }
}

void defineSafe(ModuleContext &module, llvm::Function &function) {
  LedgerDefined local;
  LedgerDefined &counts =
      module.options.ledger ? module.options.ledger->defined : local;
  function.removeFnAttr(llvm::Attribute::MustProgress);
  if (!function.hasFnAttribute("probe-stack")) {
    function.addFnAttr("probe-stack", "inline-asm");
    ++counts.probed;
  }
  std::vector<llvm::BinaryOperator *> divisions;
  std::vector<llvm::BinaryOperator *> shifts;
  std::vector<llvm::CastInst *> conversions;
  std::vector<llvm::UnreachableInst *> ends;
  std::vector<llvm::Instruction *> assumptions;
  std::vector<llvm::MemCpyInst *> copies;
  for (llvm::BasicBlock &block : function) {
    forgetProgress(*block.getTerminator());
    for (llvm::Instruction &instruction : block) {
      if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
        switch (binary->getOpcode()) {
        case llvm::Instruction::Add:
        case llvm::Instruction::Sub:
        case llvm::Instruction::Mul:
          binary->setHasNoSignedWrap(false);
          break;
        case llvm::Instruction::Shl:
          binary->setHasNoSignedWrap(false);
          shifts.push_back(binary);
          break;
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
          binary->setIsExact(false);
          shifts.push_back(binary);
          break;
        case llvm::Instruction::SDiv:
        case llvm::Instruction::UDiv:
          // Clang divides a pointer difference by the element's size with
          // `exact`, which makes a remainder poison.
          binary->setIsExact(false);
          divisions.push_back(binary);
          break;
        case llvm::Instruction::SRem:
        case llvm::Instruction::URem:
          divisions.push_back(binary);
          break;
        default:
          break;
        }
      } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(&instruction)) {
        if (cast->getOpcode() == llvm::Instruction::FPToSI ||
            cast->getOpcode() == llvm::Instruction::FPToUI)
          conversions.push_back(cast);
      } else if (auto *end =
                     llvm::dyn_cast<llvm::UnreachableInst>(&instruction)) {
        ends.push_back(end);
      } else if (auto *intrinsic =
                     llvm::dyn_cast<llvm::IntrinsicInst>(&instruction)) {
        switch (intrinsic->getIntrinsicID()) {
        case llvm::Intrinsic::assume:
          assumptions.push_back(intrinsic);
          break;
        case llvm::Intrinsic::memcpy:
        case llvm::Intrinsic::memcpy_inline:
          // An assignment of an object to one it overlaps copies as a
          // move: `memcpy`'s operands may not overlap.
          copies.push_back(llvm::cast<llvm::MemCpyInst>(intrinsic));
          break;
        case llvm::Intrinsic::abs:
        case llvm::Intrinsic::ctlz:
        case llvm::Intrinsic::cttz:
          // The least value's absolute value, and zero's leading zeros, have
          // a result.
          intrinsic->setArgOperand(1,
                                   llvm::ConstantInt::getFalse(module.context));
          break;
        default:
          break;
        }
      }
    }
  }
  for (llvm::Instruction *assumption : assumptions)
    assumption->eraseFromParent();
  for (llvm::MemCpyInst *copy : copies) {
    llvm::IRBuilder<> builder(copy);
    llvm::CallInst *moved = builder.CreateMemMove(
        copy->getRawDest(), copy->getDestAlign(), copy->getRawSource(),
        copy->getSourceAlign(), copy->getLength(), copy->isVolatile());
    moved->setDebugLoc(copy->getDebugLoc());
    moved->copyMetadata(*copy);
    copy->eraseFromParent();
  }
  for (llvm::BinaryOperator *shift : shifts)
    defineShift(*shift, counts);
  for (llvm::CastInst *cast : conversions)
    defineConversion(module, *cast, counts);
  for (llvm::UnreachableInst *end : ends)
    defineUnreachable(module, *end, counts);
  for (llvm::BinaryOperator *divide : divisions)
    defineDivision(module, *divide, counts);
  if (!module.options.safe->cxx)
    defineReturnsC(module, function, counts);
  if (!module.options.safe->defined)
    zeroLarge(module, function);
}

/// The allocation kind LLVM's inference of the library's attributes
/// (InferFunctionAttrs, which runs after this pass) gives the declaration
/// `call` calls, asked of a copy of it (`malloc` and `aligned_alloc` say
/// `uninitialized`, `calloc` says `zeroed`); 0 for none.
static std::uint64_t inferredKind(const llvm::CallBase &call,
                                  const llvm::TargetLibraryInfo &info,
                                  llvm::StringMap<std::uint64_t> &known) {
  const llvm::Function *callee = call.getCalledFunction();
  llvm::LibFunc libraryFunction{};
  if (callee == nullptr || !callee->isDeclaration() ||
      !info.getLibFunc(*callee, libraryFunction))
    return 0;
  auto [entry, inserted] = known.try_emplace(callee->getName(), 0);
  if (!inserted)
    return entry->second;
  llvm::Module scratch("weavec.allocation", callee->getContext());
  scratch.setDataLayout(callee->getParent()->getDataLayout());
  scratch.setTargetTriple(callee->getParent()->getTargetTriple());
  llvm::Function *copy = llvm::Function::Create(
      callee->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
      callee->getName(), scratch);
  llvm::inferNonMandatoryLibFuncAttrs(*copy, info);
  entry->second = static_cast<std::uint64_t>(
      copy->getAttributes().getFnAttrs().getAllocKind());
  return entry->second;
}

/// The allocation kind that makes LLVM take nothing of the first bytes of
/// the block `call` returns, for a call whose block it would take to be
/// undefined (a builtin `new` or `malloc`, or a kind that says
/// `uninitialized`, the call's own or its declaration's to be); or nothing.
/// The kind is the call's without `uninitialized`, and not `zeroed`
/// either: a program's own allocator serves the call in place of the
/// runtime's (RFC 0038 §1, RFC 0039 amendment 18), and its blocks are not
/// zeroes, so the zeroes a program stores into a fresh block must stay.
static std::optional<std::uint64_t>
unknownKind(const llvm::CallBase &call, const llvm::TargetLibraryInfo &info,
            llvm::StringMap<std::uint64_t> &known) {
  constexpr auto Uninitialized =
      static_cast<std::uint64_t>(llvm::AllocFnKind::Uninitialized);
  constexpr auto Families =
      static_cast<std::uint64_t>(llvm::AllocFnKind::Alloc) |
      static_cast<std::uint64_t>(llvm::AllocFnKind::Realloc) |
      static_cast<std::uint64_t>(llvm::AllocFnKind::Free);
  const llvm::Attribute own = call.getFnAttr(llvm::Attribute::AllocKind);
  std::uint64_t kind =
      own.isValid() ? own.getValueAsInt() : inferredKind(call, info, known);
  const bool undefined =
      llvm::isa_and_nonnull<llvm::UndefValue>(llvm::getInitialValueOfAllocation(
          &call, &info, llvm::Type::getInt8Ty(call.getContext())));
  if (!undefined && (kind & Uninitialized) == 0)
    return std::nullopt;
  kind &= ~Uninitialized;
  if ((kind & Families) == 0)
    kind |= static_cast<std::uint64_t>(llvm::AllocFnKind::Alloc);
  return kind;
}

void defineAllocations(ModuleContext &module, llvm::Module &ir) {
  const SafeUnit *safe = module.options.safe.get();
  if (safe == nullptr)
    return;
  const auto safeCode = [&](const llvm::Function &function) {
    return definesSafe(module, function);
  };
  if (!safe->defined && !llvm::any_of(ir, safeCode))
    return;
  const llvm::TargetLibraryInfoImpl library(ir.getTargetTriple());
  const llvm::TargetLibraryInfo info(library);
  llvm::StringMap<std::uint64_t> known;
  for (llvm::Function &function : ir)
    for (llvm::BasicBlock &block : function)
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (call == nullptr)
          continue;
        const std::optional<std::uint64_t> kind =
            unknownKind(*call, info, known);
        if (!kind)
          continue;
        // The call is no longer the library's builtin, whose block LLVM
        // takes to be undefined; its kind, which the call's own attributes
        // give before its declaration's, still makes it an allocation, one
        // the optimizer may remove with its release.
        call->removeFnAttr(llvm::Attribute::Builtin);
        call->addFnAttr(llvm::Attribute::NoBuiltin);
        call->removeFnAttr(llvm::Attribute::AllocKind);
        call->addFnAttr(llvm::Attribute::get(
            module.context, llvm::Attribute::AllocKind, *kind));
      }
}

} // namespace weavec::frontend::guard
