//===- GuardTyped.cpp - Typed functions' fields ---------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0042 §5, §6. A typed unit's checker gave each pointer field, and each
// field that counts one, an implicit annotation, which Clang emits at every
// access to the field (`llvm.ptr.annotation`). Before anything else runs,
// the annotations of typed functions are read here, by the address of the
// field the access computed, and every annotation is removed (it would
// keep the optimizer from merging accesses).
//
// Then, before GuardInsert, each typed function gets the reads its checks
// need, so that they are guarded as any access is where the struct's own
// pointer has no bounds:
//
//   - a counted pointer's load, its count, read beside it (the pair is a
//     snapshot: §5);
//   - a store of a counted pointer or of its count: the two stores of a
//     pair (§3, `counted-pair`) are found, and a store not in a pair reads
//     the count it is checked against (the current count, or the count's
//     old value, which a lone count may only decrease);
//   - a local whose type holds a pointer is zeroed whatever its size (§2).
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace weavec::frontend::guard {

/// The integers of an annotation's arguments (`@.args`, a constant struct).
static llvm::SmallVector<std::int64_t, 5>
annotationArguments(const llvm::IntrinsicInst &call) {
  llvm::SmallVector<std::int64_t, 5> values;
  const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      call.getArgOperand(4)->stripPointerCasts());
  if (global == nullptr || !global->hasInitializer())
    return values;
  const llvm::Constant *init = global->getInitializer();
  for (unsigned i = 0; i < init->getNumOperands(); ++i)
    if (const auto *integer =
            llvm::dyn_cast<llvm::ConstantInt>(init->getOperand(i)))
      values.push_back(integer->getSExtValue());
  return values;
}

/// The annotation's text: `weavec.field`, `weavec.count`, or another's.
static llvm::StringRef annotationName(const llvm::IntrinsicInst &call) {
  const auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      call.getArgOperand(1)->stripPointerCasts());
  if (global == nullptr || !global->hasInitializer())
    return {};
  const auto *text =
      llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer());
  return text != nullptr && text->isCString() ? text->getAsCString()
                                              : llvm::StringRef();
}

void readTypedFields(ModuleContext &module, llvm::Module &ir) {
  std::vector<llvm::IntrinsicInst *> calls;
  for (llvm::Function &function : ir)
    for (llvm::Instruction &instruction : llvm::instructions(function))
      if (auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
          call != nullptr &&
          call->getIntrinsicID() == llvm::Intrinsic::ptr_annotation) {
        const llvm::StringRef name = annotationName(*call);
        if (name == FieldAnnotation || name == CountAnnotation)
          calls.push_back(call);
      }
  for (llvm::IntrinsicInst *call : calls) {
    // Several annotations of one field chain: each names the field's
    // address, the first one's operand.
    llvm::Value *address = call->getArgOperand(0);
    while (const auto *inner = llvm::dyn_cast<llvm::IntrinsicInst>(address)) {
      if (inner->getIntrinsicID() != llvm::Intrinsic::ptr_annotation)
        break;
      address = inner->getArgOperand(0);
    }
    const auto values = annotationArguments(*call);
    if (isSafe(module, *call->getFunction())) {
      TypedField &field = module.typedState.fields[address];
      if (annotationName(*call) == FieldAnnotation && values.size() == 6) {
        field.field = {.kind = static_cast<FieldBounds::Kind>(values[0]),
                       .elementSize = values[1],
                       .countOffset = values[2],
                       .countWidth = values[3],
                       .flags = static_cast<std::uint64_t>(values[4]),
                       .pointee = values[5]};
        // A pointee the field's struct could not size: the unit's end did.
        const auto &later = module.options.safe->pointeeSizes;
        if (field.field.pointee > 0 &&
            static_cast<std::size_t>(field.field.pointee) <= later.size())
          field.field.elementSize = static_cast<std::int64_t>(
              later[static_cast<std::size_t>(field.field.pointee) - 1]);
      } else if (values.size() == 3) {
        field.count = true;
        field.counts.push_back(
            {.pointerOffset = values[0],
             .elementSize = values[1],
             .flags = static_cast<std::uint64_t>(values[2])});
      }
    }
  }
  for (llvm::IntrinsicInst *call : calls) {
    call->replaceAllUsesWith(call->getArgOperand(0));
    call->eraseFromParent();
  }
  // §9: a program with a safe function records no heap stacks by default.
  if (llvm::any_of(ir, [&](const llvm::Function &function) {
        return isSafe(module, function);
      }) &&
      ir.getFunction("weavec.typed") == nullptr) {
    auto *constructor = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false),
        llvm::GlobalValue::InternalLinkage, "weavec.typed", ir);
    llvm::IRBuilder<> builder(
        llvm::BasicBlock::Create(module.context, "entry", constructor));
    builder.CreateCall(module.runtime(
        "typed_program",
        llvm::FunctionType::get(llvm::Type::getVoidTy(module.context), false)));
    builder.CreateRetVoid();
    llvm::appendToGlobalCtors(ir, constructor, 65535);
  }
}

