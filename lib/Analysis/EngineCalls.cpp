//===- EngineCalls.cpp - Calls in the object engine -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §5.4: a call's callee is resolved in RFC 0030's order — a
// declared ownership contract, the unit's summary, the program database's,
// the library table's row, a platform declaration (borrow-only), and the
// unknown-callee default — and its effects are applied to the objects its
// arguments reach. The call's own sites are decided before its effects
// (EngineDecide.cpp), from the state the call sees.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "EngineIntegers.h"
#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"

#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"

#include <algorithm>
#include <deque>
#include <tuple>

using namespace clang;

namespace weavec::analysis::engine {

/// The parameters a context is over, by type: a callee's, or (for a target
/// this unit does not declare) the call's arguments'.
using ParamShape = std::vector<QualType>;
static ParamShape shapeOf(const FunctionDecl &function) {
  ParamShape shape;
  for (const ParmVarDecl *param : function.parameters())
    shape.push_back(param->getType());
  return shape;
}
static ParamShape shapeOf(const CallExpr &call) {
  ParamShape shape;
  for (const Expr *arg : call.arguments())
    shape.push_back(arg->getType());
  return shape;
}

namespace {
/// The row whose effects are being applied, for the terms that read the
/// call itself (`strlen`, `fmtlen`).
struct RowCall {
  const CallExpr *call = nullptr;
  const core::LibraryMatch *match = nullptr;
};
} // namespace

/// A library term over the call's arguments as a domain term, when it is
/// linear in one argument.
static core::Term termOfLibTerm(Transfer &transfer, const core::LibTerm &term,
                                const std::vector<core::Sym> &args,
                                const RowCall &rowCall = {}) {
  using Kind = core::LibTerm::Kind;
  switch (term.kind) {
  case Kind::Constant:
    return core::Term::of(term.value);
  case Kind::Argument:
    if (term.arg < args.size() && args[term.arg] != core::ZeroSym)
      return transfer.termOf(args[term.arg]);
    return core::Term::unknown();
  case Kind::Sum: {
    core::Term a = termOfLibTerm(transfer, term.operands[0], args, rowCall);
    core::Term b = termOfLibTerm(transfer, term.operands[1], args, rowCall);
    auto sum = a.plus(b);
    return sum ? *sum : core::Term::unknown();
  }
  case Kind::Difference: {
    core::Term a = termOfLibTerm(transfer, term.operands[0], args, rowCall);
    return a.plusConstant(-term.value);
  }
  case Kind::Product: {
    core::Term a = termOfLibTerm(transfer, term.operands[0], args, rowCall);
    core::Term b = termOfLibTerm(transfer, term.operands[1], args, rowCall);
    if (!a.known || !b.known)
      return core::Term::unknown();
    // (A product beyond 64 bits is no term: RFC 0017.)
    std::int64_t product = 0;
    if (a.isConstant() && b.isConstant())
      return __builtin_mul_overflow(a.constant, b.constant, &product)
                 ? core::Term::unknown()
                 : core::Term::of(product);
    const core::Term &constant = a.isConstant() ? a : b;
    const core::Term &other = a.isConstant() ? b : a;
    std::int64_t scale = 0;
    if (!constant.isConstant() ||
        __builtin_mul_overflow(other.scale, constant.constant, &scale) ||
        __builtin_mul_overflow(other.constant, constant.constant, &product))
      return core::Term::unknown();
    return core::Term::ofSym(other.var, scale, product);
  }
  case Kind::Min: {
    core::Term a = termOfLibTerm(transfer, term.operands[0], args, rowCall);
    core::Term b = termOfLibTerm(transfer, term.operands[1], args, rowCall);
    if (a.isConstant() && b.isConstant())
      return core::Term::of(std::min(a.constant, b.constant));
    return core::Term::unknown();
  }
  case Kind::Quotient: {
    core::Term a = termOfLibTerm(transfer, term.operands[0], args, rowCall);
    if (a.isConstant() && a.constant >= 0 && term.value > 0)
      return core::Term::of(a.constant / term.value);
    return core::Term::unknown();
  }
  case Kind::StringLength: {
    // RFC 0012 *Length places*: after a call that required the string (a
    // `str` argument), its length is known or becomes a length symbol;
    // otherwise only what the facts already say.
    if (term.arg >= args.size() || args[term.arg] == core::ZeroSym)
      return core::Term::unknown();
    bool required = rowCall.match != nullptr &&
                    term.arg < rowCall.match->entry->params.size() &&
                    rowCall.match->entry->params[term.arg].string;
    if (required) {
      const Expr *argument = nullptr;
      int index = rowCall.match->callArgument(term.arg);
      if (rowCall.call != nullptr && index >= 0 &&
          static_cast<unsigned>(index) < rowCall.call->getNumArgs())
        argument = rowCall.call->getArg(static_cast<unsigned>(index));
      return transfer.measureString(args[term.arg], argument);
    }
    Transfer::StringFacts facts = transfer.stringFacts(args[term.arg]);
    return facts.length == Transfer::StringFacts::Length::Exact
               ? facts.term
               : core::Term::unknown();
  }
  case Kind::FormatLength: {
    if (rowCall.call == nullptr || rowCall.match == nullptr)
      return core::Term::unknown();
    Transfer::FormatFacts format =
        transfer.formatFacts(*rowCall.call, *rowCall.match);
    return format.literal && format.exact ? core::Term::of(format.lower)
                                          : core::Term::unknown();
  }
  case Kind::Macro:
    return core::Term::unknown();
  }
  return core::Term::unknown();
}

std::vector<core::ObjectId> reachableFrom(const core::Heap &heap,
                                          const core::HeapState &state,
                                          std::vector<core::ObjectId> start) {
  std::set<core::ObjectId> seen(start.begin(), start.end());
  std::deque<core::ObjectId> work(start.begin(), start.end());
  while (!work.empty()) {
    core::ObjectId id = work.front();
    work.pop_front();
    const core::ObjectState *object = heap.findObject(state, id);
    if (object == nullptr)
      continue;
    for (const auto &[key, sym] : object->cells) {
      const core::SymInfo &value = heap.info(state, sym);
      for (const core::Target &target : value.targets)
        if (seen.insert(target.object).second)
          work.push_back(target.object);
    }
  }
  return {seen.begin(), seen.end()};
}

/// Whether objects of this kind can be released by a callee.
static bool releasable(core::ObjectKind kind) {
  return kind == core::ObjectKind::HeapRecent ||
         kind == core::ObjectKind::HeapOld || kind == core::ObjectKind::Entry ||
         kind == core::ObjectKind::EntrySummary ||
         kind == core::ObjectKind::CallResult ||
         kind == core::ObjectKind::Materialized ||
         kind == core::ObjectKind::Focus || kind == core::ObjectKind::Unknown;
}

namespace {
/// What one call does, applied to the transfer's state.
class CallApplier {
public:
  CallApplier(Transfer &transfer, const CallExpr &call,
              std::vector<core::Sym> args)
      : transfer(transfer), run(transfer.functionRun()),
        heap(transfer.domain()), state(transfer.heapState()), call(call),
        args(std::move(args)), context(run.ast()) {}

  core::Sym apply(const FunctionDecl *callee, core::Sym calleeValue);

private:
  Transfer &transfer;
  FunctionRun &run;
  core::Heap &heap;
  core::HeapState &state;
  const CallExpr &call;

  std::vector<core::Sym> args;
  ASTContext &context;
  /// The library row being applied (its terms read the call).
  RowCall rowCall;

