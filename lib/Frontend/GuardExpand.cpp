//===- GuardExpand.cpp - Guards after the optimizer -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §2.3, §6. After the optimizer, each marker that is left is an
// access to guard. The rules of §6 remove the guards a local fact makes
// redundant; the frame is laid out for the stack objects the remaining
// guards can reach (GuardFrames.cpp); each remaining guard becomes an
// inline check of the shadow with a call of the runtime's slow path, and
// each string marker a call of the runtime's checked `strlen`. In verify
// mode a removed guard is emitted too, as a monitor of the rule. RFC 0039
// §6: a trap of the C++ library's checks becomes a library-assertion
// report.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>

namespace weavec::frontend::guard {

namespace {

/// One marker the optimizer left.
struct Access {
  llvm::CallInst *marker = nullptr;
  enum class Kind : std::uint8_t { Guard, Range, String };
  Kind kind = Kind::Guard;
  llvm::Value *pointer = nullptr;
  /// Constant bytes; 0 when `length` gives them at run time.
  std::uint64_t width = 0;
  /// A range's run-time length; a string's bound.
  llvm::Value *length = nullptr;
  llvm::Align alignment{1};
  bool write = false;
  /// Verify mode: a guard GuardPrune proved redundant.
  bool pruned = false;
  /// A string the call accepts as null.
  bool nullOk = false;
  llvm::Value *nullBase = nullptr;
  unsigned row = 0;

  enum class State : std::uint8_t { Guard, InBounds, Dominated, Merged };
  State state = State::Guard;

  /// The pointer stripped of constant offsets, and the offset.
  llvm::Value *base = nullptr;
  std::int64_t offset = 0;
  /// The bytes the guard covers from `base` once merged.
  std::int64_t guardOffset = 0;
  std::uint64_t guardWidth = 0;
  /// Merged: the access whose guard covers this one.
  std::size_t leader = 0;
  /// The object `pointer` is based on, if one is known.
  llvm::Value *object = nullptr;
  /// RFC 0037 §8: the pointer the address is computed from, its shadow
  /// byte, and whether its check is a monitor (verify mode, §8.1).
  llvm::Value *root = nullptr;
  llvm::Value *rootShadow = nullptr;
  bool rootIsAlloca = false;
  bool rootMonitor = false;

  [[nodiscard]] bool constant() const {
    return kind != Kind::String && length == nullptr;
  }
};

} // namespace

/// The access a marker stands for.
static std::optional<Access> accessOf(llvm::CallInst &call,
                                      llvm::StringRef marker) {
  Access access;
  access.marker = &call;
  access.pointer = call.getArgOperand(0);
  const auto *flags = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(2));
  if (flags == nullptr)
    return std::nullopt;
  const std::uint64_t bits = flags->getZExtValue();
  access.write = (bits & MarkerWrite) != 0;
  access.pruned = (bits & MarkerProven) != 0;
  access.alignment =
      llvm::Align(std::uint64_t{1} << ((bits >> MarkerAlignShift) & 63U));
  const unsigned rowArgument = marker == MarkerGuard ? 4 : 3;
  if (const auto *row =
          llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(rowArgument)))
    access.row = static_cast<unsigned>(row->getZExtValue());
  if (marker == MarkerString) {
    access.kind = Access::Kind::String;
    access.nullOk = (bits & MarkerNullOk) != 0;
    access.length = call.getArgOperand(1);
    return access;
  }
  access.kind =
      marker == MarkerGuard ? Access::Kind::Guard : Access::Kind::Range;
  if (const auto *width =
          llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(1)))
    access.width = width->getZExtValue();
  else
    access.length = call.getArgOperand(1);
  if (marker == MarkerGuard &&
      !llvm::isa<llvm::ConstantPointerNull>(call.getArgOperand(3)))
    access.nullBase = call.getArgOperand(3);
  return access;
}

//===----------------------------------------------------------------------===//
// Removing guards (section 6)
//===----------------------------------------------------------------------===//

