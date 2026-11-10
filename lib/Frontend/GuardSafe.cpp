//===- GuardSafe.cpp - Safe functions' bounds -----------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0041 §4–§8, as amended. In a safe function, before the optimizer:
//
//   1. its locals that can live in registers are promoted, and its address
//      arithmetic loses `inbounds`, so that no check below reads a value
//      the optimizer may treat as poison;
//   2. a local whose address may outlive its scope (stored in memory,
//      returned, passed to an escaping parameter, kept through a
//      `lifetimebound` result, or used where the local may be out of scope)
//      is allocated from the runtime's heap for the scope instead (§6);
//   3. GuardInsert guards it as any function;
//   4. its bounded pointer parameters are validated at entry against the
//      shadow (a range or string guard) and escaping ones tested not to
//      point into the stack (§5);
//   5. each guard whose pointer derives, through arithmetic, phis and
//      selects, from a root with known bounds (a validated parameter, a
//      local, a global the unit defines, an allocation, a call whose result
//      the table bounds) is replaced by a comparison against the bounds:
//      `pointer-out-of-bounds` when it fails. Verify mode keeps the guard as
//      its monitor, and `-fweavec-safe-checks=full` keeps it as a guard.
//
// Every other access keeps its guard: a pointer loaded from memory or
// returned by a call the table does not bound is checked against the
// shadow, as in guarded code.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"
#include "weavec/Frontend/SafeCheck.h"

#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

namespace weavec::frontend::guard {

bool isSafe(const ModuleContext &module, const llvm::Function &function) {
  return module.options.safe != nullptr &&
         module.options.safe->isSafe(function.getName());
}

/// The table's entry for the function `call` calls directly, or null.
static const FunctionBounds *calleeBounds(const ModuleContext &module,
                                          const llvm::CallBase &call) {
  const llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr || module.options.safe == nullptr)
    return nullptr;
  return module.options.safe->find(callee->getName());
}

/// The location a function's own checks report: its first line.
static llvm::DebugLoc functionLocation(const llvm::Function &function) {
  if (llvm::DISubprogram *subprogram = function.getSubprogram())
    return llvm::DILocation::get(function.getContext(), subprogram->getLine(),
                                 0, subprogram);
  return {};
}

//===----------------------------------------------------------------------===//
// Locals placed on the heap (§6)
//===----------------------------------------------------------------------===//

/// Whether the address of `alloca` may outlive its scope.
static bool mayOutlive(const ModuleContext &module, llvm::AllocaInst &alloca,
                       Scopes &scopes) {
  llvm::SmallVector<const llvm::Value *, 16> work{&alloca};
  llvm::SmallPtrSet<const llvm::Value *, 16> seen{&alloca};
  auto derive = [&](const llvm::Value *value) {
    if (seen.insert(value).second)
      work.push_back(value);
  };
  while (!work.empty()) {
    const llvm::Value *value = work.pop_back_val();
    for (const llvm::Use &use : value->uses()) {
      const auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
      if (user == nullptr)
        return true;
      if (const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user)) {
        if (intrinsic->isLifetimeStartOrEnd() ||
            intrinsic->isDebugOrPseudoInst() ||
            llvm::isa<llvm::VAStartInst, llvm::VAEndInst, llvm::VACopyInst>(
                intrinsic))
          continue;
        // Queries that keep nothing (fortification's object size).
        switch (intrinsic->getIntrinsicID()) {
        case llvm::Intrinsic::objectsize:
        case llvm::Intrinsic::is_constant:
        case llvm::Intrinsic::prefetch:
          continue;
        default:
          break;
        }
        if (llvm::isa<llvm::MemIntrinsic>(intrinsic)) {
          if (scopes.mayBeOutOfScope(alloca, *user))
            return true;
          continue;
        }
        return true;
      }
      // Address arithmetic is not a use: what uses its result is.
      if (llvm::isa<llvm::GetElementPtrInst, llvm::PHINode, llvm::SelectInst,
                    llvm::AddrSpaceCastInst>(user)) {
        derive(user);
        continue;
      }
      if (scopes.mayBeOutOfScope(alloca, *user))
        return true;
      if (llvm::isa<llvm::LoadInst, llvm::ICmpInst>(user))
        continue;
      if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
        if (store->getValueOperand() == value)
          return true;
        continue;
      }
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(user)) {
        if (call->isCallee(&use))
          return true;
        const unsigned index = call->getArgOperandNo(&use);
        const FunctionBounds *table = calleeBounds(module, *call);
        if (table != nullptr && index < table->parameters.size()) {
          const PointerBounds &parameter = table->parameters[index];
          if (parameter.escapes)
            return true;
          if (parameter.lifetimeBound)
            derive(call);
        }
        continue;
      }
      // A return, ptrtoint, an atomic operation's operand and the rest.
      return true;
    }
  }
  return false;
}

