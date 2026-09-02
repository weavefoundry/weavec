//===- Dataflow.cpp - CFG dataflow driving the core model -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Structure (RFC 0002, *Detailed design*):
//
//   1. Pre-passes over the AST allocate one lifetime per lexical scope,
//      record which statements sit inside `WEAVEC_UNSAFE` blocks and classify
//      every place expression by the role it plays in its parent (read,
//      written, consumed, borrowed).
//   2. A forward worklist iteration over `clang::CFG` computes the entry
//      state of every reachable block. Transfer functions translate CFG
//      elements into core events; diagnostics are suppressed.
//   3. A final pass re-runs the transfer function once per block from the
//      fixpoint entry states with diagnostics enabled, emits them in source
//      order, and records the function's summary (RFC 0003): what it does
//      to its parameters and globals, what it stores through them, and where
//      its result comes from.
//
// Calls are interpreted through the callee's summary (RFC 0003, *Applying a
// summary at a call*), so the same semantic actions that handle `free(p)`
// handle `node_free(p)`.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Core/Ownership.h"

#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <tuple>
#include <utility>

using namespace clang;

namespace weavec::analysis {

/// Upper bound on visits per block before giving up on convergence. Every
/// state component is a finite lattice so this is never hit in practice; it
/// guards against a bug turning into a hang.
static constexpr unsigned MaxVisitsPerBlock = 64;

/// Longest place path the analysis will synthesise when mirroring facts
/// between aliases. Paths written in the source are never truncated; this
/// only bounds the closure over alias classes (see `mirrors`).
static constexpr std::size_t MaxPlaceDepth = 8;

FunctionDataflow::FunctionDataflow(ASTContext &ctx, const FunctionDecl &fn,
                                   core::DiagnosticSink &diagSink,
                                   const AnalysisOptions &analysisOptions,
                                   SummaryStore &summaryStore, bool emitDiags)
    : context(ctx), function(fn), sink(diagSink), options(analysisOptions),
      summaries(summaryStore), emitDiagnostics(emitDiags),
      builder(places, summaryStore), callerLifetime(lifetimes.fresh("caller")),
      fnLifetime(lifetimes.fresh("fn")),
      paramReassigned(fn.getNumParams(), false),
      signature(collectAnnotations(fn)) {
  lifetimes.addOutlives(callerLifetime, fnLifetime);
}

// -- Pre-passes ---------------------------------------------------------------

void FunctionDataflow::collectScopes(const Stmt *stmt,
                                     core::LifetimeId current) {
  if (stmt == nullptr)
    return;

  if (const auto *compound = dyn_cast<CompoundStmt>(stmt)) {
    core::LifetimeId scope = current;
    if (stmt != function.getBody()) {
      const core::SourceLocation begin = locate(compound->getLBracLoc());
      scope = lifetimes.fresh("scope@" + std::to_string(begin.line) + ":" +
                              std::to_string(begin.column));
      lifetimes.addOutlives(current, scope);
    }
    scopeEnds[scope.value] = locate(compound->getRBracLoc());
    for (const Stmt *child : compound->body())
      collectScopes(child, scope);
    return;
  }

  if (const auto *loop = dyn_cast<ForStmt>(stmt)) {
    // `for (int i = ...; ...)` declares into a scope of its own.
    const core::SourceLocation begin = locate(loop->getBeginLoc());
    const core::LifetimeId scope =
        lifetimes.fresh("for@" + std::to_string(begin.line) + ":" +
                        std::to_string(begin.column));
    lifetimes.addOutlives(current, scope);
    scopeEnds[scope.value] = locate(loop->getEndLoc());
    for (const Stmt *child : loop->children())
      collectScopes(child, scope);
    return;
  }

  if (const auto *decl = dyn_cast<DeclStmt>(stmt)) {
    for (const Decl *d : decl->decls()) {
      if (const auto *var = dyn_cast<VarDecl>(d)) {
        varLifetimes[var->getCanonicalDecl()] =
            var->isLocalVarDecl() && !var->isStaticLocal()
                ? current
                : core::LifetimeId::staticLifetime();
      }
    }
    return;
  }

  for (const Stmt *child : stmt->children())
    collectScopes(child, current);
}

void FunctionDataflow::collectUnsafe(const Stmt &stmt) {
  unsafeStmts.insert(&stmt);
  for (const Stmt *child : stmt.children()) {
    if (child != nullptr)
      collectUnsafe(*child);
  }
}

void FunctionDataflow::classifyStmt(const Stmt *stmt) {
  if (stmt == nullptr)
    return;
  if (isUnsafeBlock(*stmt)) {
    collectUnsafe(*stmt);
    return;
  }
  if (const auto *decl = dyn_cast<DeclStmt>(stmt)) {
    for (const Decl *d : decl->decls()) {
      const auto *var = dyn_cast<VarDecl>(d);
      if (var == nullptr || var->getInit() == nullptr)
        continue;
      classifyExpr(var->getInit(), Role::Read);
      if (var->getType()->isPointerType())
        noteAssignedCall(*var->getInit());
    }
    return;
  }
  if (const auto *ret = dyn_cast<ReturnStmt>(stmt)) {
    classifyExpr(ret->getRetValue(), Role::Read);
    return;
  }
  if (const auto *expr = dyn_cast<Expr>(stmt)) {
    classifyExpr(expr, Role::Read);
    return;
  }
  for (const Stmt *child : stmt->children()) {
    if (const auto *expr = dyn_cast_or_null<Expr>(child))
      classifyExpr(expr, Role::Read);
    else
      classifyStmt(child);
  }
}

void FunctionDataflow::noteAssignedCall(const Expr &rhs) {
  const Expr &stripped = PlaceBuilder::stripTransparent(rhs);
  if (const auto *call = dyn_cast<CallExpr>(&stripped)) {
    const auto effects = classifyCall(*call, summaries);
    if (effects && effects->isRealloc)
      assignedCalls.insert(call);
  }
}

void FunctionDataflow::noteParamAccess(const Expr &place, Role role) {
  // A parameter that is assigned, or whose address escapes, no longer holds
  // the argument; effects on paths under it are then recorded as they
  // happen rather than read from the exit state (RFC 0003, *Deriving a
  // summary*).
  if (role != Role::Write && role != Role::ReadWrite && role != Role::AddressOf)
    return;
  const auto *ref = dyn_cast<DeclRefExpr>(&place);
  if (ref == nullptr)
    return;
  if (const auto *param = dyn_cast<ParmVarDecl>(ref->getDecl())) {
    const unsigned index = param->getFunctionScopeIndex();
    if (index < paramReassigned.size())
      paramReassigned[index] = true;
  }
}

void FunctionDataflow::classifyExpr(const Expr *expr, Role role) {
  if (expr == nullptr)
    return;
  const Expr *e = expr->IgnoreParens();

  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    classifyExpr(cast->getSubExpr(), role);
    return;
  }

  if (PlaceBuilder::isPlaceExpr(*e)) {
    roles[e] = role;
    noteParamAccess(*e, role);
    markPathInterior(*e);
    return;
  }

  if (const auto *unary = dyn_cast<UnaryOperator>(e)) {
    switch (unary->getOpcode()) {
    case UO_AddrOf:
      classifyExpr(unary->getSubExpr(), Role::AddressOf);
      return;
    case UO_PreInc:
    case UO_PreDec:
    case UO_PostInc:
    case UO_PostDec:
      classifyExpr(unary->getSubExpr(), Role::ReadWrite);
      return;
    default:
      classifyExpr(unary->getSubExpr(), Role::Read);
      return;
    }
  }