/// Rule 6.1: the access lies inside its object at every execution, and a
/// local's scope holds it.
static bool inBounds(const Access &access, llvm::ScalarEvolution &evolution,
                     const llvm::DataLayout &layout, Scopes &scopes) {
  if (access.object == nullptr || access.kind == Access::Kind::String)
    return false;
  if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(access.object);
      alloca != nullptr && scopes.mayBeOutOfScope(*alloca, *access.marker))
    return false;
  const std::optional<std::uint64_t> size = exactSize(access.object, layout);
  if (!size)
    return false;
  // A constant offset from the object itself.
  if (access.base == access.object && access.length == nullptr)
    return insideAt(access.object, access.offset, access.width, layout);
  if (!evolution.isSCEVable(access.pointer->getType()))
    return false;
  const llvm::SCEV *pointer = evolution.getSCEV(access.pointer);
  const llvm::SCEV *object = evolution.getSCEV(access.object);
  const llvm::SCEV *offset = evolution.getMinusSCEV(pointer, object);
  if (llvm::isa<llvm::SCEVCouldNotCompute>(offset) ||
      !offset->getType()->isIntegerTy())
    return false;
  llvm::Type *type = offset->getType();
  const llvm::SCEV *zero = evolution.getZero(type);
  const llvm::SCEV *end = nullptr;
  if (access.length != nullptr) {
    if (!evolution.isSCEVable(access.length->getType()))
      return false;
    const llvm::SCEV *length = evolution.getTruncateOrZeroExtend(
        evolution.getSCEV(access.length), type);
    if (!evolution.isKnownPredicateAt(llvm::ICmpInst::ICMP_ULE, length,
                                      evolution.getConstant(type, *size),
                                      access.marker))
      return false;
    end = evolution.getAddExpr(offset, length);
  } else {
    if (access.width > *size)
      return false;
    end =
        evolution.getAddExpr(offset, evolution.getConstant(type, access.width));
  }
  return evolution.isKnownPredicateAt(llvm::ICmpInst::ICMP_SGE, offset, zero,
                                      access.marker) &&
         evolution.isKnownPredicateAt(llvm::ICmpInst::ICMP_SLE, end,
                                      evolution.getConstant(type, *size),
                                      access.marker);
}

/// Whether an instruction may release memory or change the shadow of an
/// object, so that a guard before it says nothing about an access after.
bool killsGuards(const llvm::Instruction &instruction) {
  const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
  if (call == nullptr || ModuleContext::markerOf(*call))
    return false;
  if (const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call)) {
    switch (intrinsic->getIntrinsicID()) {
    case llvm::Intrinsic::lifetime_end:
    case llvm::Intrinsic::lifetime_start:
    case llvm::Intrinsic::stackrestore:
      return true;
    default:
      return !intrinsic->hasFnAttr(llvm::Attribute::NoFree);
    }
  }
  if (const llvm::Function *callee = call->getCalledFunction();
      callee && ModuleContext::isRuntimeName(callee->getName()))
    return false;
  return !call->hasFnAttr(llvm::Attribute::NoFree);
}

/// Whether `access` is one rules 6.2 and 6.3 may remove or merge.
static bool removable(const Access &access) {
  return access.state == Access::State::Guard && access.constant() &&
         access.base != nullptr;
}

/// Rule 6.2: removes each guard whose bytes a guard of the same base that
/// is available (executed on every path to it with no kill since) covers.
static void removeDominated(llvm::Function &function,
                            std::vector<Access> &accesses) {
  std::vector<std::size_t> candidates;
  llvm::DenseMap<const llvm::Instruction *, unsigned> byMarker;
  llvm::DenseMap<const llvm::Value *, llvm::SmallVector<unsigned, 4>> byBase;
  for (std::size_t i = 0; i < accesses.size(); ++i) {
    const Access &access = accesses[i];
    if (!removable(access))
      continue;
    const auto index = static_cast<unsigned>(candidates.size());
    byMarker[access.marker] = index;
    byBase[access.base].push_back(index);
    candidates.push_back(i);
  }
  constexpr std::size_t MaxCandidates = 20000;
  if (candidates.empty() || candidates.size() > MaxCandidates ||
      function.size() > MaxCandidates)
    return;
  const auto n = static_cast<unsigned>(candidates.size());
  llvm::DenseMap<const llvm::BasicBlock *, llvm::BitVector> out;
  llvm::ReversePostOrderTraversal<llvm::Function *> order(&function);
  for (const llvm::BasicBlock *block : order)
    out[block] = llvm::BitVector(n, block != &function.getEntryBlock());
  auto covered = [&](const llvm::BitVector &available, unsigned candidate) {
    const Access &access = accesses[candidates[candidate]];
    for (const unsigned other : byBase[access.base]) {
      if (other == candidate || !available.test(other))
        continue;
      const Access &guard = accesses[candidates[other]];
      if (guard.offset <= access.offset &&
          access.offset + static_cast<std::int64_t>(access.width) <=
              guard.offset + static_cast<std::int64_t>(guard.width))
        return true;
    }
    return false;
  };
  auto transfer = [&](llvm::BasicBlock &block, llvm::BitVector set,
                      bool decide) {
    for (const llvm::Instruction &instruction : block) {
      if (killsGuards(instruction))
        set.reset();
      const auto found = byMarker.find(&instruction);
      if (found == byMarker.end())
        continue;
      if (decide && covered(set, found->second))
        accesses[candidates[found->second]].state = Access::State::Dominated;
      set.set(found->second);
    }
    return set;
  };
  auto in = [&](llvm::BasicBlock *block) {
    if (block == &function.getEntryBlock())
      return llvm::BitVector(n, false);
    llvm::BitVector set(n, true);
    bool any = false;
    for (const llvm::BasicBlock *predecessor : llvm::predecessors(block)) {
      const auto found = out.find(predecessor);
      if (found == out.end())
        continue;
      set &= found->second;
      any = true;
    }
    if (!any)
      set.reset();
    return set;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (llvm::BasicBlock *block : order) {
      llvm::BitVector next = transfer(*block, in(block), false);
      if (next != out[block]) {
        out[block] = std::move(next);
        changed = true;
      }
    }
  }
  for (llvm::BasicBlock *block : order)
    (void)transfer(*block, in(block), true);
}

