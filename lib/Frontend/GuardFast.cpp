//===- GuardFast.cpp - Type-stable memory in fast safe units --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0045. A unit compiled as safe in the fast mode (`-fweavec-safe`
// without `-fweavec-safe-checks=full`):
//
//   - types its allocations: each call Clang gave the type it allocates
//     (`!alloc_token`) becomes, in GuardExpand, a call of the runtime's typed
//     entry point with a record of the type, whose pool the runtime finds
//     once per site (§2);
//   - marks the releases of its safe code, and of the standard library's
//     code it compiles, with the site flag that returns a typed block to its
//     pool (§3);
//   - in C++, places no guard on an access at a constant offset inside the
//     bytes a believed pointer covers (§4), clones the standard library's
//     trusted functions its safe functions call without guards (§5), lays
//     out frames whose locals never share bytes, and checks that a safe
//     function's pointer result is not into its own frame (Y4).
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"
#include "weavec/Core/LibrarySpec.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <bit>
#include <functional>
#include <tuple>

namespace weavec::frontend::guard {

// RFC 0045 §12: measure what each half of the fast mode buys (`-mllvm
// -weavec-pools=0`: no typed allocation or release; `-weavec-believe=0`:
// no believed access, trusted clone, unshared frame or result check); not
// a user's switch.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static llvm::cl::opt<bool> pools("weavec-pools", llvm::cl::init(true),
                                 llvm::cl::Hidden);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static llvm::cl::opt<bool> believe("weavec-believe", llvm::cl::init(true),
                                   llvm::cl::Hidden);

bool fastUnit(const ModuleContext &module) {
  return module.options.safe != nullptr && module.options.safe->fast;
}

bool typedRelease(const ModuleContext &module, const llvm::Function &function) {
  return pools && fastCodeOf(module, function) != FastCode::None;
}

bool fastCxxUnit(const ModuleContext &module) {
  return fastUnit(module) && module.options.safe->cxx && believe;
}

FastCode fastCodeOf(const ModuleContext &module,
                    const llvm::Function &function) {
  if (!fastUnit(module))
    return FastCode::None;
  const SafeUnit &safe = *module.options.safe;
  // A safe C function's public entry (RFC 0041 amendment 18) is its code,
  // after its parameters' checks.
  if (!safe.cxx)
    return isSafe(module, function) ||
                   function.hasFnAttribute("weavec-safe-entry")
               ? FastCode::Safe
               : FastCode::None;
  const llvm::StringRef name = function.getName();
  if (name.ends_with(TrustedSuffix))
    return FastCode::Trusted;
  if (safe.cxxSafe.contains(name))
    return FastCode::Safe;
  // The unit's other functions, and the initializers of its globals.
  if (safe.cxxOther.contains(name) || name.starts_with("__cxx_global_var") ||
      name.starts_with("_GLOBAL__"))
    return FastCode::None;
  return FastCode::Library;
}

/// §2: the key of the type a `!alloc_token` names: a hash of its name, and
/// of the unit for a type the name does not identify across units (every
/// C type, a C++ type in an unnamed namespace or a function); an array
/// `new`, which may put a cookie before its elements, apart.
static std::uint64_t typeKey(const ModuleContext &module, llvm::StringRef name,
                             bool array) {
  std::uint64_t key = llvm::xxh3_64bits(name);
  if (!module.options.safe->cxx || name.contains('(') ||
      name.contains("anonymous")) {
    llvm::SmallString<256> path(module.module.getSourceFileName());
    std::ignore = llvm::sys::fs::make_absolute(path);
    key ^= llvm::xxh3_64bits(path.str()) * 0x9E3779B97F4A7C15ULL;
  }
  if (array)
    key = std::rotl(key, 1) ^ 0x5BD1E9955BD1E995ULL;
  return key != 0 ? key : 1;
}