  if (const auto *binary = dyn_cast<BinaryOperator>(e)) {
    if (binary->getOpcode() == BO_Assign) {
      classifyExpr(binary->getLHS(), Role::Write);
      classifyExpr(binary->getRHS(), Role::Read);
      if (binary->getLHS()->getType()->isPointerType())
        noteAssignedCall(*binary->getRHS());
      return;
    }
    if (binary->isCompoundAssignmentOp()) {
      classifyExpr(binary->getLHS(), Role::ReadWrite);
      classifyExpr(binary->getRHS(), Role::Read);
      return;
    }
    classifyExpr(binary->getLHS(), Role::Read);
    classifyExpr(binary->getRHS(), Role::Read);
    return;
  }

  if (const auto *call = dyn_cast<CallExpr>(e)) {
    classifyExpr(call->getCallee(), Role::Read);
    const auto effects = classifyCall(*call, summaries);
    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
      const bool consumed = effects && effects->consumes(i);
      classifyExpr(call->getArg(i), consumed ? Role::Consume : Role::Read);
    }
    return;
  }

  for (const Stmt *child : e->children()) {
    if (const auto *childExpr = dyn_cast_or_null<Expr>(child))
      classifyExpr(childExpr, Role::Read);
  }
}

void FunctionDataflow::markPathInterior(const Expr &root) {
  const auto markBase = [this](const Expr &base) {
    const Expr &stripped = PlaceBuilder::stripTransparent(base);
    if (PlaceBuilder::isPlaceExpr(stripped)) {
      roles[&stripped] = Role::Ignore;
      markPathInterior(stripped);
    } else {
      classifyExpr(&stripped, Role::Read);
    }
  };

  if (const auto *member = dyn_cast<MemberExpr>(&root)) {
    markBase(*member->getBase());
    return;
  }
  if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(&root)) {
    markBase(*subscript->getBase());
    classifyExpr(subscript->getIdx(), Role::Read);
    return;
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(&root);
      unary != nullptr && unary->getOpcode() == UO_Deref) {
    const Expr &operand = PlaceBuilder::stripTransparent(*unary->getSubExpr());
    if (const auto *binary = dyn_cast<BinaryOperator>(&operand);
        binary != nullptr &&
        (binary->getOpcode() == BO_Add || binary->getOpcode() == BO_Sub)) {
      const bool lhsIsPointer = binary->getLHS()->getType()->isPointerType();
      markBase(lhsIsPointer ? *binary->getLHS() : *binary->getRHS());
      classifyExpr(lhsIsPointer ? binary->getRHS() : binary->getLHS(),
                   Role::Read);
      return;
    }
    markBase(operand);
    return;
  }
}

core::AnalysisState FunctionDataflow::initialState() {
  core::AnalysisState state;
  for (const ParmVarDecl *param : function.parameters()) {
    const core::PlaceId place = builder.placeForVar(*param);
    varLifetimes[param->getCanonicalDecl()] = fnLifetime;
    if (!param->getType()->isPointerType())
      continue;
    const AnnotationSet annotations = getAnnotations(*param);
    core::OwnershipKind kind = core::OwnershipKind::Unknown;
    if (annotations.owned)
      kind = core::OwnershipKind::Owned;
    else if (annotations.mutBorrowed)
      kind = core::OwnershipKind::Mutable;
    else if (annotations.borrowed)
      kind = core::OwnershipKind::Shared;
    setKind(place, kind, state);
  }
  return state;
}

// -- Engine -------------------------------------------------------------------

void FunctionDataflow::run() {
  Stmt *body = function.getBody();
  if (body == nullptr)
    return;

  CFG::BuildOptions buildOptions;
  buildOptions.AddLifetime = true;
  buildOptions.setAllAlwaysAdd();
  cfg = CFG::buildCFG(&function, body, &context, buildOptions);
  if (!cfg)
    return;

  collectScopes(body, fnLifetime);
  classifyStmt(body);

  entryStates.assign(cfg->getNumBlockIDs(), std::nullopt);
  std::vector<unsigned> visits(cfg->getNumBlockIDs(), 0);
  std::vector<bool> queued(cfg->getNumBlockIDs(), false);

  const CFGBlock &entry = cfg->getEntry();
  entryStates[entry.getBlockID()] = initialState();

  std::deque<const CFGBlock *> worklist{&entry};
  queued[entry.getBlockID()] = true;
  while (!worklist.empty()) {
    const CFGBlock *block = worklist.front();
    worklist.pop_front();
    queued[block->getBlockID()] = false;
    if (++visits[block->getBlockID()] > MaxVisitsPerBlock)
      continue;

    core::AnalysisState out = *entryStates[block->getBlockID()];
    transfer(*block, out);

    unsigned index = 0;
    for (const CFGBlock::AdjacentBlock &adjacent : block->succs()) {
      const unsigned succIndex = index++;
      const CFGBlock *succ = adjacent.getReachableBlock();
      if (succ == nullptr)
        continue;
      core::AnalysisState edgeState = out;
      applyEdge(*block, succIndex, edgeState);

      std::optional<core::AnalysisState> &target =
          entryStates[succ->getBlockID()];
      bool changed = false;
      if (!target) {
        target = std::move(edgeState);
        changed = true;
      } else {
        core::AnalysisState joined = *target;
        joined.join(edgeState);
        if (joined != *target) {
          *target = std::move(joined);
          changed = true;
        }
      }
      if (changed && !queued[succ->getBlockID()]) {
        queued[succ->getBlockID()] = true;
        worklist.push_back(succ);
      }
    }
  }

  // Final pass: once per reachable block, from its fixpoint entry state.
  // Diagnostics and summary facts are recorded from here only, so they see
  // the fixpoint states and each program point exactly once.
  phase = Phase::Final;
  for (const CFGBlock *block : *cfg) {
    if (block == nullptr || !entryStates[block->getBlockID()])
      continue;
    core::AnalysisState state = *entryStates[block->getBlockID()];
    transfer(*block, state);
  }
  const auto &exitState = entryStates[cfg->getExit().getBlockID()];
  finalizeSummary(exitState ? &*exitState : nullptr);
  phase = Phase::Fixpoint;
  flushDiagnostics();

  if (options.dumpStream != nullptr && emitDiagnostics)
    dump(exitState ? &*exitState : nullptr);
}

void FunctionDataflow::transfer(const CFGBlock &block,
                                core::AnalysisState &state) {
  for (const CFGElement &element : block) {
    if (const auto stmtElement = element.getAs<CFGStmt>()) {
      const Stmt *stmt = stmtElement->getStmt();
      if (stmt == nullptr || unsafeStmts.contains(stmt))
        continue;
      if (const auto *expr = dyn_cast<Expr>(stmt))
        handleExpr(*expr, state);
      else if (const auto *decl = dyn_cast<DeclStmt>(stmt))
        handleDecl(*decl, state);
      else if (const auto *ret = dyn_cast<ReturnStmt>(stmt))
        handleReturn(*ret, state);
      continue;
    }
    if (const auto lifetimeEnd = element.getAs<CFGLifetimeEnds>()) {
      if (const VarDecl *var = lifetimeEnd->getVarDecl())
        handleLifetimeEnd(*var, state);
    }
  }
}

/// Recognises `q`, `!q`, `q == NULL`, `q != NULL` (and the flipped forms).
/// Returns the tested place and whether the *true* edge is the null one.
static std::optional<std::pair<const Expr *, bool>>
matchNullTest(const Expr &condition, ASTContext &context) {
  const Expr *e = condition.IgnoreParenImpCasts();
  if (const auto *unary = dyn_cast<UnaryOperator>(e);
      unary != nullptr && unary->getOpcode() == UO_LNot) {
    const Expr *operand = unary->getSubExpr()->IgnoreParenImpCasts();
    if (operand->getType()->isPointerType())
      return std::make_pair(operand, true);
    return std::nullopt;
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(e);
      binary != nullptr &&
      (binary->getOpcode() == BO_EQ || binary->getOpcode() == BO_NE)) {
    const bool nullOnTrue = binary->getOpcode() == BO_EQ;
    const auto isNull = [&context](const Expr *side) {
      return side->isNullPointerConstant(
                 context, Expr::NPC_ValueDependentIsNull) != Expr::NPCK_NotNull;
    };
    if (isNull(binary->getRHS()))
      return std::make_pair(binary->getLHS(), nullOnTrue);
    if (isNull(binary->getLHS()))
      return std::make_pair(binary->getRHS(), nullOnTrue);
    return std::nullopt;
  }
  if (e->getType()->isPointerType())
    return std::make_pair(e, false);
  return std::nullopt;
}