/// The name a remark gives a local.
static std::string localName(const llvm::AllocaInst &alloca) {
  if (alloca.hasName())
    return alloca.getName().str();
  return "a local";
}

/// Replaces `alloca` by a block of the runtime's heap for each of its
/// scopes, released when the scope (or the function) ends.
static void placeOnHeap(ModuleContext &module, llvm::Function &function,
                        llvm::AllocaInst &alloca) {
  llvm::LLVMContext &context = module.context;
  const std::uint64_t alignment = alloca.getAlign().value();
  llvm::FunctionCallee allocate = module.module.getOrInsertFunction(
      "calloc", module.ptr, module.i64, module.i64);
  llvm::FunctionCallee alignedAllocate = module.module.getOrInsertFunction(
      "aligned_alloc", module.ptr, module.i64, module.i64);
  llvm::FunctionCallee release = module.module.getOrInsertFunction(
      "free", llvm::Type::getVoidTy(context), module.ptr);

  llvm::IRBuilder<> entry(&*function.getEntryBlock().getFirstInsertionPt());
  llvm::AllocaInst *slot =
      entry.CreateAlloca(module.ptr, nullptr, alloca.getName() + ".heap");
  entry.SetInsertPoint(alloca.getNextNode());
  entry.CreateStore(llvm::ConstantPointerNull::get(module.ptr), slot);

  // The bytes, computed where the block is allocated.
  auto bytesAt = [&](llvm::IRBuilder<> &builder) -> llvm::Value * {
    const std::uint64_t element =
        module.layout.getTypeAllocSize(alloca.getAllocatedType())
            .getFixedValue();
    llvm::Value *count =
        builder.CreateZExtOrTrunc(alloca.getArraySize(), module.i64);
    llvm::Value *bytes =
        builder.CreateMul(count, llvm::ConstantInt::get(module.i64, element));
    // At least one byte: a zero-sized local still has an address.
    return builder.CreateBinaryIntrinsic(llvm::Intrinsic::umax, bytes,
                                         llvm::ConstantInt::get(module.i64, 1));
  };
  auto allocateAt = [&](llvm::IRBuilder<> &builder) {
    llvm::Value *bytes = bytesAt(builder);
    llvm::Value *block = nullptr;
    if (alignment <= 16) {
      block = builder.CreateCall(
          allocate, {llvm::ConstantInt::get(module.i64, 1), bytes});
    } else {
      auto *const align = llvm::ConstantInt::get(module.i64, alignment);
      llvm::Value *rounded = builder.CreateMul(
          builder.CreateUDiv(
              builder.CreateAdd(
                  bytes, llvm::ConstantInt::get(module.i64, alignment - 1)),
              align),
          align);
      block = builder.CreateCall(alignedAllocate, {align, rounded});
      builder.CreateMemSet(block, llvm::ConstantInt::get(module.i8, 0), rounded,
                           llvm::MaybeAlign(alignment));
    }
    builder.CreateStore(block, slot);
  };
  auto releaseAt = [&](llvm::IRBuilder<> &builder) {
    builder.CreateCall(release, {builder.CreateLoad(module.ptr, slot)});
    builder.CreateStore(llvm::ConstantPointerNull::get(module.ptr), slot);
  };

  std::vector<llvm::IntrinsicInst *> lifetimes;
  std::vector<llvm::Use *> uses;
  for (llvm::Use &use : alloca.uses()) {
    if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(use.getUser());
        intrinsic != nullptr && intrinsic->isLifetimeStartOrEnd())
      lifetimes.push_back(intrinsic);
    else
      uses.push_back(&use);
  }
  const bool scoped = llvm::any_of(lifetimes, [](llvm::IntrinsicInst *i) {
    return i->getIntrinsicID() == llvm::Intrinsic::lifetime_start;
  });
  if (!scoped)
    allocateAt(entry);
  for (llvm::IntrinsicInst *lifetime : lifetimes) {
    llvm::IRBuilder<> builder(lifetime);
    if (lifetime->getIntrinsicID() == llvm::Intrinsic::lifetime_start)
      allocateAt(builder);
    else
      releaseAt(builder);
  }
  for (llvm::BasicBlock &block : function)
    if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
      llvm::IRBuilder<> builder(block.getTerminator());
      releaseAt(builder);
    }
  for (llvm::Use *use : uses) {
    auto *user = llvm::cast<llvm::Instruction>(use->getUser());
    llvm::Instruction *before = user;
    if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(user))
      before = phi->getIncomingBlock(*use)->getTerminator();
    llvm::IRBuilder<> builder(before);
    use->set(builder.CreateLoad(module.ptr, slot, alloca.getName() + ".at"));
  }
  for (llvm::IntrinsicInst *lifetime : lifetimes)
    lifetime->eraseFromParent();

  const llvm::Instruction *located = &alloca;
  for (const llvm::User *user : slot->users())
    if (const auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
        instruction != nullptr && instruction->getDebugLoc()) {
      located = instruction;
      break;
    }
  llvm::OptimizationRemark remark("weavec-safe", "HeapLocal", located);
  remark << "local '" << localName(alloca)
         << "' is placed on the heap: its address may outlive its scope";
  context.diagnose(remark);
  alloca.eraseFromParent();
}