/// §2: the row of an allocation function whose calls a fast unit types:
/// one with a `typed` clause, called with its row's arguments.
static const core::LibraryEntry *typedRow(const llvm::CallBase &call) {
  const llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr)
    return nullptr;
  const std::optional<core::LibraryMatch> match =
      core::LibrarySpec::shipped().lookup(libraryName(callee->getName()),
                                          call.arg_size());
  if (!match || match->entry == nullptr || match->alias != nullptr ||
      !match->entry->typed || match->entry->params.size() != call.arg_size())
    return nullptr;
  return match->entry;
}

/// Why an allocation a fast unit's code makes is not typed: a row that
/// gives an owner without a typed form, or one that reallocates.
static llvm::StringRef untypedReason(const llvm::CallBase &call) {
  const llvm::Function *callee = call.getCalledFunction();
  if (callee == nullptr)
    return {};
  const std::optional<core::LibraryMatch> match =
      core::LibrarySpec::shipped().lookup(libraryName(callee->getName()),
                                          call.arg_size());
  if (!match || match->entry == nullptr)
    return {};
  const core::LibraryEntry &entry = *match->entry;
  if (llvm::any_of(entry.params, [](const core::LibraryParam &param) {
        return param.effect == core::LibraryParam::Effect::Realloc;
      }))
    return "reallocated";
  if (entry.typed)
    return "no type";
  return entry.owned ? "no typed form" : llvm::StringRef();
}

/// RFC 0045 §10: what the ledger says of a fast unit beyond its accesses.
static void noteLibrary(const ModuleContext &module, llvm::StringRef kind,
                        const llvm::Function &function,
                        const llvm::DebugLoc &location, llvm::StringRef name,
                        llvm::StringRef reason) {
  if (!module.options.ledger)
    return;
  LedgerLibrary entry{.kind = kind.str(),
                      .function = llvm::demangle(name),
                      .file = {},
                      .line = 0,
                      .reason = reason.str()};
  if (location) {
    const ModuleContext::Position at = module.positionOf(location);
    entry.file = at.file;
    entry.line = at.line;
  } else if (const llvm::DISubprogram *program = function.getSubprogram()) {
    entry.file = program->getFilename().str();
    entry.line = program->getLine();
  }
  module.options.ledger->library.push_back(std::move(entry));
}

/// The operand bundle that carries a typed site's record from GuardInsert
/// to GuardExpand: the inliner keeps a call's bundles where it drops its
/// `!alloc_token` (a call it turns into an `invoke`).
static constexpr llvm::StringLiteral TypeBundle = "weavec.type";

/// Before the optimizer: each allocation call whose type Clang inferred gets
/// a record of its type (the key, and the pool the runtime resolves), named
/// by a bundle; the metadata goes.
static void markAllocations(ModuleContext &module, llvm::Module &ir) {
  auto *recordType =
      llvm::StructType::get(module.context, {module.i64, module.i64});
  std::vector<llvm::CallBase *> calls;
  for (llvm::Function &function : ir)
    for (llvm::BasicBlock &block : function)
      for (llvm::Instruction &instruction : block)
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            call != nullptr &&
            call->hasMetadata(llvm::LLVMContext::MD_alloc_token))
          calls.push_back(call);
  for (llvm::CallBase *call : calls) {
    const llvm::MDNode *token =
        call->getMetadata(llvm::LLVMContext::MD_alloc_token);
    call->setMetadata(llvm::LLVMContext::MD_alloc_token, nullptr);
    const core::LibraryEntry *row = typedRow(*call);
    const auto *name =
        token->getNumOperands() != 0
            ? llvm::dyn_cast<llvm::MDString>(token->getOperand(0))
            : nullptr;
    if (row == nullptr || name == nullptr || name->getString().empty())
      continue;
    // An array `new`, which may put a cookie before its elements, apart.
    const bool array =
        row->typed->name == "new_typed" && (row->typedFlags & 1U) != 0;
    auto *record = new llvm::GlobalVariable(
        ir, recordType, /*isConstant=*/false, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(
            recordType,
            {llvm::ConstantInt::get(module.i64,
                                    typeKey(module, name->getString(), array)),
             llvm::ConstantInt::get(module.i64, 0)}),
        "__weavec.type");
    record->setAlignment(llvm::Align(16));
    llvm::CallBase *marked = llvm::CallBase::addOperandBundle(
        call, module.context.getOrInsertBundleTag(TypeBundle)->getValue(),
        llvm::OperandBundleDef(TypeBundle.str(),
                               llvm::ArrayRef<llvm::Value *>{record}),
        call->getIterator());
    call->replaceAllUsesWith(marked);
    call->eraseFromParent();
  }
}

