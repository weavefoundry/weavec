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

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

namespace weavec::frontend::guard {

/// Amendment 18: a safe function with validated parameters is split into
/// its body (internal, `weavec-safe-body` naming the public symbol) and an
/// entry with the public symbol that validates them and calls the body.
static constexpr llvm::StringLiteral BodyAttribute = "weavec-safe-body";
static constexpr llvm::StringLiteral EntryAttribute = "weavec-safe-entry";

/// The table's entry for `function`, a body by its public symbol.
static const FunctionBounds *tableOf(const ModuleContext &module,
                                     const llvm::Function &function) {
  if (module.options.safe == nullptr)
    return nullptr;
  if (function.hasFnAttribute(BodyAttribute))
    return module.options.safe->find(
        function.getFnAttribute(BodyAttribute).getValueAsString());
  return module.options.safe->find(function.getName());
}

bool isSafe(const ModuleContext &module, const llvm::Function &function) {
  if (module.options.safe == nullptr || function.hasFnAttribute(EntryAttribute))
    return false;
  return function.hasFnAttribute(BodyAttribute) ||
         module.options.safe->isSafe(function.getName());
}

/// The table's entry for the function `call` calls directly, or null.
static const FunctionBounds *calleeBounds(const ModuleContext &module,
                                          const llvm::CallBase &call) {
  const llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr)
    return nullptr;
  return tableOf(module, *callee);
}

/// Whether a parameter's bytes are checked against the shadow at entry.
static bool validated(const PointerBounds &bounds) {
  return bounds.kind == PointerBounds::Kind::Counted ||
         bounds.kind == PointerBounds::Kind::Ended;
}

/// RFC 0042 §6: in a typed unit, a single pointer of known size and a
/// string are checked at the entry too, so that the body believes them.
static bool typedValidated(const PointerBounds &bounds) {
  return validated(bounds) ||
         (bounds.kind == PointerBounds::Kind::Single &&
          bounds.pointeeSize != 0) ||
         bounds.kind == PointerBounds::Kind::NullTerminated;
}

/// Whether a typed function's callers call its body: it takes or returns a
/// pointer (a typed unit's other units bind to the body by this rule too).
static bool hasTypedBody(const FunctionBounds &table) {
  return table.result.kind != PointerBounds::Kind::None ||
         llvm::any_of(table.parameters, [](const PointerBounds &bounds) {
           return bounds.kind != PointerBounds::Kind::None;
         });
}

/// `count` elements of `element` bytes; in a typed unit saturated, so that
/// a count whose bytes overflow cannot pass a check (RFC 0042 §7: the index
/// form compares the count itself).
static llvm::Value *timesElement(const ModuleContext &module,
                                 llvm::IRBuilder<> &builder, llvm::Value *count,
                                 std::uint64_t element) {
  llvm::Value *size = llvm::ConstantInt::get(module.i64, element);
  if (!module.typed() || element == 1)
    return builder.CreateMul(count, size);
  llvm::Value *product = builder.CreateBinaryIntrinsic(
      llvm::Intrinsic::umul_with_overflow, count, size);
  return builder.CreateSelect(builder.CreateExtractValue(product, 1),
                              llvm::Constant::getAllOnesValue(module.i64),
                              builder.CreateExtractValue(product, 0));
}

/// The count `bounds` gives (a counted pointer's), as an i64, a negative
/// signed count 0; null when it gives none.
static llvm::Value *boundCount(const ModuleContext &module,
                               const PointerBounds &bounds,
                               llvm::IRBuilder<> &builder,
                               llvm::ArrayRef<llvm::Value *> arguments) {
  if (bounds.kind != PointerBounds::Kind::Counted)
    return nullptr;
  if (bounds.count < 0 ||
      static_cast<std::size_t>(bounds.count) >= arguments.size())
    return llvm::ConstantInt::get(module.i64, bounds.constant);
  llvm::Value *value = arguments[static_cast<std::size_t>(bounds.count)];
  if (!value->getType()->isIntegerTy())
    return nullptr;
  return bounds.countSigned ? builder.CreateBinaryIntrinsic(
                                  llvm::Intrinsic::smax,
                                  builder.CreateSExtOrTrunc(value, module.i64),
                                  llvm::ConstantInt::get(module.i64, 0))
                            : builder.CreateZExtOrTrunc(value, module.i64);
}