void FunctionDataflow::applyEdge(const CFGBlock &from, unsigned succIndex,
                                 core::AnalysisState &state) {
  if (state.reallocs.empty() || from.succ_size() != 2)
    return;
  const Stmt *condition = from.getTerminatorCondition();
  if (condition == nullptr)
    return;
  const auto *conditionExpr = dyn_cast<Expr>(condition);
  if (conditionExpr == nullptr)
    return;
  const auto test = matchNullTest(*conditionExpr, context);
  if (!test)
    return;
  const auto tested = builder.resolvePointerValue(*test->first);
  if (!tested)
    return;
  const auto entry = state.reallocs.find(tested->place);
  if (entry == state.reallocs.end())
    return;

  // Successor 0 is the edge taken when the condition holds.
  const bool isNullEdge = (succIndex == 0) == test->second;
  if (isNullEdge) {
    // realloc failed: the old block is still owned by the argument.
    for (const core::PlaceId consumed : entry->second)
      state.moves.reinitialize(consumed);
  }
  state.reallocs.erase(entry);
}

// -- Element handlers ---------------------------------------------------------

void FunctionDataflow::handleExpr(const Expr &expr,
                                  core::AnalysisState &state) {
  if (PlaceBuilder::isPlaceExpr(expr)) {
    const auto it = roles.find(&expr);
    const Role role = it == roles.end() ? Role::Read : it->second;
    if (role == Role::Ignore)
      return;
    const auto ref = builder.resolve(expr);
    if (!ref)
      return;
    switch (role) {
    case Role::Read:
      doRead(*ref, expr, state, /*includeSelf=*/true);
      break;
    case Role::ReadWrite:
      doRead(*ref, expr, state, /*includeSelf=*/true);
      doMutationCheck(ref->place, expr, state);
      checkAnnotationOnWrite(*ref, expr, state);
      recordAccess(ref->place, /*write=*/true, state);
      if (expr.getType()->isPointerType()) {
        // Pointer arithmetic in place: the pointer now denotes an interior
        // position we cannot track.
        reinit(ref->place, state);
        setKind(ref->place, core::OwnershipKind::Unknown, state);
      }
      break;
    case Role::Write:
    case Role::Consume:
    case Role::AddressOf:
      doRead(*ref, expr, state, /*includeSelf=*/false);
      break;
    case Role::Ignore:
      break;
    }
    return;
  }

  if (const auto *binary = dyn_cast<BinaryOperator>(&expr)) {
    if (binary->getOpcode() == BO_Assign)
      handleAssign(*binary, state);
    return;
  }
  if (const auto *call = dyn_cast<CallExpr>(&expr))
    handleCall(*call, state);
}

void FunctionDataflow::handleDecl(const DeclStmt &decl,
                                  core::AnalysisState &state) {
  for (const Decl *d : decl.decls()) {
    const auto *var = dyn_cast<VarDecl>(d);
    if (var == nullptr)
      continue;
    const core::PlaceId place = builder.placeForVar(*var);
    reinit(place, state);
    const Expr *init = var->getInit();
    if (!var->getType()->isPointerType()) {
      continue;
    }
    if (init == nullptr) {
      setKind(place, core::OwnershipKind::Unknown, state);
      continue;
    }
    applyPointerAssign(place, builder.classifyValue(*init), *init,
                       var->getType()->getPointeeType().isConstQualified(),
                       state);
  }
}

void FunctionDataflow::handleAssign(const BinaryOperator &assign,
                                    core::AnalysisState &state) {
  const auto lhs = builder.resolve(*assign.getLHS());
  if (!lhs)
    return;
  doMutationCheck(lhs->place, assign, state);
  checkAnnotationOnWrite(*lhs, assign, state);
  recordAccess(lhs->place, /*write=*/true, state);

  const QualType type = assign.getLHS()->getType();
  if (type->isPointerType()) {
    applyPointerAssign(lhs->place, builder.classifyValue(*assign.getRHS()),
                       assign, type->getPointeeType().isConstQualified(),
                       state);
    return;
  }
  if (type->isRecordType()) {
    // Whole-struct copy: every field is overwritten with values we do not
    // track individually.
    for (const core::PlaceId child : places.descendants(lhs->place))
      state.forget(child);
  }
}

void FunctionDataflow::handleCall(const CallExpr &call,
                                  core::AnalysisState &state) {
  const auto effects = classifyCall(call, summaries);
  if (!effects) {
    noteUnknownCallee(call);
    return;
  }
  applySummary(call, *effects, state);
}

// -- Calls (RFC 0003) ---------------------------------------------------------

void FunctionDataflow::applySummary(const CallExpr &call,
                                    const CallEffects &effects,
                                    core::AnalysisState &state) {
  const core::FunctionSummary &summary = *effects.summary;

  // 1. Consumption of the arguments themselves. A realloc whose result is
  //    stored is handled by the assignment, which needs to record the
  //    consumed places against the result.
  const bool consumeHere =
      !(effects.isRealloc && assignedCalls.contains(&call));
  if (consumeHere) {
    for (const unsigned index : effects.consumedArgs) {
      if (index >= call.getNumArgs())
        continue;
      const core::MoveReason reason = effects.frees(index) && !effects.isRealloc
                                          ? core::MoveReason::Freed
                                          : core::MoveReason::Moved;
      if (const auto arg = builder.resolvePointerValue(*call.getArg(index)))
        doConsume(*arg, reason, call, state);
    }
  }

  //    ... and of the caller's memory below them (`free(b->data)` in the
  //    callee) or of globals, skipping anything under an argument that is
  //    itself consumed and anything under a path already consumed here.
  std::vector<core::SummaryPath> consumedPaths;
  for (const auto &[path, effect] : summary.effects) {
    if (!effect.consumed() || (path.isParam() && path.isRoot()))
      continue;
    if (path.isParam() && summary.consumes(path.index))
      continue;
    if (llvm::any_of(consumedPaths, [&path](const core::SummaryPath &prefix) {
          return prefix.isProperPrefixOf(path);
        }))
      continue;
    const auto ref = builder.resolveSummaryPath(path, call);
    if (!ref)
      continue;
    consumedPaths.push_back(path);
    doConsume(*ref,
              effect.freed ? core::MoveReason::Freed : core::MoveReason::Moved,
              call, state);
  }

  // 2. Borrows for the duration of the call.
  for (const auto &[index, kind] : effects.borrowedArgs) {
    if (index >= call.getNumArgs())
      continue;
    const ValueOrigin origin = builder.classifyValue(*call.getArg(index));
    std::optional<PlaceRef> pointee;
    if (origin.kind == ValueOrigin::Kind::Borrow && origin.place) {
      pointee = *origin.place;
    } else if (origin.kind == ValueOrigin::Kind::Copy && origin.place) {
      pointee = *origin.place;
      pointee->addDeref(pointee->place, nullptr);
      pointee->place = places.deref(pointee->place);
    }
    if (!pointee)
      continue;
    checkTemporaryBorrow(*pointee, kind, call, state);
    if (kind == core::BorrowKind::Shared) {
      recordAccess(pointee->place, /*write=*/false, state);
    } else {
      checkAnnotationOnWrite(*pointee, call, state);
      recordAccess(pointee->place, /*write=*/true, state);
    }
  }

  // 3. Stores through arguments and into globals. Several stores to one
  //    destination form one conditional assignment.
  std::map<core::SummaryPath, std::vector<core::ValueSource>> byDest;
  for (const core::Store &store : summary.stores)
    byDest[store.dest].push_back(store.value);
  for (const auto &[dest, values] : byDest) {
    if (dest.isParam() && summary.consumes(dest.index))
      continue;
    const auto ref = builder.resolveSummaryPath(dest, call);
    if (!ref)
      continue;
    ValueOrigin origin;
    if (values.size() == 1) {
      origin = builder.originFromSource(values.front(), call, summary);
    } else {
      origin.kind = ValueOrigin::Kind::Conditional;
      for (const core::ValueSource &value : values)
        origin.alternatives.push_back(
            builder.originFromSource(value, call, summary));
    }
    doMutationCheck(ref->place, call, state);
    checkAnnotationOnWrite(*ref, call, state);
    recordAccess(ref->place, /*write=*/true, state);
    applyPointerAssign(ref->place, origin, call, /*constPointee=*/false, state);
  }
}