void prepareSafe(ModuleContext &module, llvm::Function &function) {
  {
    llvm::DominatorTree tree(function);
    std::vector<llvm::AllocaInst *> promotable;
    for (llvm::Instruction &instruction : function.getEntryBlock())
      if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
          alloca != nullptr && llvm::isAllocaPromotable(alloca))
        promotable.push_back(alloca);
    if (!promotable.empty())
      llvm::PromoteMemToReg(promotable, tree);
  }
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction))
        gep->setNoWrapFlags(llvm::GEPNoWrapFlags::none());
  Scopes scopes;
  std::vector<llvm::AllocaInst *> outliving;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
          alloca != nullptr && mayOutlive(module, *alloca, scopes))
        outliving.push_back(alloca);
  for (llvm::AllocaInst *alloca : outliving)
    placeOnHeap(module, function, *alloca);
}

//===----------------------------------------------------------------------===//
// Bounds (§4, §5)
//===----------------------------------------------------------------------===//

namespace {

/// [lo, hi) of a pointer value.
struct Bounds {
  llvm::Value *lo = nullptr;
  llvm::Value *hi = nullptr;
};

class SafeBounds {
public:
  SafeBounds(ModuleContext &module, llvm::Function &function)
      : module(module), function(function),
        table(module.options.safe->find(function.getName())),
        location(functionLocation(function)) {}

  void run() {
    // The guards GuardInsert placed; the validations below are guards of
    // their own and stay.
    std::vector<llvm::CallInst *> markers;
    for (llvm::BasicBlock &block : function)
      for (llvm::Instruction &instruction : block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          const auto marker = ModuleContext::markerOf(*call);
          if (marker == MarkerGuard || marker == MarkerRange)
            markers.push_back(call);
        }
    validateParameters();
    for (llvm::CallInst *marker : markers)
      bound(*marker);
  }

private:
  ModuleContext &module;
  llvm::Function &function;
  const FunctionBounds *table;
  llvm::DebugLoc location;
  llvm::DenseMap<const llvm::Value *, Bounds> known;
  llvm::DenseMap<const llvm::Value *, bool> rootable;

  llvm::Instruction *entryPoint() {
    llvm::BasicBlock &entry = function.getEntryBlock();
    llvm::BasicBlock::iterator at = entry.getFirstInsertionPt();
    while (llvm::isa<llvm::AllocaInst>(*at))
      ++at;
    return &*at;
  }