/// The bytes `bounds` gives `pointer`, with `arguments` the values of the
/// parameters it names; null when it gives none.
static llvm::Value *boundBytes(const ModuleContext &module,
                               const PointerBounds &bounds,
                               llvm::Value *pointer, llvm::IRBuilder<> &builder,
                               llvm::ArrayRef<llvm::Value *> arguments) {
  llvm::Value *bytes = nullptr;
  if (bounds.kind == PointerBounds::Kind::Counted) {
    // A negative signed count is an empty range.
    llvm::Value *count = boundCount(module, bounds, builder, arguments);
    if (count == nullptr)
      return nullptr;
    bytes = timesElement(module, builder, count, bounds.elementSize);
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
    bytes = builder.CreateSelect(builder.CreateIsNull(pointer),
                                 llvm::ConstantInt::get(module.i64, 0), bytes);
  return bytes;
}

/// The location a function's own checks report: its first line.
static llvm::DebugLoc functionLocation(const llvm::Function &function) {
  if (llvm::DISubprogram *subprogram = function.getSubprogram())
    return llvm::DILocation::get(function.getContext(), subprogram->getLine(),
                                 0, subprogram);
  return {};
}

/// The first instruction at or after `at` that is not an alloca: code
/// inserted there keeps a block's allocas together and follows them all.
static llvm::Instruction *afterAllocas(llvm::Instruction *at) {
  while (llvm::isa<llvm::AllocaInst>(at))
    at = at->getNextNode();
  return at;
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
      // A pointer difference or comparison as integers: a safe function
      // cannot make the integer a pointer again (`int-to-pointer`).
      if (llvm::isa<llvm::PtrToIntInst>(user) &&
          llvm::all_of(user->users(), [](const llvm::User *inner) {
            return llvm::isa<llvm::BinaryOperator, llvm::ICmpInst>(inner);
          }))
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
  // Null from the function's start, after its allocas: every return
  // releases the slot, and a path may return without reaching the local.
  entry.SetInsertPoint(afterAllocas(slot));
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
  if (!scoped && alloca.isStaticAlloca()) {
    allocateAt(entry);
  } else if (!scoped) {
    // A dynamic local (an array of variable length) has no lifetime
    // markers: its block lives until the local is allocated again (a loop's
    // next iteration) or the function returns.
    llvm::IRBuilder<> builder(alloca.getNextNode());
    releaseAt(builder);
    allocateAt(builder);
  }
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

/// Amendment 18: the public symbol of a safe function with validated
/// parameters becomes an entry that checks them against the shadow and
/// calls the body, which keeps the definition (internal). Every use of the
/// function in the unit (calls from guarded code, its address) now names the
/// entry; a safe caller with bounds for the arguments calls the body
/// (SafeBounds::callBody).
static void splitEntry(ModuleContext &module, llvm::Function &function) {
  const FunctionBounds *table = tableOf(module, function);
  const bool typed = isTyped(module, function);
  if (table == nullptr || function.isVarArg() ||
      !(function.hasExternalLinkage() || function.hasLocalLinkage()) ||
      (typed ? !hasTypedBody(*table)
             : llvm::none_of(table->parameters, validated)))
    return;
  // RFC 0042 §6: a typed unit's body is external with its function, so
  // that typed callers in other units call it.
  const bool external = typed && function.hasExternalLinkage();
  const std::string name = function.getName().str();
  function.setName(name + ".weavec.safe");
  llvm::Function *entry =
      llvm::Function::Create(function.getFunctionType(), function.getLinkage(),
                             function.getAddressSpace(), name, &module.module);
  entry->copyAttributesFrom(&function);
  function.replaceAllUsesWith(entry);
  if (!external) {
    function.setLinkage(llvm::GlobalValue::InternalLinkage);
    function.setVisibility(llvm::GlobalValue::DefaultVisibility);
    function.setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
  }
  function.addFnAttr(BodyAttribute, name);
  entry->addFnAttr(EntryAttribute);
  if (const llvm::DISubprogram *subprogram = function.getSubprogram()) {
    llvm::DIBuilder debug(module.module, /*AllowUnresolved=*/false,
                          subprogram->getUnit());
    llvm::DISubprogram *own = debug.createFunction(
        subprogram->getScope(), name, subprogram->getLinkageName(),
        subprogram->getFile(), subprogram->getLine(), subprogram->getType(),
        subprogram->getScopeLine(), llvm::DINode::FlagArtificial,
        subprogram->getSPFlags());
    entry->setSubprogram(own);
    debug.finalizeSubprogram(own);
  }
  const llvm::DebugLoc location = functionLocation(*entry);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(module.context, "entry", entry));
  builder.SetCurrentDebugLocation(location);
  llvm::SmallVector<llvm::Value *, 8> arguments;
  for (llvm::Argument &argument : entry->args())
    arguments.push_back(&argument);
  llvm::CallInst *call = builder.CreateCall(&function, arguments);
  call->setCallingConv(function.getCallingConv());
  call->setAttributes(function.getAttributes());
  if (call->getType()->isVoidTy())
    builder.CreateRetVoid();
  else
    builder.CreateRet(call);
  builder.SetInsertPoint(call);
  for (std::size_t i = 0; i < table->parameters.size() && i < arguments.size();
       ++i) {
    const PointerBounds &parameter = table->parameters[i];
    if (!(typed ? typedValidated(parameter) : validated(parameter)) ||
        !arguments[i]->getType()->isPointerTy())
      continue;
    // RFC 0042 §6: a string is scanned against the shadow once, at the
    // entry from code that is not typed.
    if (parameter.kind == PointerBounds::Kind::NullTerminated) {
      const unsigned row = module.row(*entry, location, "entry", 0,
                                      LedgerRow::Outcome::Guarded, "string");
      module.insertString(builder, arguments[i],
                          llvm::Constant::getAllOnesValue(module.i64), row,
                          parameter.orNull ? MarkerNullOk : 0);
      continue;
    }
    llvm::Value *bytes =
        parameter.kind == PointerBounds::Kind::Single
            ? builder.CreateSelect(
                  builder.CreateIsNull(arguments[i]),
                  llvm::ConstantInt::get(module.i64, 0),
                  llvm::ConstantInt::get(module.i64, parameter.pointeeSize))
            : boundBytes(module, parameter, arguments[i], builder, arguments);
    if (bytes == nullptr)
      continue;
    const unsigned row = module.row(*entry, location, "entry", 0,
                                    LedgerRow::Outcome::Guarded, "range");
    module.insertRange(call, arguments[i], bytes, 0, row)
        ->setDebugLoc(location);
  }
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
  if (isTyped(module, function))
    prepareTyped(module, function);
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
  splitEntry(module, function);
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

/// RFC 0042 §4: a root of a size the compiler knows. An access at a
/// constant offset inside it needs no check; inside its first 4 KiB when the
/// root may be null, where a null pointer's access faults.
struct ConstantSize {
  std::uint64_t bytes = 0;
  bool mayBeNull = false;
};

/// RFC 0042 §7: a counted root's count, for the comparison's index form.
struct RootCount {
  llvm::Value *count = nullptr;
  std::uint64_t elementSize = 0;
};

/// A null pointer's accesses below this fault (RFC 0037 §6).
constexpr std::uint64_t NullPage = 4096;

class SafeBounds {
public:
  SafeBounds(ModuleContext &module, llvm::Function &function)
      : module(module), function(function), table(tableOf(module, function)),
        location(functionLocation(function)), typed(isTyped(module, function)) {
  }

  void run() {
    // The guards GuardInsert placed; the validations below are guards of
    // their own and stay.
    std::vector<llvm::CallInst *> markers;
    std::vector<llvm::CallInst *> strings;
    std::vector<llvm::CallInst *> calls;
    std::vector<llvm::StoreInst *> stores;
    std::vector<llvm::ReturnInst *> returns;
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
        const auto marker = ModuleContext::markerOf(*call);
        const llvm::Function *callee = call->getCalledFunction();
        if (marker == MarkerGuard || marker == MarkerRange)
          markers.push_back(call);
        else if (marker == MarkerString)
          strings.push_back(call);
        else if (callee != nullptr && (callee->hasFnAttribute(EntryAttribute) ||
                                       (typed && typedElsewhere(*callee))))
          calls.push_back(call);
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
        stores.push_back(store);
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
        returns.push_back(ret);
    }
    validateParameters();
    // Before the checks below change the blocks: the strings, and the
    // bounds of each counted pointer loaded from a field, built beside its
    // count (unused ones are dead code).
    if (typed) {
      findStrings(markers);
      std::vector<llvm::Instruction *> loads;
      for (llvm::Instruction &instruction : llvm::instructions(function))
        if (boundedField(&instruction) != nullptr)
          loads.push_back(&instruction);
      for (llvm::Instruction *load : loads)
        (void)boundsOf(load);
    }
    for (llvm::CallInst *marker : markers)
      bound(*marker);
    if (typed) {
      for (llvm::CallInst *marker : strings)
        boundString(*marker);
      for (llvm::StoreInst *store : stores)
        convertStore(*store);
      for (llvm::ReturnInst *ret : returns)
        convertReturn(*ret);
    }
    for (llvm::CallInst *call : calls)
      callBody(*call);
  }

private:
  ModuleContext &module;
  llvm::Function &function;
  const FunctionBounds *table;
  llvm::DebugLoc location;
  bool typed;
  llvm::DenseMap<const llvm::Value *, Bounds> known;
  llvm::DenseMap<const llvm::Value *, bool> rootable;
  llvm::DenseMap<const llvm::Value *, ConstantSize> constantSizes;
  llvm::DenseMap<const llvm::Value *, RootCount> counts;
  /// RFC 0042 §4, §8: the values that point into a string at or before its
  /// terminator wherever they are used, and the string roots.
  llvm::DenseSet<const llvm::Value *> stringValues;
  llvm::DenseSet<const llvm::Value *> stringRoots;
  /// Accesses through a string that `strlen` measured where they are.
  llvm::DenseMap<const llvm::CallInst *, llvm::CallInst *> measured;
  /// The branches on an `i1` phi (a `&&` or `||`).
  llvm::SmallVector<const llvm::CondBrInst *, 8> phiBranches;

  llvm::Instruction *entryPoint() {
    return afterAllocas(&*function.getEntryBlock().getFirstInsertionPt());
  }

  llvm::Value *bytesOf(const PointerBounds &bounds, llvm::Value *pointer,
                       llvm::IRBuilder<> &builder,
                       llvm::ArrayRef<llvm::Value *> arguments) {
    return boundBytes(module, bounds, pointer, builder, arguments);
  }

  /// RFC 0042: whether `callee` is typed, so that its result keeps the
  /// bound it declares: a typed function of the unit, its entry or body, or
  /// another typed unit's body.
  bool typedCallee(const llvm::Function &callee) const {
    if (!module.typed())
      return false;
    if (isTyped(module, callee) || callee.hasFnAttribute(EntryAttribute))
      return true;
    return callee.isDeclaration() && callee.getName().ends_with(".weavec.safe");
  }

  /// RFC 0042 §6: a function this unit declares `WEAVEC_SAFE` and does not
  /// define, whose callers call its body in its own typed unit.
  bool typedElsewhere(const llvm::Function &callee) const {
    if (!callee.isDeclaration() || callee.isVarArg())
      return false;
    const FunctionBounds *bounds = module.options.safe->find(callee.getName());
    return bounds != nullptr && bounds->declaredTyped && hasTypedBody(*bounds);
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
    const bool body = function.hasFnAttribute(BodyAttribute);
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
      // RFC 0042: a typed body believes what its entry or its typed
      // callers checked: a single pointer's object, a string.
      if (typed && body && bounds.kind == PointerBounds::Kind::Single &&
          bounds.pointeeSize != 0) {
        known[pointer] = singleBounds(builder, pointer, bounds.pointeeSize);
        continue;
      }
      if (typed && body && bounds.kind == PointerBounds::Kind::NullTerminated) {
        stringRoots.insert(pointer);
        continue;
      }
      // A string's accesses keep their guards (amendment 13): scanning it
      // at each entry made string helpers slower than guarded code.
      if (bounds.kind == PointerBounds::Kind::NullTerminated)
        continue;
      llvm::Value *bytes = bytesOf(bounds, pointer, builder, arguments);
      if (bytes == nullptr)
        continue;
      if (typed)
        if (llvm::Value *count = boundCount(module, bounds, builder, arguments))
          counts[pointer] = {.count = count, .elementSize = bounds.elementSize};
      // A body's callers checked its parameters (its entry, or a safe
      // caller at the call).
      if (body) {
        known[pointer] = {.lo = pointer,
                          .hi = builder.CreateGEP(module.i8, pointer, bytes)};
        continue;
      }
      const unsigned row = module.row(function, location, "entry", 0,
                                      LedgerRow::Outcome::Guarded, "range");
      module.insertRange(&*builder.GetInsertPoint(), pointer, bytes, 0, row)
          ->setDebugLoc(location);
      known[pointer] = {.lo = pointer,
                        .hi = builder.CreateGEP(module.i8, pointer, bytes)};
    }
  }

  /// The bounds of a single pointer to `size` bytes, which may be null: a
  /// null one's are empty when an offset past the null page fits inside.
  Bounds singleBounds(llvm::IRBuilder<> &builder, llvm::Value *pointer,
                      std::uint64_t size) {
    constantSizes[pointer] = {.bytes = size, .mayBeNull = true};
    llvm::Value *hi = builder.CreateGEP(
        module.i8, pointer, llvm::ConstantInt::get(module.i64, size));
    if (size > NullPage)
      hi = builder.CreateSelect(builder.CreateIsNull(pointer), pointer, hi);
    return {.lo = pointer, .hi = hi};
  }

  /// A global's size: one the unit defines; in a typed function also an
  /// extern declared with its size (C makes the definition agree).
  std::optional<std::uint64_t> globalSize(const llvm::Value *value) const {
    if (auto size = exactSize(value, module.layout))
      return size;
    const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(value);
    if (!typed || global == nullptr || !global->isDeclaration() ||
        !global->getValueType()->isSized())
      return std::nullopt;
    const std::uint64_t size =
        module.layout.getTypeAllocSize(global->getValueType()).getFixedValue();
    return size != 0 ? std::optional(size) : std::nullopt;
  }

  /// The allocation size `call` makes, from its `allocsize` attribute.
  static std::optional<std::pair<unsigned, std::optional<unsigned>>>
  allocationSize(const llvm::CallBase &call) {
    llvm::Attribute size = call.getFnAttr(llvm::Attribute::AllocSize);
    if (!size.isValid())
      return std::nullopt;
    return size.getAllocSizeArgs();
  }

  /// RFC 0042 §5: the field a typed function's pointer load reads, when it
  /// gives the pointer bounds.
  const TypedField *boundedField(const llvm::Value *value) const {
    const auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
    if (!typed || load == nullptr || !load->getType()->isPointerTy())
      return nullptr;
    const TypedField *field = typedFieldOf(module, load->getPointerOperand());
    if (field == nullptr || field->count)
      return nullptr;
    if (field->field.kind == FieldBounds::Kind::Single)
      return field->field.elementSize > 0 ? field : nullptr;
    if (field->field.kind == FieldBounds::Kind::Counted &&
        module.typedState.countAt.contains(load))
      return field;
    return nullptr;
  }

  /// Whether `value` has bounds of its own: a validated parameter, a local,
  /// a global the unit defines, an allocation, or a call whose result the
  /// table bounds; in a typed function also a pointer loaded from a field
  /// whose declaration bounds it, and a call's single result. A call's
  /// bounds are computed after it, which an invoke has no place for in its
  /// block.
  bool isRoot(const llvm::Value *value) {
    // (An undefined value is an uninitialized local's, on a path that does
    // not use it.)
    if (known.contains(value) ||
        llvm::isa<llvm::ConstantPointerNull, llvm::UndefValue>(value))
      return true;
    if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value))
      return alloca->getAllocatedType()->isSized();
    if (llvm::isa<llvm::GlobalVariable>(value))
      return globalSize(value).has_value();
    if (boundedField(value) != nullptr)
      return true;
    if (const auto *call = llvm::dyn_cast<llvm::CallInst>(value)) {
      const FunctionBounds *callee = calleeBounds(module, *call);
      if (allocationSize(*call).has_value())
        return true;
      if (callee == nullptr)
        return false;
      return callee->result.kind == PointerBounds::Kind::Counted ||
             (typed && callee->result.kind == PointerBounds::Kind::Single &&
              callee->result.pointeeSize != 0);
    }
    return false;
  }

  /// What `value` derives from through arithmetic, phis and selects; empty
  /// for anything else.
  static llvm::SmallVector<const llvm::Value *, 4>
  derivedFrom(const llvm::Value *value) {
    llvm::SmallVector<const llvm::Value *, 4> from;
    if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
      from.push_back(gep->getPointerOperand());
    else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value))
      from.append(phi->value_op_begin(), phi->value_op_end());
    else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value))
      from.append({select->getTrueValue(), select->getFalseValue()});
    return from;
  }

  static bool isDerived(const llvm::Value *value) {
    // An element of a flexible array member lies past its struct's size:
    // the struct's bounds do not cover it (it keeps its guard).
    if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
      for (auto type = llvm::gep_type_begin(gep);
           type != llvm::gep_type_end(gep); ++type)
        if (const auto *array =
                llvm::dyn_cast<llvm::ArrayType>(type.getIndexedType());
            array != nullptr && array->getNumElements() == 0)
          return false;
    return llvm::isa<llvm::GetElementPtrInst, llvm::PHINode, llvm::SelectInst>(
        value);
  }

  /// Whether `value` derives only from roots (isRoot). Decided for all the
  /// values it derives from at once, as the greatest fixpoint over their
  /// cycles, so that no answer rests on an assumption another refutes.
  bool rooted(const llvm::Value *value) {
    if (const auto found = rootable.find(value); found != rootable.end())
      return found->second;
    llvm::SmallVector<const llvm::Value *, 16> closure;
    llvm::SmallVector<const llvm::Value *, 16> work{value};
    llvm::SmallPtrSet<const llvm::Value *, 16> seen{value};
    while (!work.empty()) {
      const llvm::Value *next = work.pop_back_val();
      closure.push_back(next);
      if (isRoot(next))
        continue;
      for (const llvm::Value *from : derivedFrom(next))
        if (!rootable.contains(from) && seen.insert(from).second)
          work.push_back(from);
    }
    for (const llvm::Value *member : closure)
      rootable[member] = isRoot(member) || isDerived(member);
    for (bool changed = true; changed;) {
      changed = false;
      for (const llvm::Value *member : closure)
        if (rootable.lookup(member) && !isRoot(member) &&
            !llvm::all_of(derivedFrom(member), [&](const llvm::Value *from) {
              return rootable.lookup(from);
            })) {
          rootable[member] = false;
          changed = true;
        }
    }
    return rootable.lookup(value);
  }

  /// The bounds of `value`, which `rooted` accepted: built from the roots
  /// (isRoot) through what it derives from.
  Bounds boundsOf(llvm::Value *value) {
    if (const auto found = known.find(value); found != known.end())
      return found->second;
    if (llvm::isa<llvm::ConstantPointerNull, llvm::UndefValue>(value)) {
      llvm::Value *null = llvm::ConstantPointerNull::get(module.ptr);
      return {.lo = null, .hi = null};
    }
    if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(value)) {
      const std::uint64_t size = *globalSize(global);
      constantSizes[value] = {.bytes = size, .mayBeNull = false};
      Bounds bounds{
          .lo = global,
          .hi = llvm::ConstantExpr::getGetElementPtr(
              module.i8, global, llvm::ConstantInt::get(module.i64, size))};
      return known[value] = bounds;
    }
    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
      // After the run of allocas it is in, so that they stay together.
      llvm::IRBuilder<> builder(afterAllocas(alloca->getNextNode()));
      const std::uint64_t element =
          module.layout.getTypeAllocSize(alloca->getAllocatedType())
              .getFixedValue();
      if (const auto *constant =
              llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize()))
        constantSizes[value] = {.bytes = element * constant->getZExtValue(),
                                .mayBeNull = false};
      llvm::Value *bytes = builder.CreateMul(
          builder.CreateZExtOrTrunc(alloca->getArraySize(), module.i64),
          llvm::ConstantInt::get(module.i64, element));
      return known[value] = {.lo = alloca,
                             .hi = builder.CreateGEP(module.i8, alloca, bytes)};
    }
    if (const TypedField *field = boundedField(value))
      return known[value] =
                 fieldBounds(*llvm::cast<llvm::LoadInst>(value), field->field);
    if (!isRoot(value)) {
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
        return known[value] = {.lo = builder.CreateSelect(
                                   select->getCondition(), yes.lo, no.lo),
                               .hi = builder.CreateSelect(
                                   select->getCondition(), yes.hi, no.hi)};
      }
    }
    auto *call = llvm::cast<llvm::CallInst>(value);
    llvm::IRBuilder<> builder(call->getNextNode());
    llvm::SmallVector<llvm::Value *, 8> arguments(call->args());
    llvm::Value *bytes = nullptr;
    const FunctionBounds *callee = calleeBounds(module, *call);
    const bool trusted = call->getCalledFunction() != nullptr &&
                         typedCallee(*call->getCalledFunction());
    if (const auto size = allocationSize(*call)) {
      bytes = builder.CreateZExtOrTrunc(call->getArgOperand(size->first),
                                        module.i64);
      if (size->second)
        bytes = builder.CreateMul(
            bytes, builder.CreateZExtOrTrunc(call->getArgOperand(*size->second),
                                             module.i64));
      if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(bytes))
        constantSizes[value] = {.bytes = constant->getZExtValue(),
                                .mayBeNull = true};
      // A failed allocation has no bytes.
      bytes =
          builder.CreateSelect(builder.CreateIsNull(call),
                               llvm::ConstantInt::get(module.i64, 0), bytes);
    } else if (typed && callee->result.kind == PointerBounds::Kind::Single) {
      // RFC 0042 §6: the single result of a function that is not typed is
      // checked against the shadow; a typed one's checked its return.
      builder.SetCurrentDebugLocation(call->getDebugLoc());
      const Bounds bounds =
          singleBounds(builder, call, callee->result.pointeeSize);
      if (!trusted)
        guardResult(
            builder, *call,
            builder.CreateSelect(builder.CreateIsNull(call),
                                 llvm::ConstantInt::get(module.i64, 0),
                                 llvm::ConstantInt::get(
                                     module.i64, callee->result.pointeeSize)));
      return known[value] = bounds;
    } else {
      bytes = bytesOf(callee->result, call, builder, arguments);
      if (bytes == nullptr)
        bytes = llvm::ConstantInt::get(module.i64, 0);
      if (typed)
        if (llvm::Value *count =
                boundCount(module, callee->result, builder, arguments))
          counts[value] = {.count = count,
                           .elementSize = callee->result.elementSize};
      // A declared result bound is checked against the shadow, a safe
      // callee's too: no rule makes a function return what it declares
      // (a typed callee's return is checked, RFC 0042 §6).
      if (!trusted)
        guardResult(builder, *call, bytes);
    }
    return known[value] = {.lo = call,
                           .hi = builder.CreateGEP(module.i8, call, bytes)};
  }

  void guardResult(llvm::IRBuilder<> &builder, llvm::CallInst &call,
                   llvm::Value *bytes) {
    builder.SetCurrentDebugLocation(call.getDebugLoc());
    const unsigned row = module.row(function, call.getDebugLoc(), "result", 0,
                                    LedgerRow::Outcome::Guarded, "range");
    module.insertRange(&*builder.GetInsertPoint(), &call, bytes, 0, row)
        ->setDebugLoc(call.getDebugLoc());
  }

  /// RFC 0042 §5: a pointer loaded from a field, bounded as the field's
  /// declaration says, with the count read beside it; a null one's bounds
  /// are empty.
  Bounds fieldBounds(llvm::LoadInst &load, const FieldBounds &field) {
    if (field.kind == FieldBounds::Kind::Single) {
      llvm::IRBuilder<> builder(load.getNextNode());
      builder.SetCurrentDebugLocation(load.getDebugLoc());
      return singleBounds(builder, &load,
                          static_cast<std::uint64_t>(field.elementSize));
    }
    // After the count, read beside the load or (in a pair) before it.
    llvm::Value *count = module.typedState.countAt.lookup(&load);
    auto *after = llvm::dyn_cast<llvm::Instruction>(count);
    llvm::IRBuilder<> builder(after != nullptr &&
                                      after->getParent() == load.getParent() &&
                                      load.comesBefore(after)
                                  ? after->getNextNode()
                                  : load.getNextNode());
    builder.SetCurrentDebugLocation(load.getDebugLoc());
    if ((field.flags & FieldBounds::CountSigned) != 0)
      count = builder.CreateBinaryIntrinsic(
          llvm::Intrinsic::smax, count, llvm::ConstantInt::get(module.i64, 0));
    count = builder.CreateSelect(builder.CreateIsNull(&load),
                                 llvm::ConstantInt::get(module.i64, 0), count);
    counts[&load] = {.count = count,
                     .elementSize =
                         static_cast<std::uint64_t>(field.elementSize)};
    llvm::Value *bytes = timesElement(
        module, builder, count, static_cast<std::uint64_t>(field.elementSize));
    return {.lo = &load, .hi = builder.CreateGEP(module.i8, &load, bytes)};
  }

  /// RFC 0042 §8: the values that point at or before a string's terminator
  /// wherever they are used: a string root (a parameter, a field, a typed
  /// callee's result), and what is derived from one through phis, selects
  /// and steps forward by a constant, each byte stepped over tested not to
  /// be zero where the step is used (the loop's own test, in `for (; *s;
  /// s++)`). A step whose use no test covers is not one: the accesses
  /// through it keep their guards. Also the accesses that `strlen` of their
  /// string measured.
  void findStrings(llvm::ArrayRef<llvm::CallInst *> markers) {
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        const TypedField *field =
            load->getType()->isPointerTy()
                ? typedFieldOf(module, load->getPointerOperand())
                : nullptr;
        if (field != nullptr && !field->count &&
            field->field.kind == FieldBounds::Kind::NullTerminated)
          stringRoots.insert(load);
      } else if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
        const llvm::Function *callee = call->getCalledFunction();
        const FunctionBounds *bounds = calleeBounds(module, *call);
        if (callee != nullptr && bounds != nullptr && typedCallee(*callee) &&
            bounds->result.kind == PointerBounds::Kind::NullTerminated)
          stringRoots.insert(call);
      }
    }
    if (stringRoots.empty())
      return;
    // Candidates: forward from the roots.
    llvm::SmallVector<const llvm::Value *, 32> work(stringRoots.begin(),
                                                    stringRoots.end());
    llvm::DenseMap<const llvm::Value *, std::uint64_t> steps;
    llvm::SmallVector<const llvm::Value *, 32> candidates;
    llvm::DenseSet<const llvm::Value *> seen(stringRoots.begin(),
                                             stringRoots.end());
    while (!work.empty()) {
      const llvm::Value *value = work.pop_back_val();
      candidates.push_back(value);
      for (const llvm::User *user : value->users()) {
        if (seen.contains(user))
          continue;
        if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
          llvm::APInt offset(64, 0);
          if (gep->getPointerOperand() != value ||
              !gep->accumulateConstantOffset(module.layout, offset) ||
              offset.isNegative() || offset.getZExtValue() > 8)
            continue;
          steps[gep] = offset.getZExtValue();
        } else if (!llvm::isa<llvm::PHINode, llvm::SelectInst>(user)) {
          continue;
        }
        seen.insert(user);
        work.push_back(user);
      }
    }
    llvm::DominatorTree tree(function);
    for (const llvm::BasicBlock &block : function)
      if (const auto *branch =
              llvm::dyn_cast<llvm::CondBrInst>(block.getTerminator());
          branch != nullptr && llvm::isa<llvm::PHINode>(branch->getCondition()))
        phiBranches.push_back(branch);
    for (const llvm::Value *value : candidates)
      stringValues.insert(value);
    // A step is a string position where each use it is put to is covered.
    for (const auto &[value, step] : steps) {
      const auto *gep = llvm::cast<llvm::GetElementPtrInst>(value);
      for (const llvm::Use &use : gep->uses())
        if (!coveredUse(use, gep->getPointerOperand(), step, tree)) {
          stringValues.erase(gep);
          break;
        }
    }
    // Phis and selects of positions only, as the greatest fixpoint.
    for (bool changed = true; changed;) {
      changed = false;
      for (const llvm::Value *value : candidates) {
        if (!stringValues.contains(value) || stringRoots.contains(value))
          continue;
        bool all = true;
        if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value))
          all = stringValues.contains(gep->getPointerOperand());
        else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value))
          all = llvm::all_of(phi->incoming_values(), [&](const llvm::Value *v) {
            return stringValues.contains(v);
          });
        else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value))
          all = stringValues.contains(select->getTrueValue()) &&
                stringValues.contains(select->getFalseValue());
        if (!all) {
          stringValues.erase(value);
          changed = true;
        }
      }
    }
    // `size_t n = strlen(s)`: an access through `s` it dominates is inside
    // [s, s + n + 1).
    const llvm::TargetLibraryInfoImpl library(module.module.getTargetTriple());
    const llvm::TargetLibraryInfo info(library);
    for (const llvm::CallInst *marker : markers) {
      llvm::Value *pointer = marker->getArgOperand(0);
      while (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer))
        pointer = gep->getPointerOperand();
      if (!stringRoots.contains(pointer) ||
          (ModuleContext::markerOf(*marker) == MarkerGuard &&
           stringValues.contains(marker->getArgOperand(0))))
        continue;
      for (llvm::User *user : pointer->users()) {
        // `strlen` itself, or the string marker GuardInsert made of it.
        auto *call = llvm::dyn_cast<llvm::CallInst>(user);
        llvm::LibFunc libraryFunction{};
        const auto *max =
            call != nullptr && ModuleContext::markerOf(*call) == MarkerString
                ? llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1))
                : nullptr;
        const bool length =
            call != nullptr &&
            ((max != nullptr && max->isAllOnesValue()) ||
             (call->getCalledFunction() != nullptr &&
              info.getLibFunc(*call->getCalledFunction(), libraryFunction) &&
              libraryFunction == llvm::LibFunc_strlen));
        if (length && call->getArgOperand(0) == pointer &&
            tree.dominates(call, marker)) {
          measured[marker] = call;
          break;
        }
      }
    }
  }

  /// Whether the byte at `base + at` was tested not to be zero on every
  /// path to `block`: a load of it compared with zero (or with another
  /// constant for equality), or switched on, whose edge dominates `block`.
  bool testedNonzero(const llvm::Value *base, std::uint64_t at,
                     const llvm::BasicBlock *block,
                     const llvm::DominatorTree &tree,
                     const llvm::DataLayout &layout) const {
    llvm::SmallVector<const llvm::LoadInst *, 4> loads;
    for (const llvm::User *user : base->users()) {
      if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(user);
          load != nullptr && at == 0 && load->getPointerOperand() == base)
        loads.push_back(load);
      if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
        llvm::APInt offset(64, 0);
        if (gep->getPointerOperand() == base &&
            gep->accumulateConstantOffset(layout, offset) && offset == at)
          for (const llvm::User *inner : gep->users())
            if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inner))
              loads.push_back(load);
      }
    }
    // The edges taken only when the byte is not zero, and the conditions
    // true only then.
    llvm::SmallVector<llvm::BasicBlockEdge, 4> edges;
    llvm::SmallPtrSet<const llvm::Value *, 4> truths;
    for (const llvm::LoadInst *load : loads) {
      if (!load->getType()->isIntegerTy(8))
        continue;
      llvm::SmallVector<const llvm::Value *, 4> values{load};
      for (const llvm::User *user : load->users())
        if (llvm::isa<llvm::SExtInst, llvm::ZExtInst>(user))
          values.push_back(user);
      for (const llvm::Value *value : values)
        for (const llvm::User *user : value->users()) {
          if (const auto *compare = llvm::dyn_cast<llvm::ICmpInst>(user)) {
            const auto *constant =
                llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(1));
            if (constant == nullptr || compare->getOperand(0) != value)
              continue;
            unsigned successor = 2;
            if (compare->getPredicate() == llvm::ICmpInst::ICMP_NE &&
                constant->isZero())
              successor = 0;
            else if (compare->getPredicate() == llvm::ICmpInst::ICMP_EQ)
              successor = constant->isZero() ? 1 : 0;
            if (successor == 2)
              continue;
            if (successor == 0)
              truths.insert(compare);
            for (const llvm::User *branchUser : compare->users())
              if (const auto *branch =
                      llvm::dyn_cast<llvm::CondBrInst>(branchUser))
                edges.emplace_back(branch->getParent(),
                                   branch->getSuccessor(successor));
          } else if (const auto *switched =
                         llvm::dyn_cast<llvm::SwitchInst>(user)) {
            for (const auto &each : switched->cases())
              if (!each.getCaseValue()->isZero())
                edges.emplace_back(switched->getParent(),
                                   each.getCaseSuccessor());
          }
        }
    }
    auto covered = [&](const llvm::BasicBlock *at) {
      return llvm::any_of(edges, [&](const llvm::BasicBlockEdge &edge) {
        return tree.dominates(edge, at);
      });
    };
    if (covered(block))
      return true;
    // `while (*p && f(*p))`: the branch on the `&&`'s phi is taken only
    // through a test of the byte.
    for (const llvm::CondBrInst *branch : phiBranches) {
      const auto *phi = llvm::cast<llvm::PHINode>(branch->getCondition());
      const bool implied = llvm::all_of(
          llvm::zip(phi->incoming_values(), phi->blocks()),
          [&](const auto &incoming) {
            const llvm::Value *value = std::get<0>(incoming);
            const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value);
            return (constant != nullptr && constant->isZero()) ||
                   truths.contains(value) || covered(std::get<1>(incoming));
          });
      if (implied &&
          tree.dominates(llvm::BasicBlockEdge(branch->getParent(),
                                              branch->getSuccessor(0)),
                         block))
        return true;
    }
    return false;
  }

  /// Whether a use of a step of `step` bytes from `base` is covered: it is
  /// a comparison or an integer, or every byte stepped over was tested
  /// where it is used.
  bool coveredUse(const llvm::Use &use, const llvm::Value *base,
                  std::uint64_t step, const llvm::DominatorTree &tree) const {
    const auto *user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
    if (user == nullptr)
      return false;
    if (llvm::isa<llvm::ICmpInst, llvm::PtrToIntInst>(user) ||
        user->isDebugOrPseudoInst())
      return true;
    const llvm::BasicBlock *block = user->getParent();
    if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(user))
      block = phi->getIncomingBlock(use);
    for (std::uint64_t at = 0; at < step; ++at)
      if (!testedNonzero(base, at, block, tree, module.layout))
        return false;
    return true;
  }

  /// RFC 0042: a library call's string argument that is a string position
  /// or a literal needs no scan of the shadow.
  void boundString(llvm::CallInst &marker) {
    llvm::Value *pointer = marker.getArgOperand(0);
    const auto *flags = llvm::cast<llvm::ConstantInt>(marker.getArgOperand(2));
    if (module.options.safe->full || !stringLike(pointer) ||
        ((flags->getZExtValue() & MarkerNullOk) != 0 && !marker.use_empty()))
      return;
    const auto *row = llvm::cast<llvm::ConstantInt>(marker.getArgOperand(3));
    if (module.verify()) {
      marker.setArgOperand(
          2, llvm::ConstantInt::get(module.i32,
                                    flags->getZExtValue() | MarkerProven));
      return;
    }
    module.copy(static_cast<unsigned>(row->getZExtValue()), false, "typed");
    if (!marker.use_empty()) {
      llvm::IRBuilder<> builder(&marker);
      llvm::FunctionCallee length =
          module.module.getOrInsertFunction("strlen", module.i64, module.ptr);
      marker.replaceAllUsesWith(builder.CreateBinaryIntrinsic(
          llvm::Intrinsic::umin, builder.CreateCall(length, {pointer}),
          marker.getArgOperand(1)));
    }
    marker.eraseFromParent();
  }

  /// A string position, a string literal (or a constant offset into one
  /// before its zero byte), or null.
  bool stringLike(const llvm::Value *value) const {
    if (stringValues.contains(value) ||
        llvm::isa<llvm::ConstantPointerNull>(value))
      return true;
    llvm::APInt offset(64, 0);
    const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
        value->stripAndAccumulateConstantOffsets(module.layout, offset,
                                                 /*AllowNonInbounds=*/true));
    if (global == nullptr || !global->isConstant() ||
        !global->hasDefinitiveInitializer())
      return false;
    const auto *text =
        llvm::dyn_cast<llvm::ConstantDataSequential>(global->getInitializer());
    if (text == nullptr || !text->isString() || offset.isNegative())
      return false;
    const llvm::StringRef bytes = text->getRawDataValues();
    return offset.getZExtValue() < bytes.size() &&
           bytes.find('\0', offset.getZExtValue()) != llvm::StringRef::npos;
  }

  /// Amendment 18: a call of a safe function's entry from this safe
  /// function calls its body instead when every validated argument has known
  /// bounds here, after checking them against the bytes the callee's bounds
  /// give it: one comparison each, not a scan of the shadow. RFC 0042 §6: a
  /// typed function's single and string arguments too, and a typed callee of
  /// another unit's body.
  void callBody(llvm::CallInst &call) {
    const llvm::Function *callee = call.getCalledFunction();
    const FunctionBounds *bounds = tableOf(module, *callee);
    llvm::Function *body = nullptr;
    const std::string name = (callee->getName() + ".weavec.safe").str();
    if (callee->hasFnAttribute(EntryAttribute)) {
      body = module.module.getFunction(name);
    } else {
      body = llvm::dyn_cast<llvm::Function>(
          module.module
              .getOrInsertFunction(name, callee->getFunctionType(),
                                   callee->getAttributes())
              .getCallee());
    }
    if (body == nullptr || bounds == nullptr)
      return;
    llvm::SmallVector<llvm::Value *, 8> arguments(call.args());
    for (std::size_t i = 0;
         i < bounds->parameters.size() && i < arguments.size(); ++i) {
      const PointerBounds &parameter = bounds->parameters[i];
      const bool single = parameter.kind == PointerBounds::Kind::Single &&
                          parameter.pointeeSize != 0;
      const bool string = parameter.kind == PointerBounds::Kind::NullTerminated;
      if (typed && single && !rooted(arguments[i]))
        return;
      if (typed && string && !stringLike(arguments[i]) && !rooted(arguments[i]))
        return;
      if (validated(parameter) && !rooted(arguments[i]))
        return;
    }
    for (std::size_t i = 0;
         i < bounds->parameters.size() && i < arguments.size(); ++i) {
      const PointerBounds &parameter = bounds->parameters[i];
      llvm::Value *pointer = arguments[i];
      if (typed && parameter.kind == PointerBounds::Kind::Single &&
          parameter.pointeeSize != 0) {
        convert(call, pointer,
                llvm::ConstantInt::get(module.i64, parameter.pointeeSize));
        continue;
      }
      if (typed && parameter.kind == PointerBounds::Kind::NullTerminated) {
        convertString(call, pointer);
        continue;
      }
      if (!validated(parameter))
        continue;
      const Bounds have = boundsOf(pointer);
      llvm::IRBuilder<> builder(&call);
      builder.SetCurrentDebugLocation(call.getDebugLoc());
      llvm::Value *bytes = bytesOf(parameter, pointer, builder, arguments);
      if (bytes == nullptr)
        return;
      check(call, pointer, bytes, have, /*write=*/false);
    }
    call.setCalledFunction(body);
  }

  /// The condition that `bytes` bytes at `pointer` do not lie inside
  /// `bounds`, before `before`; null when they statically do. RFC 0042 §4,
  /// §7: in a typed function an access at a constant offset from a root of
  /// known size is decided here, an element at an index of a counted root
  /// is compared by its index, and the byte form saturates nothing that
  /// the optimizer could otherwise fold with the program's own test.
  llvm::Value *outside(llvm::Instruction &before, llvm::Value *pointer,
                       llvm::Value *bytes, const Bounds &bounds) {
    llvm::IRBuilder<> builder(&before);
    builder.SetCurrentDebugLocation(before.getDebugLoc());
    const auto *width = llvm::dyn_cast<llvm::ConstantInt>(bytes);
    if (typed && width != nullptr) {
      const std::uint64_t w = width->getZExtValue();
      llvm::APInt offset(64, 0);
      const llvm::Value *base = pointer->stripAndAccumulateConstantOffsets(
          module.layout, offset, /*AllowNonInbounds=*/true);
      if (const auto found = constantSizes.find(base);
          found != constantSizes.end() && bounds.lo == base) {
        std::uint64_t limit = found->second.bytes;
        if (found->second.mayBeNull)
          limit = std::min(limit, NullPage);
        if (!offset.isNegative() && offset.getZExtValue() + w <= limit)
          return nullptr;
      }
      // `&p[i]` then a member at a constant offset: the element's index.
      std::uint64_t outer = 0;
      llvm::Value *indexed = pointer;
      if (auto *member = llvm::dyn_cast<llvm::GetElementPtrInst>(pointer)) {
        llvm::APInt constant(64, 0);
        auto *inner = llvm::dyn_cast<llvm::GetElementPtrInst>(
            member->getPointerOperand());
        if (inner != nullptr && inner->getPointerOperand() == bounds.lo &&
            member->accumulateConstantOffset(module.layout, constant) &&
            !constant.isNegative()) {
          outer = constant.getZExtValue();
          indexed = inner;
        }
      }
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(indexed);
          gep != nullptr && gep->getNumIndices() >= 1 &&
          gep->getPointerOperand() == bounds.lo &&
          counts.contains(gep->getPointerOperand())) {
        const RootCount count = counts.lookup(gep->getPointerOperand());
        const std::uint64_t element =
            module.layout.getTypeAllocSize(gep->getSourceElementType())
                .getFixedValue();
        llvm::SmallVector<llvm::Value *, 4> rest{
            llvm::ConstantInt::get(module.i64, 0)};
        bool constant = true;
        for (const auto *index = std::next(gep->idx_begin());
             index != gep->idx_end(); ++index) {
          constant = constant && llvm::isa<llvm::ConstantInt>(*index);
          rest.push_back(*index);
        }
        if (constant && element == count.elementSize && element != 0) {
          const std::int64_t inner = module.layout.getIndexedOffsetInType(
              gep->getSourceElementType(), rest);
          if (inner >= 0 &&
              static_cast<std::uint64_t>(inner) + outer + w <= element)
            return builder.CreateICmpUGE(
                builder.CreateSExtOrTrunc(gep->getOperand(1), module.i64),
                count.count);
        }
      }
      llvm::Value *lo = builder.CreatePtrToInt(bounds.lo, module.i64);
      llvm::Value *size =
          builder.CreateSub(builder.CreatePtrToInt(bounds.hi, module.i64), lo);
      llvm::Value *at =
          builder.CreateSub(builder.CreatePtrToInt(pointer, module.i64), lo);
      if (w > 1)
        size = builder.CreateBinaryIntrinsic(
            llvm::Intrinsic::usub_sat, size,
            llvm::ConstantInt::get(module.i64, w - 1));
      return builder.CreateICmpUGE(at, size);
    }
    llvm::Value *lo = builder.CreatePtrToInt(bounds.lo, module.i64);
    llvm::Value *size =
        builder.CreateSub(builder.CreatePtrToInt(bounds.hi, module.i64), lo);
    llvm::Value *limit = builder.CreateBinaryIntrinsic(
        llvm::Intrinsic::usub_sat,
        builder.CreateAdd(size, llvm::ConstantInt::get(module.i64, 1)), bytes);
    llvm::Value *offset =
        builder.CreateSub(builder.CreatePtrToInt(pointer, module.i64), lo);
    return builder.CreateICmpUGE(offset, limit);
  }

  /// Calls `fail` (pointer, bytes, lo, hi, site) before `before` when
  /// `condition` holds.
  void failWhen(llvm::Instruction &before, llvm::Value *condition,
                llvm::FunctionCallee fail, llvm::Value *pointer,
                llvm::Value *bytes, const Bounds &bounds, bool write) {
    const bool trap = module.options.mode != GuardOptions::Mode::Report;
    llvm::Instruction *then = llvm::SplitBlockAndInsertIfThen(
        condition, before.getIterator(), /*Unreachable=*/trap,
        llvm::MDBuilder(module.context).createUnlikelyBranchWeights());
    llvm::IRBuilder<> builder(then);
    builder.SetCurrentDebugLocation(before.getDebugLoc());
    ModuleContext::callSlow(
        builder, fail,
        {builder.CreatePtrToInt(pointer, module.i64),
         builder.CreateZExtOrTrunc(bytes, module.i64),
         builder.CreatePtrToInt(bounds.lo, module.i64),
         builder.CreatePtrToInt(bounds.hi, module.i64),
         module.site(before.getDebugLoc(), write ? SiteWrite : 0)});
  }

  /// Traps (`pointer-out-of-bounds`) before `before` unless `bytes` bytes at
  /// `pointer` lie inside `bounds`.
  void check(llvm::Instruction &before, llvm::Value *pointer,
             llvm::Value *bytes, const Bounds &bounds, bool write) {
    if (llvm::Value *condition = outside(before, pointer, bytes, bounds))
      failWhen(before, condition, module.boundsFailFunction(), pointer, bytes,
               bounds, write);
  }

  /// RFC 0042 §6: `value`, about to be stored, passed or returned where a
  /// pointer to `bytes` bytes is declared, covers them or is null
  /// (`bounds-conversion`); a value without bounds here is checked against
  /// the shadow.
  void convert(llvm::Instruction &before, llvm::Value *value,
               llvm::Value *bytes) {
    if (llvm::isa<llvm::ConstantPointerNull>(value) ||
        llvm::isa<llvm::UndefValue>(value))
      return;
    if (!rooted(value)) {
      llvm::IRBuilder<> builder(&before);
      builder.SetCurrentDebugLocation(before.getDebugLoc());
      const unsigned row =
          module.row(function, before.getDebugLoc(), "convert", 0,
                     LedgerRow::Outcome::Guarded, "typed-unbounded");
      module.insertRange(
          &before, value,
          builder.CreateSelect(builder.CreateIsNull(value),
                               llvm::ConstantInt::get(module.i64, 0), bytes),
          0, row);
      return;
    }
    const Bounds bounds = boundsOf(value);
    llvm::Value *condition = outside(before, value, bytes, bounds);
    if (condition == nullptr)
      return;
    llvm::IRBuilder<> builder(&before);
    builder.SetCurrentDebugLocation(before.getDebugLoc());
    condition = builder.CreateAnd(condition, builder.CreateIsNotNull(value));
    failWhen(before, condition, module.convertFailFunction(), value, bytes,
             bounds, /*write=*/false);
  }

  /// RFC 0042 §6: `value` goes where a string is declared: a string or a
  /// literal as it is; a bounded value is searched for its zero byte; a
  /// value without bounds is scanned against the shadow.
  void convertString(llvm::Instruction &before, llvm::Value *value) {
    if (stringLike(value) || llvm::isa<llvm::UndefValue>(value))
      return;
    llvm::IRBuilder<> builder(&before);
    builder.SetCurrentDebugLocation(before.getDebugLoc());
    if (!rooted(value)) {
      const unsigned row =
          module.row(function, before.getDebugLoc(), "convert", 0,
                     LedgerRow::Outcome::Guarded, "typed-unbounded");
      module.insertString(builder, value,
                          llvm::Constant::getAllOnesValue(module.i64), row,
                          MarkerNullOk);
      return;
    }
    const Bounds bounds = boundsOf(value);
    builder.CreateCall(module.convertStringFunction(),
                       {builder.CreatePtrToInt(value, module.i64),
                        builder.CreatePtrToInt(bounds.hi, module.i64),
                        module.site(before.getDebugLoc(), 0)});
  }

  /// The bytes `count` (a stored count, of the field's type) gives a
  /// pointer of `field`.
  llvm::Value *countBytes(llvm::IRBuilder<> &builder, llvm::Value *count,
                          std::int64_t element, std::uint64_t flags) {
    const bool signedCount = (flags & FieldBounds::CountSigned) != 0;
    count = signedCount ? builder.CreateSExtOrTrunc(count, module.i64)
                        : builder.CreateZExtOrTrunc(count, module.i64);
    if (signedCount)
      count = builder.CreateBinaryIntrinsic(
          llvm::Intrinsic::smax, count, llvm::ConstantInt::get(module.i64, 0));
    return timesElement(module, builder, count,
                        static_cast<std::uint64_t>(element));
  }

  /// RFC 0042 §6: a store that keeps memory typed. Into a pointer field:
  /// the value covers its bound (the new count of its pair, or the current
  /// one); into a count alone: the count does not grow.
  void convertStore(llvm::StoreInst &store) {
    llvm::SmallVector<const TypedField *, 2> fields;
    bool other = false;
    typedFieldsOf(module, store.getPointerOperand(), fields, other);
    const TypedState &state = module.typedState;
    llvm::Value *value = store.getValueOperand();
    for (const TypedField *field : fields) {
      llvm::IRBuilder<> builder(&store);
      builder.SetCurrentDebugLocation(store.getDebugLoc());
      if (field->count) {
        if (state.pairs.contains(&store) || !value->getType()->isIntegerTy())
          continue;
        llvm::Value *old = state.oldCount.lookup(&store);
        if (old == nullptr)
          continue;
        const CountBounds &count = field->counts.front();
        llvm::Value *now =
            countBytes(builder, value, count.elementSize, count.flags);
        const bool signedCount = (count.flags & FieldBounds::CountSigned) != 0;
        if (signedCount)
          old = builder.CreateBinaryIntrinsic(
              llvm::Intrinsic::smax, old,
              llvm::ConstantInt::get(module.i64, 0));
        llvm::Value *was =
            timesElement(module, builder, old,
                         static_cast<std::uint64_t>(count.elementSize));
        // A count that grows alone: its pointer's object must have the
        // bytes, which the shadow says (amendment 2); one that shrinks
        // needs nothing more.
        llvm::Instruction *then = llvm::SplitBlockAndInsertIfThen(
            builder.CreateICmpUGT(now, was), store.getIterator(),
            /*Unreachable=*/false,
            llvm::MDBuilder(module.context).createUnlikelyBranchWeights());
        llvm::IRBuilder<> grow(then);
        grow.SetCurrentDebugLocation(store.getDebugLoc());
        llvm::Value *current = grow.CreateLoad(
            module.ptr,
            grow.CreateGEP(
                module.i8, store.getPointerOperand(),
                llvm::ConstantInt::get(module.i64, static_cast<std::uint64_t>(
                                                       count.pointerOffset))));
        const unsigned row =
            module.row(function, store.getDebugLoc(), "convert", 0,
                       LedgerRow::Outcome::Guarded, "typed-unbounded");
        module.insertRange(
            then, current,
            grow.CreateSelect(grow.CreateIsNull(current),
                              llvm::ConstantInt::get(module.i64, 0), now),
            0, row);
        continue;
      }
      if (!value->getType()->isPointerTy())
        continue;
      const FieldBounds &bounds = field->field;
      switch (bounds.kind) {
      case FieldBounds::Kind::Single:
        if (bounds.elementSize > 0)
          convert(store, value,
                  llvm::ConstantInt::get(module.i64, static_cast<std::uint64_t>(
                                                         bounds.elementSize)));
        break;
      case FieldBounds::Kind::NullTerminated:
        convertString(store, value);
        break;
      case FieldBounds::Kind::Counted: {
        if (llvm::Instruction *pair = state.pairs.lookup(&store)) {
          // At the later store of the pair, with its new count.
          auto *count = llvm::cast<llvm::StoreInst>(pair);
          llvm::Instruction &at =
              state.seconds.contains(count) ? *count : store;
          llvm::IRBuilder<> later(&at);
          later.SetCurrentDebugLocation(at.getDebugLoc());
          convert(at, value,
                  countBytes(later, count->getValueOperand(),
                             bounds.elementSize, bounds.flags));
          break;
        }
        if (bounds.countWidth == 0) {
          convert(store, value,
                  timesElement(module, builder,
                               llvm::ConstantInt::get(
                                   module.i64, static_cast<std::uint64_t>(
                                                   bounds.countOffset)),
                               static_cast<std::uint64_t>(bounds.elementSize)));
          break;
        }
        if (llvm::Value *count = state.countAt.lookup(&store)) {
          if ((bounds.flags & FieldBounds::CountSigned) != 0)
            count = builder.CreateBinaryIntrinsic(
                llvm::Intrinsic::smax, count,
                llvm::ConstantInt::get(module.i64, 0));
          convert(store, value,
                  timesElement(module, builder, count,
                               static_cast<std::uint64_t>(bounds.elementSize)));
        }
        break;
      }
      }
    }
  }

  /// RFC 0042 §6: a typed body returns what its declaration bounds.
  void convertReturn(llvm::ReturnInst &ret) {
    llvm::Value *value = ret.getReturnValue();
    if (table == nullptr || value == nullptr ||
        !value->getType()->isPointerTy())
      return;
    const PointerBounds &result = table->result;
    if (result.kind == PointerBounds::Kind::Single && result.pointeeSize != 0) {
      convert(ret, value,
              llvm::ConstantInt::get(module.i64, result.pointeeSize));
      return;
    }
    if (result.kind == PointerBounds::Kind::NullTerminated) {
      convertString(ret, value);
      return;
    }
    llvm::SmallVector<llvm::Value *, 8> arguments;
    for (llvm::Argument &argument : function.args())
      arguments.push_back(&argument);
    llvm::IRBuilder<> builder(&ret);
    builder.SetCurrentDebugLocation(ret.getDebugLoc());
    if (llvm::Value *bytes = bytesOf(result, value, builder, arguments))
      convert(ret, value, bytes);
  }

  /// Replaces the guard `marker` by a check against its pointer's bounds,
  /// when they are known. In a typed function, an access through a string
  /// position needs none, and a guard that stays says why in the ledger.
  void bound(llvm::CallInst &marker) {
    llvm::Value *pointer = marker.getArgOperand(0);
    const auto *flags = llvm::cast<llvm::ConstantInt>(marker.getArgOperand(2));
    const unsigned rowArgument =
        ModuleContext::markerOf(marker) == MarkerGuard ? 4 : 3;
    const auto *row =
        llvm::cast<llvm::ConstantInt>(marker.getArgOperand(rowArgument));
    const bool guard = ModuleContext::markerOf(marker) == MarkerGuard;
    if (typed && !rooted(pointer)) {
      if (const auto found = measured.find(&marker); found != measured.end()) {
        llvm::CallInst *length = found->second;
        llvm::Value *string = length->getArgOperand(0);
        llvm::IRBuilder<> builder(length->getNextNode());
        known[pointer] = {.lo = string,
                          .hi = builder.CreateGEP(
                              module.i8, string,
                              builder.CreateAdd(length, llvm::ConstantInt::get(
                                                            module.i64, 1)))};
        rootable[pointer] = true;
      } else if (!stringValues.contains(pointer) ||
                 ((!guard ||
                   llvm::cast<llvm::ConstantInt>(marker.getArgOperand(1))
                           ->getZExtValue() > 1) &&
                  (flags->getZExtValue() & MarkerScan) == 0)) {
        module.reason(static_cast<unsigned>(row->getZExtValue()),
                      "typed-unbounded");
        return;
      } else {
        // A string position: its byte is its object's, or the zero byte;
        // a library search of the string stops at its terminator.
        if (module.options.safe->full)
          return;
        removeMarker(marker, *flags, *row);
        return;
      }
    }
    if (!rooted(pointer))
      return;
    const Bounds bounds = boundsOf(pointer);
    llvm::IRBuilder<> builder(&marker);
    builder.SetCurrentDebugLocation(marker.getDebugLoc());
    llvm::Value *width =
        builder.CreateZExtOrTrunc(marker.getArgOperand(1), module.i64);
    // One comparison against a loop-invariant limit (amendment 3).
    check(marker, pointer, width, bounds,
          (flags->getZExtValue() & MarkerWrite) != 0);
    if (module.options.safe->full)
      return;
    removeMarker(marker, *flags, *row);
  }

  /// A guard a rule made redundant: verify mode's monitor, or gone.
  void removeMarker(llvm::CallInst &marker, const llvm::ConstantInt &flags,
                    const llvm::ConstantInt &row) {
    if (module.verify()) {
      marker.setArgOperand(
          2, llvm::ConstantInt::get(module.i32,
                                    flags.getZExtValue() | MarkerProven));
      return;
    }
    module.copy(static_cast<unsigned>(row.getZExtValue()), false,
                typed ? "typed" : "safe");
    marker.eraseFromParent();
  }
};

} // namespace

void boundSafe(ModuleContext &module, llvm::Function &function) {
  SafeBounds(module, function).run();
}

} // namespace weavec::frontend::guard