void typedFieldsOf(const ModuleContext &module, const llvm::Value *address,
                   llvm::SmallVectorImpl<const TypedField *> &found,
                   bool &other) {
  llvm::SmallVector<const llvm::Value *, 8> work{address};
  llvm::SmallPtrSet<const llvm::Value *, 8> seen{address};
  const auto &fields = module.typedState.fields;
  while (!work.empty()) {
    const llvm::Value *value = work.pop_back_val();
    if (const auto entry = fields.find(value); entry != fields.end()) {
      found.push_back(&entry->second);
      continue;
    }
    auto push = [&](const llvm::Value *next) {
      if (seen.insert(next).second)
        work.push_back(next);
    };
    if (const auto *gep = llvm::dyn_cast<llvm::GEPOperator>(value)) {
      const auto array = fields.find(gep->getPointerOperand());
      if (array != fields.end() && !array->second.count &&
          (array->second.field.flags & FieldBounds::Array) != 0) {
        found.push_back(&array->second);
        continue;
      }
      if (gep->hasAllZeroIndices()) {
        push(gep->getPointerOperand());
        continue;
      }
    } else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
      for (const llvm::Value *incoming : phi->incoming_values())
        push(incoming);
      continue;
    } else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
      push(select->getTrueValue());
      push(select->getFalseValue());
      continue;
    }
    other = true;
  }
}

const TypedField *typedFieldOf(const ModuleContext &module,
                               const llvm::Value *address) {
  llvm::SmallVector<const TypedField *, 2> found;
  bool other = false;
  typedFieldsOf(module, address, found, other);
  if (other || found.empty())
    return nullptr;
  for (const TypedField *field : found)
    if (field->count != found.front()->count ||
        field->field != found.front()->field ||
        field->counts != found.front()->counts)
      return nullptr;
  return found.front();
}

/// The integer of `width` bytes at `offset` from `address`, read before
/// `before` as an i64 (sign-extended when `signedCount`); a constant when
/// `width` is 0.
static llvm::Value *readCount(ModuleContext &module, llvm::Instruction *before,
                              llvm::Value *address, std::int64_t offset,
                              std::int64_t width, bool signedCount) {
  if (width == 0)
    return llvm::ConstantInt::get(module.i64,
                                  static_cast<std::uint64_t>(offset));
  if (width != 1 && width != 2 && width != 4 && width != 8)
    return nullptr;
  llvm::IRBuilder<> builder(before);
  builder.SetCurrentDebugLocation(before->getDebugLoc());
  llvm::Value *at = builder.CreateGEP(
      module.i8, address,
      llvm::ConstantInt::get(module.i64, static_cast<std::uint64_t>(offset)));
  llvm::LoadInst *count = builder.CreateLoad(
      llvm::IntegerType::get(module.context, static_cast<unsigned>(width * 8)),
      at, "weavec.count");
  count->setAlignment(llvm::Align(1));
  return signedCount ? builder.CreateSExtOrTrunc(count, module.i64)
                     : builder.CreateZExtOrTrunc(count, module.i64);
}