void typeAllocations(ModuleContext &module, llvm::Function &function) {
  if (!fastUnit(module))
    return;
  std::vector<llvm::CallBase *> calls;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        if (call->getOperandBundle(TypeBundle)) {
          calls.push_back(call);
        } else if (fastCodeOf(module, function) != FastCode::None) {
          const llvm::StringRef reason = untypedReason(*call);
          if (!reason.empty())
            noteLibrary(module, "untyped", function, call->getDebugLoc(),
                        function.getName(), reason);
        }
      }
  for (llvm::CallBase *call : calls) {
    llvm::Value *record = call->getOperandBundle(TypeBundle)->Inputs[0];
    const core::LibraryEntry *row = typedRow(*call);
    if (row == nullptr) {
      // (No longer an allocation the table types: the bundle goes.)
      llvm::CallBase *plain = llvm::CallBase::removeOperandBundle(
          call, module.context.getOrInsertBundleTag(TypeBundle)->getValue(),
          call->getIterator());
      call->replaceAllUsesWith(plain);
      call->eraseFromParent();
      continue;
    }
    const llvm::StringRef name = row->typed->name;
    llvm::Value *flags = llvm::ConstantInt::get(module.i32, row->typedFlags);
    llvm::SmallVector<llvm::Value *, 3> arguments;
    if (name == "calloc_typed")
      arguments = {call->getArgOperand(0), call->getArgOperand(1), record};
    else if (name == "alloc_typed")
      arguments = {call->getArgOperand(0), flags, record};
    else
      arguments = {call->getArgOperand(0), record, flags};
    llvm::SmallVector<llvm::Type *, 3> types;
    for (const llvm::Value *argument : arguments)
      types.push_back(argument->getType());
    const llvm::FunctionCallee entry =
        module.runtime(name, llvm::FunctionType::get(module.ptr, types, false));
    llvm::IRBuilder<> builder(call);
    llvm::CallBase *replacement = nullptr;
    if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call))
      replacement = builder.CreateInvoke(entry, invoke->getNormalDest(),
                                         invoke->getUnwindDest(), arguments);
    else
      replacement = builder.CreateCall(entry, arguments);
    replacement->setDebugLoc(call->getDebugLoc());
    replacement->addRetAttr(llvm::Attribute::NoAlias);
    call->replaceAllUsesWith(replacement);
    call->eraseFromParent();
    if (module.options.ledger)
      ++module.options.ledger->typedSites;
  }
}

void prepareFast(ModuleContext &module, llvm::Module &ir) {
  if (!fastUnit(module))
    return;
  // §2: the metadata is for GuardExpand; LLVM's own token pass, which
  // instruments the functions that carry this attribute, does nothing.
  for (llvm::Function &function : ir)
    function.removeFnAttr(llvm::Attribute::SanitizeAllocToken);
  // §9: a program with a fast unit records no heap stacks and makes no
  // container annotations, unless its environment asks.
  if (ir.getFunction("weavec.fast") == nullptr) {
    auto *constructor = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false),
        llvm::GlobalValue::InternalLinkage, "weavec.fast", ir);
    llvm::IRBuilder<> builder(
        llvm::BasicBlock::Create(module.context, "entry", constructor));
    builder.CreateCall(module.runtime(
        "fast_program",
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false)));
    builder.CreateRetVoid();
    llvm::appendToGlobalCtors(ir, constructor, 101);
  }
  if (pools)
    markAllocations(module, ir);
  if (fastCxxUnit(module))
    cloneTrusted(module, ir);
}