  llvm::Value *countOf(const PointerBounds &bounds, llvm::IRBuilder<> &builder,
                       llvm::ArrayRef<llvm::Value *> arguments) const {
    llvm::Value *count = llvm::ConstantInt::get(module.i64, bounds.constant);
    if (bounds.count >= 0 &&
        static_cast<std::size_t>(bounds.count) < arguments.size()) {
      llvm::Value *value = arguments[static_cast<std::size_t>(bounds.count)];
      if (!value->getType()->isIntegerTy())
        return nullptr;
      // A negative signed count is an empty range.
      count = bounds.countSigned
                  ? builder.CreateBinaryIntrinsic(
                        llvm::Intrinsic::smax,
                        builder.CreateSExtOrTrunc(value, module.i64),
                        llvm::ConstantInt::get(module.i64, 0))
                  : builder.CreateZExtOrTrunc(value, module.i64);
    }
    return builder.CreateMul(
        count, llvm::ConstantInt::get(module.i64, bounds.elementSize));
  }

  /// The bytes `bounds` gives `pointer`, with `arguments` the values of the
  /// parameters it names; null when it gives none.
  llvm::Value *bytesOf(const PointerBounds &bounds, llvm::Value *pointer,
                       llvm::IRBuilder<> &builder,
                       llvm::ArrayRef<llvm::Value *> arguments) {
    llvm::Value *bytes = nullptr;
    if (bounds.kind == PointerBounds::Kind::Counted) {
      bytes = countOf(bounds, builder, arguments);
    } else if (bounds.kind == PointerBounds::Kind::Ended &&
               static_cast<std::size_t>(bounds.end) < arguments.size()) {
      llvm::Value *end = builder.CreatePtrToInt(
          arguments[static_cast<std::size_t>(bounds.end)], module.i64);
      llvm::Value *start = builder.CreatePtrToInt(pointer, module.i64);
      // An end before the start is an empty range.
      bytes = builder.CreateSelect(builder.CreateICmpULT(end, start),
                                   llvm::ConstantInt::get(module.i64, 0),
                                   builder.CreateSub(end, start));
    }
    if (bytes != nullptr && bounds.orNull)
      bytes =
          builder.CreateSelect(builder.CreateIsNull(pointer),
                               llvm::ConstantInt::get(module.i64, 0), bytes);
    return bytes;
  }

  /// §5: a bounded parameter's bytes are checked against the shadow at
  /// entry, an escaping one against the stack.
  void validateParameters() {
    if (table == nullptr)
      return;
    llvm::SmallVector<llvm::Value *, 8> arguments;
    for (llvm::Argument &argument : function.args())
      arguments.push_back(&argument);
    llvm::IRBuilder<> builder(entryPoint());
    builder.SetCurrentDebugLocation(location);
    for (std::size_t i = 0;
         i < table->parameters.size() && i < arguments.size(); ++i) {
      const PointerBounds &bounds = table->parameters[i];
      llvm::Value *pointer = arguments[i];
      if (!pointer->getType()->isPointerTy())
        continue;
      if (bounds.escapes)
        builder.CreateCall(module.escapeCheckFunction(),
                           {builder.CreatePtrToInt(pointer, module.i64),
                            module.site(location, 0)});
      // A string's accesses keep their guards (amendment 13): scanning it
      // at each entry made string helpers slower than guarded code.
      if (bounds.kind == PointerBounds::Kind::NullTerminated)
        continue;
      llvm::Value *bytes = bytesOf(bounds, pointer, builder, arguments);
      if (bytes == nullptr)
        continue;
      const unsigned row = module.row(function, location, "entry", 0,
                                      LedgerRow::Outcome::Guarded, "range");
      module.insertRange(&*builder.GetInsertPoint(), pointer, bytes, 0, row)
          ->setDebugLoc(location);
      known[pointer] = {.lo = pointer,
                        .hi = builder.CreateGEP(module.i8, pointer, bytes)};
    }
  }

  /// The allocation size `call` makes, from its `allocsize` attribute.
  static std::optional<std::pair<unsigned, std::optional<unsigned>>>
  allocationSize(const llvm::CallBase &call) {
    llvm::Attribute size = call.getFnAttr(llvm::Attribute::AllocSize);
    if (!size.isValid())
      return std::nullopt;
    return size.getAllocSizeArgs();
  }

