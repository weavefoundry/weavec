//===- GuardGlobals.cpp - Global redzones ---------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §4. Each global the unit defines exactly gets a redzone after it:
// it is replaced by a global of the same name, linkage and visibility that
// holds the original and the redzone, aligned to 32 bytes. A constructor
// registers the unit's globals with the runtime, which poisons each
// redzone; a destructor unregisters them, so `dlclose` leaves nothing in the
// shadow.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <algorithm>

namespace weavec::frontend::guard {

/// Whether `global` gets a redzone.
static bool isTracked(const llvm::GlobalVariable &global,
                      const llvm::DataLayout &layout) {
  if (global.isDeclaration() || !global.hasInitializer() ||
      !global.hasExactDefinition() || global.isInterposable() ||
      global.hasCommonLinkage() || global.hasAppendingLinkage() ||
      global.isThreadLocal() || global.hasSection() || global.hasComdat() ||
      global.getAddressSpace() != 0 || global.isExternallyInitialized())
    return false;
  const llvm::StringRef name = global.getName();
  if (name.starts_with("llvm.") || name.starts_with("__weavec") ||
      name.starts_with("__llvm"))
    return false;
  // String literals and the compiler's own tables: mergeable constants.
  if (global.isConstant() && global.hasGlobalUnnamedAddr() &&
      global.hasLocalLinkage())
    return false;
  if (global.getAlign() && global.getAlign()->value() > 4096)
    return false;
  const llvm::TypeSize size = layout.getTypeAllocSize(global.getValueType());
  return !size.isScalable() && size.getFixedValue() != 0;
}

void instrumentGlobals(ModuleContext &module) {
  const llvm::DataLayout &layout = module.layout;
  std::vector<llvm::GlobalVariable *> globals;
  for (llvm::GlobalVariable &global : module.module.globals())
    if (isTracked(global, layout))
      globals.push_back(&global);
  if (globals.empty())
    return;
  llvm::LLVMContext &context = module.context;
  auto *recordType =
      llvm::StructType::get(context, {module.ptr, module.i64, module.i64});
  std::vector<llvm::Constant *> records;
  for (llvm::GlobalVariable *global : globals) {
    llvm::Type *type = global->getValueType();
    const std::uint64_t size = layout.getTypeAllocSize(type).getFixedValue();
    const std::uint64_t padded = paddedSize(size);
    auto *zone = llvm::ArrayType::get(module.i8, padded - size);
    auto *wrapped = llvm::StructType::get(context, {type, zone});
    auto *initializer = llvm::ConstantStruct::get(
        wrapped,
        {global->getInitializer(), llvm::ConstantAggregateZero::get(zone)});
    auto *replacement = new llvm::GlobalVariable(
        module.module, wrapped, global->isConstant(), global->getLinkage(),
        initializer, "", global, global->getThreadLocalMode(),
        global->getAddressSpace());
    replacement->copyAttributesFrom(global);
    replacement->setAlignment(llvm::Align(
        std::max<std::uint64_t>(32, global->getAlign().valueOrOne().value())));
    replacement->copyMetadata(global, 0);
    replacement->takeName(global);
    global->replaceAllUsesWith(replacement);
    global->eraseFromParent();
    records.push_back(llvm::ConstantStruct::get(
        recordType, {replacement, llvm::ConstantInt::get(module.i64, size),
                     llvm::ConstantInt::get(module.i64, padded)}));
  }
  auto *arrayType = llvm::ArrayType::get(recordType, records.size());
  auto *array = new llvm::GlobalVariable(
      module.module, arrayType, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(arrayType, records), "__weavec.globals");
  llvm::Type *voidType = llvm::Type::getVoidTy(context);
  auto *entryType =
      llvm::FunctionType::get(voidType, {module.ptr, module.i64}, false);
  auto makeHook = [&](llvm::StringRef name, llvm::StringRef entry) {
    auto *hook = llvm::Function::Create(
        llvm::FunctionType::get(voidType, false),
        llvm::GlobalValue::InternalLinkage, name, module.module);
    hook->setDoesNotThrow();
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "", hook));
    builder.CreateCall(
        module.runtime(entry, entryType),
        {array, llvm::ConstantInt::get(module.i64, records.size())});
    builder.CreateRetVoid();
    return hook;
  };
  // Priority 1: before the program's own constructors read the globals.
  llvm::appendToGlobalCtors(
      module.module, makeHook("__weavec.globals.register", "globals_register"),
      1);
  llvm::appendToGlobalDtors(
      module.module,
      makeHook("__weavec.globals.unregister", "globals_unregister"), 1);
}

} // namespace weavec::frontend::guard