  core::SourceLocation here() const {
    return toCoreLocation(context.getSourceManager(), call.getBeginLoc());
  }
  core::Sym result();
  core::Sym applyDirect(const FunctionDecl &callee);
  core::Sym applyLibrary(const core::LibraryMatch &match);
  core::Sym applyPlatform(const FunctionDecl &callee);
  core::Sym applyUnknown(bool callback, const FunctionDecl *callee);
  core::Sym applyContract(const FunctionDecl &callee,
                          const OwnershipContract &contract);
  /// RFC 0003: a declaration's `WEAVEC_*` ownership annotations.
  std::optional<core::Sym> applyDeclared(const FunctionDecl &callee);
  /// The same, from annotations already collected (a function-pointer
  /// type's, §5.4 step 1).
  core::Sym applyAnnotated(const std::vector<AnnotationSet> &params,
                           const AnnotationSet &result);
  /// What a callee can reach besides its arguments escapes (§5.4).
  void escapeVisible();
  /// `wrapped`: the allocation goes through the zero-initialisation
  /// wrapper (a `zero-init` row), so its bytes are zero in that build;
  /// otherwise, unless `zeroed` (a zeroing allocator), what the callee left
  /// there is its own (a list `getaddrinfo` filled) and reads as unknown.
  core::Sym freshAllocation(const std::string &family,
                            std::optional<core::Term> extent, bool zeroed,
                            bool maybeNull, bool wrapped);
  /// `possibly`: the call may release it (RFC 0034 §6.3).
  void releaseArgument(unsigned index, const std::string &family,
                       core::ReleaseRecord::Reason reason,
                       bool possibly = false);
  /// RFC 0034 §6.3: a `range(t)` release covers argument `index`'s whole
  /// object: it points to the start and `t` is at least the extent.
  bool releasesWhole(unsigned index, const core::LibTerm &range);
  void havocReachable(unsigned index, bool constPointee, bool callback = false);
  /// RFC 0003: a mutable borrow may write what the argument reaches but
  /// releases nothing.
  void writeReachable(unsigned index);
  /// RFC 0030 §2.3, §15 item 3: the copy made part of a pointer into a
  /// value (`raw-cast`), which the call's temporal facet records.
  void partialPointerCopy();
  /// RFC 0030 §5.3: a `sync` callback argument's targets run zero or more
  /// times before the call returns: their effects on globals are possible
  /// effects of the call; what they may do to the arguments they are handed
  /// (or what an unknown target may do) is the unknown-callee default.
  void syncCallback(unsigned row, const core::LibCallback &callback,
                    const std::vector<core::Sym> &callArgs);
  void copyCells(unsigned dst, unsigned src, const core::LibTerm &length,
                 const Expr *dstExpr, const Expr *srcExpr);
  void fillCells(unsigned dst, const core::LibTerm &value,
                 const core::LibTerm &length);
  core::Sym callResultValue(QualType type);
  core::Sym rowResult(const core::LibraryResult &result, QualType type,
                      core::Sym realloced, const std::string &reallocFamily);
  /// The call reaches the C library through the zero-initialisation
  /// wrapper, which never asks `realloc` for zero bytes (RFC 0030 §11).
  bool wrappedRealloc = false;
};
} // namespace

core::Sym CallApplier::callResultValue(QualType type) {
  if (type->isVoidType())
    return transfer.unknownValue(type);
  if (!type->isPointerType())
    return transfer.unknownValue(type);
  QualType pointee = type->getPointeeType();
  if (pointee->isFunctionType())
    return transfer.unknownValue(type);
  core::ObjectId object =
      run.callResultObject(call, pointee, transfer.spell(call));
  core::ObjectState &objectState = heap.ensure(state, object);
  if (!objectState.extent)
    if (auto width = transfer.sizeOf(pointee))
      objectState.extent = core::Extent{.bytes = core::Term::of(*width),
                                        .cls = core::ExtentClass::LowerBound};
  core::SymInfo info;
  info.type = core::SymInfo::Type::Pointer;
  info.targets = {core::Target{.object = object}};
  info.null = core::PointerNull::Maybe;
  info.name = "the result of " + transfer.spell(*call.getCallee());
  info.ctype = typeHandle(type);
  return heap.fresh(state, info);
}

core::Sym CallApplier::freshAllocation(const std::string &family,
                                       std::optional<core::Term> extent,
                                       bool zeroed, bool maybeNull,
                                       bool wrapped) {
  QualType type = call.getType();
  QualType pointee =
      type->isPointerType() ? type->getPointeeType() : QualType();
  core::ObjectId recent =
      run.allocationObject(call, pointee, transfer.spell(call));
  // §4.2 recency: the previous allocation of this site becomes old.
  if (const core::ObjectState *previous = heap.findObject(state, recent)) {
    core::ObjectKey oldKey;
    oldKey.kind = core::ObjectKind::HeapOld;
    oldKey.handle = run.table().info(recent).key.handle;
    core::ObjectInfo oldInfo = run.table().info(recent);
    oldInfo.singular = false;
    core::ObjectId old = run.table().intern(oldKey, oldInfo);
    core::ObjectState moved = *previous;
    if (const core::ObjectState *existing = heap.findObject(state, old)) {
      // Merge into the old summary: every cell weakly.
      core::ObjectState merged = *existing;
      if (moved.life != merged.life)
        merged.life = core::Life::MayReleased;
      merged.owned = merged.owned && moved.owned;
      merged.escaped = merged.escaped || moved.escaped;
      state.objects.set(old, merged);
      for (const auto &[key, sym] : moved.cells) {
        if (auto have = heap.read(state, old, key))
          heap.write(state, old, key, heap.mergeWeak(state, *have, sym), false);
        else
          state.objects.at(old).cells.set(key, sym);
      }
    } else {
      state.objects.set(old, moved);
    }
    // Every pointer into the recent object now points into the old one.
    std::vector<core::Sym> retarget;
    for (const auto &[sym, info] : state.syms)
      for (const core::Target &target : info.targets)
        if (target.object == recent) {
          retarget.push_back(sym);
          break;
        }
    for (core::Sym sym : retarget)
      for (core::Target &target : state.syms.at(sym).targets)
        if (target.object == recent)
          target.object = old;
    state.objects.erase(recent);
  }
  core::ObjectState fresh;
  fresh.family = family;
  fresh.owned = true;
  const bool lowered = wrapped && run.unitRun().input.options.zeroInit;
  fresh.zeroed = zeroed || lowered;
  // What C leaves undefined, even where zero-initialisation lowered the
  // allocation to a zeroing one (RFC 0030 §11); what a callee filled in,
  // unknown.
  fresh.uninitialised = !zeroed && wrapped;
  fresh.havocked = !zeroed && !wrapped;
  if (extent && extent->known)
    fresh.extent =
        core::Extent{.bytes = *extent, .cls = core::ExtentClass::Exact};
  state.objects.set(recent, fresh);
  core::SymInfo info;
  info.type = core::SymInfo::Type::Pointer;
  info.targets = {core::Target{.object = recent}};
  info.null = maybeNull ? core::PointerNull::Maybe : core::PointerNull::NonNull;
  info.allocatorSource = maybeNull;
  if (maybeNull)
    info.nullOrigin =
        core::NullOrigin{.reason = core::NullOrigin::Reason::Allocated,
                         .where = here(),
                         .detail = transfer.spell(*call.getCallee())};
  info.name = transfer.spell(call);
  info.ctype = typeHandle(type);
  return heap.fresh(state, info);
}

bool CallApplier::releasesWhole(unsigned index, const core::LibTerm &range) {
  const core::SymInfo &info = heap.info(state, args[index]);
  if (info.top || info.targets.size() != 1)
    return false;
  const core::Target &target = info.targets[0];
  const core::ObjectState *object = heap.findObject(state, target.object);
  if (object == nullptr || !object->extent ||
      heap.lessEqual(state, target.offset, core::Term::of(0)) != true ||
      heap.lessEqual(state, core::Term::of(0), target.offset) != true)
    return false;
  core::Term bytes = termOfLibTerm(transfer, range, args, rowCall);
  return heap.lessEqual(state, object->extent->bytes, bytes) == true;
}

void CallApplier::releaseArgument(unsigned index, const std::string &family,
                                  core::ReleaseRecord::Reason reason,
                                  bool possibly) {
  if (index >= args.size())
    return;
  core::Sym pointer = args[index];
  const core::SymInfo &info = heap.info(state, pointer);
  if (info.type != core::SymInfo::Type::Pointer ||
      info.null == core::PointerNull::Null)
    return;
  core::ReleaseRecord record;
  record.reason = reason;
  record.where = here();
  record.family = family;
  record.via = transfer.spell(*call.getArg(index));
  record.paramGuard = transfer.paramGuard();
  record.pairGuard = transfer.pairGuard();
  record.entryGuard = state.entryTests;
  record.nonNullLocals = transfer.nonNullLocals();
  // A release after an unknown callee's: the record is replaced (RFC 0030
  // §3.1).
  heap.release(state, pointer, record, possibly);
}

void CallApplier::havocReachable(unsigned index, bool constPointee,
                                 bool callback) {
  if (index < args.size())
    transfer.havocArgument(call, args[index], constPointee, callback);
}

void Transfer::havocArgument(const CallExpr &call, core::Sym pointer,
                             bool constPointee, bool callback) {
  const core::SymInfo &info = heap.info(state, pointer);
  if (info.type != core::SymInfo::Type::Pointer)
    return;
  std::vector<core::ObjectId> start;
  start.reserve(info.targets.size());
  for (const core::Target &target : info.targets)
    start.push_back(target.object);
  // RFC 0034 §6.3: C's `const` is shallow. The callee may still write
  // through the pointers a `const` object holds, so what they reach is
  // havocked as for any argument; only the object itself keeps its cells.
  std::vector<core::ObjectId> reached = reachableFrom(heap, state, start);
  core::ReleaseRecord record;
  record.reason = callback ? core::ReleaseRecord::Reason::Callback
                           : core::ReleaseRecord::Reason::UnknownCallee;
  record.where =
      toCoreLocation(run.ast().getSourceManager(), call.getBeginLoc());
  record.allPaths = false;
  // (The code that may have released it, for the ledger's detail.)
  if (const FunctionDecl *direct = call.getDirectCallee())
    record.via = direct->getNameAsString();
  for (core::ObjectId id : reached) {
    if (!state.objects.contains(id))
      continue;
    core::ObjectKind kind = run.table().info(id).key.kind;
    core::ObjectState &object = state.objects.at(id);
    if (!constPointee || std::ranges::find(start, id) == start.end()) {
      object.cells = {};
      object.havocked = true;
      object.nulWithin.reset();
      object.nulFrom.reset();
    }
    if (releasable(kind) && object.life != core::Life::Released) {
      object.life = core::Life::UnknownReleased;
      object.record = record;
      object.escaped = true;
    }
  }
}

void CallApplier::syncCallback(unsigned row, const core::LibCallback &callback,
                               const std::vector<core::Sym> &callArgs) {
  const UnitRun &unit = run.unitRun();
  std::vector<const FunctionDecl *> targets = transfer.syncTargets(args[row]);
  bool handedArguments = targets.empty();
  core::FunctionEffects possible;
  possible.returns = core::FunctionEffects::Returns::Always;
  for (const FunctionDecl *target : targets) {
    const FunctionDecl *canonical = target->getCanonicalDecl();
    const core::FunctionEffects *effects = unit.summaryOf(*canonical);
    if (effects == nullptr) {
      // Not summarised yet (a cycle's first round): no effect, as a direct
      // call of it has; a function with no body here is unknown.
      handedArguments = handedArguments || !unit.hasBody(*canonical);
      continue;
    }
    if (effects->incomplete)
      handedArguments = true;
    for (core::PathEffect effect : effects->effects) {
      if (effect.path.root != core::SummaryRoot::Global) {
        handedArguments = true;
        continue;
      }
      effect.may = true;
      effect.when = core::EffectCase{};
      possible.effects.push_back(std::move(effect));
    }
    for (core::StoreEffect store : effects->stores) {
      if (store.dest.root != core::SummaryRoot::Global ||
          (store.value.kind == core::ValueDesc::Kind::Path &&
           store.value.path &&
           store.value.path->root != core::SummaryRoot::Global)) {
        handedArguments = true;
        continue;
      }
      store.may = true;
      store.when = core::EffectCase{};
      possible.stores.push_back(std::move(store));
    }
  }
  if (!possible.empty())
    (void)transfer.instantiate(call, possible, callArgs);
  if (!handedArguments)
    return;
  // What the targets may do with the pointers they are handed.
  core::ReleaseRecord record;
  record.reason = targets.empty() ? core::ReleaseRecord::Reason::Callback
                                  : core::ReleaseRecord::Reason::UnknownCallee;
  record.where = here();
  record.allPaths = false;
  std::vector<core::ObjectId> start;
  for (std::uint8_t argument : callback.arguments)
    if (argument < args.size() && args[argument] != core::ZeroSym)
      for (const core::Target &target :
           heap.info(state, args[argument]).targets)
        start.push_back(target.object);
  for (core::ObjectId id : reachableFrom(heap, state, start)) {
    if (!state.objects.contains(id))
      continue;
    core::ObjectState &object = state.objects.at(id);
    object.cells = {};
    object.havocked = true;
    object.nulWithin.reset();
    object.nulFrom.reset();
    if (releasable(run.table().info(id).key.kind) &&
        object.life == core::Life::Live) {
      object.life = core::Life::UnknownReleased;
      object.record = record;
      object.escaped = true;
    }
  }
}

void CallApplier::partialPointerCopy() {
  if (!run.isPublishing())
    return;
  for (core::SiteId id : run.sites().sitesOf(call)) {
    const SiteInfo *info = run.sites().info(id);
    if (info == nullptr || info->kind != core::SiteKind::LibCall ||
        !run.applies(id, core::Facet::Temporal))
      continue;
    run.ledger().decideAs(call, info->kind, info->boundary,
                          core::Facet::Temporal,
                          core::FacetDecision::unresolvedFor(
                              core::UnresolvedReason::RawCast,
                              "unsupported memory copy of pointer-containing "
                              "storage"));
  }
}

void CallApplier::writeReachable(unsigned index) {
  if (index >= args.size())
    return;
  const core::SymInfo &info = heap.info(state, args[index]);
  if (info.type != core::SymInfo::Type::Pointer)
    return;
  std::vector<core::ObjectId> start;
  start.reserve(info.targets.size());
  for (const core::Target &target : info.targets)
    start.push_back(target.object);
  for (core::ObjectId id : reachableFrom(heap, state, start)) {
    if (!state.objects.contains(id) ||
        run.table().info(id).key.kind == core::ObjectKind::Focus)
      continue;
    core::ObjectState &object = state.objects.at(id);
    if (object.readonly)
      continue;
    object.cells = {};
    object.segments.clear();
    object.havocked = true;
    object.stored = true;
    object.nulWithin.reset();
    object.nulFrom.reset();
  }
}

/// RFC 0030 §7.2: the extent a callee's declared result kind gives its
/// result (`alloc_size(i[, j])`: argument `i` bytes, times argument `j`).
static std::optional<core::Extent>
declaredResultExtent(const Transfer &transfer, const KindEntry &entry,
                     const CallExpr &call, const std::vector<core::Sym> &args) {
  if (!entry.hasShape() || entry.shapeFromSystemHeader() ||
      (entry.kind.shape != core::PointerShape::Sized &&
       entry.kind.shape != core::PointerShape::Counted))
    return std::nullopt;
  QualType pointee = call.getType()->getPointeeType();
  std::int64_t unit = 1;
  if (entry.kind.shape == core::PointerShape::Counted) {
    auto size = transfer.sizeOf(pointee);
    if (!size || *size <= 0)
      return std::nullopt;
    unit = *size;
  }
  const core::ExtentTerm &extent = entry.kind.extent;
  core::Term bytes = core::Term::unknown();
  if (extent.isConstant()) {
    bytes = core::Term::of(extent.offset * unit);
  } else if (extent.path->root == core::ExtentPath::Root::Param &&
             extent.path->param < args.size() &&
             extent.path->param < call.getNumArgs()) {
    bytes = transfer.termOf(args[extent.path->param]);
    if (entry.extentFactor) {
      if (*entry.extentFactor >= args.size())
        return std::nullopt;
      core::Term factor = transfer.termOf(args[*entry.extentFactor]);
      if (!factor.isConstant())
        return std::nullopt;
      bytes.scale *= factor.constant;
      bytes.constant *= factor.constant;
    }
    std::int64_t scale = extent.scale * unit;
    bytes.scale *= scale;
    bytes.constant = (bytes.constant * scale) + (extent.offset * unit);
    if (bytes.isConstant())
      bytes.scale = 0;
  }
  if (!bytes.known)
    return std::nullopt;
  return core::Extent{
      .bytes = bytes,
      .cls = entry.extentClass.value_or(core::ExtentClass::Declared)};
}

core::Sym CallApplier::applyUnknown(bool callback, const FunctionDecl *callee) {
  escapeVisible();
  run.ranUnknownCode = true;
  // RFC 0030 §5.1: per pointer argument without an ownership contract.
  const OwnershipContract *contract =
      callee != nullptr ? run.unitRun().input.kinds.ownership(*callee)
                        : nullptr;
  for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
    QualType type = call.getArg(i)->getType();
    if (!type->isPointerType())
      continue;
    bool covered = false;
    if (contract != nullptr)
      for (const auto &argument : contract->arguments)
        if (argument.index == i)
          covered = true;
    if (covered)
      continue;
    havocReachable(i, type->getPointeeType().isConstQualified(), callback);
  }
  // Escaped objects and globals external code can reach.
  core::ReleaseRecord record;
  record.reason = callback ? core::ReleaseRecord::Reason::Callback
                           : core::ReleaseRecord::Reason::UnknownCallee;
  record.where = here();
  record.allPaths = false;
  if (callee != nullptr)
    record.via = callee->getNameAsString();
  transfer.forgetGlobals(record);
  core::Sym value = callResultValue(call.getType());
  // A declared result kind (§7.2) sizes the object the result points to.
  if (callee != nullptr)
    if (const KindEntry *entry = run.unitRun().input.kinds.result(*callee))
      if (auto extent = declaredResultExtent(transfer, *entry, call, args)) {
        const core::SymInfo &info = heap.info(state, value);
        if (info.type == core::SymInfo::Type::Pointer &&
            info.targets.size() == 1 &&
            state.objects.contains(info.targets[0].object))
          state.objects.at(info.targets[0].object).extent = *extent;
      }
  return value;
}

