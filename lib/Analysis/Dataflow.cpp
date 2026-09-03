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
//      record which statements sit inside `WEAVEC_UNSAFE` blocks, classify
//      every place expression by the role it plays in its parent (read,
//      written, consumed, borrowed) and compute the liveness of the
//      function's locals (RFC 0006).
//   2. A forward worklist iteration over `clang::CFG` computes the entry
//      state of every reachable block. Transfer functions translate CFG
//      elements into core events; diagnostics are suppressed. Conditional
//      edges refine the state (RFC 0006, *Condition facts*).
//   3. A final pass re-runs the transfer function once per block from the
//      fixpoint entry states with diagnostics enabled, emits them in source
//      order, and records the function's summary (RFC 0003): what it does
//      to its parameters and globals, what it stores through them, where
//      its result comes from and, per class of result, what it consumed on
//      the paths returning it (RFC 0006).
//
// Calls are interpreted through the callee's summary (RFC 0003, *Applying a
// summary at a call*), so the same semantic actions that handle `free(p)`
// handle `node_free(p)` and `o->drop(p)`.
//
// Unsafe regions (RFC 0004) are analysed like everything else; while the
// element being transferred lies in one, raw pointers may be dereferenced
// and released and no diagnostic is emitted.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Core/Ownership.h"

#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <deque>
#include <iterator>
#include <limits>
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

/// `raw pointer 'p'`, or just `raw pointer` for a value with no place.
static std::string rawPointerPhrase(const std::optional<std::string> &name) {
  return name ? "raw pointer '" + *name + "'" : std::string("raw pointer");
}

FunctionDataflow::FunctionDataflow(ASTContext &ctx, const FunctionDecl &fn,
                                   core::DiagnosticSink &diagSink,
                                   const AnalysisOptions &analysisOptions,
                                   SummaryStore &summaryStore, bool emitDiags)
    : context(ctx), function(fn), sink(diagSink), options(analysisOptions),
      summaries(summaryStore), emitDiagnostics(emitDiags),
      builder(places, summaryStore, ctx),
      callerLifetime(lifetimes.fresh("caller")),
      fnLifetime(lifetimes.fresh("fn")),
      paramReassigned(fn.getNumParams(), false),
      signature(collectAnnotations(fn)), unsafeBody(signature.unsafe),
      inUnsafe(unsafeBody) {
  lifetimes.addOutlives(callerLifetime, fnLifetime);
  builder.setStrictExterns(options.strictExterns);
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
  // An unsafe block is analysed like any other (RFC 0004); its statements
  // are only remembered so the transfer function knows where it is.
  if (isUnsafeBlock(*stmt))
    collectUnsafe(*stmt);
  if (const auto *decl = dyn_cast<DeclStmt>(stmt)) {
    for (const Decl *d : decl->decls()) {
      const auto *var = dyn_cast<VarDecl>(d);
      if (var == nullptr || var->getInit() == nullptr)
        continue;
      classifyExpr(var->getInit(), Role::Read);
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
    // `(uintptr_t)p` takes the value out of the model (RFC 0004); whatever
    // `p` owned may live on behind the integer (RFC 0007, *Escape*).
    if (cast->getCastKind() == CK_PointerToIntegral) {
      const Expr &operand = PlaceBuilder::stripTransparent(*cast->getSubExpr());
      if (PlaceBuilder::isPlaceExpr(operand))
        escapingExprs.insert(&operand);
    }
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

/// The call a statement expression consists of, if it is one: `f(x);`,
/// `(void)f(x);`.
static const CallExpr *discardedCall(const Stmt *stmt) {
  const auto *expr = dyn_cast_or_null<Expr>(stmt);
  if (expr == nullptr)
    return nullptr;
  const Expr *e = expr->IgnoreParens();
  while (const auto *cast = dyn_cast<CastExpr>(e)) {
    if (cast->getCastKind() != CK_ToVoid && !isa<ImplicitCastExpr>(cast))
      break;
    e = cast->getSubExpr()->IgnoreParens();
  }
  return dyn_cast<CallExpr>(e);
}

void FunctionDataflow::collectDiscardedCalls(const Stmt *stmt) {
  if (stmt == nullptr)
    return;
  // Statement positions: the children of a compound statement, the bodies
  // of control statements and a `for`'s increment. Conditions are not.
  const auto note = [this](const Stmt *child) {
    if (const CallExpr *call = discardedCall(child))
      discardedCalls.insert(call);
  };
  if (const auto *compound = dyn_cast<CompoundStmt>(stmt)) {
    for (const Stmt *child : compound->body())
      note(child);
  } else if (const auto *ifStmt = dyn_cast<IfStmt>(stmt)) {
    note(ifStmt->getThen());
    note(ifStmt->getElse());
  } else if (const auto *whileLoop = dyn_cast<WhileStmt>(stmt)) {
    note(whileLoop->getBody());
  } else if (const auto *doLoop = dyn_cast<DoStmt>(stmt)) {
    note(doLoop->getBody());
  } else if (const auto *forLoop = dyn_cast<ForStmt>(stmt)) {
    note(forLoop->getBody());
    note(forLoop->getInc());
  } else if (const auto *label = dyn_cast<LabelStmt>(stmt)) {
    note(label->getSubStmt());
  } else if (const auto *switchCase = dyn_cast<SwitchCase>(stmt)) {
    note(switchCase->getSubStmt());
  } else if (const auto *switchStmt = dyn_cast<SwitchStmt>(stmt)) {
    note(switchStmt->getBody());
  }
  for (const Stmt *child : stmt->children())
    collectDiscardedCalls(child);
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
    else if (annotations.raw)
      kind = core::OwnershipKind::Raw;
    if (annotations.ownership())
      declaredKinds[place] = annotations;
    setKind(place, kind, state);
    if (annotations.owned) {
      // The caller handed the resource over: releasing it is this function's
      // job (RFC 0007, *Acquire*).
      state.resources.hold(
          place, core::ResourceRecord{.origin = core::ResourceOrigin::Declared,
                                      .location = locate(param->getLocation()),
                                      .family = {},
                                      .escaped = false});
    }
    if (annotations.raw) {
      markRaw(place,
              core::RawRecord{.reason = core::RawReason::Declared,
                              .location = locate(param->getLocation()),
                              .via = std::nullopt,
                              .detail = nameOf(place)},
              state);
    }
  }
  return state;
}

// -- Liveness (RFC 0006) ------------------------------------------------------

/// The local variable an address-of or array decay exposes: `&v`, `&v.f`,
/// `&v[i]`, `v` decaying to a pointer. Null for anything else.
static const VarDecl *addressedLocal(const Expr &operand) {
  const Expr *e = operand.IgnoreParenCasts();
  while (true) {
    if (const auto *member = dyn_cast<MemberExpr>(e);
        member != nullptr && !member->isArrow()) {
      e = member->getBase()->IgnoreParenCasts();
      continue;
    }
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(e);
        subscript != nullptr &&
        subscript->getBase()->IgnoreParenImpCasts()->getType()->isArrayType()) {
      e = subscript->getBase()->IgnoreParenCasts();
      continue;
    }
    break;
  }
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
        var != nullptr && var->isLocalVarDeclOrParm() && !var->isStaticLocal())
      return var->getCanonicalDecl();
  }
  return nullptr;
}

void FunctionDataflow::computeLiveness() {
  // Domain: the locals (parameters included) referenced anywhere in the
  // CFG. Address-taken locals are recorded on the way: they may be read
  // through the pointer at any time, so their loans never expire on
  // liveness.
  llvm::DenseSet<const DeclRefExpr *> assignedRefs;
  const auto localOf = [](const DeclRefExpr &ref) -> const VarDecl * {
    const auto *var = dyn_cast<VarDecl>(ref.getDecl());
    if (var == nullptr || !var->isLocalVarDeclOrParm() || var->isStaticLocal())
      return nullptr;
    return var->getCanonicalDecl();
  };
  const auto indexOf = [this](const VarDecl &var) {
    return liveIndex.try_emplace(&var, liveIndex.size()).first->second;
  };
  for (const CFGBlock *block : *cfg) {
    if (block == nullptr)
      continue;
    for (const CFGElement &element : *block) {
      const auto stmtElement = element.getAs<CFGStmt>();
      if (!stmtElement)
        continue;
      const Stmt *stmt = stmtElement->getStmt();
      if (const auto *ref = dyn_cast<DeclRefExpr>(stmt)) {
        if (const VarDecl *var = localOf(*ref))
          indexOf(*var);
      } else if (const auto *decl = dyn_cast<DeclStmt>(stmt)) {
        for (const Decl *d : decl->decls()) {
          if (const auto *var = dyn_cast<VarDecl>(d);
              var != nullptr && !var->isStaticLocal())
            indexOf(*var->getCanonicalDecl());
        }
      } else if (const auto *binary = dyn_cast<BinaryOperator>(stmt);
                 binary != nullptr && binary->getOpcode() == BO_Assign) {
        if (const auto *lhs =
                dyn_cast<DeclRefExpr>(binary->getLHS()->IgnoreParens()))
          assignedRefs.insert(lhs);
      } else if (const auto *unary = dyn_cast<UnaryOperator>(stmt);
                 unary != nullptr && unary->getOpcode() == UO_AddrOf) {
        if (const VarDecl *var = addressedLocal(*unary->getSubExpr()))
          addressTaken.insert(var);
      } else if (const auto *cast = dyn_cast<CastExpr>(stmt);
                 cast != nullptr &&
                 cast->getCastKind() == CK_ArrayToPointerDecay) {
        if (const VarDecl *var = addressedLocal(*cast->getSubExpr()))
          addressTaken.insert(var);
      }
    }
  }

  const unsigned width = liveIndex.size();
  const unsigned blocks = cfg->getNumBlockIDs();
  liveBefore.assign(blocks, {});
  liveOut.assign(blocks, llvm::BitVector(width));
  liveIn.assign(blocks, llvm::BitVector(width));

  // Every reference below a statement is a use of that statement: the value
  // read by an operand is consumed by the enclosing expression, so `p` in
  // `return p;` or `q = p;` must stay live up to the `ReturnStmt` /
  // assignment element, which are later elements than the `DeclRefExpr`.
  const auto markUses = [&](const Stmt &root, llvm::BitVector &live) {
    llvm::SmallVector<const Stmt *, 16> work{&root};
    while (!work.empty()) {
      const Stmt *stmt = work.pop_back_val();
      if (const auto *ref = dyn_cast<DeclRefExpr>(stmt)) {
        if (assignedRefs.contains(ref))
          continue;
        if (const VarDecl *var = localOf(*ref))
          live.set(liveIndex.lookup(var));
        continue;
      }
      for (const Stmt *child : stmt->children()) {
        if (child != nullptr)
          work.push_back(child);
      }
    }
  };

  // One element of a block, backwards: the assignment kills its target (the
  // target's own `DeclRefExpr`, an earlier element, is then not a use) and
  // then uses what its right operand references; a declaration kills what it
  // declares and uses its initialisers; anything else uses its references.
  const auto transferElement = [&](const CFGElement &element,
                                   llvm::BitVector &live) {
    const auto stmtElement = element.getAs<CFGStmt>();
    if (!stmtElement)
      return;
    const Stmt *stmt = stmtElement->getStmt();
    if (const auto *decl = dyn_cast<DeclStmt>(stmt)) {
      for (const Decl *d : decl->decls()) {
        const auto *var = dyn_cast<VarDecl>(d);
        if (var == nullptr || var->isStaticLocal())
          continue;
        live.reset(liveIndex.lookup(var->getCanonicalDecl()));
        if (const Expr *init = var->getInit())
          markUses(*init, live);
      }
      return;
    }
    if (const auto *binary = dyn_cast<BinaryOperator>(stmt);
        binary != nullptr && binary->getOpcode() == BO_Assign) {
      if (const auto *lhs =
              dyn_cast<DeclRefExpr>(binary->getLHS()->IgnoreParens())) {
        if (const VarDecl *var = localOf(*lhs))
          live.reset(liveIndex.lookup(var));
      } else {
        markUses(*binary->getLHS(), live);
      }
      markUses(*binary->getRHS(), live);
      return;
    }
    markUses(*stmt, live);
  };

  // Blocks are numbered from the exit up, so iterating them in that order
  // visits most successors before their predecessors; the loop repeats
  // until nothing changes whatever the order.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const CFGBlock *block : *cfg) {
      if (block == nullptr)
        continue;
      llvm::BitVector live(width);
      for (const CFGBlock::AdjacentBlock &adjacent : block->succs()) {
        if (const CFGBlock *succ = adjacent.getReachableBlock())
          live |= liveIn[succ->getBlockID()];
      }
      liveOut[block->getBlockID()] = live;
      std::vector<llvm::BitVector> &before = liveBefore[block->getBlockID()];
      before.assign(block->size(), llvm::BitVector(width));
      for (std::size_t i = block->size(); i-- > 0;) {
        transferElement((*block)[i], live);
        before[i] = live;
      }
      if (live != liveIn[block->getBlockID()]) {
        liveIn[block->getBlockID()] = std::move(live);
        changed = true;
      }
    }
  }
}

void FunctionDataflow::expireDeadLoans(const CFGBlock &block, std::size_t index,
                                       core::AnalysisState &state) {
  const std::vector<llvm::BitVector> &before = liveBefore[block.getBlockID()];
  if (index >= before.size())
    return;
  const llvm::BitVector &live = before[index];
  if (state.loans.loans().empty() && state.aliases.size() == 0)
    return;
  // Within a block only an element that kills a variable can expire
  // anything; the block's first element sees what the predecessors left.
  if (index > 0) {
    llvm::BitVector died = before[index - 1];
    died.reset(live);
    if (died.none())
      return;
  }
  // A local that is dead here (not live, not address-taken, not a global or
  // `static` local) will not be read again (RFC 0006, *Loans end at the last
  // use of their holder*). Memoised per root: hundreds of loans and edges
  // share a few dozen roots.
  llvm::SmallDenseMap<std::uint32_t, const VarDecl *, 32> roots;
  const auto rootVar = [this, &roots](core::PlaceId place) {
    const core::PlaceId root = places.root(place);
    auto [it, inserted] = roots.try_emplace(root.value, nullptr);
    if (inserted)
      it->second = builder.varForPlace(root);
    return it->second;
  };
  const auto deadLocal = [this, &live](const VarDecl *var) {
    if (var == nullptr || var->hasGlobalStorage())
      return false;
    const VarDecl *canonical = var->getCanonicalDecl();
    if (addressTaken.contains(canonical))
      return false;
    const auto it = liveIndex.find(canonical);
    return it != liveIndex.end() && !live.test(it->second);
  };
  state.loans.expireHolders([this, &rootVar, &deadLocal](core::PlaceId holder) {
    // Loans held through a pointer (`node->buf`) or by a global end only
    // when the holder is reassigned.
    if (places.innermostDeref(holder))
      return false;
    return deadLocal(rootVar(holder));
  });
  // A dead local variable, and everything below it, holds nothing anyone
  // can observe any more: its alias edges go too. This is what keeps the
  // relation small in a function with hundreds of block-scoped pointers
  // (an interpreter loop), where scope ends may never appear in the CFG
  // (computed `goto`). Parameters are exempt: a live local's edge to a dead
  // parameter is how its accesses reach the summary (`m = n; return m->v`
  // borrows `n`). What a dead pointer's object refers to is still held
  // there, through the pointer's live aliases (RFC 0007, *Escape*).
  const auto dying = [&rootVar, &deadLocal](core::PlaceId place) {
    const VarDecl *var = rootVar(place);
    return !isa_and_nonnull<ParmVarDecl>(var) && deadLocal(var);
  };
  std::vector<core::PlaceId> deadRoots;
  for (const auto &[a, b] : state.aliases.pairs()) {
    for (const core::PlaceId end : {a, b}) {
      const core::PlaceId root = places.root(end);
      if (end == root && dying(root) && !llvm::is_contained(deadRoots, root))
        deadRoots.push_back(root);
    }
  }
  for (const core::PlaceId root : deadRoots)
    loseTrackBelow(root, state);
  state.aliases.separateIf(dying);
}