/// Whether an instruction between a pair's two stores leaves the pair
/// unseen: debug records, markers, lifetime and assumption intrinsics,
/// arithmetic, and loads but of the pointer (a typed load of the pointer
/// reads its count with it; the count read alone is an integer).
static bool sameStruct(const llvm::Value *a, const llvm::Value *b,
                       const llvm::DataLayout &layout, unsigned depth = 0);

static bool keepsPair(const llvm::Instruction &instruction,
                      const llvm::Value *base, std::int64_t pointerAt,
                      const llvm::DataLayout &layout) {
  if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
    if (ModuleContext::markerOf(*call).has_value() ||
        ModuleContext::isScope(*call))
      return true;
    const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call);
    return intrinsic != nullptr && (intrinsic->isDebugOrPseudoInst() ||
                                    intrinsic->isLifetimeStartOrEnd() ||
                                    intrinsic->isAssumeLikeIntrinsic());
  }
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    llvm::APInt offset(64, 0);
    const llvm::Value *from =
        load->getPointerOperand()->stripAndAccumulateConstantOffsets(
            layout, offset, /*AllowNonInbounds=*/true);
    return !sameStruct(from, base, layout) ||
           offset.getSExtValue() != pointerAt;
  }
  return !instruction.mayWriteToMemory() && !instruction.isTerminator();
}

/// The store that pairs with `first` (§6): of the other field of its pair,
/// to the same struct, after it with nothing between that reads either
/// field or calls a function, through branches whose other side traps.
/// Whether `a` and `b` are the same struct's address: one value, or loads
/// of one place (`s->strm` read for each statement of a pair).
static bool sameStruct(const llvm::Value *a, const llvm::Value *b,
                       const llvm::DataLayout &layout, unsigned depth) {
  if (a == b)
    return true;
  const auto *x = llvm::dyn_cast<llvm::LoadInst>(a);
  const auto *y = llvm::dyn_cast<llvm::LoadInst>(b);
  if (x == nullptr || y == nullptr || depth > 4 || x->getType() != y->getType())
    return false;
  llvm::APInt left(64, 0);
  llvm::APInt right(64, 0);
  const llvm::Value *from =
      x->getPointerOperand()->stripAndAccumulateConstantOffsets(
          layout, left, /*AllowNonInbounds=*/true);
  const llvm::Value *to =
      y->getPointerOperand()->stripAndAccumulateConstantOffsets(
          layout, right, /*AllowNonInbounds=*/true);
  return left == right && sameStruct(from, to, layout, depth + 1);
}

/// Where both arms of a branch meet (an `if` or a `?:`), and the arms
/// passed on the way; null when the shape is another.
static llvm::BasicBlock *
joinOf(const llvm::Instruction &branch,
       llvm::SmallVectorImpl<llvm::BasicBlock *> &arms) {
  if (branch.getNumSuccessors() != 2)
    return nullptr;
  llvm::BasicBlock *first = branch.getSuccessor(0);
  llvm::BasicBlock *second = branch.getSuccessor(1);
  auto via = [](llvm::BasicBlock *arm) -> llvm::BasicBlock * {
    return arm->getSinglePredecessor() != nullptr &&
                   llvm::isa<llvm::UncondBrInst>(arm->getTerminator())
               ? arm->getTerminator()->getSuccessor(0)
               : nullptr;
  };
  if (via(first) != nullptr && via(first) == via(second)) {
    arms.append({first, second});
    return via(first);
  }
  if (via(first) == second) {
    arms.push_back(first);
    return second;
  }
  if (via(second) == first) {
    arms.push_back(second);
    return first;
  }
  return nullptr;
}