void Transfer::forgetGlobals(const core::ReleaseRecord &record) {
  run.ranUnknownCode = true;
  std::vector<core::ObjectId> globals;
  for (const auto &[id, object] : state.objects) {
    core::ObjectKind kind = run.table().info(id).key.kind;
    if (kind == core::ObjectKind::Global)
      globals.push_back(id);
  }
  // A local a global reaches escapes (the code may keep its address), and
  // the code may write every local that escaped.
  for (core::ObjectId id : reachableFrom(heap, state, globals))
    if (run.table().info(id).key.kind == core::ObjectKind::Local &&
        state.objects.contains(id))
      state.objects.at(id).escaped = true;
  std::vector<core::ObjectId> escaped;
  for (const auto &[id, object] : state.objects)
    if (object.escaped &&
        run.table().info(id).key.kind == core::ObjectKind::Local)
      escaped.push_back(id);
  for (core::ObjectId id : escaped) {
    heap.forgetCells(state, id, 0, std::nullopt);
    state.objects.at(id).havocked = true;
  }
  const SourceManager &sources = run.ast().getSourceManager();
  for (core::ObjectId id : globals) {
    std::vector<core::ObjectId> below = reachableFrom(heap, state, {id});
    for (core::ObjectId reached : below) {
      if (reached == id || !state.objects.contains(reached))
        continue;
      core::ObjectState &object = state.objects.at(reached);
      if (releasable(run.table().info(reached).key.kind) &&
          object.life == core::Life::Live) {
        object.life = core::Life::UnknownReleased;
        object.record = record;
      }
    }
    // (The C library's own globals, `stdout` and the like, keep their
    // values: code the analysis does not see may close the stream, not
    // make the name refer to another, RFC 0031 *Implementation
    // amendments*.)
    const auto *var = fromHandle<VarDecl>(run.table().info(id).key.handle);
    if (var != nullptr &&
        sources.isInSystemHeader(sources.getExpansionLoc(var->getLocation())))
      continue;
    // (What it held is gone: a cell no store here reached reads as a new
    // unknown value, not as its value at entry.)
    core::ObjectState &object = state.objects.at(id);
    object.cells = {};
    object.segments.clear();
    object.havocked = true;
    object.zeroed = false;
    object.nulWithin.reset();
    object.nulFrom.reset();
  }
}

core::Sym CallApplier::applyPlatform(const FunctionDecl &callee) {
  (void)callee;
  // RFC 0030 §5.2: borrow-only; what the arguments reach may be written.
  for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
    QualType type = call.getArg(i)->getType();
    if (!type->isPointerType() || type->getPointeeType().isConstQualified())
      continue;
    const core::SymInfo &info = heap.info(state, args[i]);
    for (const core::Target &target : info.targets)
      if (state.objects.contains(target.object) &&
          run.table().info(target.object).key.kind !=
              core::ObjectKind::Literal) {
        core::ObjectState &object = state.objects.at(target.object);
        // Integer cells may change; pointers stay what they were, except
        // behind a pointer to a pointer, which is an out-parameter the
        // function may store a pointer into (RFC 0033 §1:
        // `host_processor_info(..., (processor_info_array_t *)&info, ...)`).
        const bool pointerOut = type->getPointeeType()->isPointerType();
        std::vector<core::CellKey> scalars;
        for (const auto &[key, sym] : object.cells)
          if (pointerOut ||
              heap.info(state, sym).type != core::SymInfo::Type::Pointer)
            scalars.push_back(key);
        for (const core::CellKey &key : scalars)
          object.cells.erase(key);
        // The callee may write any byte (RFC 0012).
        if (pointerOut || !scalars.empty() || object.zeroed ||
            object.nulWithin) {
          object.havocked = true;
          object.nulWithin.reset();
          object.nulFrom.reset();
        }
      }
  }
  return callResultValue(call.getType());
}

/// The element type a copy argument points to (`b` of `char *b[2]` or
/// `char **b`), when it is not `void` or a character type.
static QualType copiedElement(const ASTContext &context, const Expr *arg) {
  if (arg == nullptr)
    return {};
  QualType type = arg->IgnoreParenImpCasts()->getType().getCanonicalType();
  QualType element;
  if (const auto *array = context.getAsArrayType(type))
    element = array->getElementType();
  else if (type->isPointerType())
    element = type->getPointeeType();
  if (element.isNull() || element->isVoidType() || element->isCharType() ||
      element->isIncompleteType())
    return {};
  return element;
}

/// The offsets of the pointers of `type` that overlap `[from, to)` and do
/// not lie inside it.
static std::vector<std::int64_t> pointerOffsets(const ASTContext &context,
                                                QualType type,
                                                std::int64_t from,
                                                std::int64_t to) {
  std::vector<std::int64_t> out;
  std::int64_t width =
      context.getTypeSizeInChars(context.VoidPtrTy).getQuantity();
  auto visit = [&](auto &self, QualType t, std::int64_t base, int depth) {
    if (depth > 4 || t.isNull() || t->isIncompleteType() || base >= to)
      return;
    t = t.getCanonicalType();
    std::int64_t size = context.getTypeSizeInChars(t).getQuantity();
    if (base + size <= from)
      return;
    if (t->isPointerType()) {
      bool whole = base >= from && base + width <= to;
      if (!whole)
        out.push_back(base);
      return;
    }
    if (const auto *array = context.getAsConstantArrayType(t)) {
      QualType element = array->getElementType();
      std::int64_t step = context.getTypeSizeInChars(element).getQuantity();
      if (step <= 0)
        return;
      std::int64_t first = std::max<std::int64_t>(0, (from - base) / step);
      auto count = static_cast<std::int64_t>(array->getSize().getZExtValue());
      for (std::int64_t i = first; i < count && base + (i * step) < to; ++i)
        self(self, element, base + (i * step), depth + 1);
      return;
    }
    if (const RecordDecl *record = t->getAsRecordDecl();
        record != nullptr && !record->isUnion() &&
        record->isCompleteDefinition()) {
      const ASTRecordLayout &layout = context.getASTRecordLayout(record);
      for (const FieldDecl *field : record->fields())
        if (!field->isBitField())
          self(self, field->getType(),
               base + static_cast<std::int64_t>(
                          layout.getFieldOffset(field->getFieldIndex()) /
                          context.getCharWidth()),
               depth + 1);
    }
  };
  visit(visit, type, 0, 0);
  return out;
}

void CallApplier::copyCells(unsigned dst, unsigned src,
                            const core::LibTerm &length, const Expr *dstExpr,
                            const Expr *srcExpr) {
  if (dst >= args.size() || src >= args.size())
    return;
  const core::SymInfo to = heap.info(state, args[dst]);
  const core::SymInfo from = heap.info(state, args[src]);
  core::Term bytes = termOfLibTerm(transfer, length, args, rowCall);
  // RFC 0015 §4: a copy of whole elements copies their values, read before
  // any is written (so an overlapping `memmove` is simultaneous).
  QualType element = copiedElement(context, srcExpr);
  if (element.isNull())
    element = copiedElement(context, dstExpr);
  auto size = element.isNull() ? std::nullopt : transfer.sizeOf(element);
  // (A destination that stands for several objects, the unknown object
  // among them, keeps its cells' other values: only the range is forgotten.)
  bool single = to.targets.size() == 1 && from.targets.size() == 1 && !to.top &&
                !from.top && to.targets[0].offset.isConstant() &&
                from.targets[0].offset.isConstant() &&
                run.table().info(to.targets[0].object).singular;
  if (single && size && *size > 0 && bytes.known) {
    // The elements the length must cover: all of a constant length, the
    // lower bound of a symbolic one.
    std::optional<std::int64_t> least;
    if (bytes.isConstant()) {
      least = bytes.constant;
    } else if (bytes.scale > 0) {
      if (auto lo = state.zone.lower(bytes.var))
        least = (*lo * bytes.scale) + bytes.constant;
    }
    std::int64_t count = least && *least > 0 ? *least / *size : 0;
    bool exactLength = bytes.isConstant() && count <= 64;
    // The bytes of a last element a constant length covers only in part.
    std::int64_t partial = exactLength ? bytes.constant % *size : 0;
    count = std::min<std::int64_t>(count, 64);
    std::vector<std::pair<std::int64_t, QualType>> leaves;
    if (!transfer.recordLeaves(element, leaves))
      leaves.clear();
    if (!leaves.empty()) {
      std::int64_t d = to.targets[0].offset.constant;
      std::int64_t s = from.targets[0].offset.constant;
      core::ObjectId target = to.targets[0].object;
      core::ObjectId source = from.targets[0].object;
      heap.ensure(state, target);
      heap.ensure(state, source);
      std::vector<std::tuple<std::int64_t, QualType, core::Sym>> copied;
      for (std::int64_t e = 0; e < count; ++e)
        for (const auto &[offset, type] : leaves) {
          Address cell;
          cell.targets = {
              core::Target{.object = source,
                           .offset = core::Term::of(s + (e * *size) + offset)}};
          copied.emplace_back(d + (e * *size) + offset, type,
                              transfer.load(cell, type, nullptr));
        }
      // A cell the length covers in part holds a value made of bytes of
      // two values: unknown, and a pointer one is a reinterpretation (§4.2,
      // RFC 0030 §2.3).
      for (const auto &[offset, type] : leaves) {
        if (offset >= partial)
          continue;
        std::int64_t width = transfer.sizeOf(type).value_or(*size);
        std::int64_t at = d + (count * *size) + offset;
        if (offset + width <= partial) {
          Address cell;
          cell.targets = {core::Target{
              .object = source,
              .offset = core::Term::of(s + (count * *size) + offset)}};
          copied.emplace_back(at, type, transfer.load(cell, type, nullptr));
          continue;
        }
        core::Sym forged = transfer.unknownValue(type);
        if (type->isPointerType()) {
          heap.infoMut(state, forged).rawCast = true;
          partialPointerCopy();
        }
        copied.emplace_back(at, type, forged);
      }
      // Past what must be copied (a symbolic length): every element may
      // now hold a value of any source element (weakly).
      std::vector<std::pair<QualType, core::Sym>> tail;
      if (!exactLength)
        for (const auto &[offset, type] : leaves) {
          core::SymInfo hint;
          hint.type = type->isPointerType() ? core::SymInfo::Type::Pointer
                                            : core::SymInfo::Type::Int;
          hint.ctype = typeHandle(type);
          core::CellKey position{.offset =
                                     (((s + offset) % *size) + *size) % *size,
                                 .stride = static_cast<std::uint32_t>(*size),
                                 .index = core::ZeroSym};
          tail.emplace_back(type, heap.load(state, source, position, hint));
        }
      // From an entry object no store has reached, each element the length
      // covers holds its own source element's entry value (§4.9 *Copies*):
      // a copied range of the elements the length counts.
      const core::ObjectState &sourceState = state.objects.at(source);
      bool byElement =
          !tail.empty() && target != source && d >= 0 && s >= 0 &&
          run.table().info(source).key.kind == core::ObjectKind::Entry &&
          run.table().info(target).singular && !sourceState.stored &&
          !sourceState.forgetsAny();
      core::Term elements = core::Term::unknown();
      if (byElement) {
        if (bytes.isConstant()) {
          elements = core::Term::of(bytes.constant / *size);
        } else if (bytes.scale % *size == 0 && bytes.constant % *size == 0) {
          elements = core::Term::ofSym(bytes.var, bytes.scale / *size,
                                       bytes.constant / *size);
        } else {
          // A count the length's term does not spell: at least what the
          // lower bound covers.
          core::Sym counted = transfer.unknownValue(context.getSizeType());
          state.zone.addRange(counted, count, INT64_MAX / *size);
          elements = core::Term::ofSym(counted);
        }
      }
      // The range first: the elements copied one by one shadow it.
      for (std::size_t i = 0; i < tail.size(); ++i) {
        std::int64_t offset = leaves[i].first;
        core::CellKey position{.offset =
                                   (((d + offset) % *size) + *size) % *size,
                               .stride = static_cast<std::uint32_t>(*size),
                               .index = core::ZeroSym};
        if (byElement) {
          core::Term first =
              core::Term::of((d + offset - position.offset) / *size);
          auto last = first.plus(elements);
          if (last) {
            heap.copyElements(state, target, position, first, *last,
                              tail[i].second, source, s - d);
            continue;
          }
        }
        heap.write(state, target, position, tail[i].second, true);
      }
      for (const auto &[offset, type, value] : copied) {
        Address cell;
        cell.targets = {
            core::Target{.object = target, .offset = core::Term::of(offset)}};
        transfer.store(cell, value, type, nullptr);
      }
      state.objects.at(target).uninitialised = false;
      if (state.objects.at(target).stride == 0)
        state.objects.at(target).stride = static_cast<std::uint32_t>(*size);
      return;
    }
  }
  if (single && bytes.isConstant()) {
    std::int64_t d = to.targets[0].offset.constant;
    std::int64_t s = from.targets[0].offset.constant;
    core::ObjectId source = from.targets[0].object;
    core::ObjectId target = to.targets[0].object;
    // A literal's bytes are cells to copy (RFC 0012: `strncpy` from one).
    if (run.table().info(source).key.kind == core::ObjectKind::Literal &&
        bytes.constant > 0 && bytes.constant <= 64 && s >= 0)
      for (std::int64_t at = s; at < s + bytes.constant; ++at)
        if (!heap.read(state, source, core::CellKey{.offset = at}))
          (void)run.unwritten(
              state, source, core::CellKey{.offset = at},
              core::SymInfo{.type = core::SymInfo::Type::Int,
                            .ctype = typeHandle(context.CharTy)});
    // The source's pointers the copy splits, which a copy of its bytes
    // must see to tell (RFC 0031 §4.2).
    if (bytes.constant > 0 && bytes.constant <= 64 && s >= 0)
      if (core::Handle handle = run.table().info(source).type; handle != 0)
        for (std::int64_t at : pointerOffsets(context, typeOfHandle(handle), s,
                                              s + bytes.constant))
          if (!heap.read(state, source, core::CellKey{.offset = at}))
            (void)run.unwritten(
                state, source, core::CellKey{.offset = at},
                core::SymInfo{.type = core::SymInfo::Type::Pointer,
                              .ctype = typeHandle(context.VoidPtrTy)});
    heap.ensure(state, target);
    heap.forgetCells(state, target, d, bytes.constant);
    if (const core::ObjectState *object = heap.findObject(state, source)) {
      // RFC 0031 §4.2: a cell copied whole keeps its symbol; part of a
      // pointer is no pointer (`raw-cast`).
      std::vector<std::pair<core::CellKey, core::Sym>> copied;
      std::vector<core::CellKey> partial;
      for (const auto &[key, sym] : object->cells) {
        if (!key.isConcrete() || key.offset >= s + bytes.constant)
          continue;
        const core::SymInfo &cell = heap.info(state, sym);
        std::int64_t width =
            cell.type == core::SymInfo::Type::Pointer
                ? static_cast<std::int64_t>(
                      context.getTypeSizeInChars(context.VoidPtrTy)
                          .getQuantity())
                : 1;
        if (cell.type == core::SymInfo::Type::Int && cell.intType)
          width = std::max<std::int64_t>(1, cell.intType->width / 8);
        if (key.offset + width <= s)
          continue;
        if (key.offset >= s && key.offset + width <= s + bytes.constant)
          copied.emplace_back(core::CellKey{.offset = key.offset - s + d}, sym);
        else if (cell.type == core::SymInfo::Type::Pointer)
          partial.push_back(
              core::CellKey{.offset = std::max(key.offset, s) - s + d});
      }
      for (const auto &[key, sym] : copied)
        heap.write(state, target, key, sym, false);
      for (const core::CellKey &key : partial) {
        core::Sym raw = transfer.unknownValue(context.VoidPtrTy);
        heap.infoMut(state, raw).rawCast = true;
        heap.write(state, target, key, raw, false);
        partialPointerCopy();
      }
    }
    state.objects.at(target).uninitialised = false;
    return;
  }
  // An unknown range: the destination's cells are forgotten (a byte-wise
  // copy of pointers is a reinterpretation, RFC 0030 §2.3).
  for (const core::Target &target : to.targets) {
    heap.ensure(state, target.object);
    heap.forgetCells(state, target.object, 0, std::nullopt);
    state.objects.at(target.object).havocked = true;
    state.objects.at(target.object).uninitialised = false;
  }
}