/// Before the optimizer a parameter, or a reference a local holds, is read
/// back from the local Clang spilled it to: the value stored there, when
/// that local is stored once and only read.
static const llvm::Value *unspilled(const llvm::Value *value) {
  const auto *load = llvm::dyn_cast<llvm::LoadInst>(value);
  const auto *slot =
      load != nullptr
          ? llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand())
          : nullptr;
  if (slot == nullptr)
    return value;
  const llvm::Value *stored = nullptr;
  for (const llvm::User *user : slot->users()) {
    if (llvm::isa<llvm::LoadInst>(user))
      continue;
    const auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
    if (store == nullptr || store->getPointerOperand() != slot ||
        stored != nullptr)
      return value;
    stored = store->getValueOperand();
  }
  return stored != nullptr ? stored : value;
}

/// §4: the bytes from `base` a believed pointer covers, or 0.
static std::uint64_t believedBytes(const ModuleContext &module,
                                   const llvm::Value *base) {
  base = unspilled(base);
  if (const auto *argument = llvm::dyn_cast<llvm::Argument>(base))
    return argument->getDereferenceableBytes();
  const auto *call = llvm::dyn_cast<llvm::CallBase>(base);
  if (call == nullptr)
    return 0;
  const llvm::Function *callee = call->getCalledFunction();
  if (callee == nullptr || callee->isIntrinsic())
    return 0;
  const SafeUnit &safe = *module.options.safe;
  const llvm::StringRef name = callee->getName();
  // A safe or trusted function's reference result; a reference the unit's
  // other code returns is trusted to be one (Y-A1).
  if (!(fastCodeOf(module, *callee) == FastCode::Safe ||
        fastCodeOf(module, *callee) == FastCode::Trusted ||
        safe.cxxTrusted.contains(name) || safe.cxxOther.contains(name)))
    return 0;
  return call->getRetDereferenceableBytes();
}

/// Below the null page: an access through a null reference faults.
static constexpr std::uint64_t BelievedLimit = 4096;

bool believedAccess(const ModuleContext &module, const llvm::Function &function,
                    const llvm::Value *pointer, std::uint64_t width) {
  if (!fastCxxUnit(module) || width == 0 ||
      fastCodeOf(module, function) == FastCode::None)
    return false;
  const llvm::DataLayout &layout = module.layout;
  llvm::APInt offset(layout.getIndexTypeSizeInBits(pointer->getType()), 0);
  const llvm::Value *base = pointer->stripAndAccumulateConstantOffsets(
      layout, offset, /*AllowNonInbounds=*/true);
  if (offset.isNegative() || offset.getActiveBits() > 32)
    return false;
  const std::uint64_t end = offset.getZExtValue() + width;
  return end <= BelievedLimit && end <= believedBytes(module, base);
}

bool sharesFrames(const ModuleContext &module) {
  return !fastCxxUnit(module);
}

void dropUntrackedLifetimes(const ModuleContext &module,
                            llvm::Function &function,
                            llvm::ArrayRef<llvm::AllocaInst *> tracked) {
  if (!fastCxxUnit(module))
    return;
  // Locals no guard reaches are laid out by the code generator, which lets
  // two of them share bytes by their lifetime markers: without the markers
  // a local's bytes are its own for the whole call, so a believed pointer
  // to one whose scope ended reads that local's type.
  llvm::SmallPtrSet<const llvm::AllocaInst *, 16> kept(tracked.begin(),
                                                       tracked.end());
  std::vector<llvm::IntrinsicInst *> markers;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
          intrinsic != nullptr && intrinsic->isLifetimeStartOrEnd()) {
        const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(
            intrinsic->getArgOperand(intrinsic->arg_size() - 1)
                ->stripPointerCasts());
        if (alloca == nullptr || !kept.contains(alloca))
          markers.push_back(intrinsic);
      }
  for (llvm::IntrinsicInst *marker : markers)
    marker->eraseFromParent();
}

