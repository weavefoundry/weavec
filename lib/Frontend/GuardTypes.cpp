//===- GuardTypes.cpp - One of several types, in the code -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0054, the passes' part. First in GuardInsert, before anything reads a
// safe function's code:
//
//   - each checked conversion (`__weavec.cast`) becomes a check that an
//     object of its target type is at the converted pointer: in C++, of a
//     class with virtual functions, one comparison of the type information
//     its vtable names, and the C++ ABI's dynamic cast when that is not
//     equal; else a call of the runtime's `__weavec_rt_cast`. What it gives
//     is `__weavec.object(pointer, size, extent)`, one object of the
//     target's size among the `extent` bytes of objects of the type the
//     runtime found there (amendment 22), which GuardSafe and the fast mode
//     take as a root until GuardExpand removes it (§2.2);
//   - each access of a tagged union's member (`__weavec.member`) is
//     preceded by a load of the tag and its comparison with the member's
//     values (`union-member` when it fails); each store of a tag
//     (`__weavec.tag`) clears the union when the value changes (§3.3);
//   - each local of a safe function whose address leaves it
//     (`weavec.recorded`) is pushed on the thread's frame records where its
//     scope begins and popped where it ends (§1.2);
//   - in a unit that does not pool its blocks, each allocation whose type
//     Clang inferred records the type with the runtime (§1.2).
//
// The layouts the runtime answers from are private constants of the unit,
// one per type (§1.1).
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"
#include "weavec/Frontend/SafeCheck.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <utility>

namespace weavec::frontend::guard {

// RFC 0054 S0: what the checks of conversions and of tagged unions' members
// cost (`-mllvm -weavec-type-checks=0`: the markers are taken out
// unchecked); not a user's switch.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static llvm::cl::opt<bool> typeChecks("weavec-type-checks",
                                      llvm::cl::init(true), llvm::cl::Hidden);

/// A marker Clang declared: by its name, or by the name an `asm` label gives
/// it (`\01` first, so that no prefix is added).
static bool isMarker(const llvm::Function *callee, llvm::StringRef name) {
  if (callee == nullptr)
    return false;
  llvm::StringRef own = callee->getName();
  own.consume_front("\1");
  return own == name;
}

llvm::Constant *layoutGlobal(ModuleContext &module, llvm::StringRef key) {
  if (module.options.safe == nullptr || key.empty())
    return nullptr;
  const auto found = module.options.safe->layouts.find(key);
  if (found == module.options.safe->layouts.end())
    return nullptr;
  const SafeUnit::TypeLayout &layout = found->second;
  const std::string symbol =
      "__weavec.layout." + llvm::utohexstr(layout.identity);
  if (llvm::GlobalVariable *existing = module.module.getNamedGlobal(symbol))
    return existing;
  llvm::LLVMContext &context = module.context;
  auto *partType = llvm::StructType::get(
      context, {module.i64, module.i32, module.i32, module.i32, module.i32});
  auto *partsType = llvm::ArrayType::get(partType, layout.parts.size());
  auto *type =
      llvm::StructType::get(context, {module.i64, module.i64, module.i64,
                                      module.i32, module.i32, partsType});
  auto *global = new llvm::GlobalVariable(
      module.module, type, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, nullptr, symbol);
  global->setAlignment(llvm::Align(8));
  std::vector<llvm::Constant *> parts;
  parts.reserve(layout.parts.size());
  for (const SafeUnit::TypeLayout::Part &part : layout.parts)
    parts.push_back(llvm::ConstantStruct::get(
        partType, {llvm::ConstantInt::get(module.i64, part.identity),
                   llvm::ConstantInt::get(module.i32, part.offset),
                   llvm::ConstantInt::get(module.i32, part.stride),
                   llvm::ConstantInt::get(module.i32, part.elements),
                   llvm::ConstantInt::get(module.i32, part.flags)}));
  global->setInitializer(llvm::ConstantStruct::get(
      type, {llvm::ConstantInt::get(module.i64, layout.identity),
             llvm::ConstantInt::get(module.i64, layout.size),
             module.relative(module.text(layout.display), global, {0, 2}),
             llvm::ConstantInt::get(module.i32, layout.flags),
             llvm::ConstantInt::get(module.i32,
                                    static_cast<std::uint64_t>(parts.size())),
             llvm::ConstantArray::get(partsType, parts)}));
  return global;
}