void CallApplier::fillCells(unsigned dst, const core::LibTerm &value,
                            const core::LibTerm &length) {
  if (dst >= args.size())
    return;
  const core::SymInfo to = heap.info(state, args[dst]);
  core::Term fill = termOfLibTerm(transfer, value, args, rowCall);
  core::Term bytes = termOfLibTerm(transfer, length, args, rowCall);
  for (const core::Target &target : to.targets) {
    heap.ensure(state, target.object);
    core::ObjectState &object = state.objects.at(target.object);
    bool whole = target.offset.isConstant() && target.offset.constant == 0 &&
                 bytes.known && object.extent && object.extent->bytes == bytes;
    // (An object that stands for several, the unknown object among them, is
    // filled only in part: its range is forgotten.)
    const bool single =
        to.targets.size() == 1 && run.table().info(target.object).singular;
    if (fill.isConstant() && fill.constant == 0 && whole && single) {
      object.cells = {};
      object.zeroed = true;
      object.uninitialised = false;
      object.havocked = false;
      object.forgotten.clear();
      object.mayForgotten.clear();
      object.nulWithin.reset();
      object.nulFrom.reset();
      continue;
    }
    if (target.offset.isConstant() && bytes.isConstant())
      heap.forgetCells(state, target.object, target.offset.constant,
                       bytes.constant);
    else
      heap.forgetCells(state, target.object, 0, std::nullopt);
    state.objects.at(target.object).uninitialised = false;
    // A constant byte over a known range: the cells hold it (RFC 0012:
    // `memset(a, 'x', sizeof a)` leaves no terminator).
    if (fill.isConstant() && target.offset.isConstant() && bytes.isConstant() &&
        bytes.constant > 0 && bytes.constant <= 64 && single) {
      core::Sym byte = transfer.constant(
          static_cast<signed char>(static_cast<std::uint64_t>(fill.constant) &
                                   0xffU),
          context.CharTy);
      for (std::int64_t at = 0; at < bytes.constant; ++at)
        heap.write(state, target.object,
                   core::CellKey{.offset = target.offset.constant + at}, byte,
                   false);
    }
  }
}

core::Sym CallApplier::rowResult(const core::LibraryResult &result,
                                 QualType type, core::Sym realloced,
                                 const std::string &reallocFamily) {
  core::Sym value = core::ZeroSym;
  switch (result.kind) {
  case core::LibraryResult::Kind::Fresh: {
    std::optional<core::Term> extent;
    // RFC 0017 §3: a product of two arguments is a checked product
    // (`calloc`, `reallocarray`): one that overflows for every value makes
    // the call fail, leaving a reallocated block as it was; otherwise the
    // block has the C product's bytes, the mathematical ones on success.
    const core::LibTerm *product = nullptr;
    if (result.extent && result.extent->kind == core::LibTerm::Kind::Product &&
        result.extent->operands.size() == 2 &&
        result.extent->operands[0].kind == core::LibTerm::Kind::Argument &&
        result.extent->operands[1].kind == core::LibTerm::Kind::Argument &&
        result.extent->operands[0].arg < args.size() &&
        result.extent->operands[1].arg < args.size() &&
        args[result.extent->operands[0].arg] != core::ZeroSym &&
        args[result.extent->operands[1].arg] != core::ZeroSym)
      product = &*result.extent;
    if (product != nullptr) {
      auto bytes = transfer.checkedProduct(args[product->operands[0].arg],
                                           args[product->operands[1].arg],
                                           context.getSizeType(), call);
      if (!bytes) {
        core::Sym failed = transfer.nullPointer(type);
        heap.infoMut(state, failed).allocatorSource = true;
        return failed;
      }
      extent = transfer.termOf(*bytes);
    } else if (result.extent) {
      extent = termOfLibTerm(transfer, *result.extent, args, rowCall);
    }
    value = freshAllocation(
        result.family.empty() ? std::string(core::HeapFamily) : result.family,
        extent, result.zeroFilled,
        result.null != core::LibraryResult::Null::Never, result.zeroInit);
    // A size that is a product which may wrap (`malloc(n * sizeof *p)`):
    // the object ends at that product at the latest.
    if (extent && extent->known && !extent->isConstant() &&
        extent->scale == 1 && extent->constant == 0)
      if (const core::SymInfo *size = state.syms.find(extent->var);
          size != nullptr && size->unwrapped) {
        const core::SymInfo &fresh = heap.info(state, value);
        if (fresh.targets.size() == 1 &&
            state.objects.contains(fresh.targets[0].object)) {
          core::ObjectState &object = state.objects.at(fresh.targets[0].object);
          if (object.extent)
            object.extent->unwrapped = size->unwrapped;
        }
      }
    // A duplicated string (`strdup`): its length is the extent's but one.
    if (result.string && extent && extent->known) {
      const core::SymInfo &fresh = heap.info(state, value);
      if (fresh.targets.size() == 1 &&
          state.objects.contains(fresh.targets[0].object)) {
        core::ObjectState &object = state.objects.at(fresh.targets[0].object);
        object.nulWithin = extent->plusConstant(-1);
        object.nulFrom = core::Term::of(0);
      }
    }
    if (realloced != core::ZeroSym) {
      // RFC 0015 §5: the new block holds the old block's values (those
      // below its size); pointers into the old block do not follow.
      const core::SymInfo &previous = heap.info(state, realloced);
      const core::SymInfo &grown = heap.info(state, value);
      if (previous.type == core::SymInfo::Type::Pointer && !previous.top &&
          previous.targets.size() == 1 &&
          previous.targets[0].offset.isConstant() &&
          previous.targets[0].offset.constant == 0 &&
          grown.targets.size() == 1) {
        core::ObjectId from = previous.targets[0].object;
        core::ObjectId into = grown.targets[0].object;
        if (const core::ObjectState *contents = heap.findObject(state, from);
            contents != nullptr && from != into) {
          core::ObjectState copy = *contents;
          core::ObjectState &target = state.objects.at(into);
          std::optional<std::int64_t> limit;
          if (target.extent && target.extent->bytes.isConstant())
            limit = target.extent->bytes.constant;
          target.cells = {};
          for (const auto &[key, sym] : copy.cells)
            if (!key.isConcrete() || !limit || key.offset < *limit)
              target.cells.set(key, sym);
          target.segments = copy.segments;
          target.stride = copy.stride;
        }
      }
      // realloc: the old block is released on success (and when the size
      // is zero); until a test of the result says which, the release is
      // conditional (RFC 0030 §9.1).
      // RFC 0030 §8.2: the old block moves into the result on success,
      // and is freed on failure when the size is zero; until a test of the
      // result says which, the release is conditional (§9.1).
      core::ReleaseRecord record;
      record.reason = core::ReleaseRecord::Reason::Moved;
      record.where = here();
      record.family = reallocFamily;
      record.conditional = true;
      const core::SymInfo &old = heap.info(state, realloced);
      if (old.type == core::SymInfo::Type::Pointer &&
          old.null != core::PointerNull::Null) {
        heap.release(state, realloced, record);
        core::PendingCase onSuccess;
        onSuccess.kind = core::PendingCase::Kind::Release;
        onSuccess.classes = {"nonnull"};
        onSuccess.subject = realloced;
        onSuccess.record = record;
        onSuccess.record.conditional = false;
        heap.infoMut(state, value).pending.push_back(onSuccess);
        // The size: zero for certain, possibly, or never. (Never through
        // the zero-initialisation wrapper, which asks for one byte instead:
        // the build the analysis models fails without freeing.)
        std::optional<bool> zero;
        if (wrappedRealloc) {
          zero = false;
        } else if (result.extent) {
          core::Term size = termOfLibTerm(transfer, *result.extent, args);
          if (size.isConstant()) {
            zero = size.constant == 0;
          } else if (size.known && size.var != core::ZeroSym) {
            // The bounds of `scale * var + constant`.
            auto varLo = state.zone.lower(size.var);
            auto varHi = state.zone.upper(size.var);
            auto scaled = [&](const std::optional<std::int64_t> &bound)
                -> std::optional<std::int64_t> {
              std::int64_t product = 0;
              std::int64_t sum = 0;
              if (!bound ||
                  __builtin_mul_overflow(size.scale, *bound, &product) ||
                  __builtin_add_overflow(product, size.constant, &sum))
                return std::nullopt;
              return sum;
            };
            auto lo = size.scale >= 0 ? scaled(varLo) : scaled(varHi);
            auto hi = size.scale >= 0 ? scaled(varHi) : scaled(varLo);
            if ((lo && *lo > 0) ||
                (size.constant == 0 && heap.info(state, size.var).nonZero))
              zero = false;
            else if (lo && hi && *lo == 0 && *hi == 0)
              zero = true;
          }
        }
        if (zero != false) {
          core::PendingCase onFailure;
          onFailure.kind = core::PendingCase::Kind::Release;
          onFailure.classes = {"null"};
          onFailure.subject = realloced;
          onFailure.record = std::move(record);
          onFailure.record.reason = core::ReleaseRecord::Reason::Freed;
          onFailure.record.conditional = zero != true;
          heap.infoMut(state, value).pending.push_back(onFailure);
        }
      }
    }
    break;
  }
  case core::LibraryResult::Kind::Arg:
    if (result.arg < args.size() && args[result.arg] != core::ZeroSym) {
      value = args[result.arg];
      // The argument, or null when the call fails (`freopen`): the same
      // pointer only on the non-null class.
      const core::SymInfo &argument = heap.info(state, value);
      if (result.null != core::LibraryResult::Null::Never &&
          argument.type == core::SymInfo::Type::Pointer &&
          argument.null != core::PointerNull::Null) {
        core::SymInfo copy = argument;
        copy.null = core::PointerNull::Maybe;
        copy.pending.clear();
        copy.entryOf.reset();
        copy.condition.reset();
        copy.allocatorSource = false;
        value = heap.fresh(state, copy);
      }
    }
    break;
  case core::LibraryResult::Kind::Interior:
    if (result.arg < args.size() && args[result.arg] != core::ZeroSym) {
      core::SymInfo info = heap.info(state, args[result.arg]);
      info.entryOf.reset();
      info.pending.clear();
      info.condition.reset();
      info.linear.reset();
      info.release.reset();
      for (core::Target &target : info.targets)
        target.offset = core::Term::unknown();
      info.null = result.null == core::LibraryResult::Null::Never
                      ? core::PointerNull::NonNull
                      : core::PointerNull::Maybe;
      info.name = transfer.spell(call);
      value = heap.fresh(state, info);
    }
    break;
  case core::LibraryResult::Kind::Int:
  case core::LibraryResult::Kind::Void:
    if (result.value) {
      core::Term term = termOfLibTerm(transfer, *result.value, args, rowCall);
      // `strlen(s)` is the length symbol itself (RFC 0012 *Length
      // places*), so two measurements of one string are one value.
      if (term.known && !term.isConstant() && term.scale == 1 &&
          term.constant == 0 &&
          heap.info(state, term.var).type == core::SymInfo::Type::Int) {
        value = term.var;
        break;
      }
      value = transfer.unknownValue(type);
      if (term.isConstant())
        state.zone.addRange(value, term.constant, term.constant);
      break;
    }
    value = transfer.unknownValue(type);
    break;
  case core::LibraryResult::Kind::Static:
    // RFC 0030 §8.2: a borrow of the slot's storage.
    if (type->isPointerType() && !result.state.empty()) {
      core::ObjectId slot = run.stateObject(result.state);
      core::ObjectState &storage = heap.ensure(state, slot);
      if (result.extent && !storage.extent) {
        core::Term bytes =
            termOfLibTerm(transfer, *result.extent, args, rowCall);
        if (bytes.known)
          storage.extent =
              core::Extent{.bytes = bytes, .cls = core::ExtentClass::Declared};
      }
      core::SymInfo info;
      info.type = core::SymInfo::Type::Pointer;
      info.targets = {core::Target{.object = slot}};
      info.null = result.null == core::LibraryResult::Null::Never
                      ? core::PointerNull::NonNull
                      : core::PointerNull::Maybe;
      info.name = transfer.spell(call);
      info.ctype = typeHandle(type);
      value = heap.fresh(state, info);
      break;
    }
    value = callResultValue(type);
    break;
  case core::LibraryResult::Kind::InteriorState:
    // A pointer into the value the slot retains (`strtok`); one the
    // program released is reported by `reads` and gives no object here.
    if (type->isPointerType() && !result.state.empty())
      if (auto held =
              heap.read(state, run.stateObject(result.state), core::CellKey{});
          held &&
          heap.info(state, *held).type == core::SymInfo::Type::Pointer &&
          heap.info(state, *held).null != core::PointerNull::Null &&
          heap.temporal(state, *held).kind ==
              core::TemporalVerdict::Kind::Proven) {
        core::SymInfo info = heap.info(state, *held);
        info.entryOf.reset();
        info.pending.clear();
        info.condition.reset();
        info.linear.reset();
        info.nullOrigin.reset();
        info.allocatorSource = false;
        info.release.reset();
        for (core::Target &target : info.targets)
          target.offset = core::Term::unknown();
        info.null = core::PointerNull::Maybe;
        info.name = transfer.spell(call);
        info.ctype = typeHandle(type);
        value = heap.fresh(state, info);
        break;
      }
    value = callResultValue(type);
    break;
  case core::LibraryResult::Kind::Unknown:
    value = callResultValue(type);
    if (result.null == core::LibraryResult::Null::Never &&
        heap.info(state, value).type == core::SymInfo::Type::Pointer)
      heap.infoMut(state, value).null = core::PointerNull::NonNull;
    break;
  }
  return value;
}