void checkResults(ModuleContext &module, llvm::Function &function) {
  if (!fastCxxUnit(module) || !function.getReturnType()->isPointerTy() ||
      fastCodeOf(module, function) != FastCode::Safe)
    return;
  std::vector<llvm::ReturnInst *> returns;
  for (llvm::BasicBlock &block : function)
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
        ret != nullptr && ret->getReturnValue() != nullptr &&
        !llvm::isa<llvm::Constant>(ret->getReturnValue()))
      returns.push_back(ret);
  if (returns.empty())
    return;
  llvm::Function *frameAddress = llvm::Intrinsic::getOrInsertDeclaration(
      &module.module, llvm::Intrinsic::frameaddress, {module.ptr});
  llvm::Function *stackSave = llvm::Intrinsic::getOrInsertDeclaration(
      &module.module, llvm::Intrinsic::stacksave, {module.ptr});
  const llvm::FunctionCallee fail = module.runtime(
      "frame_escape",
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.context),
                              {module.i64, module.ptr, module.ptr}, false));
  for (llvm::ReturnInst *ret : returns) {
    llvm::IRBuilder<> builder(ret);
    // [sp - 64 KiB, the frame record's end): the function's own frame and
    // those of the calls it made, which are gone.
    llvm::Value *low = builder.CreateSub(
        builder.CreatePtrToInt(builder.CreateCall(stackSave), module.i64),
        llvm::ConstantInt::get(module.i64, 65536));
    llvm::Value *high = builder.CreateAdd(
        builder.CreatePtrToInt(
            builder.CreateCall(frameAddress, {builder.getInt32(0)}),
            module.i64),
        llvm::ConstantInt::get(module.i64, 16));
    llvm::Value *address =
        builder.CreatePtrToInt(ret->getReturnValue(), module.i64);
    llvm::Value *inside = builder.CreateICmpULT(builder.CreateSub(address, low),
                                                builder.CreateSub(high, low));
    llvm::Instruction *then = llvm::SplitBlockAndInsertIfThen(
        inside, ret->getIterator(), /*Unreachable=*/false,
        llvm::MDBuilder(module.context).createUnlikelyBranchWeights());
    llvm::IRBuilder<> slow(then);
    slow.CreateCall(fail,
                    {address, module.text(llvm::demangle(function.getName())),
                     module.site(ret->getDebugLoc(), 0)});
  }
}

