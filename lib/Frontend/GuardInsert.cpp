//===- GuardInsert.cpp - Guards before the optimiser ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §2.1. Before the optimiser runs, every access gets a marker: a
// load, store or atomic operation a guard of its bytes, a memory intrinsic a
// guard or range of each operand, a masked or gathered lane a range of its
// element under its mask bit. An access to a stack or global object at a
// constant offset inside it gets none, and no ledger row: it names its
// object, and the object must stay promotable to registers. Accesses in
// unsafe regions get a row and no marker. GuardPrune removes, after
// inlining, the markers that have become such accesses.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"

namespace weavec::frontend::guard {

namespace {

/// One access before the optimiser.
struct Found {
  llvm::Instruction *instruction = nullptr;
  llvm::Value *pointer = nullptr;
  /// Constant bytes; 0 when `length` gives them at run time.
  std::uint64_t width = 0;
  llvm::Value *length = nullptr;
  llvm::Align alignment{1};
  bool write = false;
  llvm::StringRef operation;
  /// A lane: whether it runs (an i1), or null.
  llvm::Value *enabled = nullptr;
  /// A copy's source, for its disjointness (`operation` "disjoint").
  llvm::Value *source = nullptr;
};

} // namespace

/// Adds the accesses of `instruction` to `out`; a lane's address and
/// condition are computed before it.
static void collect(llvm::Instruction &instruction,
                    const llvm::DataLayout &layout, std::vector<Found> &out) {
  auto add = [&](llvm::Value *pointer, llvm::Type *type, llvm::Align alignment,
                 bool write, llvm::StringRef operation) {
    if (pointer->getType()->getPointerAddressSpace() != 0)
      return;
    const llvm::TypeSize size = layout.getTypeStoreSize(type);
    if (size.isScalable() || size.getFixedValue() == 0)
      return;
    out.push_back(Found{.instruction = &instruction,
                        .pointer = pointer,
                        .width = size.getFixedValue(),
                        .alignment = alignment,
                        .write = write,
                        .operation = operation});
  };
  // A volatile access to a constant address is a memory-mapped register's
  // (RFC 0035 section 2.1), which no object of the program holds.
  auto device = [](bool isVolatile, const llvm::Value *pointer) {
    return isVolatile && llvm::isa<llvm::Constant>(pointer) &&
           !llvm::isa<llvm::GlobalValue>(pointer->stripPointerCasts()) &&
           !llvm::isa<llvm::GlobalValue>(llvm::getUnderlyingObject(pointer));
  };
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    if (device(load->isVolatile(), load->getPointerOperand()))
      return;
    add(load->getPointerOperand(), load->getType(), load->getAlign(), false,
        "load");
    return;
  }
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    if (device(store->isVolatile(), store->getPointerOperand()))
      return;
    add(store->getPointerOperand(), store->getValueOperand()->getType(),
        store->getAlign(), true, "store");
    return;
  }
  if (auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
    add(rmw->getPointerOperand(), rmw->getValOperand()->getType(),
        rmw->getAlign(), true, "rmw");
    return;
  }
  if (auto *exchange = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction)) {
    add(exchange->getPointerOperand(), exchange->getNewValOperand()->getType(),
        exchange->getAlign(), true, "rmw");
    return;
  }
  auto range = [&](llvm::Value *pointer, llvm::Value *length,
                   llvm::MaybeAlign alignment, bool write,
                   llvm::StringRef operation) {
    if (pointer->getType()->getPointerAddressSpace() != 0)
      return;
    Found found{.instruction = &instruction,
                .pointer = pointer,
                .alignment = alignment.valueOrOne(),
                .write = write,
                .operation = operation};
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(length)) {
      if (constant->isZero())
        return;
      found.width = constant->getZExtValue();
    } else {
      found.length = length;
    }
    out.push_back(found);
  };
  if (auto *transfer = llvm::dyn_cast<llvm::MemTransferInst>(&instruction)) {
    // (A copy's operands must not overlap; the marker goes in first. An
    // aggregate assignment, which carries !tbaa.struct, copies whole
    // objects, which C lets overlap only exactly.)
    if (llvm::isa<llvm::MemCpyInst>(transfer) &&
        !transfer->hasMetadata(llvm::LLVMContext::MD_tbaa_struct))
      out.push_back(Found{.instruction = &instruction,
                          .pointer = transfer->getRawDest(),
                          .length = transfer->getLength(),
                          .operation = "disjoint",
                          .source = transfer->getRawSource()});
    range(transfer->getRawDest(), transfer->getLength(),
          transfer->getDestAlign(), true, "copy");
    range(transfer->getRawSource(), transfer->getLength(),
          transfer->getSourceAlign(), false, "copy");
    return;
  }
  if (auto *set = llvm::dyn_cast<llvm::MemSetInst>(&instruction)) {
    range(set->getRawDest(), set->getLength(), set->getDestAlign(), true,
          "set");
    return;
  }
  auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
  if (intrinsic == nullptr)
    return;
  // A lane is a range of its element's bytes, of length 0 when its mask bit
  // is clear.
  auto lanes = [&](llvm::Value *pointer, llvm::Value *pointers,
                   llvm::Value *mask, llvm::Type *vectorType, bool write) {
    auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(vectorType);
    if (vector == nullptr)
      return;
    const llvm::TypeSize size =
        layout.getTypeStoreSize(vector->getElementType());
    if (size.isScalable())
      return;
    llvm::IRBuilder<> builder(&instruction);
    llvm::Type *i64 = builder.getInt64Ty();
    for (unsigned lane = 0; lane < vector->getNumElements(); ++lane) {
      llvm::Value *at =
          pointers != nullptr
              ? builder.CreateExtractElement(pointers, lane)
              : builder.CreateConstGEP1_64(builder.getInt8Ty(), pointer,
                                           lane * size.getFixedValue());
      llvm::Value *enabled = builder.CreateExtractElement(mask, lane);
      Found found{.instruction = &instruction,
                  .pointer = at,
                  .length = builder.CreateSelect(
                      enabled,
                      llvm::ConstantInt::get(i64, size.getFixedValue()),
                      llvm::ConstantInt::get(i64, 0)),
                  .write = write,
                  .operation = "lane",
                  .enabled = enabled};
      out.push_back(found);
    }
  };
  switch (intrinsic->getIntrinsicID()) {
  case llvm::Intrinsic::masked_load:
    lanes(intrinsic->getArgOperand(0), nullptr, intrinsic->getArgOperand(1),
          intrinsic->getType(), false);
    break;
  case llvm::Intrinsic::masked_store:
    lanes(intrinsic->getArgOperand(1), nullptr, intrinsic->getArgOperand(2),
          intrinsic->getArgOperand(0)->getType(), true);
    break;
  case llvm::Intrinsic::masked_gather:
    lanes(nullptr, intrinsic->getArgOperand(0), intrinsic->getArgOperand(1),
          intrinsic->getType(), false);
    break;
  case llvm::Intrinsic::masked_scatter:
    lanes(nullptr, intrinsic->getArgOperand(1), intrinsic->getArgOperand(2),
          intrinsic->getArgOperand(0)->getType(), true);
    break;
  default:
    break;
  }
}