core::Sym CallApplier::applyLibrary(const core::LibraryMatch &match) {
  const core::LibraryEntry &entry = *match.entry;
  // Arguments by row position.
  std::vector<core::Sym> rowArgs(entry.params.size(), core::ZeroSym);
  for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
    int row = match.rowArgument(i);
    if (row >= 0 && static_cast<unsigned>(row) < rowArgs.size())
      rowArgs[static_cast<unsigned>(row)] = args[i];
  }
  std::swap(args, rowArgs);
  rowCall = RowCall{.call = &call, .match = &match};
  auto callArgument = [&](unsigned position) {
    int index = match.callArgument(position);
    return index < 0 ? ~0U : static_cast<unsigned>(index);
  };
  // `writes-str` lengths are over the values before the call (RFC 0030 §8).
  std::vector<core::Term> written;
  written.reserve(entry.writesString.size());
  for (const core::LibStringWrite &write : entry.writesString)
    written.push_back(
        write.length ? termOfLibTerm(transfer, *write.length, args, rowCall)
                     : core::Term::unknown());
  // Copies and fills first: they read the arguments before a release.
  auto callExpr = [&](unsigned row) -> const Expr * {
    unsigned index = callArgument(row);
    return index < call.getNumArgs() ? call.getArg(index) : nullptr;
  };
  // RFC 0012: the note on a later read of an object left unterminated.
  auto noteByteWrite = [&](unsigned dst) {
    if (dst < args.size())
      for (const core::Target &target : heap.info(state, args[dst]).targets)
        run.byteWrites[target.object] = here();
  };
  for (const core::LibCopy &copy : entry.copies) {
    copyCells(copy.dst, copy.src, copy.length, callExpr(copy.dst),
              callExpr(copy.src));
    noteByteWrite(copy.dst);
  }
  for (const core::LibFill &fill : entry.fills) {
    fillCells(fill.dst, fill.value, fill.length);
    noteByteWrite(fill.dst);
  }
  for (const core::LibStringWrite &write : entry.writesString)
    noteByteWrite(write.dst);
  for (std::size_t i = 0; i < entry.writesString.size(); ++i) {
    const core::LibStringWrite &write = entry.writesString[i];
    if (write.dst >= args.size())
      continue;
    const core::SymInfo &info = heap.info(state, args[write.dst]);
    std::vector<core::ObjectId> targets;
    targets.reserve(info.targets.size());
    for (const core::Target &target : info.targets)
      targets.push_back(target.object);
    for (const core::Target &target : info.targets) {
      if (!state.objects.contains(target.object))
        continue;
      // The bytes from the destination on (the ones before it stay).
      const core::ObjectState &object = state.objects.at(target.object);
      if (info.targets.size() == 1 && target.offset.isConstant() &&
          target.offset.constant >= 0 && object.extent &&
          object.extent->bytes.isConstant())
        heap.forgetCells(state, target.object, target.offset.constant,
                         object.extent->bytes.constant -
                             target.offset.constant);
      else
        heap.forgetCells(state, target.object, 0, std::nullopt);
      state.objects.at(target.object).uninitialised = false;
    }
    // RFC 0012: a string of exactly that many characters, when known.
    if (targets.size() == 1 && written[i].known)
      transfer.noteStringWritten(args[write.dst], written[i]);
  }
  // RFC 0031 §4.2: the other bytes a `w` or `rw` argument's row writes
  // hold what the library left there (`pthread_create`'s thread handle,
  // `fread`'s records): unknown, never the values from before the call.
  for (unsigned row = 0; row < entry.params.size() && row < args.size();
       ++row) {
    const core::LibraryParam &param = entry.params[row];
    if (param.type != core::LibraryParam::Type::Pointer ||
        (param.access != core::LibraryParam::Access::Write &&
         param.access != core::LibraryParam::Access::ReadWrite) ||
        param.out || param.effect == core::LibraryParam::Effect::Release ||
        param.effect == core::LibraryParam::Effect::Realloc ||
        args[row] == core::ZeroSym)
      continue;
    bool precise = false;
    for (const core::LibCopy &copy : entry.copies)
      precise = precise || copy.dst == row;
    for (const core::LibFill &fill : entry.fills)
      precise = precise || fill.dst == row;
    for (const core::LibStringWrite &write : entry.writesString)
      precise = precise || write.dst == row;
    if (precise)
      continue;
    const core::SymInfo info = heap.info(state, args[row]);
    if (info.type != core::SymInfo::Type::Pointer || info.top)
      continue;
    // How many bytes from the pointer: the row's size, else one element.
    std::optional<std::int64_t> bytes;
    // One integer (`__builtin_mul_overflow`'s result, `pthread_create`'s
    // handle): an unknown value in its cell.
    QualType scalar;
    if (param.bytes) {
      core::Term term = termOfLibTerm(transfer, *param.bytes, args, rowCall);
      if (term.isConstant())
        bytes = term.constant;
    } else if (!param.count) {
      if (const Expr *argument = callExpr(row)) {
        QualType type = argument->IgnoreParenImpCasts()->getType();
        if (type->isPointerType()) {
          bytes = transfer.sizeOf(type->getPointeeType());
          if (type->getPointeeType()->isIntegerType())
            scalar = type->getPointeeType();
        }
      }
    }
    for (const core::Target &target : info.targets) {
      if (!state.objects.contains(target.object) ||
          state.objects.at(target.object).readonly)
        continue;
      if (!scalar.isNull() && target.offset.isConstant()) {
        Address cell;
        cell.targets = {target};
        transfer.store(cell, transfer.unknownValue(scalar), scalar, nullptr);
      } else if (bytes && target.offset.isConstant()) {
        heap.forgetCells(state, target.object, target.offset.constant, bytes);
      } else {
        heap.forgetCells(state, target.object, 0, std::nullopt);
      }
      state.objects.at(target.object).uninitialised = false;
      state.objects.at(target.object).stored = true;
    }
  }
  core::Sym realloced = core::ZeroSym;
  std::string reallocFamily;
  for (unsigned row = 0; row < entry.params.size(); ++row) {
    const core::LibraryParam &param = entry.params[row];
    if (row >= args.size())
      break;
    switch (param.effect) {
    case core::LibraryParam::Effect::Release:
      // RFC 0034 §6.3, RFC 0033 §1's witness rule: a release of part of an
      // object (`munmap` of a guard page) is no witness of the whole
      // object's release, so one not known to cover it is possible.
      releaseArgument(row,
                      param.family.empty() ? std::string(core::HeapFamily)
                                           : param.family,
                      core::ReleaseRecord::Reason::Freed,
                      param.range && !releasesWhole(row, *param.range));
      break;
    case core::LibraryParam::Effect::Realloc:
      realloced = args[row];
      reallocFamily =
          param.family.empty() ? std::string(core::HeapFamily) : param.family;
      break;
    case core::LibraryParam::Effect::Retain:
    case core::LibraryParam::Effect::Escape: {
      const core::SymInfo &info = heap.info(state, args[row]);
      for (const core::Target &target : info.targets)
        if (state.objects.contains(target.object))
          state.objects.at(target.object).escaped = true;
      // RFC 0030 §8.2 `retain(S)`: the slot keeps the pointer for a later
      // `reads(S)` (a null argument keeps the one it holds).
      if (param.effect == core::LibraryParam::Effect::Retain &&
          !param.state.empty() && info.type == core::SymInfo::Type::Pointer &&
          info.null != core::PointerNull::Null) {
        core::ObjectId slot = run.stateObject(param.state);
        heap.ensure(state, slot);
        core::Sym kept = args[row];
        if (info.null == core::PointerNull::Maybe)
          if (auto held = heap.read(state, slot, core::CellKey{}))
            kept = heap.mergeWeak(state, *held, kept);
        heap.write(state, slot, core::CellKey{}, kept, false);
      }
      break;
    }
    case core::LibraryParam::Effect::Borrow:
    case core::LibraryParam::Effect::Init:
    case core::LibraryParam::Effect::Fini:
      break;
    }
    // Out-parameters: a result stored through the argument.
    if (param.out && args[row] != core::ZeroSym) {
      const core::SymInfo &info = heap.info(state, args[row]);
      if (info.type == core::SymInfo::Type::Pointer && !info.targets.empty()) {
        Address address;
        address.targets = info.targets;
        QualType pointee = call.getArg(callArgument(row) < call.getNumArgs()
                                           ? callArgument(row)
                                           : 0)
                               ->IgnoreParenImpCasts()
                               ->getType();
        if (pointee->isPointerType())
          pointee = pointee->getPointeeType();
        // `replaces` (`getline`): the slot's previous value is consumed as
        // by `realloc` of the same family: it moves into the new value.
        if (param.out->replaces && pointee->isPointerType()) {
          core::Sym old = transfer.load(address, pointee, nullptr);
          const core::SymInfo &previous = heap.info(state, old);
          if (previous.type == core::SymInfo::Type::Pointer &&
              previous.null != core::PointerNull::Null) {
            core::ReleaseRecord record;
            record.reason = core::ReleaseRecord::Reason::Moved;
            record.where = here();
            record.family = param.out->family.empty()
                                ? std::string(core::HeapFamily)
                                : param.out->family;
            heap.release(state, old, record);
          }
        }
        core::Sym value =
            rowResult(*param.out, pointee, core::ZeroSym, std::string());
        transfer.store(address, value, pointee, nullptr);
      }
    }
  }
  if (entry.noreturn || entry.exits) {
    state.unreachable = true;
    return transfer.unknownValue(call.getType());
  }
  for (unsigned row = 0; row < entry.params.size() && row < args.size(); ++row)
    if (const core::LibraryParam &param = entry.params[row];
        param.callback && param.callback->kind == core::LibCallback::Kind::Sync)
      syncCallback(row, *param.callback, rowArgs);
  // RFC 0030 §8.2 `invalidates(S)`: every borrow of the slot's storage may
  // end here (the call's own result borrows it afresh).
  for (const std::string &slot : entry.invalidates) {
    core::ObjectId storage = run.stateObject(slot);
    core::ReleaseRecord record;
    record.reason = core::ReleaseRecord::Reason::Freed;
    record.where = here();
    record.allPaths = false;
    std::vector<core::Sym> borrows;
    for (const auto &[sym, info] : state.syms)
      if (info.type == core::SymInfo::Type::Pointer && !info.release)
        for (const core::Target &target : info.targets)
          if (target.object == storage) {
            borrows.push_back(sym);
            break;
          }
    for (core::Sym sym : borrows)
      heap.infoMut(state, sym).release = record;
  }
  wrappedRealloc = entry.result.zeroInit && reallocFamily == core::HeapFamily &&
                   run.unitRun().input.options.zeroInit;
  core::Sym value =
      rowResult(entry.result, call.getType(), realloced, reallocFamily);
  if (value == core::ZeroSym)
    value = transfer.unknownValue(call.getType());
  std::swap(args, rowArgs);
  return value;
}