// -- Resources (RFC 0007) -----------------------------------------------------

std::vector<core::PlaceId> FunctionDataflow::storageOf(core::PlaceId place) {
  std::vector<core::PlaceId> result{place};
  for (const core::PlaceId child : places.descendants(place)) {
    bool crossesDeref = false;
    for (core::PlaceId cursor = child; cursor != place;
         cursor = *places.parent(cursor)) {
      if (places.step(cursor) == core::PathStep::Deref) {
        crossesDeref = true;
        break;
      }
    }
    if (!crossesDeref)
      result.push_back(child);
  }
  return result;
}

void FunctionDataflow::escape(core::PlaceId place, core::AnalysisState &state) {
  state.resources.escape(place);
  for (const core::PlaceId mirror : mirrors(place, state))
    state.resources.escape(mirror);
}

void FunctionDataflow::escapeValue(const ValueOrigin &origin, bool deep,
                                   core::AnalysisState &state) {
  switch (origin.kind) {
  case ValueOrigin::Kind::Conditional:
    for (const ValueOrigin &alternative : origin.alternatives)
      escapeValue(alternative, deep, state);
    break;
  case ValueOrigin::Kind::Copy:
    if (!origin.place)
      break;
    escape(origin.place->place, state);
    if (deep) {
      for (const core::PlaceId below :
           storageOf(places.deref(origin.place->place)))
        escape(below, state);
    }
    break;
  case ValueOrigin::Kind::Borrow:
    // `&s`: the callee may copy out whatever `s`'s storage holds.
    if (origin.place && deep) {
      for (const core::PlaceId below : storageOf(origin.place->place))
        escape(below, state);
    }
    break;
  default:
    break;
  }
}

bool FunctionDataflow::resourceLost(
    core::PlaceId place, const core::ResourceRecord &record,
    const std::function<bool(core::PlaceId)> &dying,
    const core::AnalysisState &state) {
  // Released or moved (any element witness: `free(a[i])` in a loop accounts
  // for `a[*]`), or handed to code nobody can see.
  if (record.escaped || state.moves.recordOf(place))
    return false;
  // Every other name must be released, moved or dying too; a name that still
  // reaches the resource keeps it. A stale may-alias from a join can only
  // make this say "not lost".
  return llvm::all_of(
      state.aliases.members(place), [&](const core::PlaceId alias) {
        return alias == place || state.resources.isEscaped(alias) ||
               state.moves.recordOf(alias) || dying(alias);
      });
}

void FunctionDataflow::reportLeak(core::PlaceId place,
                                  const core::ResourceRecord &record,
                                  std::string message,
                                  const core::SourceLocation &at) {
  core::Diagnostic diagnostic{
      .severity = core::Severity::Warning,
      .id = core::diag::Leak,
      .message = std::move(message),
      .location = at,
      .notes = {},
      .fixits = {},
  };
  if (record.location.isValid()) {
    if (record.origin == core::ResourceOrigin::Declared) {
      std::string name = nameOf(place);
      if (const NamedDecl *decl = builder.declFor(place))
        name = decl->getNameAsString();
      diagnostic.addNote("'" + name + "' is declared WEAVEC_OWNED here",
                         record.location);
    } else {
      diagnostic.addNote("allocated here", record.location);
    }
  }
  report(std::move(diagnostic));
}

void FunctionDataflow::checkLeaks(
    const std::vector<core::PlaceId> &candidates,
    const std::function<bool(core::PlaceId)> &dying, LeakForm form,
    const core::SourceLocation &at, core::AnalysisState &state,
    std::optional<core::PlaceId> container) {
  for (const core::PlaceId place : candidates) {
    const auto record = state.resources.recordOf(place);
    if (!record || !resourceLost(place, *record, dying, state))
      continue;
    std::string message = "'" + nameOf(place) + "' is leaked";
    switch (form) {
    case LeakForm::Lost:
      break;
    case LeakForm::Overwritten:
      message += ": it is overwritten without being released";
      break;
    case LeakForm::Container:
      message += " when '" + nameOf(container.value_or(place)) + "' is freed";
      break;
    }
    reportLeak(place, *record, std::move(message), at);
    // One report per resource: the dying aliases hold the same one, and a
    // later death point (the scope end after the last use) must stay quiet.
    state.resources.clear(place);
    for (const core::PlaceId alias : state.aliases.members(place)) {
      if (alias != place && dying(alias))
        state.resources.clear(alias);
    }
  }
}

core::SourceLocation FunctionDataflow::locateElement(const CFGBlock &block,
                                                     std::size_t index) const {
  for (std::size_t i = index; i < block.size(); ++i) {
    if (const auto stmtElement = block[i].getAs<CFGStmt>()) {
      if (const Stmt *stmt = stmtElement->getStmt())
        return locate(*stmt);
    }
  }
  if (const Stmt *terminator = block.getTerminatorStmt())
    return locate(*terminator);
  for (std::size_t i = std::min<std::size_t>(index, block.size()); i-- > 0;) {
    if (const auto stmtElement = block[i].getAs<CFGStmt>()) {
      if (const Stmt *stmt = stmtElement->getStmt())
        return locate(*stmt);
    }
  }
  return locate(function.getBody()->getEndLoc());
}

std::optional<unsigned>
FunctionDataflow::localWrittenBy(const CFGElement &element) const {
  const auto stmtElement = element.getAs<CFGStmt>();
  if (!stmtElement)
    return std::nullopt;
  const VarDecl *written = nullptr;
  if (const auto *decl = dyn_cast_or_null<DeclStmt>(stmtElement->getStmt())) {
    if (decl->isSingleDecl())
      written = dyn_cast<VarDecl>(decl->getSingleDecl());
  } else if (const auto *binary =
                 dyn_cast_or_null<BinaryOperator>(stmtElement->getStmt());
             binary != nullptr && binary->getOpcode() == BO_Assign) {
    if (const auto *ref =
            dyn_cast<DeclRefExpr>(binary->getLHS()->IgnoreParens()))
      written = dyn_cast<VarDecl>(ref->getDecl());
  }
  if (written == nullptr || written->hasGlobalStorage())
    return std::nullopt;
  const auto it = liveIndex.find(written->getCanonicalDecl());
  if (it == liveIndex.end())
    return std::nullopt;
  return it->second;
}

bool FunctionDataflow::isCallerMemory(core::PlaceId place) const {
  // Memory below a parameter (`s->state`) is the caller's: the record stays
  // when the parameter's name dies, so the `return` after it can still say
  // the caller's memory holds what this function stored (*Per-outcome null
  // stores*). It is never a leak candidate of this function.
  return places.innermostDeref(place) &&
         isa_and_nonnull<ParmVarDecl>(builder.varForPlace(places.root(place)));
}

void FunctionDataflow::checkDeadResources(const CFGBlock &block,
                                          std::size_t index,
                                          core::AnalysisState &state) {
  // A block that ends in `exit(1)` ends the process: nothing that dies on the
  // way there leaks (RFC 0007, *Deliberately not caught*).
  if (index == 0 || state.resources.empty() || block.hasNoReturnElement())
    return;
  const std::vector<llvm::BitVector> &before = liveBefore[block.getBlockID()];
  if (index >= before.size())
    return;
  llvm::BitVector died = before[index - 1];
  died.reset(before[index]);
  // A local written by the previous element and never read (`char *p =
  // malloc(8);` with no use of `p`) was never live, so it is not in `died`;
  // its value is lost right here all the same.
  const llvm::BitVector &live = before[index];
  if (const auto written = localWrittenBy(block[index - 1]);
      written && !live.test(*written)) {
    // Unless it is dead only because this block writes it again: that is the
    // overwrite check's report (`p = malloc(8); p = malloc(16);`).
    bool rewritten = false;
    for (std::size_t i = index; i < block.size() && !rewritten; ++i)
      rewritten = localWrittenBy(block[i]) == written;
    if (!rewritten)
      died.set(*written);
  }
  if (died.none())
    return;

  // A local (not a global, not address-taken) that is dead before this
  // element will not be read again; one that just died is where a report
  // lands. Aliases that died earlier keep no record (their death was
  // checked), so "dead now" is the right notion for them.
  const auto liveBit = [this](core::PlaceId place) -> std::optional<unsigned> {
    const VarDecl *var = builder.varForPlace(places.root(place));
    if (var == nullptr || var->hasGlobalStorage())
      return std::nullopt;
    const VarDecl *canonical = var->getCanonicalDecl();
    if (addressTaken.contains(canonical))
      return std::nullopt;
    const auto it = liveIndex.find(canonical);
    if (it == liveIndex.end())
      return std::nullopt;
    return it->second;
  };
  std::vector<core::PlaceId> candidates;
  std::vector<core::PlaceId> stale;
  for (const core::PlaceId holder : state.resources.holders()) {
    const auto bit = liveBit(holder);
    if (!bit || !died.test(*bit))
      continue;
    if (!places.innermostDeref(holder))
      candidates.push_back(holder);
    else if (isCallerMemory(holder))
      continue;
    stale.push_back(holder);
  }
  if (stale.empty())
    return;
  const auto dying = [&liveBit, &live, this](core::PlaceId place) {
    if (places.innermostDeref(place))
      return false;
    const auto bit = liveBit(place);
    return bit && !live.test(*bit);
  };
  checkLeaks(candidates, dying, LeakForm::Lost, locateElement(block, index),
             state);
  // The dead name is never read again: what it still holds is held by the
  // alias that kept it alive, and a stale copy would be reported when the
  // name is reused (`q = malloc(4)` after `free(p)` with `q` once `p`).
  for (const core::PlaceId holder : stale)
    state.resources.clear(holder);
}

void FunctionDataflow::checkBlockEndResources(const CFGBlock &block,
                                              const CFGBlock *successor,
                                              core::AnalysisState &state) {
  // The exit block's predecessors have checked already; what reaches it
  // through a `noreturn` call never leaks (RFC 0007), nor does what dies on
  // the edge into the block that makes that call (`if (!p) fatal("...")`).
  if (state.resources.empty() || block.hasNoReturnElement() ||
      &block == &cfg->getExit() ||
      (successor != nullptr && successor->hasNoReturnElement()))
    return;
  const bool toExit = successor == nullptr || successor == &cfg->getExit();
  const std::vector<llvm::BitVector> &before = liveBefore[block.getBlockID()];
  // What is live on *this* edge is what the successor reads: `p` dies on
  // the edge into `return -1` even though the other arm still frees it.
  const llvm::BitVector &out = successor != nullptr
                                   ? liveIn[successor->getBlockID()]
                                   : liveOut[block.getBlockID()];

  // At the function's end every local and parameter dies, address-taken or
  // not; elsewhere only what liveness says died at the block's end.
  const auto localVar = [this](core::PlaceId place) -> const VarDecl * {
    const VarDecl *var = builder.varForPlace(places.root(place));
    if (var == nullptr || var->hasGlobalStorage())
      return nullptr;
    return var->getCanonicalDecl();
  };
  const auto dying = [&, this](core::PlaceId place) {
    if (places.innermostDeref(place))
      return false;
    const VarDecl *var = localVar(place);
    if (var == nullptr)
      return false;
    if (toExit)
      return true;
    if (addressTaken.contains(var))
      return false;
    const auto it = liveIndex.find(var);
    return it != liveIndex.end() && !out.test(it->second);
  };
  // Only what dies *here*: a local dead earlier in the block was checked
  // then, and its alias edges are gone, so looking again would find it
  // alone and call it lost. A local the last element wrote and nothing reads
  // was never live; it is lost here too.
  const std::optional<unsigned> justWritten =
      !block.empty() ? localWrittenBy(block.back()) : std::nullopt;
  std::vector<core::PlaceId> candidates;
  std::vector<core::PlaceId> stale;
  for (const core::PlaceId holder : state.resources.holders()) {
    const VarDecl *var = localVar(holder);
    if (var == nullptr)
      continue;
    const auto it = liveIndex.find(var);
    const bool deadNow = !toExit && !addressTaken.contains(var) &&
                         it != liveIndex.end() && !out.test(it->second);
    if (deadNow && !isCallerMemory(holder))
      stale.push_back(holder);
    if (!dying(holder))
      continue;
    if (toExit && addressTaken.contains(var)) {
      candidates.push_back(holder);
      continue;
    }
    if (before.empty() || it == liveIndex.end() ||
        before.back().test(it->second) || justWritten == it->second)
      candidates.push_back(holder);
  }
  if (!candidates.empty()) {
    // The report lands where the path goes next: the first statement of the
    // arm that loses the resource, else this block's terminator or its last
    // statement (the `return` on the edge into the exit).
    core::SourceLocation at;
    if (!toExit && successor != nullptr) {
      // CFG elements are in evaluation order, operands before the statement
      // that consumes them; the report goes on the outermost statement that
      // spans the first element (`return -1`, not `-1`).
      const SourceManager &sm = context.getSourceManager();
      const Stmt *first = nullptr;
      for (const CFGElement &element : *successor) {
        const auto stmtElement = element.getAs<CFGStmt>();
        if (!stmtElement || stmtElement->getStmt() == nullptr)
          continue;
        const Stmt *stmt = stmtElement->getStmt();
        if (first == nullptr ||
            (!sm.isBeforeInTranslationUnit(first->getBeginLoc(),
                                           stmt->getBeginLoc()) &&
             !sm.isBeforeInTranslationUnit(stmt->getEndLoc(),
                                           first->getEndLoc())))
          first = stmt;
      }
      if (first != nullptr)
        at = locate(*first);
    }
    if (!at.isValid()) {
      at = locateElement(block, block.size());
      if (const Stmt *terminator = block.getTerminatorStmt())
        at = locate(*terminator);
    }
    checkLeaks(candidates, dying, LeakForm::Lost, at, state);
  }
  for (const core::PlaceId holder : stale)
    state.resources.clear(holder);
}

void FunctionDataflow::checkOverwrite(core::PlaceId dest, const Expr &at,
                                      core::AnalysisState &state) {
  if (!state.resources.holds(dest))
    return;
  checkLeaks(
      {dest}, [dest](core::PlaceId place) { return place == dest; },
      LeakForm::Overwritten, locate(at), state);
}

void FunctionDataflow::releaseStorageBelow(core::PlaceId pointer,
                                           core::AnalysisState &state) {
  // A defined destructor said what it frees below the object; what it did
  // not mention is its business too (RFC 0007, *Owned fields*: the check
  // runs for library releases only). Nothing below is ours to report.
  for (const core::PlaceId place : storageOf(places.deref(pointer))) {
    for (const core::PlaceId mirror : mirrors(place, state)) {
      if (!state.resources.holds(mirror))
        continue;
      state.resources.escape(mirror);
      for (const core::PlaceId alias : state.aliases.members(mirror))
        state.resources.escape(alias);
    }
  }
}