/// Rule 6.3: the guards of one block on one base whose hull is at most 32
/// bytes, with nothing between them that may not reach the next, become one
/// guard of their hull.
static void mergeInBlocks(llvm::Function &function,
                          std::vector<Access> &accesses) {
  llvm::DenseMap<const llvm::Instruction *, std::size_t> byMarker;
  for (std::size_t i = 0; i < accesses.size(); ++i) {
    Access &access = accesses[i];
    access.guardOffset = access.offset;
    access.guardWidth = access.width;
    if (removable(access))
      byMarker[access.marker] = i;
  }
  constexpr std::int64_t MaxHull = 64;
  for (llvm::BasicBlock &block : function) {
    llvm::DenseMap<const llvm::Value *, std::size_t> open;
    for (llvm::Instruction &instruction : block) {
      const auto found = byMarker.find(&instruction);
      if (found != byMarker.end()) {
        Access &access = accesses[found->second];
        const auto opened = open.find(access.base);
        if (opened != open.end()) {
          Access &guard = accesses[opened->second];
          const std::int64_t low = std::min(guard.guardOffset, access.offset);
          const std::int64_t high = std::max(
              guard.guardOffset + static_cast<std::int64_t>(guard.guardWidth),
              access.offset + static_cast<std::int64_t>(access.width));
          if (high - low <= MaxHull) {
            guard.guardOffset = low;
            guard.guardWidth = static_cast<std::uint64_t>(high - low);
            access.state = Access::State::Merged;
            access.leader = opened->second;
            continue;
          }
        }
        open[access.base] = found->second;
        continue;
      }
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (killsGuards(instruction) ||
          ((call == nullptr || !ModuleContext::markerOf(*call)) &&
           !llvm::isa<llvm::LoadInst, llvm::StoreInst>(instruction) &&
           !llvm::isGuaranteedToTransferExecutionToSuccessor(&instruction)))
        open.clear();
    }
  }
}

/// Whether the alloca's address can reach code other than this function's
/// own accesses: stored, passed to a call, returned or converted.
static bool escapes(const llvm::AllocaInst &alloca) {
  llvm::SmallVector<const llvm::Value *, 8> work{&alloca};
  llvm::SmallPtrSet<const llvm::Value *, 16> seen{&alloca};
  while (!work.empty()) {
    const llvm::Value *value = work.pop_back_val();
    for (const llvm::Use &use : value->uses()) {
      const auto *user = llvm::cast<llvm::Instruction>(use.getUser());
      if (llvm::isa<llvm::GetElementPtrInst, llvm::BitCastInst,
                    llvm::AddrSpaceCastInst, llvm::PHINode, llvm::SelectInst>(
              user)) {
        if (seen.insert(user).second)
          work.push_back(user);
        continue;
      }
      if (llvm::isa<llvm::LoadInst, llvm::ICmpInst>(user))
        continue;
      if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
        if (store->getValueOperand() == value)
          return true;
        continue;
      }
      if (const auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(user)) {
        if (rmw->getValOperand() == value)
          return true;
        continue;
      }
      if (const auto *exchange =
              llvm::dyn_cast<llvm::AtomicCmpXchgInst>(user)) {
        if (exchange->getPointerOperand() != value)
          return true;
        continue;
      }
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(user)) {
        if (ModuleContext::markerOf(*call))
          continue;
        if (const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call);
            intrinsic &&
            (intrinsic->isLifetimeStartOrEnd() ||
             intrinsic->isDebugOrPseudoInst() ||
             llvm::isa<llvm::MemIntrinsic>(intrinsic) ||
             intrinsic->getIntrinsicID() == llvm::Intrinsic::assume))
          continue;
        return true;
      }
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Expansion
//===----------------------------------------------------------------------===//

/// The reason a ledger row gives for a removed guard.
static llvm::StringRef reasonOf(Access::State state) {
  switch (state) {
  case Access::State::InBounds:
    return "in-bounds";
  case Access::State::Dominated:
    return "dominated";
  case Access::State::Merged:
    return "merged";
  case Access::State::Guard:
    break;
  }
  return "access";
}