/// Whether `pointer` names a stack or global object, at a constant offset,
/// with `width` bytes inside it; the object, or null.
static const llvm::Value *namedObject(const llvm::Value *pointer,
                                      std::uint64_t width,
                                      const llvm::DataLayout &layout) {
  llvm::APInt offset(layout.getIndexTypeSizeInBits(pointer->getType()), 0);
  const llvm::Value *base = pointer->stripAndAccumulateConstantOffsets(
      layout, offset, /*AllowNonInbounds=*/true);
  if (offset.getSignificantBits() <= 63 &&
      insideAt(base, offset.getSExtValue(), width, layout))
    return base;
  return nullptr;
}

/// The base a guard tests for null (section 2.4): the pointer an index or a
/// large offset was added to; null when there is none or it names an
/// object.
static llvm::Value *nullBaseOf(llvm::Value *pointer,
                               const llvm::DataLayout &layout) {
  auto *gep = llvm::dyn_cast<llvm::GEPOperator>(pointer);
  if (gep == nullptr)
    return nullptr;
  llvm::APInt constant(layout.getIndexTypeSizeInBits(gep->getType()), 0);
  const bool variable = !gep->accumulateConstantOffset(layout, constant);
  if (!variable && constant.abs().ult(4096))
    return nullptr;
  llvm::Value *base = gep->getPointerOperand();
  if (llvm::isa<llvm::AllocaInst, llvm::GlobalValue>(base->stripPointerCasts()))
    return nullptr;
  return base;
}