void FunctionDataflow::checkContainerFree(core::PlaceId pointer, const Expr &at,
                                          core::AnalysisState &state) {
  const core::PlaceId object = places.deref(pointer);
  const auto ownerRecord = state.resources.recordOf(pointer);
  const bool freshHere =
      ownerRecord && ownerRecord->origin == core::ResourceOrigin::Allocated;
  // `free(b)` in a function that never names `b->p` has no place for the
  // field yet; the declared-owned ones must exist to be found below.
  if (!freshHere) {
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(pointer))) {
      if (const RecordDecl *record =
              decl->getType()->getPointeeType()->getAsRecordDecl()) {
        for (const FieldDecl *field : record->fields()) {
          if (field->getType()->isPointerType() && getAnnotations(*field).owned)
            (void)builder.fieldPlace(object, *field);
        }
      }
    }
  }
  std::vector<core::PlaceId> storage = storageOf(object);
  // Every mirror of the storage goes too: `q->p` for `q ~ b`.
  std::set<core::PlaceId> going;
  for (const core::PlaceId place : storage) {
    going.insert(place);
    for (const core::PlaceId mirror : mirrors(place, state))
      going.insert(mirror);
  }
  const auto dying = [&going](core::PlaceId place) {
    return going.contains(place);
  };

  // A declared-owned field owns its referent when the object came from
  // outside: an object this function allocated, or was handed fresh, holds
  // only what this function stored (RFC 0007, *Owned fields*).
  std::vector<core::PlaceId> synthesised;
  if (!freshHere) {
    for (const core::PlaceId place : storage) {
      if (place == object || state.resources.holds(place) ||
          state.resources.isNull(place) || state.moves.recordOf(place))
        continue;
      const auto declared = declaredAnnotations(place);
      if (!declared || !declared->owned)
        continue;
      const NamedDecl *decl = builder.declFor(place);
      state.resources.hold(
          place, core::ResourceRecord{
                     .origin = core::ResourceOrigin::Declared,
                     .location = decl != nullptr ? locate(decl->getLocation())
                                                 : core::SourceLocation{},
                     .family = {},
                     .escaped = false});
      synthesised.push_back(place);
    }
  }
  checkLeaks(storage, dying, LeakForm::Container, locate(at), state, pointer);
  // Whatever was not lost is still someone else's; the synthesised records
  // must not outlive this check.
  for (const core::PlaceId place : synthesised)
    state.resources.clear(place);
}

void FunctionDataflow::checkReleaseFamily(core::PlaceId place,
                                          std::string_view family,
                                          const Expr &at,
                                          const core::AnalysisState &state) {
  if (family.empty())
    return;
  std::vector<core::PlaceId> candidates{place};
  llvm::append_range(candidates, state.aliases.members(place));
  for (const core::PlaceId candidate : candidates) {
    const auto record = state.resources.recordOf(candidate);
    if (!record || record->family.empty() || record->family == family)
      continue;
    core::Diagnostic diagnostic = makeError(
        core::diag::MismatchedRelease,
        "'" + nameOf(place) + "' is released with '" + std::string(family) +
            "' but must be released with '" + record->family + "'",
        at);
    if (record->location.isValid())
      diagnostic.addNote("allocated here", record->location);
    report(std::move(diagnostic));
    return;
  }
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
  collectDiscardedCalls(body);
  computeLiveness();

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

    const auto propagate = [&](unsigned succIndex, const CFGBlock &succ,
                               core::AnalysisState edgeState) {
      leaveBlock(*block, succIndex, edgeState);
      std::optional<core::AnalysisState> &target =
          entryStates[succ.getBlockID()];
      bool changed = false;
      if (!target) {
        target = std::move(edgeState);
        changed = true;
      } else {
        changed = target->join(edgeState);
      }
      if (changed && !queued[succ.getBlockID()]) {
        queued[succ.getBlockID()] = true;
        worklist.push_back(&succ);
      }
    };

    // The last reachable successor takes the block's state itself; every
    // earlier one gets a copy. States in a large function are big (the
    // alias relation over a loop body is dense), so copies are the cost.
    llvm::SmallVector<std::pair<unsigned, const CFGBlock *>, 4> reachable;
    unsigned index = 0;
    for (const CFGBlock::AdjacentBlock &adjacent : block->succs()) {
      // A block that ends in a `noreturn` call never hands control back to
      // the caller: its state is no part of what a call to this function
      // does (RFC 0003, *What a summary describes*).
      if (const CFGBlock *succ = adjacent.getReachableBlock();
          succ != nullptr &&
          (succ != &cfg->getExit() || !block->hasNoReturnElement()))
        reachable.emplace_back(index, succ);
      ++index;
    }
    if (reachable.empty())
      continue;
    for (const auto &[succIndex, succ] : llvm::drop_end(reachable))
      propagate(succIndex, *succ, out);
    propagate(reachable.back().first, *reachable.back().second, std::move(out));
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
    // What dies at the block's end is checked per edge; the edge that
    // exits the function sees the report of everything left (RFC 0007).
    if (state.resources.empty())
      continue;
    unsigned index = 0;
    for (const CFGBlock::AdjacentBlock &adjacent : block->succs()) {
      if (adjacent.getReachableBlock() != nullptr) {
        core::AnalysisState edgeState = state;
        leaveBlock(*block, index, edgeState);
      }
      ++index;
    }
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
  lastCall.reset();
  for (std::size_t index = 0; index < block.size(); ++index) {
    const CFGElement &element = block[index];
    checkDeadResources(block, index, state);
    expireDeadLoans(block, index, state);
    if (const auto stmtElement = element.getAs<CFGStmt>()) {
      const Stmt *stmt = stmtElement->getStmt();
      if (stmt == nullptr)
        continue;
      inUnsafe = unsafeBody || unsafeStmts.contains(stmt);
      if (const auto *expr = dyn_cast<Expr>(stmt))
        handleExpr(*expr, state);
      else if (const auto *decl = dyn_cast<DeclStmt>(stmt))
        handleDecl(*decl, state);
      else if (const auto *ret = dyn_cast<ReturnStmt>(stmt))
        handleReturn(*ret, state);
      inUnsafe = unsafeBody;
      continue;
    }
    if (const auto lifetimeEnd = element.getAs<CFGLifetimeEnds>()) {
      // Clang <= 22 also ends parameter lifetimes at every `return` (Clang 23
      // gates this behind `AddParameterLifetimes`). Parameters live for the
      // whole function in the model, and forgetting them here would drop
      // their facts from the exit state and the summary.
      if (const VarDecl *var = lifetimeEnd->getVarDecl();
          var != nullptr && !isa<ParmVarDecl>(var))
        handleLifetimeEnd(*var, locateElement(block, index), state);
    }
  }
}

void FunctionDataflow::leaveBlock(const CFGBlock &from, unsigned succIndex,
                                  core::AnalysisState &state) {
  // The condition first: on the null edge of `if (q)` the result owns
  // nothing, and on the other a retracted consumption may hand it back to
  // the argument (RFC 0007, *Acquiring and losing a resource*).
  applyEdge(from, succIndex, state);
  const CFGBlock *successor = nullptr;
  if (succIndex < from.succ_size())
    successor = (*std::next(from.succ_begin(), succIndex)).getReachableBlock();
  checkBlockEndResources(from, successor, state);
}

// -- Condition facts (RFC 0006) -----------------------------------------------

/// The outcome classes of an integer result that satisfy (`holds`) or
/// falsify (`!holds`) `x OP k` (RFC 0006, *Outcome tests*): a class is
/// selected when some value in its range does.
static std::set<core::Outcome> classesSatisfying(BinaryOperatorKind op,
                                                 std::int64_t k, bool holds) {
  constexpr std::int64_t Min = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t Max = std::numeric_limits<std::int64_t>::max();
  struct Range {
    core::Outcome outcome;
    std::int64_t lo;
    std::int64_t hi;
  };
  constexpr std::array<Range, 3> Ranges = {
      Range{.outcome = core::Outcome::Negative, .lo = Min, .hi = -1},
      Range{.outcome = core::Outcome::Zero, .lo = 0, .hi = 0},
      Range{.outcome = core::Outcome::Positive, .lo = 1, .hi = Max},
  };
  // `!(x OP k)` is `x OP' k` for the complementary comparison.
  if (!holds) {
    switch (op) {
    case BO_LT:
      op = BO_GE;
      break;
    case BO_GT:
      op = BO_LE;
      break;
    case BO_LE:
      op = BO_GT;
      break;
    case BO_GE:
      op = BO_LT;
      break;
    case BO_EQ:
      op = BO_NE;
      break;
    case BO_NE:
      op = BO_EQ;
      break;
    default:
      return {};
    }
  }
  std::set<core::Outcome> result;
  for (const Range &range : Ranges) {
    bool possible = false;
    switch (op) {
    case BO_LT:
      possible = range.lo < k;
      break;
    case BO_GT:
      possible = range.hi > k;
      break;
    case BO_LE:
      possible = range.lo <= k;
      break;
    case BO_GE:
      possible = range.hi >= k;
      break;
    case BO_EQ:
      possible = range.lo <= k && k <= range.hi;
      break;
    case BO_NE:
      possible = range.lo != k || range.hi != k;
      break;
    default:
      break;
    }
    if (possible)
      result.insert(range.outcome);
  }
  return result;
}

/// `k OP x` as `x OP' k`.
static BinaryOperatorKind flipComparison(BinaryOperatorKind op) {
  switch (op) {
  case BO_LT:
    return BO_GT;
  case BO_GT:
    return BO_LT;
  case BO_LE:
    return BO_GE;
  case BO_GE:
    return BO_LE;
  default:
    return op;
  }
}

static bool isNullConstant(const Expr &expr, ASTContext &context) {
  return expr.isNullPointerConstant(context, Expr::NPC_ValueDependentIsNull) !=
         Expr::NPCK_NotNull;
}

static std::optional<std::int64_t> integerConstant(const Expr &expr,
                                                   const ASTContext &context) {
  Expr::EvalResult result;
  if (expr.isValueDependent() || !expr.getType()->isIntegerType() ||
      !expr.EvaluateAsInt(result, context) || !result.Val.isInt() ||
      result.Val.getInt().getSignificantBits() > 64)
    return std::nullopt;
  return result.Val.getInt().getSExtValue();
}

void FunctionDataflow::applyEdge(const CFGBlock &from, unsigned succIndex,
                                 core::AnalysisState &state) {
  if (from.succ_size() != 2)
    return;
  const auto *condition = dyn_cast_or_null<Expr>(from.getTerminatorCondition());
  if (condition == nullptr)
    return;

  // Successor 0 is the edge taken when the condition holds.
  applyCondition(*condition, succIndex == 0, /*wrapped=*/false, state);
}

void FunctionDataflow::applyCondition(const Expr &condition, bool holds,
                                      bool wrapped,
                                      core::AnalysisState &state) {
  const Expr *e = condition.IgnoreParenImpCasts();
  for (;;) {
    // `!c` flips the edge.
    if (const auto *unary = dyn_cast<UnaryOperator>(e);
        unary != nullptr && unary->getOpcode() == UO_LNot) {
      holds = !holds;
      wrapped = true;
      e = unary->getSubExpr()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *logical = dyn_cast<BinaryOperator>(e);
        logical != nullptr && logical->isLogicalOp()) {
      // `if (fd == -1 || (p = malloc(n)) == NULL)`: the block that evaluated
      // the right operand branches on it, but Clang hands back the whole
      // condition (or, for an inner `||`, its left side). The left operands
      // were decided on earlier edges.
      if (!wrapped) {
        e = logical->getRHS()->IgnoreParenImpCasts();
        continue;
      }
      // Under `!`, `__builtin_expect` or `!= 0` the operator was computed as
      // a value and the branch is on that value (Lua's `l_unlikely(newblock
      // == NULL && nsize > 0)`): the operands are only known when the value
      // decides them, a true `&&` or a false `||`.
      if ((logical->getOpcode() == BO_LAnd) == holds) {
        applyCondition(*logical->getLHS(), holds, true, state);
        applyCondition(*logical->getRHS(), holds, true, state);
      }
      return;
    }
    // `__builtin_expect(c, k)` is `c` (Lua's `l_unlikely`, glibc's
    // `__glibc_unlikely`).
    if (const auto *call = dyn_cast<CallExpr>(e);
        call != nullptr && call->getNumArgs() == 2 &&
        call->getDirectCallee() != nullptr &&
        call->getDirectCallee()->getName() == "__builtin_expect") {
      wrapped = true;
      e = call->getArg(0)->IgnoreParenImpCasts();
      continue;
    }
    // `(c) != 0` and `(c) == 0` on a truth value are `c` and `!c`.
    if (const auto *equality = dyn_cast<BinaryOperator>(e);
        equality != nullptr && equality->isEqualityOp()) {
      const Expr *lhs = equality->getLHS()->IgnoreParenImpCasts();
      const Expr *rhs = equality->getRHS()->IgnoreParenImpCasts();
      const auto isTruthValue = [](const Expr &operand) {
        if (const auto *binary = dyn_cast<BinaryOperator>(&operand))
          return binary->isComparisonOp() || binary->isLogicalOp();
        const auto *unary = dyn_cast<UnaryOperator>(&operand);
        return unary != nullptr && unary->getOpcode() == UO_LNot;
      };
      const Expr *truth = nullptr;
      if (isTruthValue(*lhs) && integerConstant(*rhs, context) == 0)
        truth = lhs;
      else if (isTruthValue(*rhs) && integerConstant(*lhs, context) == 0)
        truth = rhs;
      if (truth != nullptr) {
        if (equality->getOpcode() == BO_EQ)
          holds = !holds;
        wrapped = true;
        e = truth;
        continue;
      }
    }
    break;
  }

  if (const auto *binary = dyn_cast<BinaryOperator>(e);
      binary != nullptr && binary->isComparisonOp()) {
    const Expr &lhs = *binary->getLHS();
    const Expr &rhs = *binary->getRHS();
    const BinaryOperatorKind op = binary->getOpcode();
    const bool equality = op == BO_EQ || op == BO_NE;

    // `x == NULL`, `NULL != x`: a null test of `x`.
    if (equality && lhs.getType()->isPointerType() &&
        rhs.getType()->isPointerType()) {
      const bool nullLhs = isNullConstant(lhs, context);
      const bool nullRhs = isNullConstant(rhs, context);
      if (nullLhs != nullRhs) {
        const bool selectsNull = (op == BO_EQ) == holds;
        applyOutcomeTest(
            nullLhs ? rhs : lhs,
            {selectsNull ? core::Outcome::Null : core::Outcome::NonNull},
            state);
        return;
      }
      if (nullLhs)
        return;
      // `p == q`: on the equal edge the two places hold the same value; on
      // the other they do not, which refutes an exact alias. `(p = f()) ==
      // q` compares what was just stored in `p`.
      const auto assigned = [](const Expr &operand) -> const Expr * {
        const Expr *stripped = operand.IgnoreParenCasts();
        if (const auto *assign = dyn_cast<BinaryOperator>(stripped);
            assign != nullptr && assign->getOpcode() == BO_Assign)
          return assign->getLHS();
        return &operand;
      };
      const auto p = builder.resolvePointerValue(*assigned(lhs));
      const auto q = builder.resolvePointerValue(*assigned(rhs));
      if (!p || !q || p->place == q->place)
        return;
      if ((op == BO_EQ) == holds)
        state.aliases.unite(p->place, q->place);
      else
        state.aliases.separateExact(p->place, q->place);
      return;
    }

    // `x OP k`, `k OP x` on an integer result.
    if (lhs.getType()->isIntegerType() && rhs.getType()->isIntegerType()) {
      if (const auto k = integerConstant(rhs, context)) {
        applyOutcomeTest(lhs, classesSatisfying(op, *k, holds), state);
      } else if (const auto flipped = integerConstant(lhs, context)) {
        applyOutcomeTest(
            rhs, classesSatisfying(flipComparison(op), *flipped, holds), state);
      }
    }
    return;
  }

  // `x` alone: non-null / non-zero on the true edge.
  if (e->getType()->isPointerType()) {
    applyOutcomeTest(*e, {holds ? core::Outcome::NonNull : core::Outcome::Null},
                     state);
  } else if (e->getType()->isIntegerType()) {
    if (holds)
      applyOutcomeTest(*e, {core::Outcome::Positive, core::Outcome::Negative},
                       state);
    else
      applyOutcomeTest(*e, {core::Outcome::Zero}, state);
  }
}