  /// Whether `value` derives from roots with bounds (computed once).
  bool rooted(const llvm::Value *value) {
    if (const auto found = rootable.find(value); found != rootable.end())
      return found->second;
    rootable[value] = true; // a cycle through phis assumes, then checks
    bool result = false;
    if (known.contains(value) || llvm::isa<llvm::ConstantPointerNull>(value)) {
      result = true;
    } else if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
      result = alloca->getAllocatedType()->isSized();
    } else if (llvm::isa<llvm::GlobalVariable>(value)) {
      result = exactSize(value, module.layout).has_value();
    } else if (const auto *gep =
                   llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
      result = rooted(gep->getPointerOperand());
    } else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
      result = llvm::all_of(phi->incoming_values(),
                            [&](const llvm::Value *v) { return rooted(v); });
    } else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
      result =
          rooted(select->getTrueValue()) && rooted(select->getFalseValue());
    } else if (const auto *call = llvm::dyn_cast<llvm::CallBase>(value)) {
      const FunctionBounds *callee = calleeBounds(module, *call);
      result = allocationSize(*call).has_value() ||
               (callee != nullptr &&
                callee->result.kind == PointerBounds::Kind::Counted);
    }
    rootable[value] = result;
    return result;
  }

  /// The bounds of `value`, which `rooted` accepted.
  Bounds boundsOf(llvm::Value *value) {
    if (const auto found = known.find(value); found != known.end())
      return found->second;
    if (llvm::isa<llvm::ConstantPointerNull>(value))
      return {.lo = value, .hi = value};
    if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(value)) {
      const std::uint64_t size = *exactSize(global, module.layout);
      Bounds bounds{
          .lo = global,
          .hi = llvm::ConstantExpr::getGetElementPtr(
              module.i8, global, llvm::ConstantInt::get(module.i64, size))};
      return known[value] = bounds;
    }
    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
      llvm::IRBuilder<> builder(alloca->getNextNode());
      if (alloca->isStaticAlloca())
        builder.SetInsertPoint(entryPoint());
      const std::uint64_t element =
          module.layout.getTypeAllocSize(alloca->getAllocatedType())
              .getFixedValue();
      llvm::Value *bytes = builder.CreateMul(
          builder.CreateZExtOrTrunc(alloca->getArraySize(), module.i64),
          llvm::ConstantInt::get(module.i64, element));
      return known[value] = {.lo = alloca,
                             .hi = builder.CreateGEP(module.i8, alloca, bytes)};
    }
    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
      return known[value] = boundsOf(gep->getPointerOperand());
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
      llvm::IRBuilder<> builder(phi);
      llvm::PHINode *lo =
          builder.CreatePHI(module.ptr, phi->getNumIncomingValues());
      llvm::PHINode *hi =
          builder.CreatePHI(module.ptr, phi->getNumIncomingValues());
      known[value] = {.lo = lo, .hi = hi};
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        const Bounds incoming = boundsOf(phi->getIncomingValue(i));
        lo->addIncoming(incoming.lo, phi->getIncomingBlock(i));
        hi->addIncoming(incoming.hi, phi->getIncomingBlock(i));
      }
      return {.lo = lo, .hi = hi};
    }
    if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
      const Bounds yes = boundsOf(select->getTrueValue());
      const Bounds no = boundsOf(select->getFalseValue());
      llvm::IRBuilder<> builder(select);
      return known[value] = {.lo = builder.CreateSelect(select->getCondition(),
                                                        yes.lo, no.lo),
                             .hi = builder.CreateSelect(select->getCondition(),
                                                        yes.hi, no.hi)};
    }
    auto *call = llvm::cast<llvm::CallBase>(value);
    llvm::IRBuilder<> builder(call->getNextNode());
    llvm::SmallVector<llvm::Value *, 8> arguments(call->args());
    llvm::Value *bytes = nullptr;
    if (const auto size = allocationSize(*call)) {
      bytes = builder.CreateZExtOrTrunc(call->getArgOperand(size->first),
                                        module.i64);
      if (size->second)
        bytes = builder.CreateMul(
            bytes, builder.CreateZExtOrTrunc(call->getArgOperand(*size->second),
                                             module.i64));
      // A failed allocation has no bytes.
      bytes =
          builder.CreateSelect(builder.CreateIsNull(call),
                               llvm::ConstantInt::get(module.i64, 0), bytes);
    } else {
      const FunctionBounds *callee = calleeBounds(module, *call);
      bytes = bytesOf(callee->result, call, builder, arguments);
      if (bytes == nullptr)
        bytes = llvm::ConstantInt::get(module.i64, 0);
      // A function that is not safe is trusted only as far as the shadow.
      if (!callee->safe) {
        builder.SetCurrentDebugLocation(call->getDebugLoc());
        const unsigned row =
            module.row(function, call->getDebugLoc(), "result", 0,
                       LedgerRow::Outcome::Guarded, "range");
        module.insertRange(&*builder.GetInsertPoint(), call, bytes, 0, row)
            ->setDebugLoc(call->getDebugLoc());
      }
    }
    return known[value] = {.lo = call,
                           .hi = builder.CreateGEP(module.i8, call, bytes)};
  }

  /// Replaces the guard `marker` by a check against its pointer's bounds,
  /// when they are known.
  void bound(llvm::CallInst &marker) {
    llvm::Value *pointer = marker.getArgOperand(0);
    if (!rooted(pointer))
      return;
    const Bounds bounds = boundsOf(pointer);
    llvm::IRBuilder<> builder(&marker);
    builder.SetCurrentDebugLocation(marker.getDebugLoc());
    llvm::Value *width = marker.getArgOperand(1);
    // As offsets from the base, one comparison: the access starts before
    // `limit`, the number of offsets a `width` access may start at (0 when
    // the bounds are smaller than the access). The limit is loop-invariant,
    // and an address below the base wraps to a large offset.
    llvm::Value *lo = builder.CreatePtrToInt(bounds.lo, module.i64);
    llvm::Value *size =
        builder.CreateSub(builder.CreatePtrToInt(bounds.hi, module.i64), lo);
    llvm::Value *bytes = builder.CreateZExtOrTrunc(width, module.i64);
    llvm::Value *limit = builder.CreateBinaryIntrinsic(
        llvm::Intrinsic::usub_sat,
        builder.CreateAdd(size, llvm::ConstantInt::get(module.i64, 1)), bytes);
    llvm::Value *offset =
        builder.CreateSub(builder.CreatePtrToInt(pointer, module.i64), lo);
    llvm::Value *inside = builder.CreateICmpULT(offset, limit);
    const auto *flags = llvm::cast<llvm::ConstantInt>(marker.getArgOperand(2));
    const bool write = (flags->getZExtValue() & MarkerWrite) != 0;
    const bool trap = module.options.mode != GuardOptions::Mode::Report;
    llvm::Instruction *fail = llvm::SplitBlockAndInsertIfThen(
        builder.CreateNot(inside), marker.getIterator(), /*Unreachable=*/trap,
        llvm::MDBuilder(module.context).createUnlikelyBranchWeights());
    builder.SetInsertPoint(fail);
    ModuleContext::callSlow(
        builder, module.boundsFailFunction(),
        {builder.CreatePtrToInt(pointer, module.i64),
         builder.CreateZExtOrTrunc(width, module.i64),
         builder.CreatePtrToInt(bounds.lo, module.i64),
         builder.CreatePtrToInt(bounds.hi, module.i64),
         module.site(marker.getDebugLoc(), write ? SiteWrite : 0)});
    if (module.options.safe->full)
      return;
    const unsigned rowArgument =
        ModuleContext::markerOf(marker) == MarkerGuard ? 4 : 3;
    const auto *row =
        llvm::cast<llvm::ConstantInt>(marker.getArgOperand(rowArgument));
    if (module.verify()) {
      marker.setArgOperand(
          2, llvm::ConstantInt::get(module.i32,
                                    flags->getZExtValue() | MarkerProven));
      return;
    }
    module.copy(static_cast<unsigned>(row->getZExtValue()), false, "safe");
    marker.eraseFromParent();
  }
};

} // namespace

void boundSafe(ModuleContext &module, llvm::Function &function) {
  SafeBounds(module, function).run();
}

} // namespace weavec::frontend::guard