void FunctionDataflow::noteUnknownCallee(const CallExpr &call) {
  if (!recording() || !emitDiagnostics)
    return;
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr || callee->getBuiltinID() != 0)
    return;

  bool hasPointer = callee->getReturnType()->isPointerType();
  for (const ParmVarDecl *param : callee->parameters())
    hasPointer = hasPointer || param->getType()->isPointerType();
  if (!hasPointer)
    return;

  const SourceManager &sm = context.getSourceManager();
  if (!options.reportUnannotated && sm.isInSystemHeader(callee->getLocation()))
    return;
  if (!summaries.noteUnknownCallee(*callee))
    return;

  const std::string name = callee->getNameAsString();
  core::Diagnostic diagnostic{
      .severity = options.strictExterns ? core::Severity::Error
                                        : core::Severity::Warning,
      .id = core::diag::AnnotationRequired,
      .message = "call to '" + name +
                 "' is not checked: it has no definition or ownership "
                 "annotations here",
      .location = locate(call),
      .notes = {},
      .fixits = {},
  };
  diagnostic.addNote("'" + name + "' is declared here",
                     locate(callee->getLocation()));
  diagnostic.addNote("annotate its pointer parameters with WEAVEC_OWNED, "
                     "WEAVEC_BORROWED or WEAVEC_MUT, or define it in this "
                     "translation unit",
                     locate(callee->getLocation()));
  report(std::move(diagnostic));
}

void FunctionDataflow::handleReturn(const ReturnStmt &ret,
                                    core::AnalysisState &state) {
  const Expr *value = ret.getRetValue();
  if (value == nullptr || !value->getType()->isPointerType())
    return;

  std::vector<ValueOrigin> origins{builder.classifyValue(*value)};
  while (!origins.empty()) {
    const ValueOrigin origin = std::move(origins.back());
    origins.pop_back();
    if (origin.kind != ValueOrigin::Kind::Conditional) {
      checkAnnotationOnReturn(origin, *value, state);
      if (recording())
        inferred.addReturn(sourceOf(origin, state));
    }
    switch (origin.kind) {
    case ValueOrigin::Kind::Conditional:
      for (const ValueOrigin &alternative : origin.alternatives)
        origins.push_back(alternative);
      break;
    case ValueOrigin::Kind::Copy:
      if (origin.place) {
        for (const core::Loan &loan : state.loans.heldBy(origin.place->place)) {
          if (!lifetimes.outlives(loan.lifetime, callerLifetime)) {
            reportLifetimeTooShort(origin.place->place, loan.place, *value,
                                   /*returned=*/true);
            break;
          }
        }
      }
      break;
    case ValueOrigin::Kind::Borrow:
      if (origin.place) {
        const core::LifetimeId lifetime =
            meet(lifetimesOfPlace(origin.place->place, state));
        if (!lifetimes.outlives(lifetime, callerLifetime))
          reportLifetimeTooShort(origin.place->place, origin.place->place,
                                 *value, /*returned=*/true);
      }
      break;
    default:
      break;
    }
  }
}

void FunctionDataflow::handleLifetimeEnd(const VarDecl &var,
                                         core::AnalysisState &state) {
  const auto place = builder.lookupVar(var);
  if (!place)
    return;
  reinit(*place, state);
}

// -- Semantic actions ---------------------------------------------------------

void FunctionDataflow::reinit(core::PlaceId place, core::AnalysisState &state) {
  state.forget(place);
  for (const core::PlaceId child : places.descendants(place))
    state.forget(child);
}

void FunctionDataflow::mirrorSubtree(core::PlaceId src, core::PlaceId dest,
                                     core::AnalysisState &state) {
  // After `dest = src` the objects below `*src` are also below `*dest`, so
  // every fact recorded about the former must hold for the latter too.
  const core::PlaceId from = places.deref(src);
  const core::PlaceId to = places.deref(dest);
  std::vector<core::PlaceId> below{from};
  llvm::append_range(below, places.descendants(from));
  const std::size_t extraDepth = places.depth(to) - places.depth(from);
  for (const core::PlaceId place : below) {
    if (places.depth(place) + extraDepth > MaxPlaceDepth)
      continue;
    const core::PlaceId mirror = places.translate(place, from, to);
    if (const auto record = state.moves.movedAt(place)) {
      state.moves.markMoved(mirror, record->reason, record->location,
                            record->via.value_or(place));
    }
    for (core::Loan loan : state.loans.loans()) {
      if (loan.place == place) {
        loan.place = mirror;
        state.loans.addLoanUnchecked(loan);
      } else if (loan.holder == place) {
        loan.holder = mirror;
        state.loans.addLoanUnchecked(loan);
      }
    }
    if (const auto it = state.kinds.find(place); it != state.kinds.end())
      state.kinds[mirror] = it->second;
  }
}

void FunctionDataflow::setKind(core::PlaceId place, core::OwnershipKind kind,
                               core::AnalysisState &state) {
  state.kinds[place] = kind;
  auto [it, inserted] = summaryKinds.try_emplace(place, kind);
  if (!inserted)
    it->second = core::join(it->second, kind);
}

void FunctionDataflow::doRead(const PlaceRef &ref, const Expr &at,
                              core::AnalysisState &state, bool includeSelf) {
  for (std::size_t i = 0; i < ref.derefs.size(); ++i) {
    // Loading a pointer stored in caller memory is a read of that memory.
    recordAccess(ref.derefs[i], /*write=*/false, state);
    if (const auto hit = findMoved(ref.derefs[i], state)) {
      const Expr *where = ref.derefExprs[i];
      reportUseOfMoved(ref.derefs[i], *hit, where != nullptr ? *where : at);
      return;
    }
  }
  if (!includeSelf)
    return;
  recordAccess(ref.place, /*write=*/false, state);
  if (const auto hit = findMoved(ref.place, state))
    reportUseOfMoved(ref.place, *hit, at);
}

bool FunctionDataflow::doConsume(const PlaceRef &ref, core::MoveReason reason,
                                 const Expr &at, core::AnalysisState &state) {
  const core::PlaceId place = ref.place;
  checkAnnotationOnConsume(ref, reason, at, state);

  if (const auto hit = findMoved(place, state)) {
    const bool bothFreed = hit->record.reason == core::MoveReason::Freed &&
                           reason == core::MoveReason::Freed;
    if (bothFreed) {
      core::Diagnostic diagnostic{
          .severity = core::Severity::Error,
          .id = core::diag::DoubleFree,
          .message = "'" + nameOf(place) + "' is freed twice",
          .location = locate(at),
          .notes = {},
          .fixits = {},
      };
      std::string note = "previously freed here";
      const core::PlaceId via = hit->record.via.value_or(hit->target);
      if (via != place)
        note += " (through '" + nameOf(via) + "')";
      diagnostic.addNote(std::move(note), hit->record.location);
      report(std::move(diagnostic));
    } else {
      reportUseOfMoved(place, *hit, at);
    }
    // Keep the original record so later diagnostics point at the first site.
    return false;
  }

  if (const auto conflict = findLoanConflict(place, std::nullopt, state)) {
    const bool freeing = reason == core::MoveReason::Freed;
    core::Diagnostic diagnostic{
        .severity = core::Severity::Error,
        .id = core::diag::ConflictingBorrow,
        .message = std::string("cannot ") + (freeing ? "free" : "move") + " '" +
                   nameOf(place) + "' while it is borrowed",
        .location = locate(at),
        .notes = {},
        .fixits = {},
    };
    diagnostic.addNote("borrowed by '" + nameOf(conflict->holder) + "' here",
                       conflict->location);
    report(std::move(diagnostic));
  }

  const core::SourceLocation here = locate(at);
  for (const core::PlaceId target : targets(place, state)) {
    std::optional<core::PlaceId> via;
    if (target != place)
      via = place;
    state.moves.markMoved(target, reason, here, via);
    recordConsume(target, reason);
  }
  return true;
}