core::Sym CallApplier::applyContract(const FunctionDecl &callee,
                                     const OwnershipContract &contract) {
  (void)callee;
  for (const auto &argument : contract.arguments) {
    if (argument.retains) {
      if (argument.index < args.size()) {
        const core::SymInfo &info = heap.info(state, args[argument.index]);
        for (const core::Target &target : info.targets)
          if (state.objects.contains(target.object))
            state.objects.at(target.object).escaped = true;
      }
    } else {
      releaseArgument(argument.index,
                      argument.family.empty() ? std::string(core::HeapFamily)
                                              : argument.family,
                      core::ReleaseRecord::Reason::Freed);
    }
  }
  if (contract.freshResult && call.getType()->isPointerType())
    return freshAllocation(contract.freshResult->empty()
                               ? std::string(core::HeapFamily)
                               : *contract.freshResult,
                           std::nullopt, false, true, false);
  return callResultValue(call.getType());
}

void CallApplier::escapeVisible() {
  std::vector<core::ObjectId> start;
  for (const auto &[id, object] : state.objects) {
    core::ObjectKind kind = run.table().info(id).key.kind;
    if (kind == core::ObjectKind::Global || kind == core::ObjectKind::Entry ||
        kind == core::ObjectKind::EntrySummary)
      start.push_back(id);
  }
  for (core::ObjectId id : reachableFrom(heap, state, start)) {
    if (!state.objects.contains(id))
      continue;
    core::ObjectKind kind = run.table().info(id).key.kind;
    if (kind == core::ObjectKind::HeapRecent ||
        kind == core::ObjectKind::HeapOld)
      state.objects.at(id).escaped = true;
  }
}

/// The ownership annotations of parameter `index` over every declaration.
static AnnotationSet parameterAnnotations(const FunctionDecl &callee,
                                          unsigned index) {
  AnnotationSet all;
  for (const FunctionDecl *redecl : callee.redecls()) {
    if (index >= redecl->getNumParams())
      continue;
    AnnotationSet set = getAnnotations(*redecl->getParamDecl(index));
    all.owned = all.owned || set.owned;
    all.borrowed = all.borrowed || set.borrowed;
    all.mutBorrowed = all.mutBorrowed || set.mutBorrowed;
    all.raw = all.raw || set.raw;
    all.retains = all.retains || set.retains;
    all.releases = all.releases || set.releases;
    all.frees = all.frees || set.frees;
    if (!set.family.empty())
      all.family = set.family;
  }
  return all;
}

std::optional<core::Sym>
CallApplier::applyDeclared(const FunctionDecl &callee) {
  bool any = false;
  std::vector<AnnotationSet> params;
  for (unsigned i = 0; i < callee.getNumParams(); ++i) {
    params.push_back(parameterAnnotations(callee, i));
    any = any || params.back().ownership();
  }
  AnnotationSet result;
  for (const FunctionDecl *redecl : callee.redecls()) {
    AnnotationSet set = getAnnotations(*redecl);
    result.owned = result.owned || set.owned;
    result.borrowed = result.borrowed || set.borrowed;
    if (!set.family.empty())
      result.family = set.family;
  }
  any = any || result.owned || result.borrowed;
  if (!any)
    return std::nullopt;
  return applyAnnotated(params, result);
}

core::Sym CallApplier::applyAnnotated(const std::vector<AnnotationSet> &params,
                                      const AnnotationSet &result) {
  for (unsigned i = 0; i < call.getNumArgs() && i < args.size(); ++i) {
    if (!call.getArg(i)->getType()->isPointerType())
      continue;
    if (i >= params.size()) {
      havocReachable(i, false);
      continue;
    }
    const AnnotationSet &set = params[i];
    std::string family =
        set.family.empty() ? std::string(core::HeapFamily) : set.family;
    if (set.owned || set.frees) {
      releaseArgument(i, family,
                      set.frees ? core::ReleaseRecord::Reason::Freed
                                : core::ReleaseRecord::Reason::Moved);
    } else if (set.releases) {
      releaseArgument(i, family, core::ReleaseRecord::Reason::ShareReleased);
    } else if (set.retains || set.raw) {
      // RFC 0007 *Escape*, RFC 0004: kept by the callee, or handed over as
      // a raw pointer the analysis does not follow.
      const core::SymInfo &info = heap.info(state, args[i]);
      std::vector<core::ObjectId> targets;
      targets.reserve(info.targets.size());
      for (const core::Target &target : info.targets)
        targets.push_back(target.object);
      for (core::ObjectId id : targets)
        if (state.objects.contains(id))
          state.objects.at(id).escaped = true;
    } else if (set.mutBorrowed) {
      writeReachable(i);
    } else if (!set.borrowed && !set.raw) {
      havocReachable(
          i, call.getArg(i)->getType()->getPointeeType().isConstQualified());
    }
  }
  if (result.owned && call.getType()->isPointerType())
    return freshAllocation(result.family.empty() ? std::string(core::HeapFamily)
                                                 : result.family,
                           std::nullopt, false, true, false);
  return callResultValue(call.getType());
}

core::Sym CallApplier::applyDirect(const FunctionDecl &callee) {
  const UnitRun &unit = run.unitRun();
  const FunctionDecl *canonical = callee.getCanonicalDecl();
  // A summary of the unit (or of the program in `weavec --whole-program`);
  // for a call whose context it does not describe, the context's (§6.6).
  if (const core::FunctionEffects *effects = unit.summaryOf(*canonical)) {
    if (const core::FunctionEffects *specific =
            transfer.contextSummary(call, callee, args, *effects))
      return transfer.instantiate(call, *specific, args);
    if (const core::FunctionEffects *remote =
            transfer.remoteContext(call, callee, args, *effects))
      return transfer.instantiate(call, *remote, args);
    return transfer.instantiate(call, *effects, args);
  }
  if (unit.hasBody(*canonical))
    // A function of this component not yet summarised: bottom (the
    // component's rounds reach the fixpoint from below, §3 step 2).
    return transfer.unknownValue(call.getType());
  const OwnershipContract *contract = unit.input.kinds.ownership(callee);
  bool allCovered = contract != nullptr && !contract->empty();
  if (allCovered)
    for (unsigned i = 0; i < call.getNumArgs(); ++i)
      if (call.getArg(i)->getType()->isPointerType()) {
        bool covered = false;
        for (const auto &argument : contract->arguments)
          covered = covered || argument.index == i;
        allCovered = allCovered && covered;
      }
  if (allCovered)
    return applyContract(callee, *contract);
  if (!governingLibraryEntry(callee, unit.library()))
    if (auto declared = applyDeclared(callee))
      return *declared;
  if (auto match = governingLibraryEntry(callee, unit.library()))
    return applyLibrary(*match);
  if (isPlatformDeclaration(callee, unit.library(), context.getSourceManager()))
    return applyPlatform(callee);
  if (contract != nullptr && !contract->empty())
    applyContract(callee, *contract);
  return applyUnknown(false, &callee);
}

std::optional<core::CallResolution>
Transfer::slotResolution(const CallExpr &call) const {
  const EngineInput &input = run.unitRun().input;
  if (input.slotCollection == nullptr)
    return std::nullopt;
  const core::SlotSolution *solution = input.slotSolution;
  if (input.database != nullptr && input.database->programFacts)
    solution = &input.database->programFacts->slots;
  if (solution == nullptr)
    return std::nullopt;
  auto slot = input.slotCollection->calleeSlot(call);
  if (!slot)
    return std::nullopt;
  // Flow-sensitively (§9.3): a callee value that is still a parameter's
  // entry value is whatever the callers passed, wherever it was stored
  // since (`global = unknown; global(p)` calls `unknown`, not what the
  // global's other stores hold).
  if (auto it = memo.find(call.getCallee()); it != memo.end()) {
    const unsigned entry = run.cfg ? run.cfg->getEntry().getBlockID() : ~0U;
    if (it->second.value != core::ZeroSym && entry < run.entryStates.size() &&
        run.entryStates[entry])
      for (unsigned i = 0; i < run.decl().getNumParams(); ++i) {
        auto held = heap.read(*run.entryStates[entry],
                              run.variableObject(*run.decl().getParamDecl(i)),
                              core::CellKey{});
        if (held && *held == it->second.value) {
          slot = core::SlotKey::param(
              SlotCollector::functionName(run.decl(),
                                          input.slotCollection->unit()),
              i);
          break;
        }
      }
  }
  return solution->resolveCall(*slot);
}

std::vector<const FunctionDecl *>
Transfer::syncTargets(core::Sym function) const {
  std::vector<const FunctionDecl *> out;
  const core::SymInfo &value = heap.info(state, function);
  if (value.type != core::SymInfo::Type::Function || !value.functionsKnown)
    return out;
  for (core::Handle handle : value.functions)
    if (const auto *target = fromHandle<FunctionDecl>(handle))
      out.push_back(target);
  return out;
}

std::vector<const FunctionDecl *>
Transfer::slotTargets(const core::CallResolution &resolution) const {
  std::vector<const FunctionDecl *> out;
  const SlotCollection *slots = run.unitRun().input.slotCollection;
  if (slots == nullptr)
    return out;
  for (const std::string &name : resolution.targets)
    if (const FunctionDecl *fn = slots->function(name))
      out.push_back(fn);
  return out;
}