void cloneTrusted(ModuleContext &module, llvm::Module &ir) {
  const SafeUnit &safe = *module.options.safe;
  // The library functions reached from the safe functions' calls of
  // trusted ones, cloned once each; the clones call clones.
  llvm::DenseMap<llvm::Function *, llvm::Function *> clones;
  std::vector<llvm::Function *> work;
  auto cloneOf = [&](llvm::Function *original) -> llvm::Function * {
    if (const auto found = clones.find(original); found != clones.end())
      return found->second;
    llvm::ValueToValueMapTy map;
    llvm::Function *copy = llvm::CloneFunction(original, map);
    copy->setName(original->getName() + TrustedSuffix);
    // Never the symbol a guarded unit's instantiation has: the same name
    // in every fast unit, kept once.
    if (original->hasLocalLinkage()) {
      copy->setLinkage(llvm::GlobalValue::InternalLinkage);
    } else {
      copy->setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
      copy->setVisibility(llvm::GlobalValue::HiddenVisibility);
      if (original->hasComdat())
        copy->setComdat(ir.getOrInsertComdat(copy->getName()));
    }
    clones[original] = copy;
    work.push_back(copy);
    noteLibrary(module, "trusted", *original, {}, original->getName(),
                safe.cxxTrusted.contains(original->getName()) ? "called"
                                                              : "reached");
    return copy;
  };
  // Whether a call from trusted code goes to a clone: a library function
  // the unit defines (not the program's, which runs as it is).
  auto clonable = [&](const llvm::Function *callee) {
    return callee != nullptr && !callee->isDeclaration() &&
           !callee->isInterposable() && !callee->isIntrinsic() &&
           !callee->hasFnAttribute(llvm::Attribute::Naked) &&
           !ModuleContext::isRuntimeName(callee->getName()) &&
           fastCodeOf(module, *callee) == FastCode::Library;
  };
  // A constant table of the library's functions trusted code calls through
  // (a variant's visitors): a copy of it that names their clones.
  llvm::DenseMap<llvm::GlobalVariable *, llvm::GlobalVariable *> tables;
  std::function<llvm::Constant *(llvm::Constant *)> remap =
      [&](llvm::Constant *value) -> llvm::Constant * {
    if (auto *function = llvm::dyn_cast<llvm::Function>(value))
      return clonable(function) ? cloneOf(function) : function;
    if (llvm::isa<llvm::GlobalValue>(value) || value->getNumOperands() == 0)
      return value;
    llvm::SmallVector<llvm::Constant *, 8> operands;
    bool changed = false;
    for (llvm::Value *operand : value->operands()) {
      llvm::Constant *mapped = remap(llvm::cast<llvm::Constant>(operand));
      changed |= mapped != operand;
      operands.push_back(mapped);
    }
    if (!changed)
      return value;
    if (const auto *array = llvm::dyn_cast<llvm::ConstantArray>(value))
      return llvm::ConstantArray::get(array->getType(), operands);
    if (const auto *structure = llvm::dyn_cast<llvm::ConstantStruct>(value))
      return llvm::ConstantStruct::get(structure->getType(), operands);
    return value;
  };
  auto tableOf = [&](llvm::GlobalVariable *table) -> llvm::GlobalVariable * {
    if (const auto found = tables.find(table); found != tables.end())
      return found->second;
    tables[table] = table;
    if (!table->isConstant() || !table->hasInitializer() ||
        table->isInterposable())
      return table;
    llvm::Constant *initializer = remap(table->getInitializer());
    if (initializer == table->getInitializer())
      return table;
    auto *copy =
        new llvm::GlobalVariable(ir, table->getValueType(), /*isConstant=*/true,
                                 llvm::GlobalValue::PrivateLinkage, initializer,
                                 table->getName() + TrustedSuffix);
    copy->setAlignment(table->getAlign());
    copy->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    tables[table] = copy;
    return copy;
  };
  auto redirect = [&](llvm::Function &caller, bool trusted) {
    for (llvm::BasicBlock &block : caller)
      for (llvm::Instruction &instruction : block) {
        if (trusted)
          for (llvm::Use &use : instruction.operands())
            if (auto *table = llvm::dyn_cast<llvm::GlobalVariable>(use.get()))
              if (llvm::GlobalVariable *copy = tableOf(table); copy != table)
                use.set(copy);
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        llvm::Function *callee =
            call != nullptr ? call->getCalledFunction() : nullptr;
        if (!clonable(callee) ||
            (!trusted && !safe.cxxTrusted.contains(callee->getName())))
          continue;
        call->setCalledFunction(cloneOf(callee));
      }
  };
  std::vector<llvm::Function *> safeFunctions;
  for (llvm::Function &function : ir)
    if (!function.isDeclaration() &&
        fastCodeOf(module, function) == FastCode::Safe)
      safeFunctions.push_back(&function);
  for (llvm::Function *function : safeFunctions)
    redirect(*function, /*trusted=*/false);
  while (!work.empty()) {
    llvm::Function *clone = work.back();
    work.pop_back();
    redirect(*clone, /*trusted=*/true);
  }
}

} // namespace weavec::frontend::guard