void FunctionDataflow::doMutationCheck(core::PlaceId place, const Expr &at,
                                       core::AnalysisState &state) {
  const auto conflict = findLoanConflict(place, std::nullopt, state);
  if (!conflict)
    return;
  core::Diagnostic diagnostic{
      .severity = core::Severity::Error,
      .id = core::diag::ConflictingBorrow,
      .message =
          "cannot assign to '" + nameOf(place) + "' while it is borrowed",
      .location = locate(at),
      .notes = {},
      .fixits = {},
  };
  diagnostic.addNote("borrowed by '" + nameOf(conflict->holder) + "' here",
                     conflict->location);
  report(std::move(diagnostic));
}

void FunctionDataflow::applyPointerAssign(core::PlaceId dest,
                                          const ValueOrigin &origin,
                                          const Expr &at, bool constPointee,
                                          core::AnalysisState &state) {
  // Facts about the source must be captured before the destination is reset:
  // `p = p->next` copies from a place below `p` that `reinit` forgets.
  struct CopySource {
    core::PlaceId place;
    core::OwnershipKind kind;
    std::optional<MovedHit> moved;
    std::vector<core::Loan> loans;
    bool belowDest;
  };
  struct Arm {
    const ValueOrigin *origin;
    std::optional<CopySource> source;
    core::ValueSource summary;
  };
  std::vector<Arm> arms;
  std::vector<const ValueOrigin *> pendingOrigins{&origin};
  while (!pendingOrigins.empty()) {
    const ValueOrigin *current = pendingOrigins.back();
    pendingOrigins.pop_back();
    if (current->kind == ValueOrigin::Kind::Conditional) {
      for (const ValueOrigin &alternative : current->alternatives)
        pendingOrigins.push_back(&alternative);
      continue;
    }
    std::optional<CopySource> source;
    if (current->kind == ValueOrigin::Kind::Copy && current->place) {
      const core::PlaceId src = current->place->place;
      source = CopySource{
          .place = src,
          .kind = state.kindOf(src),
          .moved = findMoved(src, state),
          .loans = state.loans.heldBy(src),
          .belowDest = src == dest || places.isDescendantOf(src, dest),
      };
    }
    arms.push_back(Arm{.origin = current,
                       .source = std::move(source),
                       .summary = recording() ? sourceOf(*current, state)
                                              : core::ValueSource::unknown()});
  }

  // `q = realloc(p, n)` consumes p's class before q is reset, because q may
  // be p (`p = realloc(p, n)`).
  std::vector<core::PlaceId> consumed;
  for (const Arm &arm : arms) {
    if (arm.origin->kind != ValueOrigin::Kind::Realloc || !arm.origin->place)
      continue;
    consumed = targets(arm.origin->place->place, state);
    if (!doConsume(*arm.origin->place, core::MoveReason::Moved, at, state))
      consumed.clear();
  }

  reinit(dest, state);

  for (const Arm &armRecord : arms) {
    const ValueOrigin *arm = armRecord.origin;
    const std::optional<CopySource> &source = armRecord.source;
    switch (arm->kind) {
    case ValueOrigin::Kind::Alloc:
      setKind(dest, core::join(state.kindOf(dest), core::OwnershipKind::Owned),
              state);
      break;
    case ValueOrigin::Kind::Realloc: {
      setKind(dest, core::join(state.kindOf(dest), core::OwnershipKind::Owned),
              state);
      std::vector<core::PlaceId> stillConsumed;
      for (const core::PlaceId place : consumed) {
        if (place != dest)
          stillConsumed.push_back(place);
      }
      if (!stillConsumed.empty())
        state.reallocs[dest] = std::move(stillConsumed);
      break;
    }
    case ValueOrigin::Kind::Copy: {
      if (!source)
        break;
      setKind(dest, core::join(state.kindOf(dest), source->kind), state);
      if (source->belowDest)
        break;
      state.aliases.unite(dest, source->place);
      state.loans.copyHolder(source->place, dest);
      mirrorSubtree(source->place, dest, state);
      // A copied loan must outlive its new holder (RFC 0001, *Lifetimes*),
      // just as a fresh borrow must; this is how `g = p` with `p = &local`
      // and, through a summary, `keep(local)` are caught.
      for (const core::Loan &loan : source->loans) {
        bool tooShort = false;
        for (const core::LifetimeId destLifetime :
             lifetimesOfPlace(dest, state)) {
          if (!lifetimes.outlives(loan.lifetime, destLifetime)) {
            tooShort = true;
            break;
          }
        }
        if (tooShort) {
          reportLifetimeTooShort(dest, loan.place, at, /*returned=*/false);
          break;
        }
      }
      if (source->moved) {
        // The read itself was reported at the load; keep the copy moved so
        // uses through it do not cascade into a second report per alias.
        state.moves.markMoved(
            dest, source->moved->record.reason, source->moved->record.location,
            source->moved->record.via.value_or(source->moved->target));
      }
      break;
    }
    case ValueOrigin::Kind::Borrow: {
      if (!arm->place)
        break;
      const core::BorrowKind kind = constPointee || arm->constObject
                                        ? core::BorrowKind::Shared
                                        : core::BorrowKind::Mutable;
      applyBorrow(dest, *arm->place, kind, at, state);
      break;
    }
    case ValueOrigin::Kind::Null:
    case ValueOrigin::Kind::Opaque:
    case ValueOrigin::Kind::Conditional:
      setKind(dest, state.kindOf(dest), state);
      break;
    }
  }

  if (recording()) {
    for (const Arm &arm : arms)
      recordStore(dest, arm.summary);
  }
}

void FunctionDataflow::applyBorrow(core::PlaceId dest, const PlaceRef &borrowed,
                                   core::BorrowKind kind, const Expr &at,
                                   core::AnalysisState &state) {
  const core::PlaceId target = borrowed.place;
  const core::OwnershipKind ownership = kind == core::BorrowKind::Mutable
                                            ? core::OwnershipKind::Mutable
                                            : core::OwnershipKind::Shared;
  setKind(dest, core::join(state.kindOf(dest), ownership), state);

  // Lifetime: the borrow lives as long as the borrowed object; storing it in
  // `dest` requires that to outlive wherever `dest` lives.
  const core::LifetimeId loanLifetime = meet(lifetimesOfPlace(target, state));
  for (const core::LifetimeId destLifetime : lifetimesOfPlace(dest, state)) {
    if (!lifetimes.outlives(loanLifetime, destLifetime)) {
      reportLifetimeTooShort(dest, target, at, /*returned=*/false);
      break;
    }
  }

  // Borrow rules.
  if (const auto conflict = findLoanConflict(target, kind, state)) {
    const bool mutableAttempt = kind == core::BorrowKind::Mutable;
    core::Diagnostic diagnostic{
        .severity = core::Severity::Error,
        .id = core::diag::ConflictingBorrow,
        .message = "cannot borrow '" + nameOf(target) + "' as " +
                   (mutableAttempt ? "mutable" : "shared") +
                   " because it is already " +
                   (mutableAttempt ? "borrowed" : "mutably borrowed"),
        .location = locate(at),
        .notes = {},
        .fixits = {},
    };
    diagnostic.addNote("previous borrow of '" + nameOf(conflict->place) +
                           "' by '" + nameOf(conflict->holder) + "' here",
                       conflict->location);
    report(std::move(diagnostic));
    return;
  }

  const core::SourceLocation here = locate(at);
  for (const core::PlaceId holder : mirrors(dest, state)) {
    for (const core::PlaceId place : mirrors(target, state)) {
      state.loans.addLoanUnchecked(core::Loan{.place = place,
                                              .kind = kind,
                                              .lifetime = loanLifetime,
                                              .location = here,
                                              .holder = holder});
    }
  }
}