/// The store that pairs with `first` (§6): of the other field of its pair,
/// to the same struct, after it with nothing between that reads the
/// pointer or calls a function, through branches whose other side traps
/// and through the arms of an `if` or a `?:` that meet again.
static llvm::StoreInst *
pairOf(const ModuleContext &module, llvm::StoreInst &first,
       std::int64_t otherOffset, bool firstIsPointer,
       llvm::SmallVectorImpl<llvm::LoadInst *> *pointerLoads = nullptr) {
  const llvm::DataLayout &layout = module.layout;
  llvm::APInt offset(64, 0);
  const llvm::Value *base =
      first.getPointerOperand()->stripAndAccumulateConstantOffsets(
          layout, offset, /*AllowNonInbounds=*/true);
  const std::int64_t at = offset.getSExtValue();
  const std::int64_t want = at + otherOffset;
  const std::int64_t pointerAt = firstIsPointer ? at : want;
  llvm::Instruction *next = first.getNextNode();
  for (unsigned steps = 0; next != nullptr && steps < 128; ++steps) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(next)) {
      llvm::APInt other(64, 0);
      const llvm::Value *to =
          store->getPointerOperand()->stripAndAccumulateConstantOffsets(
              layout, other, /*AllowNonInbounds=*/true);
      const bool same = sameStruct(to, base, layout);
      if (same && other.getSExtValue() == want &&
          typedFieldOf(module, store->getPointerOperand()) != nullptr)
        return store;
      // A store to the pair's other field, or to where its struct's
      // address was read, ends the search.
      if ((same &&
           (other.getSExtValue() == at || other.getSExtValue() == want)) ||
          (llvm::isa<llvm::LoadInst>(base) &&
           store->getPointerOperand() ==
               llvm::cast<llvm::LoadInst>(base)->getPointerOperand()))
        return nullptr;
      next = next->getNextNode();
      continue;
    }
    if (llvm::isa<llvm::CondBrInst, llvm::UncondBrInst>(next)) {
      llvm::BasicBlock *onward = nullptr;
      for (llvm::BasicBlock *successor : llvm::successors(next))
        if (!llvm::isa<llvm::UnreachableInst>(successor->getTerminator())) {
          if (onward != nullptr) {
            onward = nullptr;
            break;
          }
          onward = successor;
        }
      if (onward != nullptr && onward->getSinglePredecessor() != nullptr) {
        next = &onward->front();
        continue;
      }
      // A `?:` or an `if` between the two: arms that write nothing.
      llvm::SmallVector<llvm::BasicBlock *, 2> arms;
      llvm::BasicBlock *join = joinOf(*next, arms);
      if (join == nullptr ||
          !llvm::all_of(arms, [&](const llvm::BasicBlock *arm) {
            return llvm::all_of(*arm, [&](const llvm::Instruction &inner) {
              return inner.isTerminator() ||
                     (!llvm::isa<llvm::StoreInst>(inner) &&
                      keepsPair(inner, base, pointerAt, layout));
            });
          }))
        return nullptr;
      next = &join->front();
      continue;
    }
    // After the count, a load of the pointer (`n -= k; p += k`) reads it
    // with the count's old value (prepareTyped).
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(next);
        load != nullptr && !firstIsPointer && pointerLoads != nullptr &&
        !keepsPair(*next, base, want, layout)) {
      pointerLoads->push_back(load);
      next = next->getNextNode();
      continue;
    }
    if (!llvm::isa<llvm::PHINode>(next) &&
        !keepsPair(*next, base, pointerAt, layout))
      return nullptr;
    next = next->getNextNode();
  }
  return nullptr;
}

/// §2: a local whose type holds a pointer, larger than Clang's zero
/// initialization covers, is zeroed where each of its scopes starts.
static bool holdsPointer(llvm::Type *type) {
  if (type->isPointerTy())
    return true;
  if (const auto *array = llvm::dyn_cast<llvm::ArrayType>(type))
    return holdsPointer(array->getElementType());
  if (const auto *vector = llvm::dyn_cast<llvm::VectorType>(type))
    return holdsPointer(vector->getElementType());
  if (const auto *record = llvm::dyn_cast<llvm::StructType>(type))
    return llvm::any_of(record->elements(), holdsPointer);
  return false;
}