/// The record of an allocation site's type (RFC 0045 §2, as amended): its
/// pool's key and cache, its layout and the runtime's index of it.
llvm::GlobalVariable *typeRecord(ModuleContext &module, std::uint64_t key,
                                 llvm::StringRef layoutKey) {
  auto *recordType = llvm::StructType::get(
      module.context, {module.i64, module.i64, module.ptr, module.i64});
  llvm::Constant *layout = layoutGlobal(module, layoutKey);
  auto *record = new llvm::GlobalVariable(
      module.module, recordType, /*isConstant=*/false,
      llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          recordType,
          {llvm::ConstantInt::get(module.i64, key),
           llvm::ConstantInt::get(module.i64, 0),
           layout != nullptr ? layout
                             : llvm::ConstantPointerNull::get(module.ptr),
           llvm::ConstantInt::get(module.i64, 0)}),
      "__weavec.type");
  record->setAlignment(llvm::Align(16));
  return record;
}

/// `ptr __weavec.object(ptr returned, i64 size, i64 extent)`: one object of
/// `size` bytes, among `extent` bytes of objects of its type (amendment 22).
static llvm::FunctionCallee objectMarker(ModuleContext &module) {
  llvm::Function *existing = module.module.getFunction(ObjectMarker);
  if (existing != nullptr)
    return existing;
  auto *type = llvm::FunctionType::get(
      module.ptr, {module.ptr, module.i64, module.i64}, false);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, ObjectMarker, module.module);
  function->setDoesNotThrow();
  function->setWillReturn();
  function->setDoesNotAccessMemory();
  function->addParamAttr(0, llvm::Attribute::Returned);
  return function;
}

/// The instruction that follows `call` once its markers are gone.
static void eraseMarker(llvm::CallInst &call, llvm::Value *replacement) {
  call.replaceAllUsesWith(replacement);
  call.eraseFromParent();
}

namespace {

class Expansion {
public:
  Expansion(ModuleContext &module, llvm::Function &function)
      : module(module), function(function), unit(*module.options.safe) {}

  void run() {
    std::vector<llvm::CallInst *> casts;
    std::vector<llvm::CallInst *> members;
    std::vector<llvm::CallInst *> tags;
    for (llvm::Instruction &instruction : llvm::instructions(function))
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
        const llvm::Function *callee = call->getCalledFunction();
        if (isMarker(callee, CastMarker))
          casts.push_back(call);
        else if (isMarker(callee, MemberMarker))
          members.push_back(call);
        else if (isMarker(callee, TagMarker))
          tags.push_back(call);
      }
    if (!typeChecks) {
      for (const auto &calls : {casts, members, tags})
        for (llvm::CallInst *call : calls)
          eraseMarker(*call, call->getArgOperand(0));
      return;
    }
    // The stores of a tag are found from its marker, before the others
    // change the blocks.
    for (llvm::CallInst *call : tags)
      tag(*call);
    for (llvm::CallInst *call : members)
      member(*call);
    for (llvm::CallInst *call : casts)
      cast(*call);
  }

private:
  ModuleContext &module;
  llvm::Function &function;
  const SafeUnit &unit;