void FunctionDataflow::checkTemporaryBorrow(const PlaceRef &borrowed,
                                            core::BorrowKind kind,
                                            const Expr &at,
                                            const core::AnalysisState &state) {
  const auto conflict = findLoanConflict(borrowed.place, kind, state);
  if (!conflict)
    return;
  const bool mutableAttempt = kind == core::BorrowKind::Mutable;
  core::Diagnostic diagnostic{
      .severity = core::Severity::Error,
      .id = core::diag::ConflictingBorrow,
      .message = "cannot borrow '" + nameOf(borrowed.place) + "' as " +
                 (mutableAttempt ? "mutable" : "shared") +
                 " because it is already " +
                 (mutableAttempt ? "borrowed" : "mutably borrowed"),
      .location = locate(at),
      .notes = {},
      .fixits = {},
  };
  diagnostic.addNote("previous borrow of '" + nameOf(conflict->place) +
                         "' by '" + nameOf(conflict->holder) + "' here",
                     conflict->location);
  report(std::move(diagnostic));
}

// -- Queries ------------------------------------------------------------------

std::vector<core::PlaceId>
FunctionDataflow::mirrors(core::PlaceId place,
                          const core::AnalysisState &state) {
  const auto parent = places.parent(place);
  if (!parent)
    return {place};

  std::vector<core::PlaceId> result;
  const auto add = [&result](core::PlaceId id) {
    if (!llvm::is_contained(result, id))
      result.push_back(id);
  };
  const core::PathStep step = places.step(place);
  for (const core::PlaceId parentMirror : mirrors(*parent, state)) {
    if (step == core::PathStep::Deref) {
      // `*p` is also `*q` for every alias q of p. Aliases *below* p
      // (`p ~ p->next`, which joins of a cyclic walk can produce) would make
      // the mirror deeper than the original and the expansion unbounded, so
      // they are skipped along with anything past the depth limit.
      for (const core::PlaceId alias : state.aliases.members(parentMirror)) {
        if (places.isDescendantOf(alias, parentMirror) ||
            places.depth(alias) >= MaxPlaceDepth)
          continue;
        add(places.deref(alias));
      }
    } else if (step == core::PathStep::Field) {
      add(places.field(parentMirror, places.fieldName(place)));
    } else {
      add(places.index(parentMirror));
    }
  }
  return result;
}

std::vector<core::PlaceId>
FunctionDataflow::targets(core::PlaceId place,
                          const core::AnalysisState &state) {
  // Direct aliases of every mirror. Deliberately not the transitive closure:
  // the relation is already closed under copies, and chasing aliases of
  // aliases would re-introduce the join-time transitivity `AliasRelation`
  // avoids (see its header).
  std::vector<core::PlaceId> result;
  for (const core::PlaceId mirror : mirrors(place, state)) {
    for (const core::PlaceId member : state.aliases.members(mirror)) {
      if (!llvm::is_contained(result, member))
        result.push_back(member);
    }
  }
  std::ranges::sort(result);
  return result;
}

std::vector<core::PlaceId> FunctionDataflow::related(core::PlaceId place) {
  std::vector<core::PlaceId> result{place};
  llvm::append_range(result, places.ancestors(place));
  llvm::append_range(result, places.descendants(place));
  return result;
}

std::optional<FunctionDataflow::MovedHit>
FunctionDataflow::findMoved(core::PlaceId place,
                            const core::AnalysisState &state) {
  // Facts are propagated eagerly to every alias and mirror when they are
  // created (`doConsume`, `mirrorSubtree`), so a query only needs to look at
  // the place itself. Looking at the whole class here would turn the
  // may-alias over-approximation introduced by joins into false positives:
  // after `cur = next` in a list walk, `cur` may alias both the freed node
  // and the live one, but only the freed node carries a move record.
  if (const auto record = state.moves.movedAt(place))
    return MovedHit{.target = place, .record = *record};
  return std::nullopt;
}

std::optional<core::Loan>
FunctionDataflow::findLoanConflict(core::PlaceId place,
                                   std::optional<core::BorrowKind> kind,
                                   const core::AnalysisState &state) {
  for (const core::PlaceId candidate : related(place)) {
    for (const core::Loan &loan : state.loans.loans()) {
      if (loan.place != candidate)
        continue;
      // Moves and mutations conflict with any loan; a new borrow only with
      // a mutable one on either side.
      if (!kind || *kind == core::BorrowKind::Mutable ||
          loan.kind == core::BorrowKind::Mutable)
        return loan;
    }
  }
  return std::nullopt;
}

core::LifetimeId FunctionDataflow::rootLifetime(core::PlaceId place) {
  const core::PlaceId root = places.root(place);
  const VarDecl *var = builder.varForPlace(root);
  if (var == nullptr)
    return fnLifetime;
  if (var->hasGlobalStorage())
    return core::LifetimeId::staticLifetime();
  const auto it = varLifetimes.find(var->getCanonicalDecl());
  return it == varLifetimes.end() ? fnLifetime : it->second;
}

std::vector<core::LifetimeId>
FunctionDataflow::lifetimesOfPlace(core::PlaceId place,
                                   const core::AnalysisState &state) {
  const auto deref = places.innermostDeref(place);
  if (!deref)
    return {rootLifetime(place)};

  // The object lives as long as whatever the dereferenced pointer refers to:
  // the objects it borrows if it holds loans, otherwise something owned by
  // the caller or the heap, which we can only assume outlives this call.
  const core::PlaceId pointer = *places.parent(*deref);
  std::vector<core::LifetimeId> result;
  for (const core::Loan &loan : state.loans.heldBy(pointer)) {
    if (!llvm::is_contained(result, loan.lifetime))
      result.push_back(loan.lifetime);
  }
  if (result.empty())
    result.push_back(callerLifetime);
  return result;
}

core::LifetimeId FunctionDataflow::meet(std::vector<core::LifetimeId> ids) {
  if (ids.size() == 1)
    return ids.front();
  std::vector<std::uint32_t> key;
  key.reserve(ids.size());
  for (const core::LifetimeId id : ids)
    key.push_back(id.value);
  std::ranges::sort(key);
  key.erase(std::ranges::unique(key).begin(), key.end());
  if (key.size() == 1)
    return core::LifetimeId{key.front()};

  // The shortest of several lifetimes: a fresh region each of them outlives.
  // Memoised so the dataflow state stays finite.
  auto [it, inserted] = meetCache.try_emplace(key, core::LifetimeId{});
  if (inserted) {
    std::string name = "min(";
    for (std::size_t i = 0; i < key.size(); ++i) {
      if (i != 0)
        name += ',';
      name += lifetimes.name(core::LifetimeId{key[i]});
    }
    name += ')';
    it->second = lifetimes.fresh(std::move(name));
    for (const std::uint32_t id : key)
      lifetimes.addOutlives(core::LifetimeId{id}, it->second);
  }
  return it->second;
}

// -- Diagnostics --------------------------------------------------------------

core::SourceLocation FunctionDataflow::locate(const Stmt &stmt) const {
  return toCoreLocation(context.getSourceManager(), stmt.getBeginLoc());
}