/// Emits the check of one marker (or its monitor in verify mode) and
/// removes the marker. `members` are the accesses a merged guard covers.
static void expand(FunctionContext &context, const Access &access,
                   llvm::ArrayRef<FunctionContext::Member> members = {}) {
  ModuleContext &module = context.module;
  llvm::CallInst *marker = access.marker;
  if (access.kind == Access::Kind::String) {
    llvm::IRBuilder<> builder(marker);
    if (access.rootShadow != nullptr) {
      context.colorCheck(
          marker, builder.CreatePtrToInt(access.pointer, module.i64),
          llvm::ConstantInt::get(module.i64, 1),
          module.site(marker->getDebugLoc(), 0),
          RootCheck{.root = access.root, .shadow = access.rootShadow});
      builder.SetInsertPoint(marker);
    }
    // The bound from the marker itself: it may be another string's length,
    // whose marker an earlier expansion replaced.
    llvm::Value *max = marker->getArgOperand(1);
    // A short bounded scan whose length nothing reads: its bytes all
    // addressable, inline; otherwise the runtime decides where it stops.
    const auto *bound = llvm::dyn_cast<llvm::ConstantInt>(max);
    if (marker->use_empty() && bound != nullptr && !bound->isZero() &&
        bound->getZExtValue() <= 113 && module.layout.isLittleEndian()) {
      context.chunkCheckString(
          marker, access.pointer, bound->getZExtValue(),
          module.site(marker->getDebugLoc(), access.nullOk ? SiteNullOk : 0));
      marker->eraseFromParent();
      return;
    }
    llvm::CallInst *length = builder.CreateCall(
        module.strlenFunction(),
        {access.pointer, max,
         module.site(marker->getDebugLoc(), access.nullOk ? SiteNullOk : 0)});
    length->setDebugLoc(marker->getDebugLoc());
    marker->replaceAllUsesWith(length);
    marker->eraseFromParent();
    return;
  }
  const bool removed = access.state != Access::State::Guard || access.pruned;
  if (!removed || module.verify()) {
    const unsigned flags =
        (access.write ? SiteWrite : 0) | (removed ? SiteProven : 0);
    llvm::Constant *site = module.site(marker->getDebugLoc(), flags);
    const RootCheck root =
        removed
            ? RootCheck{}
            : RootCheck{.root = access.root,
                        .shadow = access.rootShadow,
                        .monitorSite = access.rootMonitor
                                           ? module.site(marker->getDebugLoc(),
                                                         flags | SiteProven)
                                           : nullptr};
    if (access.length != nullptr) {
      if (!removed && access.rootShadow != nullptr) {
        llvm::IRBuilder<> builder(marker);
        context.colorCheck(marker,
                           builder.CreatePtrToInt(access.pointer, module.i64),
                           access.length, site, root);
      }
      context.emitRange(marker, access.pointer, access.length, site);
    } else if (access.state == Access::State::Guard &&
               (access.guardOffset != access.offset ||
                access.guardWidth != access.width)) {
      llvm::IRBuilder<> builder(marker);
      llvm::Value *hull = builder.CreateGEP(
          module.i8, access.base,
          llvm::ConstantInt::getSigned(module.i64, access.guardOffset));
      context.emitGuard(marker, hull, access.guardWidth, llvm::Align(1), site,
                        access.nullBase, members, root);
    } else {
      context.emitGuard(marker, access.pointer, access.width, access.alignment,
                        site, access.nullBase, {}, root);
    }
  }
  marker->eraseFromParent();
}

namespace {
/// The pointer an integer is computed from by `add`, `sub` and `or`: none,
/// one, or several (`p + (q - p)`, which has no root).
struct IntegerPointer {
  llvm::Value *pointer = nullptr;
  bool mixed = false;
};
} // namespace

static IntegerPointer pointerUnder(llvm::Value *integer, unsigned depth) {
  if (auto *cast = llvm::dyn_cast<llvm::PtrToIntOperator>(integer))
    return {.pointer = cast->getPointerOperand()};
  const auto *operation = llvm::dyn_cast<llvm::BinaryOperator>(integer);
  if (operation == nullptr)
    return {};
  const llvm::Instruction::BinaryOps opcode = operation->getOpcode();
  if (opcode != llvm::Instruction::Add && opcode != llvm::Instruction::Sub &&
      opcode != llvm::Instruction::Or)
    return {};
  // Deeper arithmetic may hold another pointer: no root (amendment 25).
  if (depth > 8)
    return {.mixed = true};
  const IntegerPointer left = pointerUnder(operation->getOperand(0), depth + 1);
  const IntegerPointer right =
      pointerUnder(operation->getOperand(1), depth + 1);
  // A pointer subtracted, or two added, is a distance, not an address.
  if (left.mixed || right.mixed || (left.pointer && right.pointer) ||
      (opcode == llvm::Instruction::Sub && right.pointer))
    return {.mixed = true};
  return left.pointer != nullptr ? left : right;
}