void FunctionDataflow::markNullWithCopies(core::PlaceId place,
                                          core::AnalysisState &state) {
  state.resources.markNull(place);
  for (const auto &[alias, edge] : state.aliases.edgesFrom(place)) {
    if (edge.exact)
      state.resources.markNull(alias);
  }
}

void FunctionDataflow::forgetBelowNull(core::PlaceId place,
                                       core::AnalysisState &state) {
  // Nothing lies below a null pointer: what this path recorded about `*p`
  // and deeper (`if (L) exit(1); return 0;` after a callee freed `L->g`)
  // describes memory the path cannot reach, and a caller who passed a
  // non-null pointer never takes this edge (RFC 0006, *Null edges*).
  const auto drop = [this, &state](core::PlaceId pointer) {
    for (const core::PlaceId child : places.descendants(pointer)) {
      if (const auto path = builder.summaryPathOf(child))
        state.consumed.erase(*path);
    }
    forgetBelow(pointer, state);
  };
  drop(place);
  for (const auto &[alias, edge] : state.aliases.edgesFrom(place)) {
    if (edge.exact)
      drop(alias);
  }
}

void FunctionDataflow::markNullOutcomes(const core::PendingOutcome &narrowed,
                                        core::AnalysisState &state) {
  for (const core::PlaceId place : narrowed.nullInAll())
    markNullWithCopies(place, state);
}

void FunctionDataflow::applyOutcomeTest(const Expr &operand,
                                        const std::set<core::Outcome> &selected,
                                        core::AnalysisState &state) {
  if (selected.empty())
    return;
  const Expr *e = operand.IgnoreParenCasts();
  // `(r = f(p)) < 0` tests what was just stored in `r`.
  if (const auto *assign = dyn_cast<BinaryOperator>(e);
      assign != nullptr && assign->getOpcode() == BO_Assign)
    e = assign->getLHS()->IgnoreParenCasts();

  // On this edge the consumption did not happen: the move record goes, and
  // so does the flow-sensitive consumption that feeds the outcome classes
  // at `return` (RFC 0006, *Inference*), so a wrapper's own summary sees
  // the retraction. A place that was reassigned since the call keeps its
  // `consumed` entry: that consumption was real (RFC 0003).
  const auto reinstate = [this,
                          &state](const std::vector<core::PlaceId> &targets) {
    for (const core::PlaceId place : targets) {
      if (!state.moves.recordOf(place))
        continue;
      state.moves.reinitialize(place);
      if (const auto path = builder.summaryPathOf(place))
        state.consumed.erase(*path);
    }
  };

  if (const auto *call = dyn_cast<CallExpr>(e)) {
    // A result tested directly: the call sits in this block, and its
    // pending outcome was recorded when it was transferred.
    if (!lastCall || lastCall->call != call)
      return;
    core::PendingOutcome narrowed = lastCall->pending;
    reinstate(narrowed.select(selected));
    markNullOutcomes(narrowed, state);
    return;
  }
  if (!PlaceBuilder::isPlaceExpr(*e))
    return;
  const auto ref = builder.resolve(*e);
  if (!ref)
    return;
  // On the edge where the holder is null it owns nothing (RFC 0007, *Null*):
  // `p = malloc(n); if (!p) return -1;` is not a leak. Exact copies hold the
  // same null.
  if (selected == std::set<core::Outcome>{core::Outcome::Null}) {
    markNullWithCopies(ref->place, state);
    forgetBelowNull(ref->place, state);
  }
  const auto entry = state.pending.find(ref->place);
  if (entry == state.pending.end())
    return;
  // The narrowed entry stays even once nothing more can be retracted: it
  // records which classes the result can still be in, which a later
  // `return` of it needs. It goes when the result is reassigned.
  const std::vector<core::PlaceId> reinstated = entry->second.select(selected);
  reinstate(reinstated);
  markNullOutcomes(entry->second, state);
  // `q = try_ptr(p); if (q) ...`: on the non-null edge where `p` was not
  // consumed the callee returned `p` itself, so `q` is `p`, not a resource
  // of its own (RFC 0007).
  if (selected == std::set<core::Outcome>{core::Outcome::NonNull}) {
    const core::PlaceId holder = ref->place;
    for (const core::PlaceId place : reinstated) {
      if (place == holder || !llvm::is_contained(entry->second.returned, place))
        continue;
      state.aliases.unite(holder, place);
      state.resources.clear(holder);
    }
  }
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
    if (!ref) {
      // `((T *)(uintptr_t)x)->f`: a dereference of a raw value that lives
      // in no place (RFC 0004, *Raw pointers*, rule 1).
      if (const auto raw = builder.rawBaseOf(expr)) {
        if (const auto record = rawRecordOf(*raw, expr, state))
          reportRawOperation(
              "dereference of raw pointer outside an unsafe region", "",
              *record, expr);
      }
      return;
    }
    switch (role) {
    case Role::Read:
      doRead(*ref, expr, state, /*includeSelf=*/true);
      if (escapingExprs.contains(&expr))
        escape(ref->place, state);
      break;
    case Role::ReadWrite:
      // `p++` keeps `p` on the same object (RFC 0004, *Pointer identity*),
      // so nothing about it changes.
      doRead(*ref, expr, state, /*includeSelf=*/true);
      doMutationCheck(ref->place, expr, state);
      checkAnnotationOnWrite(*ref, expr, state);
      recordAccess(ref->place, /*write=*/true, state);
      noteVariableWrite(ref->place, state);
      break;
    case Role::Write:
    case Role::AddressOf:
      doRead(*ref, expr, state, /*includeSelf=*/false);
      noteVariableWrite(ref->place, state);
      break;
    case Role::Consume:
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
    noteVariableWrite(place, state);
    const Expr *init = var->getInit();
    if (!var->getType()->isPointerType()) {
      if (init != nullptr && var->getType()->isRecordType())
        copyRecord(place, *init, state);
      else
        attachOutcome(place, init, state);
      continue;
    }
    const AnnotationSet annotations = getAnnotations(*var);
    if (annotations.ownership())
      declaredKinds[place] = annotations;
    if (init == nullptr) {
      setKind(place,
              annotations.raw ? core::OwnershipKind::Raw
                              : core::OwnershipKind::Unknown,
              state);
      continue;
    }
    applyPointerAssign(place, builder.classifyValue(*init), *init,
                       var->getType()->getPointeeType().isConstQualified(),
                       state);
    attachOutcome(place, init, state);
  }
}

void FunctionDataflow::attachOutcome(core::PlaceId dest, const Expr *init,
                                     core::AnalysisState &state) {
  if (init == nullptr || !lastCall)
    return;
  const auto *call = dyn_cast<CallExpr>(init->IgnoreParenCasts());
  if (call == nullptr || call != lastCall->call)
    return;
  // `p = realloc(p, n)`: `p` now holds the result, so nothing about the old
  // block can be retracted through it.
  core::PendingOutcome outcome = std::move(lastCall->pending);
  lastCall.reset();
  for (auto &[cls, consumed] : outcome.consumedBy)
    std::erase(consumed, dest);
  // Worth keeping while a class still has something to retract, or a null
  // fact to apply (`err = init(&s); if (err != 0) return err;`).
  if (outcome.places().empty() && outcome.nullOn.empty())
    return;
  state.pending[dest] = std::move(outcome);
}

void FunctionDataflow::noteVariableWrite(core::PlaceId place,
                                         core::AnalysisState &state) {
  if (places.isBase(place))
    state.moves.forgetWitness(place);
}

void FunctionDataflow::handleAssign(const BinaryOperator &assign,
                                    core::AnalysisState &state) {
  const auto lhs = builder.resolve(*assign.getLHS());
  if (!lhs) {
    // `*slot() = p`: the value went somewhere the model cannot name (RFC
    // 0007, *Escape*).
    if (assign.getLHS()->getType()->isPointerType())
      escapeValue(builder.classifyValue(*assign.getRHS()), /*deep=*/false,
                  state);
    return;
  }
  doMutationCheck(lhs->place, assign, state);
  checkAnnotationOnWrite(*lhs, assign, state);
  recordAccess(lhs->place, /*write=*/true, state);

  const QualType type = assign.getLHS()->getType();
  if (type->isPointerType()) {
    applyPointerAssign(lhs->place, builder.classifyValue(*assign.getRHS()),
                       assign, type->getPointeeType().isConstQualified(), state,
                       lhs->element);
    attachOutcome(lhs->place, assign.getRHS(), state);
    return;
  }
  if (type->isRecordType()) {
    copyRecord(lhs->place, *assign.getRHS(), state);
    return;
  }
  // A scalar result (`int rc = try_take(p)`) carries its call's pending
  // outcome to the place that will be tested.
  state.pending.erase(lhs->place);
  attachOutcome(lhs->place, assign.getRHS(), state);
}

void FunctionDataflow::copyRecord(core::PlaceId dest, const Expr &value,
                                  core::AnalysisState &state) {
  const Expr *source = &PlaceBuilder::stripTransparent(value);
  if (const auto *literal = dyn_cast<CompoundLiteralExpr>(source))
    source = &PlaceBuilder::stripTransparent(*literal->getInitializer());
  {
    // Whatever the record's fields held is overwritten (RFC 0007).
    const std::vector<core::PlaceId> storage = storageOf(dest);
    const std::set<core::PlaceId> going(storage.begin(), storage.end());
    checkLeaks(
        storage,
        [&going](core::PlaceId place) { return going.contains(place); },
        LeakForm::Overwritten, locate(value), state);
  }
  if (const auto *init = dyn_cast<InitListExpr>(source)) {
    reinit(dest, state);
    initRecord(dest, *init, state);
    return;
  }
  const std::optional<PlaceRef> src = PlaceBuilder::isPlaceExpr(*source)
                                          ? builder.resolve(*source)
                                          : std::nullopt;
  if (!src) {
    // A struct produced by a call or some other opaque expression: every
    // field is overwritten with values nothing is known about.
    reinit(dest, state);
    return;
  }
  if (src->place == dest)
    return;

  // `b = a` is `b.f = a.f` for every field path `f` known under `a`: the
  // fields themselves become aliases carrying the same loans, moves and raw
  // records; the objects below a copied pointer are the same objects, so
  // their facts are mirrored as `mirrorSubtree` does for a pointer copy.
  // Facts are captured first because `a` may lie below `b` (`*n = *n->next`).
  struct FieldFacts {
    core::PlaceId from;
    core::PlaceId to;
    bool belowPointer;
    std::optional<core::MoveRecord> moved;
    std::optional<core::RawRecord> raw;
    std::optional<core::OwnershipKind> kind;
    std::optional<core::ResourceRecord> resource;
    std::vector<core::Loan> loans;
  };
  std::vector<FieldFacts> facts;
  const std::size_t srcDepth = places.depth(src->place);
  const std::size_t destDepth = places.depth(dest);
  for (const core::PlaceId place : places.descendants(src->place)) {
    if (places.depth(place) - srcDepth + destDepth > MaxPlaceDepth)
      continue;
    bool belowPointer = false;
    for (core::PlaceId cursor = place; cursor != src->place;
         cursor = *places.parent(cursor)) {
      if (places.step(cursor) == core::PathStep::Deref) {
        belowPointer = true;
        break;
      }
    }
    FieldFacts field{
        .from = place,
        .to = places.translate(place, src->place, dest),
        .belowPointer = belowPointer,
        .moved = std::nullopt,
        .raw = std::nullopt,
        .kind = std::nullopt,
        .resource = std::nullopt,
        .loans = {},
    };
    if (const auto record = state.moves.recordOf(place))
      field.moved = *record;
    if (const auto record = state.resources.recordOf(place))
      field.resource = *record;
    if (const auto record = state.raw.rawAt(place))
      field.raw = *record;
    if (const auto it = state.kinds.find(place); it != state.kinds.end())
      field.kind = it->second;
    for (const core::Loan &loan : state.loans.loans()) {
      if (loan.holder == place || (belowPointer && loan.place == place))
        field.loans.push_back(loan);
    }
    facts.push_back(std::move(field));
  }

  reinit(dest, state);
  for (const FieldFacts &field : facts) {
    if (field.kind)
      setKind(field.to, *field.kind, state);
    if (field.moved) {
      state.moves.markMoved(field.to, field.moved->reason,
                            field.moved->location,
                            field.moved->via.value_or(field.from),
                            field.moved->element, field.moved->family);
    }
    if (field.raw)
      state.raw.markRaw(field.to, *field.raw);
    if (field.resource)
      state.resources.hold(field.to, *field.resource);
    if (!field.belowPointer) {
      state.aliases.unite(field.to, field.from);
      state.loans.copyHolder(field.from, field.to);
      continue;
    }
    for (core::Loan loan : field.loans) {
      if (loan.place == field.from)
        loan.place = field.to;
      if (loan.holder == field.from)
        loan.holder = field.to;
      state.loans.addLoanUnchecked(loan);
    }
  }
}

void FunctionDataflow::initRecord(core::PlaceId dest, const InitListExpr &init,
                                  core::AnalysisState &state) {
  const InitListExpr *semantic =
      init.isSemanticForm() ? &init : init.getSemanticForm();
  const RecordDecl *record = init.getType()->getAsRecordDecl();
  if (semantic == nullptr || record == nullptr)
    return;

  const auto assignField = [&](const FieldDecl &field, const Expr &value) {
    if (isa<ImplicitValueInitExpr>(&value))
      return;
    const core::PlaceId place = builder.fieldPlace(dest, field);
    const QualType type = field.getType();
    if (type->isPointerType()) {
      applyPointerAssign(place, builder.classifyValue(value), value,
                         type->getPointeeType().isConstQualified(), state);
    } else if (type->isRecordType()) {
      copyRecord(place, value, state);
    }
  };

  if (record->isUnion()) {
    const FieldDecl *field = semantic->getInitializedFieldInUnion();
    if (field != nullptr && semantic->getNumInits() > 0 &&
        semantic->getInit(0) != nullptr)
      assignField(*field, *semantic->getInit(0));
    return;
  }
  // Mirrors the semantic form's layout: one initializer per field in
  // declaration order, unnamed bit-fields skipped.
  unsigned next = 0;
  for (const FieldDecl *field : record->fields()) {
    if (field->isUnnamedBitField())
      continue;
    if (next >= semantic->getNumInits())
      break;
    const Expr *value = semantic->getInit(next++);
    if (value != nullptr)
      assignField(*field, *value);
  }
}