core::SourceLocation FunctionDataflow::locate(clang::SourceLocation loc) const {
  return toCoreLocation(context.getSourceManager(), loc);
}

std::string FunctionDataflow::nameOf(core::PlaceId place) const {
  return std::string(places.name(place));
}

void FunctionDataflow::report(core::Diagnostic diagnostic) {
  if (phase == Phase::Final && emitDiagnostics)
    pending.push_back(std::move(diagnostic));
}

void FunctionDataflow::flushDiagnostics() {
  const auto key = [](const core::Diagnostic &d) {
    return std::tie(d.location.file, d.location.line, d.location.column, d.id,
                    d.message);
  };
  std::ranges::stable_sort(pending, [&key](const core::Diagnostic &lhs,
                                           const core::Diagnostic &rhs) {
    return key(lhs) < key(rhs);
  });
  for (const core::Diagnostic &diagnostic : pending)
    sink.report(diagnostic);
  pending.clear();
}

void FunctionDataflow::reportUseOfMoved(core::PlaceId used, const MovedHit &hit,
                                        const Expr &at) {
  const bool freed = hit.record.reason == core::MoveReason::Freed;
  core::Diagnostic diagnostic{
      .severity = core::Severity::Error,
      .id = freed ? core::diag::UseAfterFree : core::diag::UseAfterMove,
      .message = "use of '" + nameOf(used) + "' after it was " +
                 (freed ? "freed" : "moved"),
      .location = locate(at),
      .notes = {},
      .fixits = {},
  };
  std::string note = freed ? "freed here" : "moved here";
  const core::PlaceId via = hit.record.via.value_or(hit.target);
  if (via != used)
    note += " (through '" + nameOf(via) + "')";
  diagnostic.addNote(std::move(note), hit.record.location);
  report(std::move(diagnostic));
}

void FunctionDataflow::reportLifetimeTooShort(core::PlaceId holder,
                                              core::PlaceId borrowed,
                                              const Expr &at, bool returned) {
  const core::PlaceId borrowedRoot = places.root(borrowed);
  const std::string borrowedName = nameOf(borrowedRoot);
  core::Diagnostic diagnostic{
      .severity = core::Severity::Error,
      .id = core::diag::LifetimeTooShort,
      .message = returned && holder == borrowed
                     ? "returned pointer may outlive '" + borrowedName +
                           "', which it points to"
                     : "'" + nameOf(holder) + "' may outlive '" + borrowedName +
                           "', which it points to",
      .location = locate(at),
      .notes = {},
      .fixits = {},
  };
  if (const VarDecl *var = builder.varForPlace(borrowedRoot)) {
    diagnostic.addNote("'" + borrowedName + "' is declared here",
                       locate(var->getLocation()));
    if (!returned) {
      const auto end = scopeEnds.find(rootLifetime(borrowedRoot).value);
      if (end != scopeEnds.end())
        diagnostic.addNote("'" + borrowedName + "' goes out of scope here",
                           end->second);
    }
  }
  report(std::move(diagnostic));
}

// -- Dump ---------------------------------------------------------------------

void FunctionDataflow::dump(const core::AnalysisState *exitState) {
  llvm::raw_ostream &os = *options.dumpStream;
  os << "function '" << function.getNameAsString() << "':\n";

  os << "  places:";
  for (const VarDecl *var : builder.variables()) {
    const auto place = builder.lookupVar(*var);
    if (!place)
      continue;
    const char *storage = "local";
    if (isa<ParmVarDecl>(var))
      storage = "param";
    else if (var->hasGlobalStorage())
      storage = "global";
    core::OwnershipKind kind = core::OwnershipKind::Unknown;
    if (const auto it = summaryKinds.find(*place); it != summaryKinds.end())
      kind = it->second;
    os << " " << places.name(*place) << " (" << storage;
    if (var->getType()->isPointerType())
      os << ", " << core::toString(kind);
    os << ")";
  }
  os << "\n";

  os << "  lifetimes:";
  for (std::uint32_t i = 1; i < lifetimes.size(); ++i)
    os << " " << lifetimes.name(core::LifetimeId{i});
  os << "\n";

  os << "  exit:";
  if (exitState == nullptr) {
    os << " <unreachable>\n";
  } else {
    os << " moved{";
    bool first = true;
    for (const core::PlaceId place : exitState->moves.movedPlaces()) {
      const auto record = exitState->moves.movedAt(place);
      os << (first ? "" : ", ") << places.name(place) << "@"
         << record->location.line << ":" << record->location.column << " "
         << (record->reason == core::MoveReason::Freed ? "freed" : "moved");
      first = false;
    }
    os << "} loans{";
    first = true;
    for (const core::Loan &loan : exitState->loans.loans()) {
      os << (first ? "" : ", ") << places.name(loan.place) << ": "
         << core::toString(loan.kind) << " by " << places.name(loan.holder)
         << " for " << lifetimes.name(loan.lifetime);
      first = false;
    }
    os << "} aliases{";
    first = true;
    for (const auto &[a, b] : exitState->aliases.pairs()) {
      os << (first ? "" : ", ") << places.name(a) << "~" << places.name(b);
      first = false;
    }
    os << "}\n";
  }

  os << "  summary:";
  const auto describeSource = [this](const core::ValueSource &source) {
    std::string text(core::toString(source.kind));
    if (source.path)
      text += " " + summaryName(*source.path);
    return text;
  };
  for (const auto &[path, effect] : inferred.effects) {
    os << " " << summaryName(path) << ":";
    const char *sep = " ";
    for (const auto &[flag, label] :
         {std::pair{effect.read, "read"}, std::pair{effect.written, "written"},
          std::pair{effect.freed, "freed"}, std::pair{effect.moved, "moved"}}) {
      if (flag) {
        os << sep << label;
        sep = "|";
      }
    }
    os << ";";
  }
  os << " stores{";
  bool first = true;
  for (const core::Store &store : inferred.stores) {
    os << (first ? "" : ", ") << summaryName(store.dest) << " = "
       << describeSource(store.value);
    first = false;
  }
  os << "} returns{";
  first = true;
  for (const core::ValueSource &source : inferred.returns) {
    os << (first ? "" : ", ") << describeSource(source);
    first = false;
  }
  os << "}";
  if (inferred.reallocLike)
    os << " realloc-like";
  os << "\n";
}

std::string FunctionDataflow::summaryName(const core::SummaryPath &path) const {
  std::string root;
  if (path.isParam()) {
    root = path.index < function.getNumParams()
               ? function.getParamDecl(path.index)->getNameAsString()
               : "param" + std::to_string(path.index);
    if (root.empty())
      root = "param" + std::to_string(path.index);
  } else {
    root = summaries.globals().nameOf(path.index).str();
  }
  return path.toString(root);
}

// -- Summary recording (RFC 0003) ---------------------------------------------

std::optional<core::SummaryPath>
FunctionDataflow::stableSummaryPathOf(core::PlaceId place) {
  auto path = builder.summaryPathOf(place);
  if (path && path->isParam() && path->index < paramReassigned.size() &&
      paramReassigned[path->index])
    return std::nullopt;
  return path;
}

void FunctionDataflow::recordAccess(core::PlaceId place, bool write,
                                    const core::AnalysisState &state) {
  if (!recording())
    return;
  std::vector<core::PlaceId> affected = mirrors(place, state);
  if (!llvm::is_contained(affected, place))
    affected.push_back(place);
  for (const core::PlaceId affectedPlace : affected) {
    const auto path = builder.summaryPathOf(affectedPlace);
    // Only caller memory counts: the parameter variable itself is the
    // callee's own copy, and a global's value is reported through stores.
    if (!path || !path->hasDeref())
      continue;
    inferred.addEffect(*path, write ? core::PlaceEffect{.written = true}
                                    : core::PlaceEffect{.read = true});
  }
}