void insertGuards(ModuleContext &module, llvm::Function &function) {
  std::vector<Found> found;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      collect(instruction, module.layout, found);
  for (const Found &access : found) {
    if (access.source != nullptr) {
      if (!module.isUnsafe(access.instruction->getDebugLoc()))
        module.insertDisjoint(access.instruction, access.pointer, access.source,
                              access.length);
      continue;
    }
    if (access.length == nullptr &&
        namedObject(access.pointer, access.width, module.layout) != nullptr)
      continue;
    const llvm::DebugLoc &location = access.instruction->getDebugLoc();
    if (module.isUnsafe(location)) {
      (void)module.row(function, location, access.operation, access.width,
                       LedgerRow::Outcome::Unguarded, "unsafe");
      continue;
    }
    const bool ranged = access.length != nullptr || access.width > 113;
    const unsigned row =
        module.row(function, location, access.operation, access.width,
                   LedgerRow::Outcome::Guarded, ranged ? "range" : "access");
    const unsigned flags = access.write ? MarkerWrite : 0;
    if (access.length != nullptr)
      module.insertRange(access.instruction, access.pointer, access.length,
                         flags, row);
    else
      module.insertGuard(access.instruction, access.pointer, access.width,
                         access.alignment, flags,
                         nullBaseOf(access.pointer, module.layout), row);
  }
  guardLibraryCalls(module, function, /*late=*/false);
  dropUnsafeBoundsChecks(module, function);
  addScopes(module, function);
}

void addScopes(ModuleContext &module, llvm::Function &function) {
  // A scope barrier after each lifetime.start and before each lifetime.end
  // that has none yet (the inliner adds lifetime markers).
  std::vector<llvm::IntrinsicInst *> lifetimes;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
        if (intrinsic->isLifetimeStartOrEnd())
          lifetimes.push_back(intrinsic);
  auto isScope = [](const llvm::Instruction *instruction) {
    const auto *call = llvm::dyn_cast_or_null<llvm::CallInst>(instruction);
    return call != nullptr && ModuleContext::isScope(*call);
  };
  for (llvm::IntrinsicInst *lifetime : lifetimes) {
    const bool start =
        lifetime->getIntrinsicID() == llvm::Intrinsic::lifetime_start;
    if (isScope(start ? lifetime->getNextNode() : lifetime->getPrevNode()))
      continue;
    llvm::Instruction *before = start ? lifetime->getNextNode() : lifetime;
    llvm::CallInst::Create(module.markerScope(), {}, "", before->getIterator());
  }
}

void dropScopes(llvm::Function &function, bool orphansOnly) {
  std::vector<llvm::CallInst *> scopes;
  for (llvm::BasicBlock &block : function) {
    // A barrier stays while its block keeps a lifetime marker: passes may
    // move other instructions between the two.
    const bool lifetimes =
        orphansOnly &&
        llvm::any_of(block, [](const llvm::Instruction &instruction) {
          const auto *intrinsic =
              llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
          return intrinsic != nullptr && intrinsic->isLifetimeStartOrEnd();
        });
    if (lifetimes)
      continue;
    for (llvm::Instruction &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (ModuleContext::isScope(*call))
          scopes.push_back(call);
  }
  for (llvm::CallInst *call : scopes)
    call->eraseFromParent();
}

void pruneGuards(ModuleContext &module, llvm::Function &function) {
  // The barriers of the allocas SROA promoted; those of the lifetime
  // markers inlining brought.
  dropScopes(function, /*orphansOnly=*/true);
  addScopes(module, function);
  std::vector<llvm::CallInst *> markers;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (ModuleContext::markerOf(*call) == MarkerGuard)
          markers.push_back(call);
  Scopes scopes;
  for (llvm::CallInst *call : markers) {
    const auto *width =
        llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
    const auto *flags =
        llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
    const auto *row = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(4));
    if (width == nullptr || flags == nullptr || row == nullptr)
      continue;
    const llvm::Value *object = namedObject(
        call->getArgOperand(0), width->getZExtValue(), module.layout);
    if (object == nullptr)
      continue;
    if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(object);
        alloca != nullptr && scopes.mayBeOutOfScope(*alloca, *call))
      continue;
    // Verify mode keeps the guard as a monitor of the proof.
    if (module.verify()) {
      call->setArgOperand(
          2, llvm::ConstantInt::get(module.i32,
                                    flags->getZExtValue() | MarkerProven));
      continue;
    }
    module.copy(static_cast<unsigned>(row->getZExtValue()), false, "in-bounds");
    call->eraseFromParent();
  }
}

} // namespace weavec::frontend::guard