namespace {
/// One search for a root: the nodes being looked through, the phis and
/// selects whose root is known, how often an input came back to a node
/// being looked through, and how many more nodes it may look through.
struct RootSearch {
  llvm::SmallPtrSet<llvm::Value *, 8> visiting;
  RootCache *known = nullptr;
  unsigned cycles = 0;
  unsigned budget = 256;
};
} // namespace

/// RFC 0038 §3: the root under `value`: its getelementptrs and casts
/// stripped, through `inttoptr` of integer arithmetic on one pointer, and
/// through a phi or select whose inputs have one root (a loop-carried input
/// is the node itself). A phi or select whose inputs' roots differ is its own
/// root when each input is a pointer the program holds; when one is computed
/// from another root (an offset the optimizer hoisted, a match finder's
/// `dictBase - delta` beside `base`) it has none. Null for no root, and past
/// the search's budget. A node's root that does not depend on the nodes
/// being looked through is remembered (amendment 25).
static llvm::Value *rootUnder(llvm::Value *value, RootSearch &search) {
  unsigned steps = 0;
  for (; steps < 64; ++steps) {
    value = value->stripPointerCasts();
    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(value)) {
      value = gep->getPointerOperand();
      continue;
    }
    if (const auto *cast = llvm::dyn_cast<llvm::IntToPtrInst>(value)) {
      const IntegerPointer under = pointerUnder(cast->getOperand(0), 0);
      if (under.pointer == nullptr || under.mixed)
        return nullptr;
      value = under.pointer;
      continue;
    }
    break;
  }
  if (steps == 64)
    return nullptr;
  if (!llvm::isa<llvm::PHINode, llvm::SelectInst>(value))
    return value;
  if (search.visiting.contains(value)) {
    ++search.cycles;
    return value;
  }
  if (const auto found = search.known->find(value);
      found != search.known->end())
    return found->second;
  if (search.budget == 0)
    return nullptr;
  --search.budget;
  search.visiting.insert(value);
  const unsigned cycles = search.cycles;
  llvm::SmallVector<llvm::Value *, 4> inputs;
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value))
    inputs = {select->getTrueValue(), select->getFalseValue()};
  else
    inputs.append(llvm::cast<llvm::PHINode>(value)->incoming_values().begin(),
                  llvm::cast<llvm::PHINode>(value)->incoming_values().end());
  llvm::Value *common = nullptr;
  llvm::Value *cycle = nullptr;
  bool differ = false;
  bool derived = false;
  bool none = false;
  for (llvm::Value *input : inputs) {
    llvm::Value *root = rootUnder(input, search);
    if (root == nullptr) {
      none = true;
      break;
    }
    // An input that comes back to a node being looked through constrains
    // nothing.
    if (search.visiting.contains(root)) {
      cycle = root;
      continue;
    }
    derived = derived || root != input->stripPointerCasts();
    if (common == nullptr)
      common = root;
    else if (common != root)
      differ = true;
  }
  search.visiting.erase(value);
  llvm::Value *root = nullptr;
  if (none)
    root = nullptr;
  else if (common == nullptr)
    root = cycle;
  else if (!differ)
    root = common;
  else if (!derived)
    root = value;
  // (A budget spent below makes the answer this search's alone.)
  if (search.cycles == cycles && search.budget != 0)
    (*search.known)[value] = root;
  return root;
}

/// RFC 0037 §8.1, RFC 0038 §3: the pointer an address is computed from (its
/// root); null when the address is the pointer itself, or the root is not
/// a pointer the program holds.
llvm::Value *colorRoot(llvm::Value *base, RootCache *cache) {
  if (base == nullptr ||
      !llvm::isa<llvm::GEPOperator, llvm::IntToPtrInst>(base))
    return nullptr;
  RootCache own;
  RootSearch search;
  search.known = cache != nullptr ? cache : &own;
  llvm::Value *value = rootUnder(base, search);
  if (value == nullptr ||
      llvm::isa<llvm::ConstantPointerNull, llvm::IntToPtrInst, llvm::UndefValue,
                llvm::ConstantExpr, llvm::PoisonValue>(value))
    return nullptr;
  return value;
}

/// RFC 0037 §8.1: whether the offset moves by a small constant step from a
/// constant start near the root, so that it reaches the object's redzone,
/// where the guard stops it, before any other object; `anyStart` also takes
/// a variable start (which then may begin in another object).
static bool contiguousOffset(const Access &access, llvm::Value *root,
                             llvm::ScalarEvolution &evolution, bool anyStart) {
  if (!evolution.isSCEVable(access.pointer->getType()))
    return false;
  const llvm::SCEV *offset = evolution.getMinusSCEV(
      evolution.getSCEV(access.pointer), evolution.getSCEV(root));
  const auto *rec = llvm::dyn_cast<llvm::SCEVAddRecExpr>(offset);
  if (rec == nullptr || !rec->isAffine())
    return false;
  const auto *step = llvm::dyn_cast<llvm::SCEVConstant>(rec->getOperand(1));
  const auto *start = llvm::dyn_cast<llvm::SCEVConstant>(rec->getStart());
  return step != nullptr && step->getAPInt().abs().ule(32) &&
         (anyStart || (start != nullptr && start->getAPInt().abs().ule(4096)));
}