core::Sym CallApplier::apply(const FunctionDecl *callee,
                             core::Sym calleeValue) {
  if (callee != nullptr)
    return applyDirect(*callee);
  const core::SymInfo &value = heap.info(state, calleeValue);
  // §7 *Amendment (cross-unit contexts)*: functions of other units, by the
  // summaries the program database has of them.
  std::vector<const core::FunctionEffects *> foreign;
  bool foreignUnknown = false;
  if (value.type == core::SymInfo::Type::Function && value.functionsKnown) {
    const EngineInput &unitInput = transfer.functionRun().unitRun().input;
    for (const std::string &name : value.foreignFunctions) {
      const core::FunctionEffects *effects =
          unitInput.database != nullptr ? unitInput.database->findEffects(name)
                                        : nullptr;
      if (effects == nullptr)
        foreignUnknown = true;
      else
        foreign.push_back(
            transfer.functionRun().unitRun().importEffects(name, *effects));
    }
  }
  if (foreignUnknown)
    return applyUnknown(true, nullptr);
  if (value.type == core::SymInfo::Type::Function && value.functionsKnown &&
      value.functions.size() == 1 && foreign.empty()) {
    const auto *target = fromHandle<FunctionDecl>(value.functions.front());
    if (target != nullptr)
      return applyDirect(*target);
  }
  if (value.type == core::SymInfo::Type::Function && value.functionsKnown &&
      value.functions.empty() && foreign.size() == 1)
    return transfer.instantiate(call, *foreign.front(), args);
  std::vector<const FunctionDecl *> targets;
  if (value.type == core::SymInfo::Type::Function && value.functionsKnown)
    for (core::Handle handle : value.functions)
      if (const auto *target = fromHandle<FunctionDecl>(handle))
        targets.push_back(target);
  if (targets.empty() && foreign.empty())
    if (auto resolution = transfer.slotResolution(call);
        resolution && resolution->kind != core::IndirectCallKind::OpenUnknown &&
        resolution->kind != core::IndirectCallKind::ClosedEmpty) {
      targets = transfer.slotTargets(*resolution);
      // Targets another unit defines (the program's solved slots name
      // them): by their summaries, `unit:name` being `unit#name` there.
      const SlotCollection *slots =
          transfer.functionRun().unitRun().input.slotCollection;
      const EngineInput &unitInput = transfer.functionRun().unitRun().input;
      for (const std::string &name : resolution->targets) {
        if (slots != nullptr && slots->function(name) != nullptr)
          continue;
        const core::FunctionEffects *effects = nullptr;
        std::string portable = name;
        if (unitInput.database != nullptr) {
          effects = unitInput.database->findEffects(portable);
          if (effects == nullptr)
            if (std::size_t colon = name.rfind(':');
                colon != std::string::npos) {
              portable = name.substr(0, colon) + "#" + name.substr(colon + 1);
              effects = unitInput.database->findEffects(portable);
            }
        }
        if (effects == nullptr)
          return applyUnknown(true, nullptr);
        const core::FunctionEffects *imported =
            transfer.functionRun().unitRun().importEffects(portable, *effects);
        // In the call's context when its unit serves it (an alias context
        // for `callback(p, p)`).
        if (const core::FunctionEffects *remote = transfer.remoteContext(
                call, portable, shapeOf(call), args, *imported))
          imported = remote;
        foreign.push_back(imported);
      }
      if (targets.empty() && foreign.size() == 1)
        return transfer.instantiate(call, *foreign.front(), args);
    }
  // RFC 0005: in `weavec --whole-program` an indirect call nothing resolves
  // may reach any address-taken function of its type, this unit's and
  // (joined) the program's.
  const core::FunctionEffects *programCandidates = nullptr;
  const EngineInput &input = transfer.functionRun().unitRun().input;
  if (targets.empty() && foreign.empty() && input.database != nullptr) {
    QualType type = call.getCallee()->getType();
    if (type->isPointerType())
      type = type->getPointeeType();
    std::string key = functionTypeKey(type, context);
    if (!key.empty()) {
      targets = transfer.functionRun().unitRun().localCandidates(key);
      programCandidates = input.database->candidateEffects(key);
    }
  }
  if (targets.size() == 1 && programCandidates == nullptr && foreign.empty())
    return applyDirect(*targets.front());
  if (targets.empty() && programCandidates != nullptr)
    return transfer.instantiate(call, *programCandidates, args);
  if (!targets.empty() || !foreign.empty()) {
    // Several targets: each on a copy, then the join (§5.4).
    core::HeapState before = state;
    std::optional<core::HeapState> joined;
    core::Sym joinedResult = core::ZeroSym;
    // (Pointer results raw from some targets and tracked from others.)
    bool anyRaw = false;
    bool anyTracked = false;
    auto fold = [&](core::Sym result) {
      if (result != core::ZeroSym &&
          heap.info(state, result).type == core::SymInfo::Type::Pointer &&
          heap.info(state, result).null != core::PointerNull::Null)
        (heap.info(state, result).raw ? anyRaw : anyTracked) = true;
      state.result = result;
      if (!joined)
        joined = state;
      else
        *joined = heap.join(*joined, state, handleOf(&call),
                            /*loopHead=*/false, before.nextSym);
    };
    for (const FunctionDecl *target : targets) {
      if (target == nullptr)
        continue;
      state = before;
      fold(applyDirect(*target));
    }
    for (const core::FunctionEffects *effects : foreign) {
      state = before;
      fold(transfer.instantiate(call, *effects, args));
    }
    if (programCandidates != nullptr) {
      state = before;
      fold(transfer.instantiate(call, *programCandidates, args));
    }
    if (joined) {
      state = *joined;
      joinedResult = state.result;
      state.result = before.result;
      if (anyRaw && anyTracked && joinedResult != core::ZeroSym &&
          heap.info(state, joinedResult).raw)
        heap.infoMut(state, joinedResult).rawSome = true;
      return joinedResult != core::ZeroSym
                 ? joinedResult
                 : transfer.unknownValue(call.getType());
    }
  }
  // RFC 0031 §5.4 step 1: the annotations of the function-pointer type the
  // callee expression names (a typedef, a field, a parameter).
  const Expr *named = call.getCallee()->IgnoreParenImpCasts();
  const Decl *declaration = nullptr;
  if (const auto *ref = dyn_cast<DeclRefExpr>(named))
    declaration = ref->getDecl();
  else if (const auto *member = dyn_cast<MemberExpr>(named))
    declaration = member->getMemberDecl();
  if (declaration != nullptr) {
    FunctionTypeAnnotations annotations =
        collectFunctionTypeAnnotations(*declaration);
    if (annotations.anyOwnership())
      return applyAnnotated(annotations.params, annotations.result);
  }
  return applyUnknown(true, nullptr);
}

/// The single object and constant offset a pointer value points to: one
/// runtime object (§6.6: two pointers to an object that stands for several
/// need not be the same).
static std::optional<std::pair<core::ObjectId, std::int64_t>>
singleTarget(core::Heap &heap, const core::HeapState &state, core::Sym sym) {
  const core::SymInfo &info = heap.info(state, sym);
  if (info.type != core::SymInfo::Type::Pointer || info.top ||
      info.targets.size() != 1 || !info.targets[0].offset.isConstant())
    return std::nullopt;
  const core::ObjectInfo &object = heap.objects().info(info.targets[0].object);
  if (!object.singular || object.key.kind == core::ObjectKind::Unknown)
    return std::nullopt;
  return std::make_pair(info.targets[0].object,
                        info.targets[0].offset.constant);
}