  /// The marker's index, when it names a record.
  static std::optional<std::uint64_t> indexOf(const llvm::CallInst &call,
                                              std::size_t count) {
    const auto *index =
        llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(1));
    if (index == nullptr || index->getZExtValue() >= count)
      return std::nullopt;
    return index->getZExtValue();
  }

  /// A cold branch, taken when `condition` holds, before `before`.
  static llvm::Instruction *coldIf(llvm::Value *condition,
                                   llvm::Instruction *before) {
    llvm::MDBuilder weights(before->getContext());
    return llvm::SplitBlockAndInsertIfThen(
        condition, before->getIterator(),
        /*Unreachable=*/false, weights.createUnlikelyBranchWeights());
  }

  /// §2.2.
  void cast(llvm::CallInst &call) {
    llvm::Value *pointer = call.getArgOperand(0);
    const auto index = indexOf(call, unit.casts.size());
    if (!index) {
      eraseMarker(call, pointer);
      return;
    }
    const SafeUnit::CheckedCast &record = unit.casts[*index];
    llvm::Constant *layout = layoutGlobal(module, record.type);
    const auto found = unit.layouts.find(record.type);
    if (layout == nullptr || found == unit.layouts.end()) {
      eraseMarker(call, pointer);
      return;
    }
    const std::uint64_t size = found->second.size;
    llvm::IRBuilder<> builder(&call);
    const llvm::DebugLoc location = call.getDebugLoc();
    builder.SetCurrentDebugLocation(location);
    llvm::Value *isNull = builder.CreateIsNull(pointer);
    // The object's start: null for a null pointer.
    llvm::Value *object =
        record.delta == 0
            ? pointer
            : builder.CreateSelect(
                  isNull, llvm::ConstantPointerNull::get(module.ptr),
                  builder.CreateGEP(module.i8, pointer,
                                    llvm::ConstantInt::getSigned(
                                        module.i64, -static_cast<std::int64_t>(
                                                        record.delta))));
    llvm::Constant *site = module.site(location, 0);
    llvm::Constant *one = llvm::ConstantInt::get(module.i64, size);
    // Amendment 22: the bytes of objects of the type from the object's start,
    // as the runtime's records show them; one object where nothing asks them.
    llvm::Value *extent = one;
    {
      llvm::BasicBlock *head = call.getParent();
      llvm::Instruction *then = llvm::SplitBlockAndInsertIfThen(
          builder.CreateNot(isNull), call.getIterator(), false);
      llvm::IRBuilder<> check(then);
      check.SetCurrentDebugLocation(location);
      // (A class whose type information the unit does not have: the
      // runtime's records answer, as for a class without virtual functions.)
      if (record.polymorphic &&
          module.module.getNamedGlobal(record.derivedInfo) != nullptr) {
        llvm::Constant *derived =
            module.module.getOrInsertGlobal(record.derivedInfo, module.i8);
        llvm::Constant *base =
            module.module.getOrInsertGlobal(record.baseInfo, module.i8);
        // Through the source's own vtable (the pointer converted is an
        // object of its class): the complete object it is part of is exactly
        // the class, and starts where the conversion puts it.
        llvm::Value *table =
            check.CreateAlignedLoad(module.ptr, pointer, llvm::Align(8));
        llvm::Value *information = check.CreateAlignedLoad(
            module.ptr,
            check.CreateGEP(module.i8, table,
                            llvm::ConstantInt::getSigned(module.i64, -8)),
            llvm::Align(8));
        llvm::Value *top = check.CreateAlignedLoad(
            module.i64,
            check.CreateGEP(module.i8, table,
                            llvm::ConstantInt::getSigned(module.i64, -16)),
            llvm::Align(8));
        llvm::Value *start = check.CreateGEP(module.i8, pointer, top);
        llvm::Instruction *slow =
            coldIf(check.CreateOr(check.CreateICmpNE(information, derived),
                                  check.CreateICmpNE(start, object)),
                   then);
        llvm::IRBuilder<> other(slow);
        other.SetCurrentDebugLocation(location);
        llvm::FunctionCallee dynamic = module.module.getOrInsertFunction(
            "__dynamic_cast", module.ptr, module.ptr, module.ptr, module.ptr,
            module.i64);
        llvm::Value *result = other.CreateCall(
            dynamic, {pointer, base, derived,
                      llvm::ConstantInt::getSigned(module.i64, -1)});
        llvm::Instruction *fail =
            coldIf(other.CreateICmpNE(result, object), slow);
        llvm::IRBuilder<> report(fail);
        report.SetCurrentDebugLocation(location);
        report.CreateCall(
            module.runtime("cast_fail",
                           llvm::FunctionType::get(
                               llvm::Type::getVoidTy(module.context),
                               {module.ptr, module.ptr, module.ptr}, false)),
            {object, layout, site});
      } else {
        llvm::Value *bytes = check.CreateCall(
            module.runtime(
                "cast",
                llvm::FunctionType::get(
                    module.i64, {module.ptr, module.ptr, module.ptr}, false)),
            {object, layout, site});
        llvm::IRBuilder<> join(&call.getParent()->front());
        llvm::PHINode *phi = join.CreatePHI(module.i64, 2);
        phi->addIncoming(one, head);
        phi->addIncoming(bytes, then->getParent());
        extent = phi;
      }
    }
    builder.SetInsertPoint(&call);
    llvm::Value *checked =
        builder.CreateCall(objectMarker(module), {object, one, extent});
    // What the code had: the pointer it converted, now derived from the
    // checked object.
    llvm::Value *given =
        record.delta == 0
            ? checked
            : builder.CreateSelect(
                  isNull, llvm::ConstantPointerNull::get(module.ptr),
                  builder.CreateGEP(
                      module.i8, checked,
                      llvm::ConstantInt::get(module.i64, record.delta)));
    if (module.options.ledger)
      ++module.options.ledger->checkedCasts;
    eraseMarker(call, given);
  }

  /// A private constant of the member's names, for `union-member`.
  llvm::Constant *memberRecord(const SafeUnit::TaggedAccess &access) {
    auto *type = llvm::StructType::get(
        module.context, {module.i64, module.i64, module.i32, module.i32});
    auto *global = new llvm::GlobalVariable(
        module.module, type, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, nullptr, "__weavec.member");
    global->setAlignment(llvm::Align(8));
    global->setInitializer(llvm::ConstantStruct::get(
        type, {module.relative(module.text(access.member), global, {0, 0}),
               module.relative(module.text(access.tag), global, {0, 1}),
               llvm::ConstantInt::get(module.i32, access.write ? 1 : 0),
               llvm::ConstantInt::get(module.i32, 0)}));
    return global;
  }

  /// The accesses of memory through `root`, through address arithmetic:
  /// loads, stores, and the memory intrinsics.
  static std::vector<llvm::Instruction *> accessesOf(llvm::Value *root) {
    std::vector<llvm::Instruction *> out;
    std::vector<llvm::Value *> work{root};
    llvm::SmallPtrSet<llvm::Value *, 16> seen;
    while (!work.empty()) {
      llvm::Value *value = work.back();
      work.pop_back();
      if (!seen.insert(value).second)
        continue;
      for (llvm::User *user : value->users()) {
        auto *instruction = llvm::dyn_cast<llvm::Instruction>(user);
        if (instruction == nullptr)
          continue;
        if (llvm::isa<llvm::GetElementPtrInst, llvm::IntrinsicInst>(
                instruction) &&
            !llvm::isa<llvm::MemIntrinsic>(instruction)) {
          if (llvm::isa<llvm::GetElementPtrInst>(instruction) ||
              llvm::cast<llvm::IntrinsicInst>(instruction)->getIntrinsicID() ==
                  llvm::Intrinsic::ptr_annotation)
            work.push_back(instruction);
          continue;
        }
        if (llvm::isa<llvm::LoadInst, llvm::StoreInst, llvm::MemIntrinsic,
                      llvm::AtomicRMWInst, llvm::AtomicCmpXchgInst>(
                instruction))
          out.push_back(instruction);
      }
    }
    return out;
  }

  /// The comparison of the tag, before `before`.
  void compare(const SafeUnit::TaggedAccess &access, llvm::Value *structure,
               llvm::Instruction *before, const llvm::DebugLoc &location,
               llvm::Constant *record) {
    llvm::IRBuilder<> builder(before);
    builder.SetCurrentDebugLocation(location);
    auto *tagType = llvm::IntegerType::get(module.context, access.tagWidth * 8);
    llvm::Value *tag = builder.CreateAlignedLoad(
        tagType,
        builder.CreateGEP(module.i8, structure,
                          llvm::ConstantInt::get(module.i64, access.tagOffset)),
        llvm::Align(access.tagWidth));
    llvm::Value *masked =
        builder.CreateAnd(builder.CreateZExt(tag, module.i64),
                          llvm::ConstantInt::get(module.i64, access.mask));
    llvm::Value *ok = builder.getFalse();
    for (const std::uint64_t value : access.values)
      ok = builder.CreateOr(
          ok, builder.CreateICmpEQ(masked,
                                   llvm::ConstantInt::get(module.i64, value)));
    llvm::Instruction *fail = coldIf(builder.CreateNot(ok), before);
    llvm::IRBuilder<> report(fail);
    report.SetCurrentDebugLocation(location);
    report.CreateCall(
        module.runtime("union_fail",
                       llvm::FunctionType::get(
                           llvm::Type::getVoidTy(module.context),
                           {module.i64, module.ptr, module.ptr}, false)),
        {masked, record, module.site(location, 0)});
  }

  /// §3.3: the tag compared with the member's values, right before each
  /// access of the member (a call between the member's address and its
  /// access, which C may evaluate there, may store the tag).
  void member(llvm::CallInst &call) {
    llvm::Value *structure = call.getArgOperand(0);
    const auto index = indexOf(call, unit.taggedAccesses.size());
    if (!index) {
      eraseMarker(call, structure);
      return;
    }
    const SafeUnit::TaggedAccess &access = unit.taggedAccesses[*index];
    const llvm::DebugLoc location = call.getDebugLoc();
    llvm::Constant *record = memberRecord(access);
    std::vector<llvm::Instruction *> accesses = accessesOf(&call);
    // (The struct's own tag field is not the member's access.)
    if (accesses.empty())
      compare(access, structure, &call, location, record);
    for (llvm::Instruction *each : accesses)
      compare(access, structure, each, location, record);
    if (module.options.ledger)
      ++module.options.ledger->checkedMembers;
    eraseMarker(call, structure);
  }

  /// §3.3: a store of the tag that changes its value clears the union.
  void tag(llvm::CallInst &call) {
    llvm::Value *structure = call.getArgOperand(0);
    const auto index = indexOf(call, unit.tagStores.size());
    if (!index) {
      eraseMarker(call, structure);
      return;
    }
    const SafeUnit::TagStore &store = unit.tagStores[*index];
    auto *tagType = llvm::IntegerType::get(module.context, store.tagWidth * 8);
    std::vector<llvm::StoreInst *> stores;
    std::vector<std::pair<llvm::Value *, std::int64_t>> work{{&call, 0}};
    llvm::SmallPtrSet<llvm::Value *, 8> seen;
    while (!work.empty()) {
      const auto [value, offset] = work.back();
      work.pop_back();
      if (!seen.insert(value).second)
        continue;
      for (llvm::User *user : value->users()) {
        if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
            gep != nullptr && gep->getPointerOperand() == value) {
          llvm::APInt more(64, 0);
          if (gep->accumulateConstantOffset(module.layout, more))
            work.emplace_back(gep, offset + more.getSExtValue());
        } else if (auto *write = llvm::dyn_cast<llvm::StoreInst>(user);
                   write != nullptr && write->getPointerOperand() == value &&
                   std::cmp_equal(offset, store.tagOffset) &&
                   write->getValueOperand()->getType() == tagType) {
          stores.push_back(write);
        }
      }
    }
    eraseMarker(call, structure);
    for (llvm::StoreInst *write : stores) {
      llvm::IRBuilder<> builder(write);
      const llvm::DebugLoc location = write->getDebugLoc();
      builder.SetCurrentDebugLocation(location);
      llvm::Value *old = builder.CreateAlignedLoad(
          tagType, write->getPointerOperand(), llvm::Align(store.tagWidth));
      llvm::Instruction *next = write->getNextNode();
      builder.SetInsertPoint(next);
      llvm::Value *mask = llvm::ConstantInt::get(tagType, store.mask);
      llvm::Value *changed = builder.CreateICmpNE(
          builder.CreateAnd(old, mask),
          builder.CreateAnd(write->getValueOperand(), mask));
      llvm::Instruction *clear =
          llvm::SplitBlockAndInsertIfThen(changed, next->getIterator(), false);
      llvm::IRBuilder<> zero(clear);
      zero.SetCurrentDebugLocation(location);
      zero.CreateMemSet(
          zero.CreateGEP(module.i8, structure,
                         llvm::ConstantInt::get(module.i64, store.unionOffset)),
          llvm::ConstantInt::get(module.i8, 0), store.unionSize,
          llvm::MaybeAlign(1));
    }
  }
};

} // namespace