/// RFC 0037 §8.1, §8.2: finds each remaining guard's root, and loads each
/// root's shadow byte once, where it dominates the root's accesses and
/// outside any loop the root does not change in. A stale value is good: the
/// object is not reused while the root is held (the sweep sees it), and the
/// access's own shadow byte says whether it is still live. A walk its guard
/// stops at the object's redzone first has no color check; in verify mode
/// it has one as a monitor.
static void loadRootShadows(FunctionContext &context,
                            std::vector<Access> &accesses,
                            llvm::ScalarEvolution &evolution,
                            const llvm::DominatorTree &dominators,
                            const llvm::LoopInfo &loops) {
  const ModuleContext &module = context.module;
  if (!module.options.colors)
    return;
  llvm::DenseMap<llvm::Value *, llvm::SmallVector<std::size_t, 4>> byRoot;
  llvm::SmallVector<llvm::Value *, 8> order;
  // (Nothing below changes the IR until every root is found.)
  RootCache roots;
  for (std::size_t i = 0; i < accesses.size(); ++i) {
    Access &access = accesses[i];
    if (access.state != Access::State::Guard || access.pruned)
      continue;
    const bool string = access.kind == Access::Kind::String;
    llvm::Value *root =
        string
            ? colorRoot(access.pointer->stripInBoundsConstantOffsets(), &roots)
            : colorRoot(access.base, &roots);
    if (root == nullptr)
      continue;
    if (!string && contiguousOffset(access, root, evolution, false)) {
      if (!module.verify())
        continue;
      access.rootMonitor = true;
    }
    access.root = root;
    access.rootIsAlloca = llvm::isa<llvm::AllocaInst>(root);
    auto &list = byRoot[root];
    if (list.empty())
      order.push_back(root);
    list.push_back(i);
  }
  for (llvm::Value *root : order) {
    // Allocas: the frame's layout gives the color.
    if (llvm::isa<llvm::AllocaInst>(root))
      continue;
    const auto &list = byRoot[root];
    llvm::BasicBlock *block = accesses[list[0]].marker->getParent();
    for (const std::size_t i : list)
      block = dominators.findNearestCommonDominator(
          block, accesses[i].marker->getParent());
    const auto *definition = llvm::dyn_cast<llvm::Instruction>(root);
    const unsigned depth =
        definition != nullptr ? loops.getLoopDepth(definition->getParent()) : 0;
    while (loops.getLoopDepth(block) > depth) {
      const auto *node = dominators.getNode(block)->getIDom();
      if (node == nullptr)
        break;
      block = node->getBlock();
    }
    llvm::Instruction *at = block->getTerminator();
    for (llvm::Instruction &instruction : *block)
      if (llvm::any_of(list, [&](std::size_t i) {
            return accesses[i].marker == &instruction;
          })) {
        at = &instruction;
        break;
      }
    llvm::IRBuilder<> builder(at);
    llvm::Value *address = builder.CreatePtrToInt(root, module.i64);
    llvm::Value *shadow = builder.CreateSExt(
        builder.CreateLoad(module.i8, context.shadowAddress(builder, address)),
        module.i32);
    for (const std::size_t i : list)
      accesses[i].rootShadow = shadow;
  }
}

/// A copy's disjointness (section 2.5): operands that overlap, unless they
/// are the same, call the runtime.
static void expandDisjoint(ModuleContext &module, llvm::CallInst &marker) {
  // The same operand, or two distinct objects, never overlap.
  const llvm::Value *first = llvm::getUnderlyingObject(marker.getArgOperand(0));
  const llvm::Value *second =
      llvm::getUnderlyingObject(marker.getArgOperand(1));
  if (marker.getArgOperand(0)->stripPointerCasts() ==
          marker.getArgOperand(1)->stripPointerCasts() ||
      (first != second && llvm::isIdentifiedObject(first) &&
       llvm::isIdentifiedObject(second))) {
    marker.eraseFromParent();
    return;
  }
  llvm::IRBuilder<> builder(&marker);
  llvm::Value *destination =
      builder.CreatePtrToInt(marker.getArgOperand(0), module.i64);
  llvm::Value *source =
      builder.CreatePtrToInt(marker.getArgOperand(1), module.i64);
  llvm::Value *length = marker.getArgOperand(2);
  llvm::Value *overlap = builder.CreateAnd(
      builder.CreateAnd(
          builder.CreateICmpULT(destination, builder.CreateAdd(source, length)),
          builder.CreateICmpULT(source,
                                builder.CreateAdd(destination, length))),
      builder.CreateICmpNE(destination, source));
  llvm::Instruction *slow = llvm::SplitBlockAndInsertIfThen(
      overlap, &marker, false,
      llvm::MDBuilder(module.context).createUnlikelyBranchWeights());
  llvm::IRBuilder<>(slow).CreateCall(
      module.runtime("overlap",
                     llvm::FunctionType::get(
                         llvm::Type::getVoidTy(module.context),
                         {module.i64, module.i64, module.i64, module.ptr},
                         false)),
      {destination, source, length,
       module.site(marker.getDebugLoc(), SiteWrite)});
  marker.eraseFromParent();
}