static void zeroPointerLocals(const ModuleContext &module,
                              llvm::Function &function) {
  for (llvm::Instruction &instruction : function.getEntryBlock()) {
    auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    if (alloca == nullptr || !alloca->isStaticAlloca() ||
        !holdsPointer(alloca->getAllocatedType()))
      continue;
    const auto size = alloca->getAllocationSize(module.layout);
    // Clang zeroes the smaller ones (-fweavec-zero-init).
    if (!size.has_value() || size->getFixedValue() <= 4096)
      continue;
    llvm::SmallVector<llvm::Instruction *, 2> starts;
    for (llvm::User *user : alloca->users())
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user);
          intrinsic != nullptr &&
          intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start)
        starts.push_back(intrinsic->getNextNode());
    if (starts.empty()) {
      llvm::Instruction *at = alloca->getNextNode();
      while (llvm::isa<llvm::AllocaInst>(at))
        at = at->getNextNode();
      starts.push_back(at);
    }
    for (llvm::Instruction *at : starts) {
      llvm::IRBuilder<> builder(at);
      builder.CreateMemSet(alloca, llvm::ConstantInt::get(module.i8, 0),
                           size->getFixedValue(), alloca->getAlign());
    }
  }
}

void prepareTyped(ModuleContext &module, llvm::Function &function) {
  TypedState &state = module.typedState;
  zeroPointerLocals(module, function);
  std::vector<llvm::LoadInst *> loads;
  std::vector<llvm::StoreInst *> stores;
  for (llvm::Instruction &instruction : llvm::instructions(function)) {
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        load != nullptr && load->getType()->isPointerTy())
      loads.push_back(load);
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
      stores.push_back(store);
  }
  for (llvm::LoadInst *load : loads) {
    const TypedField *field = typedFieldOf(module, load->getPointerOperand());
    if (field == nullptr || field->count ||
        field->field.kind != FieldBounds::Kind::Counted)
      continue;
    if (llvm::Value *count =
            readCount(module, load->getNextNode(), load->getPointerOperand(),
                      field->field.countOffset, field->field.countWidth,
                      (field->field.flags & FieldBounds::CountSigned) != 0))
      state.countAt[load] = count;
  }
  for (llvm::StoreInst *store : stores) {
    const TypedField *field = typedFieldOf(module, store->getPointerOperand());
    if (field == nullptr || state.pairs.contains(store))
      continue;
    if (!field->count && field->field.kind == FieldBounds::Kind::Counted &&
        field->field.countWidth != 0) {
      if (llvm::StoreInst *other =
              pairOf(module, *store, field->field.countOffset, true)) {
        state.pairs[store] = other;
        state.pairs[other] = store;
        state.seconds.insert(other);
        continue;
      }
    } else if (field->count && field->counts.size() == 1) {
      llvm::SmallVector<llvm::LoadInst *, 2> between;
      if (llvm::StoreInst *other =
              pairOf(module, *store, field->counts.front().pointerOffset, false,
                     &between)) {
        state.pairs[store] = other;
        state.pairs[other] = store;
        state.seconds.insert(other);
        const CountBounds &count = field->counts.front();
        if (!between.empty())
          if (llvm::Value *old = readCount(
                  module, store, store->getPointerOperand(), 0,
                  static_cast<std::int64_t>(module.layout.getTypeStoreSize(
                      store->getValueOperand()->getType())),
                  (count.flags & FieldBounds::CountSigned) != 0))
            for (llvm::LoadInst *load : between)
              state.countAt[load] = old;
        continue;
      }
    }
  }
  for (llvm::StoreInst *store : stores) {
    const TypedField *field = typedFieldOf(module, store->getPointerOperand());
    if (field == nullptr || state.pairs.contains(store))
      continue;
    if (!field->count && field->field.kind == FieldBounds::Kind::Counted) {
      // A pointer stored alone: against the current count.
      if (llvm::Value *count =
              readCount(module, store, store->getPointerOperand(),
                        field->field.countOffset, field->field.countWidth,
                        (field->field.flags & FieldBounds::CountSigned) != 0))
        state.countAt[store] = count;
    } else if (field->count &&
               store->getValueOperand()->getType()->isIntegerTy()) {
      // A count stored alone: its old value, which it may not exceed.
      const bool signedCount =
          (field->counts.front().flags & FieldBounds::CountSigned) != 0;
      if (llvm::Value *old = readCount(
              module, store, store->getPointerOperand(), 0,
              static_cast<std::int64_t>(module.layout.getTypeStoreSize(
                  store->getValueOperand()->getType())),
              signedCount))
        state.oldCount[store] = old;
    }
  }
}

} // namespace weavec::frontend::guard