void FunctionDataflow::handleCall(const CallExpr &call,
                                  core::AnalysisState &state) {
  lastCall.reset();
  const auto effects = classifyCall(call, summaries);
  if (!effects) {
    handleUncheckedCall(call, state);
    return;
  }
  applySummary(call, *effects, state);
  // `strdup(s);`: nobody holds the result (RFC 0007, *Discarded results*).
  if (discardedCalls.contains(&call) && effects->summary->returnsOnlyFresh() &&
      recording()) {
    const ValueOrigin origin = builder.classifyValue(call);
    reportLeak(core::PlaceId{},
               core::ResourceRecord{.origin = core::ResourceOrigin::Allocated,
                                    .location = {},
                                    .family = origin.family,
                                    .escaped = false},
               "result of " + calleeName(call) + " is leaked", locate(call));
  }
}

// -- Calls (RFC 0003) ---------------------------------------------------------

void FunctionDataflow::applySummary(const CallExpr &call,
                                    const CallEffects &effects,
                                    core::AnalysisState &state) {
  const core::FunctionSummary &summary = *effects.summary;
  const bool library = effects.source == SummarySource::Builtin;

  // 0. Escapes (RFC 0007, *Escape*): an argument the summary says nothing
  //    about may be retained when the summary is an annotation or the
  //    library table (a body's silence is trusted); so may anything in a
  //    variadic position of a callee that is not in the table; and a value
  //    the callee stores a copy of has a second home now.
  const bool trustSilence = effects.source == SummarySource::Inferred ||
                            effects.source == SummarySource::Program;
  for (unsigned i = 0; i < call.getNumArgs(); ++i) {
    const Expr &arg = *call.getArg(i);
    if (!arg.getType()->isPointerType())
      continue;
    if (i >= effects.declaredParams) {
      if (!library)
        escapeValue(builder.classifyValue(arg), /*deep=*/true, state);
      continue;
    }
    if (trustSilence || effects.consumes(i) ||
        llvm::any_of(effects.borrowedArgs,
                     [i](const auto &borrowed) { return borrowed.first == i; }))
      continue;
    escapeValue(builder.classifyValue(arg), /*deep=*/true, state);
  }
  for (const core::Store &store : summary.stores) {
    if (store.value.kind != core::ValueSource::Kind::Copy || !store.value.path)
      continue;
    if (const auto ref = builder.resolveSummaryPath(*store.value.path, call))
      escape(ref->place, state);
  }

  // 1. Consumption: the arguments themselves, the caller's memory below them
  //    (`free(b->data)` in the callee) and globals, deepest path first so a
  //    caller's copy of a freed field is marked before the object holding
  //    the field is (RFC 0007, *Applying a summary: deepest paths first*).
  struct Consumed {
    core::SummaryPath path;
    PlaceRef ref;
    bool freed;
    std::string family;
  };
  std::vector<Consumed> consumed;
  for (const auto &[path, effect] : summary.effects) {
    if (!effect.consumed())
      continue;
    std::optional<PlaceRef> ref;
    if (path.isParam() && path.isRoot()) {
      if (path.index >= call.getNumArgs())
        continue;
      checkRawArgument(call, path.index,
                       effect.freed ? "releases" : "takes ownership of", state);
      ref = builder.resolvePointerValue(*call.getArg(path.index));
    } else {
      ref = builder.resolveSummaryPath(path, call);
    }
    if (ref) {
      consumed.push_back(Consumed{.path = path,
                                  .ref = std::move(*ref),
                                  .freed = effect.freed,
                                  .family = effect.family});
    }
  }
  std::ranges::stable_sort(consumed, [](const Consumed &a, const Consumed &b) {
    return a.path.steps.size() > b.path.steps.size();
  });

  std::vector<std::pair<core::SummaryPath, std::vector<core::PlaceId>>>
      consumedTargets;
  std::vector<core::PlaceId> markedHere;
  for (const Consumed &entry : consumed) {
    //    Memory below another object this call frees goes with it. It is
    //    consumed on its own only when this function knows the place; when
    //    the container is already gone the one report is the container's.
    const auto container =
        llvm::find_if(consumed, [&entry](const Consumed &other) {
          return other.path.isProperPrefixOf(entry.path);
        });
    if (container != consumed.end() &&
        (findMoved(container->ref.place, state, container->ref.element) ||
         !knowsPlace(entry.ref.place, entry.ref.element, state))) {
      consumedTargets.emplace_back(entry.path, std::vector<core::PlaceId>{});
      continue;
    }
    //    Two paths of one summary can name one cell (`g->allgc` and
    //    `g->twups->l_G->allgc` while `g->twups ~ L`): the second is the same
    //    release, not a second one.
    if (llvm::is_contained(markedHere, entry.ref.place)) {
      consumedTargets.emplace_back(entry.path, std::vector<core::PlaceId>{});
      continue;
    }
    std::vector<core::PlaceId> marked = doConsume(
        entry.ref,
        entry.freed ? core::MoveReason::Freed : core::MoveReason::Moved, call,
        state, entry.family, library);
    llvm::append_range(markedHere, marked);
    consumedTargets.emplace_back(entry.path, std::move(marked));
  }
  notePendingOutcome(call, summary, consumedTargets);

  //    A callee that overwrote an object (`memcpy(root, &tmp, n)`) leaves
  //    nothing known about what lies below it (RFC 0006, *`written` forgets
  //    what lies below*); its stores, applied below, say what is there now.
  //    Which of the callee's written paths name a place here depends only
  //    on the places this function has interned so far, so a block visited
  //    again with the same table reuses the answer (the callee's summary is
  //    fixed for the run; Lua's are hundreds of paths long).
  WrittenPlaces &written = writtenAt[&call];
  if (written.placesSeen != places.size()) {
    written.placesSeen = places.size();
    written.places.clear();
    llvm::SmallVector<bool, 8> consumedParam(call.getNumArgs(), false);
    for (unsigned i = 0; i < call.getNumArgs(); ++i)
      consumedParam[i] = summary.consumes(i);
    PlaceBuilder::PathLookupCache lookups;
    for (const auto &[path, effect] : summary.effects) {
      if (!effect.written || effect.consumed())
        continue;
      if (path.isParam() && path.index < consumedParam.size() &&
          consumedParam[path.index])
        continue;
      // Nothing is known below a place this function never named.
      if (const auto place = builder.lookupSummaryPath(path, call, lookups))
        written.places.push_back(*place);
    }
  }
  for (const core::PlaceId place : written.places)
    forgetBelow(place, state);

  // 2. Borrows for the duration of the call. The callee dereferences the
  //    argument, which is a raw operation if the argument is raw (RFC 0004).
  for (const auto &[index, kind] : effects.borrowedArgs) {
    if (index >= call.getNumArgs())
      continue;
    checkRawArgument(call, index, "dereferences", state);
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
      continue;
    }
    checkAnnotationOnWrite(*pointee, call, state);
    // What the callee wrote is what this function wrote. `written` on the
    // pointee itself means the object was overwritten (RFC 0006, *`written`
    // forgets what lies below*), which a callee that set `strm->total_in`
    // did not do; only a summary that says nothing more (an annotation)
    // is that coarse.
    replayWrites(call, *pointee, index, summary, state);
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
    // What the caller held there is the callee's business now: RFC 0003
    // reads a sub-path's consumption from the callee's exit state, so
    // `free(b->data); b->data = NULL;` shows only the store (RFC 0007,
    // *Deliberately not caught*).
    if (state.resources.holds(ref->place)) {
      escape(ref->place, state);
      for (const core::PlaceId alias : state.aliases.members(ref->place))
        state.resources.escape(alias);
    }
    applyPointerAssign(ref->place, origin, call, /*constPointee=*/false, state,
                       ref->element);
  }
}

void FunctionDataflow::notePendingOutcome(
    const CallExpr &call, const core::FunctionSummary &summary,
    const std::vector<std::pair<core::SummaryPath, std::vector<core::PlaceId>>>
        &consumedTargets) {
  if (summary.outcomes.empty())
    return;
  core::PendingOutcome conditional;
  for (const auto &[path, targets] : consumedTargets) {
    if (targets.empty() || summary.consumesUnconditionally(path))
      continue;
    for (const auto &[outcome, effects] : summary.outcomes) {
      std::vector<core::PlaceId> &consumed = conditional.consumedBy[outcome];
      const auto it = effects.find(path);
      if (it == effects.end() || !it->second.consumed())
        continue;
      for (const core::PlaceId target : targets) {
        if (!llvm::is_contained(consumed, target))
          consumed.push_back(target);
      }
    }
  }
  // `if (!make(&s)) return;`: on the classes the caller selects, the places
  // the callee left null hold nothing (RFC 0007, *Per-outcome null stores*).
  for (const auto &[outcome, paths] : summary.nullOn) {
    for (const core::SummaryPath &path : paths) {
      const auto ref = builder.resolveSummaryPath(path, call);
      if (!ref || !ref->element.isWhole())
        continue;
      for (const auto &[cls, effects] : summary.outcomes)
        conditional.consumedBy.try_emplace(cls);
      conditional.nullOn[outcome].push_back(ref->place);
    }
  }
  if (conditional.consumedBy.empty())
    return;
  // Which consumed arguments the callee may hand back as its result.
  for (const core::ValueSource &source : summary.returns) {
    if (source.kind != core::ValueSource::Kind::Copy || !source.path ||
        source.interior || !source.path->isParam() || !source.path->isRoot())
      continue;
    for (const auto &[path, targets] : consumedTargets) {
      if (path != *source.path)
        continue;
      for (const core::PlaceId target : targets) {
        if (!llvm::is_contained(conditional.returned, target))
          conditional.returned.push_back(target);
      }
    }
  }
  lastCall = CallOutcome{.call = &call, .pending = std::move(conditional)};
}

/// Compiler intrinsics (`__builtin_*`, `__sync_*`, ...) are not a checking
/// boundary: they are part of the language, not unknown code.
static bool isCompilerIntrinsic(const FunctionDecl &callee) {
  return callee.getBuiltinID() != 0 && callee.getName().starts_with("__");
}

bool FunctionDataflow::callInvolvesPointers(const CallExpr &call) {
  return call.getType()->isPointerType() ||
         llvm::any_of(call.arguments(), [](const Expr *arg) {
           return arg->getType()->isPointerType();
         });
}

std::string FunctionDataflow::calleeName(const CallExpr &call) {
  if (const FunctionDecl *callee = call.getDirectCallee())
    return "'" + callee->getNameAsString() + "'";
  if (const auto ref = builder.resolvePointerValue(*call.getCallee()))
    return "'" + nameOf(ref->place) + "'";
  return "a function pointer";
}

void FunctionDataflow::handleUncheckedCall(const CallExpr &call,
                                           core::AnalysisState &state) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee != nullptr && isCompilerIntrinsic(*callee))
    return;
  if (!callInvolvesPointers(call))
    return;
  // Whatever the callee was handed may be kept (RFC 0007, *Escape*).
  for (const Expr *arg : call.arguments()) {
    if (arg->getType()->isPointerType())
      escapeValue(builder.classifyValue(*arg), /*deep=*/true, state);
  }

  if (!options.strictExterns) {
    noteUnknownCallee(call);
    return;
  }

  // Strict mode (RFC 0004, *Boundaries*): calling into unchecked code is a
  // raw operation. Its arguments are untouched (the region's author vouches
  // for the callee) and its result is raw, which `classifyValue` arranges.
  if (!recording() || !emitDiagnostics || inUnsafe)
    return;
  const bool direct = callee != nullptr;
  const std::string name = calleeName(call);
  core::Diagnostic diagnostic =
      makeError(core::diag::UnsafeOperation,
                std::string("unchecked call ") + (direct ? "to " : "through ") +
                    name + " outside an unsafe region",
                call);
  if (direct) {
    diagnostic.addNote(name + " is declared here",
                       locate(callee->getLocation()));
    diagnostic.addNote(
        "annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, "
        "WEAVEC_MUT or WEAVEC_RAW, define it in this program, or "
        "move the call into a WEAVEC_UNSAFE region",
        locate(callee->getLocation()));
  } else {
    diagnostic.addNote(
        "annotate the parameters of its function type, take the address of "
        "a function of that type in this program, or move the call "
        "into a WEAVEC_UNSAFE region",
        locate(call));
  }
  report(std::move(diagnostic));
}

void FunctionDataflow::noteUnknownCallee(const CallExpr &call) {
  if (!recording() || !emitDiagnostics || inUnsafe)
    return;
  const FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr) {
    // A call through a function pointer with no signature: once per
    // function type (RFC 0004, *Boundaries*).
    if (!summaries.noteUnknownIndirect(call) || options.deferBoundary)
      return;
    core::Diagnostic diagnostic{
        .severity = core::Severity::Warning,
        .id = core::diag::AnnotationRequired,
        .message = "call through " + calleeName(call) +
                   " is not checked: its function type has no ownership "
                   "annotations and no function of that type has its address "
                   "taken in this program",
        .location = locate(call),
        .notes = {},
        .fixits = {},
    };
    diagnostic.addNote(
        "annotate the parameters of its function type with WEAVEC_OWNED, "
        "WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or take the address of a "
        "function of that type in this program",
        locate(call));
    report(std::move(diagnostic));
    return;
  }

  const SourceManager &sm = context.getSourceManager();
  if (!options.reportUnannotated && sm.isInSystemHeader(callee->getLocation()))
    return;
  // RFC 0005: in the compile step of the driver the boundary is recorded
  // for the exports and the link step reports it if the program has no
  // definition either.
  if (!summaries.noteUnknownCallee(*callee) || options.deferBoundary)
    return;

  const std::string name = callee->getNameAsString();
  core::Diagnostic diagnostic{
      .severity = core::Severity::Warning,
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
                     "WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it "
                     "in this program",
                     locate(callee->getLocation()));
  report(std::move(diagnostic));
}