/// A site made before inlining names the line of the function it was in;
/// a call inlined from an artificial function (section 5.3, amendment 14)
/// gets the site of its call site instead.
static void resiteInlinedCalls(ModuleContext &module,
                               llvm::Function &function) {
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr || !call->getDebugLoc() ||
          call->getDebugLoc()->getInlinedAt() == nullptr)
        continue;
      for (llvm::Use &argument : call->args()) {
        auto *site = llvm::dyn_cast<llvm::GlobalVariable>(argument.get());
        if (site == nullptr || !site->getName().starts_with("__weavec.site") ||
            !site->hasInitializer())
          continue;
        const auto *record =
            llvm::dyn_cast<llvm::ConstantStruct>(site->getInitializer());
        const auto *flags =
            record != nullptr && record->getNumOperands() == 4
                ? llvm::dyn_cast<llvm::ConstantInt>(record->getOperand(3))
                : nullptr;
        if (flags != nullptr)
          argument.set(
              module.site(call->getDebugLoc(),
                          static_cast<unsigned>(flags->getZExtValue())));
      }
    }
}

/// RFC 0039 §6: a failed check of the C++ library (libc++'s hardening) is
/// `__builtin_verbose_trap`, a trap whose debug location is inlined at an
/// artificial function named `__clang_trap_msg$<category>$<message>`: the
/// runtime reports the message, at the trap's site, before the trap.
static void reportLibraryTraps(ModuleContext &module,
                               llvm::Function &function) {
  llvm::SmallVector<std::pair<llvm::IntrinsicInst *, llvm::StringRef>, 2> traps;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block) {
      auto *trap = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
      if (trap == nullptr || trap->getIntrinsicID() != llvm::Intrinsic::trap ||
          !trap->getDebugLoc())
        continue;
      const llvm::DISubprogram *scope =
          trap->getDebugLoc()->getScope()->getSubprogram();
      llvm::StringRef name = scope != nullptr ? scope->getName() : "";
      if (!name.consume_front("__clang_trap_msg$"))
        continue;
      // libc++'s message starts with the header's `<file>:<line>: ` (the
      // report names the caller) and ends in a newline.
      llvm::StringRef message = name.split('$').second.rtrim();
      if (const auto [place, rest] = message.split(": ");
          !rest.empty() && !place.contains(' ') &&
          llvm::all_of(place.rsplit(':').second, llvm::isDigit) &&
          !place.rsplit(':').second.empty())
        message = rest;
      traps.emplace_back(trap, message);
    }
  if (traps.empty())
    return;
  llvm::FunctionCallee report = module.runtime(
      "library_assertion",
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.context),
                              {module.ptr, module.ptr}, false));
  for (const auto &[trap, message] : traps) {
    llvm::IRBuilder<> builder(trap);
    llvm::CallInst *call = builder.CreateCall(
        report, {builder.CreateGlobalString(message, "__weavec.message"),
                 module.site(trap->getDebugLoc(), 0)});
    call->setDebugLoc(trap->getDebugLoc());
    call->setDoesNotReturn();
  }
}