void expandTypeMarkers(ModuleContext &module, llvm::Function &function) {
  if (module.options.safe == nullptr || function.isDeclaration())
    return;
  Expansion(module, function).run();
}

void dropTypeMarkerDeclarations(const llvm::Module &ir) {
  for (const llvm::StringRef name : {CastMarker, MemberMarker, TagMarker})
    for (const std::string &symbol : {name.str(), "\1" + name.str()})
      if (llvm::Function *function = ir.getFunction(symbol);
          function != nullptr && function->use_empty())
        function->eraseFromParent();
}

void dropObjects(const llvm::Module &ir) {
  llvm::Function *marker = ir.getFunction(ObjectMarker);
  if (marker == nullptr)
    return;
  std::vector<llvm::CallInst *> calls;
  for (llvm::User *user : marker->users())
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(user))
      calls.push_back(call);
  for (llvm::CallInst *call : calls)
    eraseMarker(*call, call->getArgOperand(0));
  if (marker->use_empty())
    marker->eraseFromParent();
}

llvm::Value *checkedExtent(llvm::Value &value) {
  auto *call = llvm::dyn_cast<llvm::CallInst>(&value);
  if (call == nullptr || call->getCalledFunction() == nullptr ||
      call->getCalledFunction()->getName() != ObjectMarker)
    return nullptr;
  return call->getArgOperand(2);
}