void FunctionDataflow::recordConsume(core::PlaceId target,
                                     core::MoveReason reason) {
  if (!recording())
    return;
  const auto path = builder.summaryPathOf(target);
  if (!path)
    return;
  eventEffects[*path].join(reason == core::MoveReason::Freed
                               ? core::PlaceEffect{.freed = true}
                               : core::PlaceEffect{.moved = true});
}

void FunctionDataflow::recordStore(core::PlaceId dest,
                                   const core::ValueSource &value) {
  if (!recording())
    return;
  const auto path = stableSummaryPathOf(dest);
  // Caller-visible destinations only: memory below a dereference, or a
  // global (including a `static` local, which outlives the call).
  if (!path || (path->isParam() && !path->hasDeref()))
    return;
  inferred.addStore(core::Store{.dest = *path, .value = value});
}

core::ValueSource FunctionDataflow::sourceOf(const ValueOrigin &origin,
                                             const core::AnalysisState &state) {
  switch (origin.kind) {
  case ValueOrigin::Kind::Alloc:
  case ValueOrigin::Kind::Realloc:
    return core::ValueSource::fresh();
  case ValueOrigin::Kind::Null:
    return core::ValueSource::null();
  case ValueOrigin::Kind::Borrow:
    if (origin.place) {
      if (const auto path = stableSummaryPathOf(origin.place->place))
        return core::ValueSource::borrow(*path);
    }
    return core::ValueSource::unknown();
  case ValueOrigin::Kind::Copy: {
    if (!origin.place)
      return core::ValueSource::unknown();
    const core::PlaceId src = origin.place->place;
    if (const auto path = stableSummaryPathOf(src))
      return core::ValueSource::copy(*path);
    // A local: resolve through what it aliases, then what it borrows, then
    // what it owns.
    for (const core::PlaceId alias : state.aliases.members(src)) {
      if (const auto path = stableSummaryPathOf(alias))
        return core::ValueSource::copy(*path);
    }
    for (const core::Loan &loan : state.loans.heldBy(src)) {
      if (const auto path = stableSummaryPathOf(loan.place))
        return core::ValueSource::borrow(*path);
    }
    if (state.kindOf(src) == core::OwnershipKind::Owned)
      return core::ValueSource::fresh();
    return core::ValueSource::unknown();
  }
  case ValueOrigin::Kind::Opaque:
  case ValueOrigin::Kind::Conditional:
    return core::ValueSource::unknown();
  }
  return core::ValueSource::unknown();
}

void FunctionDataflow::finalizeSummary(const core::AnalysisState *exitState) {
  // Parameter roots, and everything under a reassigned parameter, take their
  // consumption from the events; everything else from the exit state (RFC
  // 0003, *Deriving a summary*).
  for (const auto &[path, effect] : eventEffects) {
    const bool eventBased =
        path.isParam() &&
        (path.isRoot() ||
         (path.index < paramReassigned.size() && paramReassigned[path.index]));
    if (eventBased)
      inferred.addEffect(path, effect);
  }
  if (exitState != nullptr) {
    for (const core::PlaceId place : exitState->moves.movedPlaces()) {
      const auto path = builder.summaryPathOf(place);
      if (!path)
        continue;
      const auto record = exitState->moves.movedAt(place);
      inferred.addEffect(*path, record->reason == core::MoveReason::Freed
                                    ? core::PlaceEffect{.freed = true}
                                    : core::PlaceEffect{.moved = true});
    }
  }
}

// -- Reconciliation (RFC 0003) ------------------------------------------------

std::optional<FunctionDataflow::AnnotatedParam>
FunctionDataflow::borrowedParamFor(core::PlaceId place,
                                   const core::AnalysisState &state) {
  std::vector<core::PlaceId> candidates{place};
  llvm::append_range(candidates, state.aliases.members(place));
  for (const core::PlaceId candidate : candidates) {
    if (!places.isBase(candidate))
      continue;
    const auto *param =
        dyn_cast_if_present<ParmVarDecl>(builder.varForPlace(candidate));
    if (param == nullptr)
      continue;
    const unsigned index = param->getFunctionScopeIndex();
    if (index >= signature.params.size())
      continue;
    if (signature.params[index].borrowed)
      return AnnotatedParam{.place = candidate,
                            .annotation = Annotation::Borrowed};
    if (signature.params[index].mutBorrowed)
      return AnnotatedParam{.place = candidate,
                            .annotation = Annotation::MutBorrowed};
  }
  return std::nullopt;
}

void FunctionDataflow::checkAnnotationOnConsume(
    const PlaceRef &ref, core::MoveReason reason, const Expr &at,
    const core::AnalysisState &state) {
  if (!recording())
    return;
  const char *verb = reason == core::MoveReason::Freed ? "freed" : "moved";
  if (const auto param = borrowedParamFor(ref.place, state)) {
    reportMismatch(*param, std::string("is ") + verb + " here", ref.place, at);
    return;
  }
  // Releasing something the borrowed object owns mutates it.
  if (!ref.derefs.empty()) {
    const core::PlaceId through = ref.derefs.back();
    const auto param = borrowedParamFor(through, state);
    if (param && param->annotation == Annotation::Borrowed)
      reportMismatch(*param, "'" + nameOf(ref.place) + "' is " + verb + " here",
                     through, at);
  }
}

void FunctionDataflow::checkAnnotationOnWrite(
    const PlaceRef &ref, const Expr &at, const core::AnalysisState &state) {
  if (!recording() || ref.derefs.empty())
    return;
  const core::PlaceId through = ref.derefs.back();
  const auto param = borrowedParamFor(through, state);
  if (param && param->annotation == Annotation::Borrowed)
    reportMismatch(*param, "is written through here", through, at);
}

void FunctionDataflow::checkAnnotationOnReturn(
    const ValueOrigin &origin, const Expr &at,
    const core::AnalysisState &state) {
  if (!recording())
    return;
  const AnnotationSet &annotation = signature.result;
  const bool promisesOwned = annotation.owned;
  const bool promisesBorrow = annotation.borrowed || annotation.mutBorrowed;
  if (!promisesOwned && !promisesBorrow)
    return;

  const core::ValueSource source = sourceOf(origin, state);
  std::string message;
  if (promisesOwned && source.kind == core::ValueSource::Kind::Borrow) {
    message = "function returns a borrow but its return type is annotated "
              "WEAVEC_OWNED";
  } else if (promisesBorrow && source.kind == core::ValueSource::Kind::Fresh) {
    message = std::string("function returns a fresh allocation but its return "
                          "type is annotated ") +
              (annotation.borrowed ? "WEAVEC_BORROWED" : "WEAVEC_MUT");
  } else {
    return;
  }
  core::Diagnostic diagnostic{
      .severity = core::Severity::Error,
      .id = core::diag::AnnotationMismatch,
      .message = std::move(message),
      .location = locate(at),
      .notes = {},
      .fixits = {},
  };
  diagnostic.addNote("annotated here", locate(function.getLocation()));
  report(std::move(diagnostic));
}

void FunctionDataflow::reportMismatch(const AnnotatedParam &param,
                                      const std::string &what,
                                      core::PlaceId through, const Expr &at) {
  const std::string paramName = nameOf(param.place);
  const char *macro = param.annotation == Annotation::Borrowed
                          ? "WEAVEC_BORROWED"
                          : "WEAVEC_MUT";
  core::Diagnostic diagnostic{
      .severity = core::Severity::Error,
      .id = core::diag::AnnotationMismatch,
      .message = "'" + paramName + "' is annotated " + macro + " but " + what,
      .location = locate(at),
      .notes = {},
      .fixits = {},
  };
  if (const VarDecl *var = builder.varForPlace(param.place))
    diagnostic.addNote("'" + paramName + "' is annotated here",
                       locate(var->getLocation()));
  if (through != param.place)
    diagnostic.addNote("'" + nameOf(through) + "' is a copy of '" + paramName +
                           "'",
                       locate(at));
  report(std::move(diagnostic));
}

} // namespace weavec::analysis