void expandGuards(ModuleContext &module, llvm::Function &function,
                  llvm::FunctionAnalysisManager &analyses) {
  const llvm::DataLayout &layout = module.layout;
  dropScopes(function, /*orphansOnly=*/false);
  resiteInlinedCalls(module, function);
  reportLibraryTraps(module, function);
  // The read-only library calls the optimizer left (section 2.5).
  guardLibraryCalls(module, function, /*late=*/true);
  std::vector<Access> accesses;
  std::vector<llvm::CallInst *> disjoint;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (const std::optional<llvm::StringRef> marker =
                ModuleContext::markerOf(*call)) {
          if (*marker == MarkerDisjoint)
            disjoint.push_back(call);
          else if (std::optional<Access> access = accessOf(*call, *marker))
            accesses.push_back(*access);
        }

  const auto &dominators =
      analyses.getResult<llvm::DominatorTreeAnalysis>(function);
  auto &evolution = analyses.getResult<llvm::ScalarEvolutionAnalysis>(function);
  auto &assumptions = analyses.getResult<llvm::AssumptionAnalysis>(function);
  Scopes scopes;
  for (Access &access : accesses) {
    if (access.kind == Access::Kind::String)
      continue;
    llvm::APInt offset(layout.getIndexTypeSizeInBits(access.pointer->getType()),
                       0);
    access.base = access.pointer->stripAndAccumulateConstantOffsets(
        layout, offset, /*AllowNonInbounds=*/true);
    if (offset.getSignificantBits() <= 63)
      access.offset = offset.getSExtValue();
    else
      access.base = nullptr;
    access.object = llvm::getUnderlyingObject(access.pointer, 0);
    if (access.pruned || inBounds(access, evolution, layout, scopes))
      access.state = Access::State::InBounds;
    // Section 2.4: a base known not to be null needs no test.
    if (access.nullBase != nullptr &&
        llvm::isKnownNonZero(access.nullBase,
                             llvm::SimplifyQuery(layout, &dominators,
                                                 &assumptions, access.marker)))
      access.nullBase = nullptr;
  }
  removeDominated(function, accesses);
  mergeInBlocks(function, accesses);

  // The allocas a guard can reach: those whose address escapes, those with
  // a guard left, and in verify mode every one a marker names.
  llvm::SetVector<llvm::AllocaInst *> tracked;
  for (const Access &access : accesses) {
    if (access.state != Access::State::Guard && !module.verify())
      continue;
    // Every alloca the pointer (or the string) may name, through phis and
    // selects.
    llvm::SmallVector<const llvm::Value *, 4> objects;
    llvm::getUnderlyingObjects(access.pointer, objects);
    for (const llvm::Value *object : objects)
      if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(object)) {
        // getUnderlyingObjects yields only const values; the alloca is this
        // function's own.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        tracked.insert(const_cast<llvm::AllocaInst *>(alloca));
      }
  }
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
          alloca && !tracked.contains(alloca) && escapes(*alloca))
        tracked.insert(alloca);

  for (const Access &access : accesses)
    if (access.kind != Access::Kind::String)
      module.copy(access.row, access.state == Access::State::Guard,
                  reasonOf(access.state));

  // The frame first, so that every guard reads the shadow it wrote; the
  // accesses then name what replaced their allocas.
  FunctionContext context(module, function);
  loadRootShadows(context, accesses, evolution, dominators,
                  analyses.getResult<llvm::LoopAnalysis>(function));
  const llvm::DenseMap<llvm::Value *, llvm::Value *> moved =
      layoutFrame(context, tracked.getArrayRef());
  // RFC 0037 §7.2: a local's color is the frame layout's constant.
  for (Access &access : accesses)
    if (access.rootIsAlloca) {
      const auto color = context.frameColors.find(access.root);
      access.rootShadow = llvm::ConstantInt::get(
          module.i32, color != context.frameColors.end() ? color->second : 0);
    }
  if (!moved.empty())
    for (Access &access : accesses) {
      auto remap = [&](llvm::Value *&value) {
        const auto found = moved.find(value);
        if (found != moved.end())
          value = found->second;
      };
      remap(access.pointer);
      remap(access.base);
      remap(access.nullBase);
      remap(access.root);
    }
  for (llvm::CallInst *marker : disjoint)
    expandDisjoint(module, *marker);
  // A merged guard's slow path checks each access it covers, in order, at
  // the address the access uses (its base and offset).
  llvm::DenseMap<std::size_t, llvm::SmallVector<FunctionContext::Member, 4>>
      members;
  for (std::size_t i = 0; i < accesses.size(); ++i) {
    const Access &access = accesses[i];
    if (access.state != Access::State::Merged)
      continue;
    auto &list = members[access.leader];
    auto member = [&](const Access &covered) {
      llvm::IRBuilder<> builder(accesses[access.leader].marker);
      return FunctionContext::Member{
          .pointer = builder.CreateConstGEP1_64(
              module.i8, covered.base,
              static_cast<std::uint64_t>(covered.offset)),
          .width = covered.width,
          .site = module.site(covered.marker->getDebugLoc(),
                              covered.write ? SiteWrite : 0)};
    };
    if (list.empty())
      list.push_back(member(accesses[access.leader]));
    list.push_back(member(access));
  }
  // Strings last: a range's length may be a string's.
  for (std::size_t i = 0; i < accesses.size(); ++i)
    if (accesses[i].kind != Access::Kind::String) {
      const auto found = members.find(i);
      expand(context, accesses[i],
             found != members.end()
                 ? llvm::ArrayRef<FunctionContext::Member>(found->second)
                 : llvm::ArrayRef<FunctionContext::Member>());
    }
  for (const Access &access : accesses)
    if (access.kind == Access::Kind::String)
      expand(context, access);
  unpoisonBeforeNoReturn(context);
}

} // namespace weavec::frontend::guard