std::optional<std::uint64_t> checkedObject(const llvm::Value &value) {
  const auto *call = llvm::dyn_cast<llvm::CallInst>(&value);
  if (call == nullptr || call->getCalledFunction() == nullptr ||
      call->getCalledFunction()->getName() != ObjectMarker)
    return std::nullopt;
  const auto *size = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
  return size != nullptr ? std::optional(size->getZExtValue()) : std::nullopt;
}

/// The text of a `llvm.var.annotation`.
static llvm::StringRef varAnnotation(const llvm::IntrinsicInst &call) {
  const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      call.getArgOperand(1)->stripPointerCasts());
  if (global == nullptr || !global->hasInitializer())
    return {};
  const auto *text =
      llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer());
  return text != nullptr && text->isCString() ? text->getAsCString()
                                              : llvm::StringRef();
}

void recordLocals(ModuleContext &module, llvm::Function &function) {
  std::vector<llvm::IntrinsicInst *> marks;
  for (llvm::Instruction &instruction : llvm::instructions(function))
    if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
        intrinsic != nullptr &&
        intrinsic->getIntrinsicID() == llvm::Intrinsic::var_annotation &&
        varAnnotation(*intrinsic) == RecordedAnnotation)
      marks.push_back(intrinsic);
  if (marks.empty())
    return;
  const SafeUnit *unit = module.options.safe.get();
  for (llvm::IntrinsicInst *mark : marks) {
    auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(
        mark->getArgOperand(0)->stripPointerCasts());
    std::int64_t index = -1;
    if (const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
            mark->getArgOperand(4)->stripPointerCasts());
        global != nullptr && global->hasInitializer())
      if (const auto *value = llvm::dyn_cast<llvm::ConstantInt>(
              global->getInitializer()->getAggregateElement(0U)))
        index = value->getSExtValue();
    mark->eraseFromParent();
    if (alloca == nullptr || unit == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= unit->recordedLocals.size() ||
        !(isSafe(module, function) || definesSafe(module, function)))
      continue;
    llvm::Constant *layout = layoutGlobal(
        module, unit->recordedLocals[static_cast<std::size_t>(index)]);
    if (layout == nullptr)
      continue;
    llvm::FunctionCallee push = module.runtime(
        "frame_push",
        llvm::FunctionType::get(module.i32,
                                {module.ptr, module.ptr, module.i64}, false));
    const std::uint64_t bytes = alloca->getAllocationSize(module.layout)
                                    .value_or(llvm::TypeSize::getFixed(0))
                                    .getFixedValue();
    llvm::FunctionCallee pop = module.runtime(
        "frame_pop",
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.context),
                                {module.i32}, false));
    // The depth to pop back to, kept beside the function's locals.
    llvm::IRBuilder<> entry(&*function.getEntryBlock().getFirstInsertionPt());
    llvm::AllocaInst *depth =
        entry.CreateAlloca(module.i32, nullptr, alloca->getName() + ".record");
    entry.CreateStore(llvm::ConstantInt::getAllOnesValue(module.i32), depth);
    std::vector<llvm::IntrinsicInst *> starts;
    std::vector<llvm::IntrinsicInst *> ends;
    for (llvm::User *user : alloca->users())
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user)) {
        if (intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start)
          starts.push_back(intrinsic);
        else if (intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end)
          ends.push_back(intrinsic);
      }
    const auto pushAt = [&](llvm::Instruction *after) {
      llvm::IRBuilder<> builder(after);
      builder.CreateStore(
          builder.CreateCall(push, {alloca, layout,
                                    llvm::ConstantInt::get(module.i64, bytes)}),
          depth);
    };
    const auto popAt = [&](llvm::Instruction *before) {
      llvm::IRBuilder<> builder(before);
      builder.CreateCall(pop, {builder.CreateLoad(module.i32, depth)});
    };
    if (starts.empty())
      pushAt(alloca->getNextNode() == depth
                 ? depth->getNextNode()->getNextNode()
                 : alloca->getNextNode());
    for (llvm::IntrinsicInst *start : starts)
      pushAt(start->getNextNode());
    for (llvm::IntrinsicInst *end : ends)
      popAt(end);
    for (llvm::BasicBlock &block : function)
      if (llvm::isa<llvm::ReturnInst, llvm::ResumeInst>(block.getTerminator()))
        popAt(block.getTerminator());
  }
}