/// §6.6: the context of a call to `definition`: which of its entry objects
/// the arguments make the same, and the integer arguments the caller knows.
static AliasContext contextOf(const FunctionRun &run, core::Heap &heap,
                              const core::HeapState &state,
                              const ParamShape &definition,
                              const std::vector<core::Sym> &args) {
  AliasContext alias;
  auto count = static_cast<unsigned>(definition.size());
  std::vector<std::optional<std::pair<core::ObjectId, std::int64_t>>> objects(
      count);
  for (unsigned i = 0; i < count; ++i) {
    alias.params.emplace_back(i, 0);
    if (i >= args.size() || !definition[i]->isPointerType())
      continue;
    objects[i] = singleTarget(heap, state, args[i]);
    if (!objects[i])
      continue;
    for (unsigned j = 0; j < i; ++j)
      if (objects[j] && objects[j]->first == objects[i]->first) {
        unsigned rep = alias.params[j].first;
        std::int64_t base = objects[rep] ? objects[rep]->second : 0;
        alias.params[i] = {rep, objects[i]->second - base};
        break;
      }
  }
  // Globals whose value points into an argument's object, or into the
  // object an earlier global's value points to.
  std::map<core::ObjectId, std::pair<const VarDecl *, std::int64_t>>
      globalTargets;
  for (const auto &[id, object] : state.objects) {
    const core::ObjectInfo &info = run.table().info(id);
    if (info.key.kind != core::ObjectKind::Global)
      continue;
    const auto *var =
        dyn_cast_or_null<VarDecl>(fromHandle<Decl>(info.key.handle));
    if (var == nullptr || !var->getType()->isPointerType())
      continue;
    const core::Sym *held = object.cells.find(core::CellKey{});
    if (held == nullptr)
      continue;
    auto target = singleTarget(heap, state, *held);
    if (!target)
      continue;
    bool param = false;
    for (unsigned i = 0; i < count; ++i)
      if (objects[i] && objects[i]->first == target->first) {
        unsigned rep = alias.params[i].first;
        std::int64_t base = objects[rep] ? objects[rep]->second : 0;
        alias.globals.emplace_back(var, rep, target->second - base);
        param = true;
        break;
      }
    if (param)
      continue;
    auto [first, inserted] =
        globalTargets.try_emplace(target->first, var, target->second);
    if (!inserted)
      alias.globalAliases.emplace_back(var, first->second.first,
                                       target->second - first->second.second);
  }
  // Pointer cells of the arguments' objects that point into one object
  // (`s.a = s.b = p` passed as `&s`).
  {
    std::map<core::ObjectId,
             std::pair<unsigned, std::pair<std::int64_t, std::int64_t>>>
        cellTargets;
    for (unsigned i = 0; i < count; ++i) {
      if (!objects[i] || alias.params[i].first != i)
        continue;
      const core::ObjectState *object =
          heap.findObject(state, objects[i]->first);
      if (object == nullptr)
        continue;
      for (const auto &[key, held] : object->cells) {
        if (!key.isConcrete())
          continue;
        auto target = singleTarget(heap, state, held);
        if (!target || target->first == objects[i]->first)
          continue;
        std::int64_t cell = key.offset - objects[i]->second;
        auto [first, inserted] = cellTargets.try_emplace(
            target->first, i, std::make_pair(cell, target->second));
        if (!inserted)
          alias.cellAliases.push_back(AliasContext::CellAlias{
              .param = i,
              .cell = cell,
              .repParam = first->second.first,
              .repCell = first->second.second.first,
              .shift = target->second - first->second.second.second});
      }
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    std::optional<std::int64_t> value;
    if (i < args.size() && definition[i]->isIntegerType()) {
      value = state.zone.constant(args[i]);
      // (An unsigned 64-bit value above `INT64_MAX`, by its bits.)
      if (const core::SymInfo *info = state.syms.find(args[i]);
          !value && info != nullptr && info->values)
        if (auto constant = info->values->constant();
            constant && !constant->type.isSigned && constant->type.width == 64)
          value = static_cast<std::int64_t>(constant->bits);
    }
    alias.constants.push_back(value);
  }
  return alias;
}

void Transfer::checkAliasContext(const CallExpr &call,
                                 const FunctionDecl &callee,
                                 const std::vector<core::Sym> &args) {
  static constexpr unsigned MaxContextDepth = 3;
  static constexpr std::size_t MaxContextsPerCallee = 16;
  const FunctionDecl *definition = nullptr;
  if (!callee.hasBody(definition) || definition == nullptr ||
      !run.unitRun().hasBody(callee))
    return;
  AliasContext alias = contextOf(run, heap, state, shapeOf(*definition), args);
  if (alias.trivial())
    return;
  // A context the limits leave out: the call rests on what it would have
  // shown (RFC 0030 §5.5, the shared context budget).
  auto outOfBudget = [&] {
    run.contextIncomplete = true;
    if (run.isPublishing())
      run.ledger().decideAs(
          call, core::SiteKind::Call, core::Boundary::Call,
          core::Facet::Temporal,
          core::FacetDecision::unresolvedFor(core::UnresolvedReason::Budget));
  };
  if (run.depth() >= MaxContextDepth) {
    outOfBudget();
    return;
  }
  // RFC 0034 §7.2: no context run (diagnostics only) of an expensive callee.
  static constexpr std::uint64_t MaxContextWork = 200000;
  if (auto cost = run.unitRun().workOf.find(definition->getCanonicalDecl());
      cost == run.unitRun().workOf.end() || cost->second > MaxContextWork) {
    outOfBudget();
    return;
  }
  std::set<AliasContext> &done =
      run.unitRun().contextsRun[definition->getCanonicalDecl()];
  core::SourceLocation at =
      toCoreLocation(context.getSourceManager(), call.getBeginLoc());
  const auto reportFindings = [&](std::vector<core::Diagnostic> findings) {
    for (core::Diagnostic &diagnostic : findings) {
      core::Certainty certainty = diagnostic.certainty;
      diagnostic.addNote("called here with related pointer arguments", at);
      run.report(std::move(diagnostic), certainty, &call,
                 core::Facet::Temporal);
    }
  };
  if (done.contains(alias)) {
    // (The replay of this caller sees what the first run of it saw.)
    if (run.witnessing)
      if (auto found = run.unitRun().contextFindings.find(
              {definition->getCanonicalDecl(), alias});
          found != run.unitRun().contextFindings.end())
        reportFindings(found->second);
    return;
  }
  if (done.size() >= MaxContextsPerCallee) {
    outOfBudget();
    return;
  }
  done.insert(alias);
  if (core::AnalysisStats *stats = run.unitRun().input.options.stats)
    stats->add("alias_context_runs");
  LedgerAdapter collector(context, LedgerAdapter::Mode::Collecting);
  FunctionRun contextRun(run.unitRun(), *definition, collector,
                         RunMode::Context, &done.find(alias).operator*(),
                         run.depth() + 1);
  (void)contextRun.run();
  if (contextRun.contextIncomplete)
    outOfBudget();
  std::vector<core::Diagnostic> findings;
  for (const core::Diagnostic &diagnostic : collector.diagnostics())
    if (diagnostic.id == core::diag::UseAfterFree ||
        diagnostic.id == core::diag::DoubleFree ||
        diagnostic.id == core::diag::UseAfterMove)
      findings.push_back(diagnostic);
  run.unitRun().contextFindings[{definition->getCanonicalDecl(), alias}] =
      findings;
  reportFindings(std::move(findings));
}

/// §6.6 *Amendment (numeric contexts)*: whether what `effects` says of a
/// call depends on values a context can know: an integer it returns or
/// stores that is not one constant, an allocation whose size it does not
/// know, or an effect it may or may not have (not one keyed on the result,
/// which the caller's test of the result selects).
static bool dependsOnInputs(const core::FunctionEffects &effects) {
  auto vague = [](const core::ValueDesc &value) {
    switch (value.kind) {
    case core::ValueDesc::Kind::Int:
      return !value.lo || !value.hi || *value.lo != *value.hi;
    case core::ValueDesc::Kind::Fresh:
      return !value.extent || value.extent->path.has_value();
    case core::ValueDesc::Kind::Unknown:
      return true;
    default:
      return false;
    }
  };
  // (Which of several results is returned.)
  if (effects.results.size() > 1)
    return true;
  for (const core::ResultEffect &result : effects.results)
    if (vague(result.value))
      return true;
  for (const core::StoreEffect &store : effects.stores)
    if (vague(store.value) || store.may)
      return true;
  return std::ranges::any_of(
      effects.effects, [](const core::PathEffect &effect) {
        return effect.may || effect.lossy || effect.when.paramZero;
      });
}

/// §6.6 *Amendment (numeric contexts)*: the integers the objects of pointer
/// arguments hold (their first cells).
static void argumentCells(core::Heap &heap, const core::HeapState &state,
                          const ParamShape &definition,
                          const std::vector<core::Sym> &args,
                          std::size_t perArgument, AliasContext &context) {
  for (unsigned i = 0; i < definition.size() && i < args.size(); ++i) {
    if (context.params[i].first != i || !definition[i]->isPointerType())
      continue;
    auto target = singleTarget(heap, state, args[i]);
    if (!target)
      continue;
    const core::ObjectState *object = heap.findObject(state, target->first);
    if (object == nullptr)
      continue;
    std::size_t kept = 0;
    for (const auto &[key, sym] : object->cells) {
      if (!key.isConcrete() || key.offset < target->second)
        continue;
      const core::SymInfo *value = state.syms.find(sym);
      auto number = state.zone.constant(sym);
      if (value == nullptr || value->type != core::SymInfo::Type::Int ||
          !number || value->uninit)
        continue;
      context.cells.emplace_back(i, key.offset - target->second, *number);
      if (++kept == perArgument)
        break;
    }
  }
}

const core::FunctionEffects *
Transfer::remoteContext(const CallExpr &call, const FunctionDecl &callee,
                        const std::vector<core::Sym> &args,
                        const core::FunctionEffects &general) {
  const UnitRun &unit = run.unitRun();
  if (unit.hasBody(callee) || !callee.isExternallyVisible() ||
      callee.getIdentifier() == nullptr ||
      call.getNumArgs() != callee.getNumParams())
    return nullptr;
  return remoteContext(call, unit.portableName(callee), shapeOf(callee), args,
                       general);
}

const core::FunctionEffects *
Transfer::remoteContext(const CallExpr &call, const std::string &callee,
                        const std::vector<QualType> &shape,
                        const std::vector<core::Sym> &args,
                        const core::FunctionEffects &general) {
  (void)call;
  // §7 *Amendment (cross-unit contexts)*: within the limits of a unit's own.
  static constexpr unsigned MaxContextDepth = 2;
  static constexpr std::size_t MaxRequestsPerCallee = 16;
  static constexpr std::size_t MaxCellsPerArgument = 8;
  UnitRun &unit = run.unitRun();
  const EngineInput &unitInput = unit.input;
  if (unitInput.database == nullptr || run.depth() > MaxContextDepth ||
      general.incomplete || shape.size() != args.size())
    return nullptr;
  AliasContext callContext = contextOf(run, heap, state, shape, args);
  // (Another unit numbers the globals differently: those that point into an
  // argument's object go by name; the rest are not carried.)
  callContext.globalAliases.clear();
  callContext.cellAliases.clear();
  argumentCells(heap, state, shape, args, MaxCellsPerArgument, callContext);
  // The callbacks the call passes, by portable name.
  for (unsigned i = 0; i < shape.size() && i < args.size(); ++i) {
    QualType type = shape[i];
    if (!type->isPointerType() || !type->getPointeeType()->isFunctionType())
      continue;
    const core::SymInfo &value = heap.info(state, args[i]);
    if (value.type != core::SymInfo::Type::Function || !value.functionsKnown ||
        (value.functions.empty() && value.foreignFunctions.empty()))
      continue;
    std::vector<std::string> names = value.foreignFunctions;
    for (core::Handle handle : value.functions)
      if (const auto *fn = fromHandle<FunctionDecl>(handle))
        names.push_back(unit.portableName(*fn));
    std::ranges::sort(names);
    auto repeated = std::ranges::unique(names);
    names.erase(repeated.begin(), repeated.end());
    callContext.callbacks.emplace_back(i, std::move(names));
  }
  bool numeric =
      !callContext.cells.empty() ||
      std::ranges::any_of(callContext.constants,
                          [](const auto &value) { return value.has_value(); });
  if (callContext.trivial() && callContext.callbacks.empty() &&
      !(numeric && dependsOnInputs(general)))
    return nullptr;
  ContextRequest request{
      .callee = callee,
      .key = contextKeyText(callContext, [&](const VarDecl &var) {
        return unit.portableName(var);
      })};
  // Asked for as long as a call needs it (the definer serves what is asked).
  std::size_t asked = 0;
  for (const ContextRequest &other : unit.contextRequests)
    asked += other.callee == request.callee ? 1 : 0;
  if (asked < MaxRequestsPerCallee || unit.contextRequests.contains(request))
    unit.contextRequests.insert(request);
  else
    return nullptr;
  if (const core::FunctionEffects *served =
          unitInput.database->contextEffects(request))
    return unit.importEffects("context " + request.callee + " " + request.key,
                              *served);
  return nullptr;
}

const core::FunctionEffects *
Transfer::contextSummary(const CallExpr &call, const FunctionDecl &callee,
                         const std::vector<core::Sym> &args,
                         const core::FunctionEffects &general) {
  static constexpr unsigned MaxContextDepth = 2;
  static constexpr std::size_t MaxContextsPerCallee = 8;
  static constexpr std::size_t MaxCellsPerArgument = 8;
  // (A context run costs what the callee's own run did: only small ones.)
  static constexpr std::uint64_t MaxCalleeTransfers = 64;
  UnitRun &unit = run.unitRun();
  const FunctionDecl *definition = nullptr;
  auto cost = unit.transfersOf.find(callee.getCanonicalDecl());
  if (cost == unit.transfersOf.end() || cost->second > MaxCalleeTransfers ||
      run.depth() >= MaxContextDepth || general.incomplete ||
      !callee.hasBody(definition) || definition == nullptr ||
      !unit.hasBody(callee) ||
      unit.unsettled.contains(callee.getCanonicalDecl()) ||
      call.getNumArgs() != definition->getNumParams())
    return nullptr;
  AliasContext callContext =
      contextOf(run, heap, state, shapeOf(*definition), args);
  argumentCells(heap, state, shapeOf(*definition), args, MaxCellsPerArgument,
                callContext);
  // And those of the objects the pointer globals the callee names point to.
  {
    class GlobalReads : public RecursiveASTVisitor<GlobalReads> {
    public:
      std::set<const VarDecl *> globals;
      // RecursiveASTVisitor's CRTP hooks are found by name.
      // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
      bool VisitDeclRefExpr(DeclRefExpr *ref) {
        if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
            var != nullptr && var->hasGlobalStorage() &&
            var->getType()->isPointerType())
          globals.insert(var->getCanonicalDecl());
        return true;
      }
    } reads;
    reads.TraverseStmt(definition->getBody());
    for (const VarDecl *var : reads.globals) {
      const core::ObjectState *holder =
          heap.findObject(state, run.variableObject(*var));
      const core::Sym *held =
          holder != nullptr ? holder->cells.find(core::CellKey{}) : nullptr;
      auto target =
          held != nullptr ? singleTarget(heap, state, *held) : std::nullopt;
      const core::ObjectState *object =
          target ? heap.findObject(state, target->first) : nullptr;
      if (object == nullptr)
        continue;
      std::size_t kept = 0;
      for (const auto &[key, sym] : object->cells) {
        if (!key.isConcrete() || key.offset < target->second)
          continue;
        const core::SymInfo *value = state.syms.find(sym);
        auto number = state.zone.constant(sym);
        if (value == nullptr || value->type != core::SymInfo::Type::Int ||
            !number || value->uninit)
          continue;
        callContext.globalCells.emplace_back(var, key.offset - target->second,
                                             *number);
        if (++kept == MaxCellsPerArgument)
          break;
      }
    }
  }
  bool numeric =
      !callContext.cells.empty() || !callContext.globalCells.empty() ||
      std::ranges::any_of(callContext.constants,
                          [](const auto &value) { return value.has_value(); });
  if ((!callContext.trivial() || numeric) &&
      (!callContext.trivial() || dependsOnInputs(general))) {
    auto &known = unit.contextSummaries[definition->getCanonicalDecl()];
    auto found = known.find(callContext);
    if (found == known.end()) {
      if (known.size() >= MaxContextsPerCallee)
        return nullptr;
      found = known.emplace(callContext, std::nullopt).first;
      if (core::AnalysisStats *stats = unit.input.options.stats)
        stats->add("context_runs");
      FunctionRun contextRun(unit, *definition, unit.discarding,
                             RunMode::Summary, &found->first, run.depth() + 1);
      RunResult result = contextRun.run();
      if (!result.overBudget && !result.effects.incomplete)
        found->second = std::move(result.effects);
      if (std::getenv("WEAVEC_ENGINE_DUMP") != nullptr && found->second)
        llvm::errs() << "context summary " << definition->getNameAsString()
                     << "\n"
                     << core::toText(*found->second);
    }
    if (found->second)
      return &*found->second;
  }
  return nullptr;
}

core::Sym Transfer::call(const CallExpr &callExpr) {
  // Builtins that only compute a value.
  if (const FunctionDecl *direct = callExpr.getDirectCallee()) {
    switch (direct->getBuiltinID()) {
    case Builtin::BI__builtin_expect:
    case Builtin::BI__builtin_expect_with_probability:
    case Builtin::BI__builtin_assume_aligned:
      if (callExpr.getNumArgs() > 0)
        return valueOf(*callExpr.getArg(0));
      break;
    case Builtin::BI__builtin_unreachable:
    case Builtin::BI__builtin_trap:
    case Builtin::BI__builtin_debugtrap:
    case Builtin::BI__builtin_verbose_trap:
      state.unreachable = true;
      return unknownValue(callExpr.getType());
    case Builtin::BI__builtin_object_size:
    case Builtin::BI__builtin_dynamic_object_size:
    case Builtin::BI__builtin_constant_p:
    case Builtin::BI__builtin_classify_type:
      return unknownValue(callExpr.getType());
    // `va_start` and `va_copy` initialise the `va_list` they are given (by
    // reference): its bytes are the implementation's, never uninitialised.
    case Builtin::BI__builtin_va_start:
#if CLANG_VERSION_MAJOR >= 20
    case Builtin::BI__builtin_c23_va_start:
#endif
    case Builtin::BI__va_start:
    case Builtin::BI__builtin_va_copy: {
      for (unsigned i = 1; i < callExpr.getNumArgs(); ++i)
        (void)valueOf(*callExpr.getArg(i));
      if (callExpr.getNumArgs() > 0) {
        const Expr *list = callExpr.getArg(0)->IgnoreParenImpCasts();
        if (list->isGLValue())
          for (const core::Target &target : addressOf(*list).targets) {
            core::ObjectState &object = heap.ensure(state, target.object);
            object.uninitialised = false;
            heap.forgetCells(state, target.object, 0, std::nullopt);
          }
      }
      return unknownValue(callExpr.getType());
    }
    default:
      break;
    }
  }
  // RFC 0030 §6.2: `WEAVEC_ASSUME`.
  if (assumption(callExpr))
    return unknownValue(callExpr.getType());
  std::vector<core::Sym> args;
  args.reserve(callExpr.getNumArgs());
  for (const Expr *arg : callExpr.arguments()) {
    if (arg->getType()->isRecordType()) {
      (void)evaluate(*arg);
      args.push_back(core::ZeroSym);
    } else {
      args.push_back(valueOf(*arg));
    }
  }
  // RFC 0030 §7.6: an object handed to a callee meets the invariants.
  if (run.checkingInvariants && run.inFinalPass)
    for (core::Sym arg : args)
      if (arg != core::ZeroSym)
        for (const core::Target &target : heap.info(state, arg).targets)
          if (target.offset == core::Term::of(0))
            run.checkInvariants(state, target.object);
  core::Sym calleeValue = core::ZeroSym;
  const FunctionDecl *direct = callExpr.getDirectCallee();
  if (direct == nullptr)
    calleeValue = valueOf(*callExpr.getCallee());
  if (run.isPublishing()) {
    decideCall(callExpr, args, calleeValue);
    const FunctionDecl *target = direct;
    if (target == nullptr) {
      const core::SymInfo &value = heap.info(state, calleeValue);
      if (value.type == core::SymInfo::Type::Function && value.functionsKnown &&
          value.functions.size() == 1)
        target = fromHandle<FunctionDecl>(value.functions.front());
    }
    if (target != nullptr)
      checkAliasContext(callExpr, *target, args);
    callBoundary(callExpr, args);
  }
  CallApplier applier(*this, callExpr, args);
  core::Sym result = applier.apply(direct, calleeValue);
  if (result == core::ZeroSym)
    result = unknownValue(callExpr.getType());
  // A callee declared not to return does not (C11 6.7.4: returning from
  // one is undefined), whatever its summary could show.
  if (direct != nullptr && direct->isNoReturn())
    state.unreachable = true;
  // RFC 0017: the overflow-checking builtins' values (their rows give only
  // the write through the result pointer).
  if (auto op = checkedIntegerOp(callExpr); op && args.size() == 3)
    result = checkedArithmetic(callExpr, *op, args, result);
  return result;
}

} // namespace weavec::analysis::engine