void FunctionDataflow::handleReturn(const ReturnStmt &ret,
                                    core::AnalysisState &state) {
  const Expr *value = ret.getRetValue();
  if (value == nullptr)
    return;
  if (!value->getType()->isPointerType()) {
    if (value->getType()->isIntegerType())
      recordOutcomes(*value, ValueOrigin{}, state);
    // A struct returned by value hands its fields to the caller (RFC 0007,
    // *Escape*).
    if (const Expr &stripped = PlaceBuilder::stripTransparent(*value);
        value->getType()->isRecordType() &&
        PlaceBuilder::isPlaceExpr(stripped)) {
      if (const auto ref = builder.resolve(stripped)) {
        for (const core::PlaceId place : storageOf(ref->place))
          escape(place, state);
      }
    }
    return;
  }

  const ValueOrigin returned = builder.classifyValue(*value);
  recordOutcomes(*value, returned, state);
  std::vector<ValueOrigin> origins{returned};
  while (!origins.empty()) {
    const ValueOrigin origin = std::move(origins.back());
    origins.pop_back();
    if (origin.kind != ValueOrigin::Kind::Conditional) {
      checkAnnotationOnReturn(origin, *value, state);
      // Returning a raw value from a function whose signature promises a
      // safe kind asserts that kind (RFC 0004, *Raw pointers*, rule 4).
      if (signature.result.safeKind()) {
        if (const auto raw = rawRecordOf(origin, *value, state)) {
          const std::optional<std::string> name =
              origin.place ? std::optional(nameOf(origin.place->place))
                           : std::nullopt;
          reportRawOperation(
              rawPointerPhrase(name) +
                  " is returned from a function whose return type is "
                  "annotated " +
                  macroSpelling(signature.result) + " outside an unsafe region",
              name.value_or(""), *raw, *value);
        }
      }
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
  // After the summary saw it as `fresh`: the caller owns it now, and nothing
  // that dies with this function's locals leaks (RFC 0007, *Escape*).
  escapeValue(returned, /*deep=*/false, state);
}

void FunctionDataflow::handleLifetimeEnd(const VarDecl &var,
                                         const core::SourceLocation &at,
                                         core::AnalysisState &state) {
  const auto place = builder.lookupVar(var);
  if (!place)
    return;
  // Liveness declares every other local dead at its last use, where its
  // resources were checked; an address-taken one lives until here. The
  // report lands on the statement the scope ends after (the `return`), not
  // on the declaration.
  if (addressTaken.contains(var.getCanonicalDecl())) {
    const std::vector<core::PlaceId> storage = storageOf(*place);
    const std::set<core::PlaceId> going(storage.begin(), storage.end());
    checkLeaks(
        storage, [&going](core::PlaceId p) { return going.contains(p); },
        LeakForm::Lost, at.isValid() ? at : locate(var.getEndLoc()), state);
  }
  reinit(*place, state);
}

// -- Semantic actions ---------------------------------------------------------

void FunctionDataflow::reinit(core::PlaceId place, core::AnalysisState &state,
                              core::ElementWitness element) {
  // `a[i] = ...` overwrites one element: a record that another element was
  // freed still holds (RFC 0006, *Element witnesses*).
  std::optional<core::MoveRecord> survivor;
  if (!element.isWhole()) {
    if (const auto record = state.moves.recordOf(place);
        record && !record->element.matches(element))
      survivor = record;
  } else {
    loseTrackBelow(place, state);
    reinitMirrors(place, state);
  }
  state.forget(place);
  if (survivor) {
    state.moves.markMoved(place, survivor->reason, survivor->location,
                          survivor->via, survivor->element, survivor->family);
  }
  for (const core::PlaceId child : places.descendants(place))
    state.forget(child);
}

void FunctionDataflow::reinitMirrors(core::PlaceId place,
                                     core::AnalysisState &state) {
  // The other names of the same cell (`L->twups->stack.p` while `L->twups ~
  // L`): a move recorded under one of them (marked through this place when
  // it was consumed, copied by `mirrorSubtree` when the alias was made, or
  // made by consuming the mirror itself) says the cell held a released
  // pointer, and the value written here replaces it under every name. RFC
  // 0002 applies a fact about a place to every place that may alias it;
  // this is that rule for the write. Without it, `realloc(L->stack.p)`
  // followed by `L->stack.p = fresh` left `L->twups->stack.p: freed` in the
  // summary and a `double-free` at every second call.
  //
  // Not `mirrors()`: that skips an alias below the pointer itself (`L->twups
  // ~ L`, exactly the Lua case) to keep synthesised paths finite. Here no
  // place is created: only a mirror something already named can hold a
  // record, so a non-interning lookup under every alias of every pointer on
  // the path is enough.
  const auto clear = [&state, this](core::PlaceId mirror) {
    state.moves.reinitialize(mirror);
    for (const core::PlaceId below : places.descendants(mirror))
      state.moves.reinitialize(below);
  };
  for (std::optional<core::PlaceId> deref = places.innermostDeref(place); deref;
       deref = places.innermostDeref(*places.parent(*deref))) {
    const core::PlaceId pointer = *places.parent(*deref);
    for (const core::PlaceId alias : state.aliases.members(pointer)) {
      if (alias == pointer)
        continue;
      const auto aliasDeref = places.child(alias, core::PathStep::Deref, {});
      if (!aliasDeref)
        continue;
      if (const auto mirror =
              places.lookupTranslated(place, *deref, *aliasDeref);
          mirror && *mirror != place)
        clear(*mirror);
    }
  }
}

void FunctionDataflow::loseTrackBelow(core::PlaceId pointer,
                                      core::AnalysisState &state) {
  // The name goes but the object may stay, reachable through an alias whose
  // subtree does not carry the edges recorded below this one (`mirrors`
  // finds them through `pointer`, until now). A resource the object refers
  // to is still held there: `n->prev = p; ...; n = mk();` in a list-building
  // loop must not leave `p`'s node with a single reference that the next
  // `a->child = n` overwrites (RFC 0007, *Escape*).
  const auto unrelated = [this, pointer](core::PlaceId other) {
    return other != pointer && !places.isDescendantOf(other, pointer) &&
           !places.isDescendantOf(pointer, other);
  };
  if (llvm::none_of(state.aliases.edgesFrom(pointer),
                    [&unrelated](const auto &e) { return unrelated(e.first); }))
    return;
  for (const core::PlaceId below : places.descendants(pointer)) {
    for (const auto &[other, edge] : state.aliases.edgesFrom(below)) {
      if (unrelated(other) && state.resources.holds(other))
        escape(other, state);
    }
  }
}

void FunctionDataflow::forgetBelow(core::PlaceId place,
                                   core::AnalysisState &state) {
  // The objects below were overwritten: what they held is unknown. They
  // still exist, so loans *against* them stay.
  for (const core::PlaceId child : places.descendants(place)) {
    state.moves.reinitialize(child);
    state.aliases.separate(child);
    state.loans.dropHolder(child);
    state.pending.erase(child);
    state.kinds.erase(child);
    state.raw.clear(child);
    state.resources.forget(child);
  }
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
    if (const auto record = state.moves.recordOf(place)) {
      state.moves.markMoved(mirror, record->reason, record->location,
                            record->via.value_or(place), record->element,
                            record->family);
    }
    if (const auto record = state.resources.recordOf(place))
      state.resources.hold(mirror, *record);
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
    if (const auto record = state.raw.rawAt(place))
      state.raw.markRaw(mirror, *record);
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
    const Expr *where = ref.derefExprs[i];
    if (const auto hit =
            findMoved(ref.derefs[i], state, ref.derefElements[i])) {
      reportUseOfMoved(ref.derefs[i], *hit, where != nullptr ? *where : at);
      return;
    }
    // Dereferencing a raw pointer (RFC 0004, *Raw pointers*, rule 1).
    if (const auto raw = rawAt(ref.derefs[i], state)) {
      const std::string name = nameOf(ref.derefs[i]);
      reportRawOperation("dereference of raw pointer '" + name +
                             "' outside an unsafe region",
                         name, *raw, where != nullptr ? *where : at);
      return;
    }
  }
  if (!includeSelf)
    return;
  recordAccess(ref.place, /*write=*/false, state);
  if (const auto hit = findMoved(ref.place, state, ref.element))
    reportUseOfMoved(ref.place, *hit, at);
}

std::vector<core::PlaceId>
FunctionDataflow::doConsume(const PlaceRef &ref, core::MoveReason reason,
                            const Expr &at, core::AnalysisState &state,
                            std::string_view family, bool library) {
  const core::PlaceId place = ref.place;
  checkAnnotationOnConsume(ref, reason, at, state);

  if (const auto hit = findMoved(place, state, ref.element)) {
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
    return {};
  }

  // Freeing or moving a borrowed object invalidates the loan (RFC 0006,
  // *Conflict rules*): the one borrow conflict reported by default. A loan
  // on an ancestor is not invalidated: `strm = &s->strm; init(&s->strm)`,
  // where `init` frees `s->strm.state->window`, leaves `strm` pointing at
  // storage nothing released.
  if (const auto conflict =
          findLoanConflict(place, std::nullopt, state, /*ancestors=*/false)) {
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

  // RFC 0007: the wrong family, and the resources the freed object's own
  // storage still holds (`free(b)` with `b->data` owned).
  checkReleaseFamily(place, family, at, state);
  if (reason == core::MoveReason::Freed && ref.element.isWhole()) {
    if (library)
      checkContainerFree(place, at, state);
    else
      releaseStorageBelow(place, state);
  }

  const core::SourceLocation here = locate(at);
  std::vector<core::PlaceId> marked;
  for (const ConsumeTarget &target :
       consumeTargets(place, ref.element, state)) {
    std::optional<core::PlaceId> via;
    if (target.place != place)
      via = place;
    state.moves.markMoved(target.place, reason, here, via, target.element,
                          std::string(family));
    recordConsume(target.place, reason, family, state);
    marked.push_back(target.place);
  }
  return marked;
}

void FunctionDataflow::doMutationCheck(core::PlaceId place, const Expr &at,
                                       core::AnalysisState &state) {
  // Writing to a borrowed place is Rust's exclusivity rule, opt-in under
  // `--exclusive-borrows` (RFC 0006, *Conflict rules*).
  if (!options.exclusiveBorrows)
    return;
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
                                          core::AnalysisState &state,
                                          core::ElementWitness element) {
  // Facts about the source must be captured before the destination is reset:
  // `p = p->next` copies from a place below `p` that `reinit` forgets.
  struct CopySource {
    core::PlaceId place;
    core::ElementWitness element;
    bool interior;
    core::OwnershipKind kind;
    std::optional<MovedHit> moved;
    std::optional<core::ResourceRecord> resource;
    std::vector<core::Loan> loans;
    bool belowDest;
  };
  struct Arm {
    const ValueOrigin *origin;
    std::optional<CopySource> source;
    std::optional<core::RawRecord> raw;
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
          .element = current->place->element,
          .interior = current->interior,
          .kind = state.kindOf(src),
          .moved = findMoved(src, state, current->place->element),
          .resource = state.resources.recordOf(src),
          .loans = state.loans.heldBy(src),
          .belowDest = src == dest || places.isDescendantOf(src, dest),
      };
    }
    arms.push_back(Arm{.origin = current,
                       .source = std::move(source),
                       .raw = rawRecordOf(*current, at, state),
                       .summary = recording() ? sourceOf(*current, state)
                                              : core::ValueSource::unknown()});
  }

  // `p = p + k`, `p = (T *)p`, `p = p`: the value is the place's own, so every
  // fact about it (aliases, loans, move and raw records) stays. RFC 0004,
  // *Pointer identity*, makes `p = p + 1` mean the same as `p += 1`.
  if (arms.size() == 1 && arms[0].origin->kind == ValueOrigin::Kind::Copy &&
      arms[0].origin->place && arms[0].origin->place->place == dest)
    return;

  // RFC 0004, *Raw pointers*: a `WEAVEC_RAW` destination takes the value out
  // of the model, whatever it was; a destination declared with a safe kind
  // takes a raw value only as an assertion, which needs an unsafe region.
  const std::optional<AnnotationSet> declared = declaredAnnotations(dest);
  if (declared && declared->raw) {
    // The value leaves the model (RFC 0007, *Escape*), and so does whatever
    // the destination held before.
    for (const Arm &arm : arms) {
      if (arm.source)
        escape(arm.source->place, state);
    }
    if (element.isWhole())
      checkOverwrite(dest, at, state);
    reinit(dest, state);
    markRaw(dest,
            core::RawRecord{.reason = core::RawReason::Declared,
                            .location = locate(at),
                            .via = std::nullopt,
                            .detail = nameOf(dest)},
            state);
    if (recording()) {
      for (std::size_t i = 0; i < arms.size(); ++i)
        recordStore(dest, core::ValueSource::raw());
    }
    return;
  }
  const bool assertsKind = declared && declared->safeKind().has_value();
  for (Arm &arm : arms) {
    if (!arm.raw)
      continue;
    if (assertsKind) {
      const std::optional<std::string> name =
          arm.source ? std::optional(nameOf(arm.source->place)) : std::nullopt;
      reportRawOperation(rawPointerPhrase(name) + " is assigned to '" +
                             nameOf(dest) + "', which is declared " +
                             macroSpelling(*declared) +
                             ", outside an unsafe region",
                         name.value_or(""), *arm.raw, at);
      // The assertion holds from here on either way; not asserting would
      // only cascade into a report per later use.
      arm.raw.reset();
    }
  }

  // An element write does not overwrite the summary place's other elements
  // (RFC 0007, *Death points*).
  if (element.isWhole())
    checkOverwrite(dest, at, state);
  reinit(dest, state, element);

  bool allNull = !arms.empty();
  for (const Arm &armRecord : arms) {
    const ValueOrigin *arm = armRecord.origin;
    const std::optional<CopySource> &source = armRecord.source;
    if (arm->kind != ValueOrigin::Kind::Null)
      allNull = false;
    if (armRecord.raw) {
      // Raw values copy freely but carry no ownership, loans or aliases
      // worth tracking (RFC 0004, *Raw pointers*).
      markRaw(dest, *armRecord.raw, state);
      continue;
    }
    if (assertsKind && arm->kind != ValueOrigin::Kind::Copy &&
        arm->kind != ValueOrigin::Kind::Borrow &&
        arm->kind != ValueOrigin::Kind::Null) {
      // Anything else stored into an annotated place is the declared kind.
      setKind(dest, core::join(state.kindOf(dest), *declared->safeKind()),
              state);
    }
    switch (arm->kind) {
    case ValueOrigin::Kind::Raw:
      // Handled above: `rawRecordOf` never misses a raw origin.
      break;
    case ValueOrigin::Kind::Alloc:
      setKind(dest, core::join(state.kindOf(dest), core::OwnershipKind::Owned),
              state);
      state.resources.hold(
          dest,
          core::ResourceRecord{
              .origin = core::ResourceOrigin::Allocated,
              .location = locate(arm->call != nullptr
                                     ? static_cast<const Stmt &>(*arm->call)
                                     : static_cast<const Stmt &>(at)),
              .family = arm->family,
              .escaped = false});
      break;
    case ValueOrigin::Kind::Copy: {
      if (!source)
        break;
      // The copy holds the same resource; both names now account for it.
      if (source->resource)
        state.resources.hold(dest, *source->resource);
      // An asserted raw source has the declared kind from here on.
      const core::OwnershipKind sourceKind =
          assertsKind && source->kind == core::OwnershipKind::Raw
              ? *declared->safeKind()
              : source->kind;
      setKind(dest, core::join(state.kindOf(dest), sourceKind), state);
      if (source->belowDest)
        break;
      state.aliases.unite(dest, source->place, !source->interior, element,
                          source->element);
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
            source->moved->record.via.value_or(source->moved->target), element,
            source->moved->record.family);
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
  if (allNull && element.isWhole())
    state.resources.markNull(dest);
  // `box->buf = p` where `box` came from code nobody here can follow: the
  // object belongs to whoever handed the pointer out, so what is stored in
  // it is reachable from there. Likewise `tb = &L->strt; tb->hash = p`: the
  // store landed in the caller's object, but under a name the summary
  // cannot report (RFC 0007, *Escape*).
  if (isBelowOpaquePointer(dest, state) ||
      isBelowBorrowOfCallerMemory(dest, state)) {
    escape(dest, state);
    for (const Arm &arm : arms) {
      if (arm.source)
        escape(arm.source->place, state);
    }
  }

  if (recording()) {
    for (const Arm &arm : arms)
      recordStore(dest, arm.summary);
  }
}

bool FunctionDataflow::isBelowOpaquePointer(core::PlaceId dest,
                                            const core::AnalysisState &state) {
  const auto deref = places.innermostDeref(dest);
  if (!deref)
    return false;
  const core::PlaceId pointer = *places.parent(*deref);
  // Caller memory is reported to the caller as a store; an object this
  // function owns, borrows or reached by a copy of a known pointer is
  // tracked through that pointer's own facts.
  if (builder.summaryPathOf(pointer) ||
      state.kindOf(pointer) == core::OwnershipKind::Owned ||
      state.resources.recordOf(pointer) || !state.loans.heldBy(pointer).empty())
    return false;
  return llvm::all_of(
      state.aliases.members(pointer),
      [pointer](const core::PlaceId alias) { return alias == pointer; });
}

bool FunctionDataflow::isBelowBorrowOfCallerMemory(
    core::PlaceId dest, const core::AnalysisState &state) {
  const auto deref = places.innermostDeref(dest);
  if (!deref || builder.summaryPathOf(dest))
    return false;
  const core::PlaceId pointer = *places.parent(*deref);
  if (builder.summaryPathOf(pointer))
    return false;
  // A loan on the caller's memory (below a parameter's dereference) or on a
  // global: the object outlives this function, and the store is not a
  // summary store the caller would see.
  return llvm::any_of(state.loans.heldBy(pointer), [this](const auto &loan) {
    const auto target = builder.summaryPathOf(loan.place);
    return target && (!target->isParam() || target->hasDeref());
  });
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

  // Exclusivity (RFC 0001), opt-in under `--exclusive-borrows` (RFC 0006,
  // *Conflict rules*).
  if (const auto conflict = options.exclusiveBorrows
                                ? findLoanConflict(target, kind, state)
                                : std::nullopt) {
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
  if (!options.exclusiveBorrows)
    return;
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

std::vector<FunctionDataflow::ConsumeTarget>
FunctionDataflow::consumeTargets(core::PlaceId place,
                                 core::ElementWitness element,
                                 const core::AnalysisState &state) {
  std::vector<ConsumeTarget> result;
  const auto add = [&result](core::PlaceId id, core::ElementWitness witness) {
    for (ConsumeTarget &existing : result) {
      if (existing.place != id)
        continue;
      // Named twice (through two mirrors): the record covers both.
      if (existing.element != witness)
        existing.element = core::ElementWitness::whole();
      return;
    }
    result.push_back(ConsumeTarget{.place = id, .element = witness});
  };
  for (const core::PlaceId mirror : mirrors(place, state)) {
    add(mirror, element);
    for (const auto &[alias, edge] : state.aliases.edgesFrom(mirror)) {
      // `edge.element` is the element of `alias` this mirror holds; the
      // edge back says which element of the mirror `alias` holds, which
      // must be the one being consumed.
      const auto back = state.aliases.edge(alias, mirror);
      if (!back || !back->element.matches(element))
        continue;
      add(alias, edge.element);
    }
  }
  std::ranges::sort(result, {}, &ConsumeTarget::place);
  return result;
}

bool FunctionDataflow::knowsPlace(core::PlaceId place,
                                  core::ElementWitness element,
                                  const core::AnalysisState &state) {
  // A place only a summary has ever named (`o->child->child` while applying
  // a recursive destructor) has no alias to mark and no record to settle.
  return llvm::any_of(consumeTargets(place, element, state),
                      [&](const ConsumeTarget &target) {
                        return builder.declFor(target.place) != nullptr ||
                               !state.aliases.edgesFrom(target.place).empty() ||
                               state.resources.holds(target.place) ||
                               state.resources.isNull(target.place) ||
                               state.moves.recordOf(target.place);
                      });
}

std::vector<core::PlaceId> FunctionDataflow::related(core::PlaceId place) {
  std::vector<core::PlaceId> result{place};
  llvm::append_range(result, places.ancestors(place));
  llvm::append_range(result, places.descendants(place));
  return result;
}

std::optional<FunctionDataflow::MovedHit>
FunctionDataflow::findMoved(core::PlaceId place,
                            const core::AnalysisState &state,
                            core::ElementWitness element) {
  // Facts are propagated eagerly to every alias and mirror when they are
  // created (`doConsume`, `mirrorSubtree`), so a query only needs to look at
  // the place itself. Looking at the whole class here would turn the
  // may-alias over-approximation introduced by joins into false positives:
  // after `cur = next` in a list walk, `cur` may alias both the freed node
  // and the live one, but only the freed node carries a move record.
  if (const auto record = state.moves.movedAt(place, element))
    return MovedHit{.target = place, .record = *record};
  return std::nullopt;
}

std::optional<core::Loan> FunctionDataflow::findLoanConflict(
    core::PlaceId place, std::optional<core::BorrowKind> kind,
    const core::AnalysisState &state, bool ancestors) {
  std::vector<core::PlaceId> candidates{place};
  if (ancestors)
    llvm::append_range(candidates, places.ancestors(place));
  llvm::append_range(candidates, places.descendants(place));
  for (const core::PlaceId candidate : candidates) {
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
  // Nothing is reported for code inside an unsafe region (RFC 0004, *Unsafe
  // regions*); the region is still analysed so its effects reach the code
  // around it, where they are checked.
  if (phase == Phase::Final && emitDiagnostics && !inUnsafe)
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
  // The same leak found on two edges out of one block is one report.
  const auto duplicates =
      std::ranges::unique(pending, [&key](const core::Diagnostic &lhs,
                                          const core::Diagnostic &rhs) {
        return key(lhs) == key(rhs);
      });
  pending.erase(duplicates.begin(), duplicates.end());
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

core::Diagnostic FunctionDataflow::makeError(std::string_view id,
                                             std::string message,
                                             const Expr &at) const {
  return core::Diagnostic{
      .severity = core::Severity::Error,
      .id = id,
      .message = std::move(message),
      .location = locate(at),
      .notes = {},
      .fixits = {},
  };
}

// -- Raw pointers (RFC 0004) --------------------------------------------------

std::optional<AnnotationSet>
FunctionDataflow::declaredAnnotations(core::PlaceId place) const {
  if (const auto it = declaredKinds.find(place); it != declaredKinds.end())
    return it->second;
  const NamedDecl *decl = builder.declFor(place);
  if (decl == nullptr)
    return std::nullopt;
  if (const auto *field = dyn_cast<FieldDecl>(decl)) {
    const AnnotationSet annotations = getAnnotations(*field);
    if (annotations.ownership())
      return annotations;
    return std::nullopt;
  }
  if (const auto *var = dyn_cast<VarDecl>(decl);
      var != nullptr && var->hasGlobalStorage()) {
    const AnnotationSet annotations = getAnnotations(*var);
    if (annotations.ownership())
      return annotations;
  }
  return std::nullopt;
}

std::optional<core::RawRecord>
FunctionDataflow::rawAt(core::PlaceId place,
                        const core::AnalysisState &state) const {
  if (auto record = state.raw.rawAt(place))
    return record;
  if (!builder.isDeclaredRaw(place))
    return std::nullopt;
  const NamedDecl *decl = builder.declFor(place);
  return core::RawRecord{
      .reason = core::RawReason::Declared,
      .location = decl != nullptr ? locate(decl->getLocation())
                                  : core::SourceLocation{},
      .via = std::nullopt,
      .detail = nameOf(place),
  };
}

std::optional<core::RawRecord>
FunctionDataflow::rawRecordOf(const ValueOrigin &origin, const Expr &at,
                              const core::AnalysisState &state) {
  switch (origin.kind) {
  case ValueOrigin::Kind::Raw: {
    // Point at the cast or the call that made the value raw when we can.
    const Expr *where = origin.call != nullptr
                            ? static_cast<const Expr *>(origin.call)
                            : origin.source;
    return core::RawRecord{
        .reason = origin.rawReason,
        .location = locate(where != nullptr ? *where : at),
        .via = std::nullopt,
        .detail = origin.call != nullptr ? calleeName(*origin.call) : "",
    };
  }
  case ValueOrigin::Kind::Copy: {
    if (!origin.place)
      return std::nullopt;
    if (auto record = rawAt(origin.place->place, state)) {
      // Remember the first place the value was copied from, as `MoveTracker`
      // does, so notes can say "(through 'q')".
      if (!record->via)
        record->via = origin.place->place;
      return record;
    }
    // A value loaded through a raw pointer is raw (RFC 0004, *Raw pointers*).
    for (const core::PlaceId deref : origin.place->derefs) {
      if (const auto record = rawAt(deref, state)) {
        return core::RawRecord{.reason = core::RawReason::LoadedThroughRaw,
                               .location = locate(at),
                               .via = std::nullopt,
                               .detail = nameOf(deref)};
      }
    }
    return std::nullopt;
  }
  case ValueOrigin::Kind::Alloc:
  case ValueOrigin::Kind::Borrow:
  case ValueOrigin::Kind::Null:
  case ValueOrigin::Kind::Opaque:
  case ValueOrigin::Kind::Conditional:
    return std::nullopt;
  }
  return std::nullopt;
}

void FunctionDataflow::markRaw(core::PlaceId place,
                               const core::RawRecord &record,
                               core::AnalysisState &state) {
  setKind(place, core::OwnershipKind::Raw, state);
  for (const core::PlaceId mirror : mirrors(place, state))
    state.raw.markRaw(mirror, record);
  for (const core::PlaceId alias : targets(place, state))
    state.raw.markRaw(alias, record);
}

std::string FunctionDataflow::rawNote(const core::RawRecord &record,
                                      std::string_view name) const {
  std::string note = name.empty() ? "the pointer is raw: "
                                  : "'" + std::string(name) + "' is raw: ";
  switch (record.reason) {
  case core::RawReason::IntegerCast:
    note += "cast from an integer";
    break;
  case core::RawReason::Declared:
    note += "declared WEAVEC_RAW";
    break;
  case core::RawReason::LoadedThroughRaw:
    note += "loaded through raw pointer '" + record.detail + "'";
    break;
  case core::RawReason::Callee:
    // `detail` is `calleeName`: quoted, or `a function pointer`.
    note += "handed out by " +
            (record.detail.empty() ? std::string("a callee") : record.detail);
    break;
  case core::RawReason::UnknownCallee:
    note += "returned by a call into unchecked code";
    if (!record.detail.empty())
      note += " (" + record.detail + ")";
    break;
  }
  note += " here";
  if (record.via && nameOf(*record.via) != name)
    note += " (through '" + nameOf(*record.via) + "')";
  return note;
}

std::optional<std::string> FunctionDataflow::pointerName(const Expr &value) {
  if (const auto ref = builder.resolvePointerValue(value))
    return nameOf(ref->place);
  return std::nullopt;
}

void FunctionDataflow::reportRawOperation(std::string message,
                                          std::string_view name,
                                          const core::RawRecord &record,
                                          const Expr &at) {
  if (inUnsafe)
    return;
  core::Diagnostic diagnostic =
      makeError(core::diag::UnsafeOperation, std::move(message), at);
  if (record.location.isValid())
    diagnostic.addNote(rawNote(record, name), record.location);
  diagnostic.addNote("move this operation into a WEAVEC_UNSAFE block or "
                     "function, or assert the pointer's ownership first",
                     locate(at));
  report(std::move(diagnostic));
}

void FunctionDataflow::checkRawArgument(const CallExpr &call, unsigned index,
                                        const char *verb,
                                        const core::AnalysisState &state) {
  if (inUnsafe || index >= call.getNumArgs())
    return;
  const Expr &arg = *call.getArg(index);
  const auto raw = rawRecordOf(builder.classifyValue(arg), arg, state);
  if (!raw)
    return;
  const std::optional<std::string> argName = pointerName(arg);
  reportRawOperation(calleeName(call) + " " + verb + " " +
                         rawPointerPhrase(argName) +
                         " outside an unsafe region",
                     argName.value_or(""), *raw, arg);
}

// -- Dump ---------------------------------------------------------------------

void FunctionDataflow::dump(const core::AnalysisState *exitState) {
  llvm::raw_ostream &os = *options.dumpStream;
  os << "function '" << function.getNameAsString() << "'"
     << (unsafeBody ? " (unsafe)" : "") << ":\n";

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
      if (!record->family.empty())
        os << "(" << record->family << ")";
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
    os << "} raw{";
    first = true;
    for (const core::PlaceId place : exitState->raw.rawPlaces()) {
      const auto record = exitState->raw.rawAt(place);
      os << (first ? "" : ", ") << places.name(place);
      if (record->location.isValid())
        os << "@" << record->location.line << ":" << record->location.column;
      os << " " << core::toString(record->reason);
      first = false;
    }
    os << "} owned{";
    first = true;
    for (const core::PlaceId place : exitState->resources.holders()) {
      const auto record = exitState->resources.recordOf(place);
      os << (first ? "" : ", ") << places.name(place);
      if (record->location.isValid())
        os << "@" << record->location.line << ":" << record->location.column;
      os << " " << core::toString(record->origin);
      if (!record->family.empty())
        os << " " << record->family;
      if (record->escaped)
        os << " escaped";
      first = false;
    }
    os << "}\n";
  }

  os << "  summary:";
  const auto describeSource = [this](const core::ValueSource &source) {
    std::string text(source.kind == core::ValueSource::Kind::Copy &&
                             source.interior
                         ? std::string_view("interior")
                         : core::toString(source.kind));
    if (source.isFresh() && !source.family.empty())
      text += "(" + source.family + ")";
    if (source.path)
      text += " " + summaryName(*source.path);
    return text;
  };
  const auto describeConsume = [](const core::PlaceEffect &effect,
                                  const char *label) {
    std::string text(label);
    if (!effect.family.empty())
      text += "(" + effect.family + ")";
    return text;
  };
  for (const auto &[path, effect] : inferred.effects) {
    os << " " << summaryName(path) << ":";
    const char *sep = " ";
    for (const auto &[flag, label] :
         {std::pair{effect.read, std::string("read")},
          std::pair{effect.written, std::string("written")},
          std::pair{effect.freed, describeConsume(effect, "freed")},
          std::pair{effect.moved, describeConsume(effect, "moved")}}) {
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
  for (const auto &[outcome, effects] : inferred.outcomes) {
    os << " outcome " << core::toString(outcome) << "{";
    first = true;
    for (const auto &[path, effect] : effects) {
      os << (first ? "" : ", ") << summaryName(path) << ":"
         << (effect.freed ? " " + describeConsume(effect, "freed") : "")
         << (effect.moved ? " " + describeConsume(effect, "moved") : "");
      first = false;
    }
    os << "}";
    if (const auto nulls = inferred.nullOn.find(outcome);
        nulls != inferred.nullOn.end()) {
      os << " null{";
      first = true;
      for (const core::SummaryPath &path : nulls->second) {
        os << (first ? "" : ", ") << summaryName(path);
        first = false;
      }
      os << "}";
    }
  }
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

void FunctionDataflow::replayWrites(const CallExpr &call,
                                    const PlaceRef &pointee,
                                    std::uint32_t argument,
                                    const core::FunctionSummary &summary,
                                    const core::AnalysisState &state) {
  if (!recording())
    return;
  // The callee's paths below `param(argument)*` are this function's paths
  // below the pointee (and its mirrors), by prefix substitution: resolving
  // each of them to a place and back would intern a place per path per
  // call, and Lua-sized programs pass a state pointer with dozens of
  // written fields to every call.
  std::vector<core::SummaryPath> bases;
  std::vector<core::PlaceId> affected = mirrors(pointee.place, state);
  if (!llvm::is_contained(affected, pointee.place))
    affected.push_back(pointee.place);
  for (const core::PlaceId place : affected) {
    const auto path = builder.summaryPathOf(place);
    if (!path || !path->hasDeref())
      continue;
    // The summary only grows, so a base replayed at this call on an earlier
    // visit of its block has nothing new to add.
    if (replayed.insert(std::pair{&call, *path}).second)
      bases.push_back(*path);
  }
  if (bases.empty())
    return;
  const core::SummaryPath pointeePath =
      core::SummaryPath::param(argument).deref();
  bool replayedAny = false;
  // Paths order by root, then step by step, so those at or below the
  // pointee are one contiguous run.
  for (auto it = summary.effects.lower_bound(pointeePath);
       it != summary.effects.end() &&
       (it->first == pointeePath || pointeePath.isProperPrefixOf(it->first));
       ++it) {
    const auto &[path, effect] = *it;
    if (!effect.written)
      continue;
    replayedAny = true;
    for (const core::SummaryPath &base : bases) {
      core::SummaryPath written = base;
      written.steps.insert(
          written.steps.end(),
          std::next(path.steps.begin(),
                    static_cast<std::ptrdiff_t>(pointeePath.steps.size())),
          path.steps.end());
      if (written.steps.size() <= MaxPlaceDepth)
        inferred.addEffect(std::move(written),
                           core::PlaceEffect{.written = true});
    }
  }
  if (!replayedAny) {
    for (const core::SummaryPath &base : bases)
      inferred.addEffect(base, core::PlaceEffect{.written = true});
  }
}

bool FunctionDataflow::isEventBased(const core::SummaryPath &path) const {
  return path.isParam() &&
         (path.isRoot() ||
          (path.index < paramReassigned.size() && paramReassigned[path.index]));
}

/// The summary effect a move record stands for.
static core::PlaceEffect effectOfMove(core::MoveReason reason,
                                      std::string_view family) {
  core::PlaceEffect effect;
  if (reason == core::MoveReason::Freed)
    effect.freed = true;
  else
    effect.moved = true;
  effect.family = std::string(family);
  return effect;
}

void FunctionDataflow::recordConsume(core::PlaceId target,
                                     core::MoveReason reason,
                                     std::string_view family,
                                     core::AnalysisState &state) {
  const auto path = builder.summaryPathOf(target);
  if (!path)
    return;
  const core::PlaceEffect effect = effectOfMove(reason, family);
  // The flow-sensitive record feeds the outcome classes at each `return`
  // (RFC 0006); it is part of the state so the fixpoint sees it.
  if (isEventBased(*path))
    state.consumed[*path].join(effect);
  if (recording())
    eventEffects[*path].join(effect);
}

core::OutcomeEffects
FunctionDataflow::consumptionAt(const core::AnalysisState &state) {
  core::OutcomeEffects result = state.consumed;
  for (const core::PlaceId place : state.moves.movedPlaces()) {
    const auto path = builder.summaryPathOf(place);
    if (!path || isEventBased(*path))
      continue;
    const auto record = state.moves.recordOf(place);
    result[*path].join(effectOfMove(record->reason, record->family));
  }
  return result;
}

/// The outcome classes a returned value may fall in (RFC 0006,
/// *Inference*).
static std::set<core::Outcome>
outcomesOf(const Expr &value, const ValueOrigin &origin, ASTContext &context) {
  const QualType type = value.getType();
  if (type->isPointerType()) {
    switch (origin.kind) {
    case ValueOrigin::Kind::Null:
      return {core::Outcome::Null};
    case ValueOrigin::Kind::Borrow:
      return {core::Outcome::NonNull};
    case ValueOrigin::Kind::Conditional: {
      std::set<core::Outcome> result;
      for (const ValueOrigin &alternative : origin.alternatives)
        result.merge(outcomesOf(value, alternative, context));
      return result;
    }
    default:
      return {core::Outcome::Null, core::Outcome::NonNull};
    }
  }
  if (!type->isIntegerType())
    return {};
  if (const auto k = integerConstant(value, context)) {
    if (*k == 0)
      return {core::Outcome::Zero};
    return {*k > 0 ? core::Outcome::Positive : core::Outcome::Negative};
  }
  if (type->isUnsignedIntegerType())
    return {core::Outcome::Zero, core::Outcome::Positive};
  // A comparison or `!x` is 0 or 1.
  const Expr *e = value.IgnoreParenImpCasts();
  const auto *binary = dyn_cast<BinaryOperator>(e);
  const auto *unary = dyn_cast<UnaryOperator>(e);
  if ((binary != nullptr &&
       (binary->isComparisonOp() || binary->isLogicalOp())) ||
      (unary != nullptr && unary->getOpcode() == UO_LNot))
    return {core::Outcome::Zero, core::Outcome::Positive};
  return {core::Outcome::Zero, core::Outcome::Positive,
          core::Outcome::Negative};
}

void FunctionDataflow::recordOutcomes(const Expr &value,
                                      const ValueOrigin &origin,
                                      const core::AnalysisState &state) {
  if (!recording())
    return;
  std::set<core::Outcome> classes = outcomesOf(value, origin, context);
  if (classes.empty())
    return;
  const core::OutcomeEffects base = consumptionAt(state);

  // `return realloc(p, n)`, `q = realloc(p, n); return q;`: the paths
  // returning each class inherit what that class retracts, and the result
  // can only be in a class the (possibly narrowed) pending outcome still
  // allows: after `if (!q) return NULL;`, `return q` is `nonnull`.
  const core::PendingOutcome *retractable = nullptr;
  const Expr *e = value.IgnoreParenCasts();
  if (const auto *call = dyn_cast<CallExpr>(e)) {
    if (lastCall && lastCall->call == call)
      retractable = &lastCall->pending;
  } else if (const auto ref = builder.resolvePointerValue(*e)) {
    if (const auto it = state.pending.find(ref->place);
        it != state.pending.end())
      retractable = &it->second;
  }
  if (retractable != nullptr) {
    std::set<core::Outcome> allowed;
    for (const auto &[outcome, consumed] : retractable->consumedBy) {
      if (classes.contains(outcome))
        allowed.insert(outcome);
    }
    if (!allowed.empty())
      classes = std::move(allowed);
  }

  // The caller memory known null here, plus what the returned test itself
  // says (`return *out != NULL` returns zero exactly when `*out` is null);
  // per class the summary keeps what holds at *every* return of that class
  // (RFC 0007, *Per-outcome null stores*).
  std::set<core::SummaryPath> nullHere;
  for (const core::PlaceId place : state.resources.nullPlaces()) {
    if (const auto path = callerVisiblePath(place))
      nullHere.insert(*path);
  }
  // What holds a resource here: a `fresh` store's destination that holds
  // none at any return of a class was not stored on that class's paths
  // (`if (strm == NULL) return Z_STREAM_ERROR;` before `strm->state = s`).
  std::set<core::SummaryPath> heldHere;
  for (const core::PlaceId place : state.resources.holders()) {
    if (const auto path = callerVisiblePath(place))
      heldHere.insert(*path);
  }
  std::optional<std::pair<core::SummaryPath, core::Outcome>> tested;
  if (const auto test = nullTestReturn(value)) {
    if (const auto ref = builder.resolve(*test->first);
        ref && ref->element.isWhole()) {
      if (const auto path = callerVisiblePath(ref->place))
        tested.emplace(*path, test->second);
    }
  }

  for (const core::Outcome outcome : classes) {
    core::OutcomeEffects effects = base;
    if (retractable != nullptr) {
      core::PendingOutcome narrowed = *retractable;
      for (const core::PlaceId place : narrowed.select({outcome})) {
        if (const auto path = builder.summaryPathOf(place))
          effects.erase(*path);
      }
    }
    inferred.addOutcome(outcome);
    for (const auto &[path, effect] : effects)
      inferred.addOutcome(outcome, path, effect);

    std::set<core::SummaryPath> nullInClass = nullHere;
    if (tested && tested->second == outcome)
      nullInClass.insert(tested->first);
    // `return *out != NULL` holds a record at the statement and none on the
    // zero class: what the class says null is not held there.
    std::set<core::SummaryPath> heldInClass;
    std::ranges::set_difference(heldHere, nullInClass,
                                std::inserter(heldInClass, heldInClass.end()));
    const auto [it, first] = nullAtReturn.try_emplace(
        outcome, NullAtReturn{.null = nullInClass, .held = heldInClass});
    if (first)
      continue;
    std::set<core::SummaryPath> both;
    std::ranges::set_intersection(it->second.null, nullInClass,
                                  std::inserter(both, both.end()));
    it->second.null = std::move(both);
    it->second.held.insert(heldInClass.begin(), heldInClass.end());
  }
}

std::optional<core::SummaryPath>
FunctionDataflow::callerVisiblePath(core::PlaceId place) {
  auto path = stableSummaryPathOf(place);
  if (!path || (path->isParam() && !path->hasDeref()))
    return std::nullopt;
  return path;
}

std::optional<std::pair<const Expr *, core::Outcome>>
FunctionDataflow::nullTestReturn(const Expr &value) const {
  if (!value.getType()->isIntegerType())
    return std::nullopt;
  // `!x` flips which class means null; `!!x` is `x`.
  bool negated = false;
  const Expr *e = value.IgnoreParenImpCasts();
  while (const auto *unary = dyn_cast<UnaryOperator>(e)) {
    if (unary->getOpcode() != UO_LNot)
      return std::nullopt;
    negated = !negated;
    e = unary->getSubExpr()->IgnoreParenImpCasts();
  }
  // Which class the test yields when the place is null.
  const auto classify = [negated](const Expr &place, bool trueWhenNull) {
    const bool positiveWhenNull = trueWhenNull != negated;
    return std::pair{&place, positiveWhenNull ? core::Outcome::Positive
                                              : core::Outcome::Zero};
  };
  if (const auto *binary = dyn_cast<BinaryOperator>(e);
      binary != nullptr &&
      (binary->getOpcode() == BO_EQ || binary->getOpcode() == BO_NE)) {
    const Expr &lhs = *binary->getLHS()->IgnoreParenImpCasts();
    const Expr &rhs = *binary->getRHS()->IgnoreParenImpCasts();
    if (!lhs.getType()->isPointerType() || !rhs.getType()->isPointerType())
      return std::nullopt;
    const bool equal = binary->getOpcode() == BO_EQ;
    if (isNullConstant(rhs, context) && PlaceBuilder::isPlaceExpr(lhs))
      return classify(lhs, equal);
    if (isNullConstant(lhs, context) && PlaceBuilder::isPlaceExpr(rhs))
      return classify(rhs, equal);
    return std::nullopt;
  }
  // `return !p;` / `return !!p;`: a pointer converted to a truth value; the
  // bare `return p;` of a pointer-typed function is not an integer.
  if (e->getType()->isPointerType() && PlaceBuilder::isPlaceExpr(*e) && negated)
    return classify(*e, /*trueWhenNull=*/false);
  return std::nullopt;
}

void FunctionDataflow::recordStore(core::PlaceId dest,
                                   const core::ValueSource &value) {
  if (!recording())
    return;
  // Deliberately not mirrored onto the destination's aliases (`b = outer;
  // b->buf = p` is not recorded as a store into `outer->buf`, though reads
  // and writes are): on Lua, where `L` aliases half the heap, the mirrored
  // stores made every summary application replay dozens of borrows and the
  // program analysis twenty times slower.
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
    return core::ValueSource::fresh(origin.family);
  case ValueOrigin::Kind::Null:
    return core::ValueSource::null();
  case ValueOrigin::Kind::Borrow:
    if (origin.place) {
      if (const auto path = stableSummaryPathOf(origin.place->place))
        return core::ValueSource::borrow(*path);
    }
    return core::ValueSource::unknown();
  case ValueOrigin::Kind::Raw:
    return core::ValueSource::raw();
  case ValueOrigin::Kind::Copy: {
    if (!origin.place)
      return core::ValueSource::unknown();
    const core::PlaceId src = origin.place->place;
    // A raw value stays raw for the caller (RFC 0004): a `WEAVEC_RAW`
    // parameter or field, or anything made raw on the way.
    if (state.raw.isRaw(src) || builder.isDeclaredRaw(src))
      return core::ValueSource::raw();
    // `p + 1` is a copy into the argument's object, not of its value (RFC
    // 0006, *Alias exactness*); so is a local that aliases the argument
    // through an interior edge.
    const auto copyOf = [](const core::SummaryPath &path, bool interior) {
      return interior ? core::ValueSource::interiorCopy(path)
                      : core::ValueSource::copy(path);
    };
    if (const auto path = stableSummaryPathOf(src))
      return copyOf(*path, origin.interior);
    // A local: resolve through what it aliases, then what it borrows, then
    // what it owns.
    for (const auto &[alias, edge] : state.aliases.edgesFrom(src)) {
      if (const auto path = stableSummaryPathOf(alias))
        return copyOf(*path, origin.interior || !edge.exact);
    }
    for (const core::Loan &loan : state.loans.heldBy(src)) {
      if (const auto path = stableSummaryPathOf(loan.place))
        return core::ValueSource::borrow(*path);
    }
    if (state.kindOf(src) == core::OwnershipKind::Owned) {
      // An owned local that also went to code nobody can see is not the
      // caller's alone (RFC 0007, *Inference*).
      if (const auto record = state.resources.recordOf(src)) {
        if (record->escaped)
          return core::ValueSource::unknown();
        return core::ValueSource::fresh(record->family);
      }
      return core::ValueSource::fresh();
    }
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
    if (isEventBased(path))
      inferred.addEffect(path, effect);
  }
  if (exitState != nullptr) {
    for (const core::PlaceId place : exitState->moves.movedPlaces()) {
      const auto path = builder.summaryPathOf(place);
      if (!path)
        continue;
      const auto record = exitState->moves.recordOf(place);
      inferred.addEffect(*path, effectOfMove(record->reason, record->family));
    }
  }
  // Every class's consumption is part of the unconditional effects; a
  // class recorded from a path whose effects the exit state lacks (it
  // returned before a later reinitialisation) must not claim more than the
  // union does.
  bool conditional = false;
  for (const auto &[outcome, effects] : inferred.outcomes) {
    for (const auto &[path, effect] : effects) {
      inferred.addEffect(path, effect);
      if (effect.consumed() && !inferred.consumesUnconditionally(path))
        conditional = true;
    }
  }
  // A class on whose every return some caller memory is null, or holds no
  // resource this function stored there, keeps the classes too (RFC 0007,
  // *Per-outcome null stores*).
  std::set<core::SummaryPath> freshDests;
  for (const core::Store &store : inferred.stores) {
    if (store.value.kind == core::ValueSource::Kind::Fresh)
      freshDests.insert(store.dest);
  }
  for (const auto &[outcome, nulls] : nullAtReturn) {
    std::set<core::SummaryPath> paths = nulls.null;
    for (const core::SummaryPath &dest : freshDests) {
      if (!nulls.held.contains(dest))
        paths.insert(dest);
    }
    if (paths.empty())
      continue;
    inferred.addOutcome(outcome);
    inferred.nullOn[outcome] = std::move(paths);
    conditional = true;
  }
  // Classes only matter when some consumption depends on them; a summary
  // without conditional consumption stays as small as an RFC 0003 one.
  if (!conditional)
    inferred.outcomes.clear();
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