void noteAllocations(ModuleContext &module, llvm::Module &ir) {
  if (module.options.safe == nullptr || fastUnit(module))
    return;
  // LLVM's token pass, which instruments the functions that carry this
  // attribute, does nothing: the metadata is for the record below.
  for (llvm::Function &function : ir)
    function.removeFnAttr(llvm::Attribute::SanitizeAllocToken);
  std::vector<llvm::CallBase *> calls;
  for (llvm::Function &function : ir)
    for (llvm::Instruction &instruction : llvm::instructions(function))
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
          call != nullptr &&
          call->hasMetadata(llvm::LLVMContext::MD_alloc_token))
        calls.push_back(call);
  for (llvm::CallBase *call : calls) {
    const llvm::MDNode *token =
        call->getMetadata(llvm::LLVMContext::MD_alloc_token);
    call->setMetadata(llvm::LLVMContext::MD_alloc_token, nullptr);
    const auto *name =
        token->getNumOperands() != 0
            ? llvm::dyn_cast<llvm::MDString>(token->getOperand(0))
            : nullptr;
    if (name == nullptr || !call->getType()->isPointerTy() ||
        !module.options.safe->layouts.contains(name->getString()))
      continue;
    // A block an array `new` puts a cookie in records no type: its
    // elements do not begin it.
    if (arrayNew(*call) &&
        module.options.safe->cookieTypes.contains(name->getString()))
      continue;
    llvm::GlobalVariable *record = typeRecord(
        module, typeKey(module, name->getString(), false), name->getString());
    llvm::Instruction *after = nullptr;
    if (auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
      llvm::BasicBlock *edge =
          llvm::SplitEdge(invoke->getParent(), invoke->getNormalDest());
      after = &*edge->getFirstInsertionPt();
    } else {
      after = call->getNextNode();
    }
    llvm::IRBuilder<> builder(after);
    builder.SetCurrentDebugLocation(call->getDebugLoc());
    builder.CreateCall(
        module.runtime("note_type", llvm::FunctionType::get(
                                        llvm::Type::getVoidTy(module.context),
                                        {module.ptr, module.ptr}, false)),
        {call, record});
  }
}

} // namespace weavec::frontend::guard
