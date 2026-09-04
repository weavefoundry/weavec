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
      if (const auto *call = dyn_cast<CallExpr>(&stripped);
          call != nullptr && call->getType()->isPointerType())
        dereferencedCalls.insert(call);
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

/// True if some alternative of `origin` is a fresh allocation.
static bool mayBeFresh(const ValueOrigin &origin) {
  if (origin.kind == ValueOrigin::Kind::Alloc)
    return true;
  return origin.kind == ValueOrigin::Kind::Conditional &&
         llvm::any_of(origin.alternatives, mayBeFresh);
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
      // A block that ends in a call to a function inferred never to return
      // (RFC 0009) reaches no successor, so nothing is live after it: a
      // loan used before `die()` does not outlive the scope it never leaves.
      // Clang already routes a declared `noreturn` call to the exit.
      if (!blockNeverReturns(*block)) {
        for (const CFGBlock::AdjacentBlock &adjacent : block->succs()) {
          if (const CFGBlock *succ = adjacent.getReachableBlock())
            live |= liveIn[succ->getBlockID()];
        }
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

void FunctionDataflow::forgetNullnessReachable(const ValueOrigin &origin,
                                               core::AnalysisState &state) {
  // Both the RFC 0008 fact and the RFC 0007 must-null flag (which
  // `nullnessAt` reads as `Null` too). A must-null place holds no resource
  // record, so forgetting it in the resource tracker clears the flag alone.
  const auto drop = [&state](core::PlaceId place) {
    state.nulls.forget(place);
    if (state.resources.isNull(place))
      state.resources.forget(place);
    // Integer facts about the memory too (RFC 0009): the callee may have
    // written any value there.
    state.scalars.forget(place);
    state.dropGuardsOn(place);
  };
  const auto dropBelow = [this, &drop](core::PlaceId place) {
    for (const core::PlaceId child : places.descendants(place))
      drop(child);
  };
  switch (origin.kind) {
  case ValueOrigin::Kind::Conditional:
    for (const ValueOrigin &alternative : origin.alternatives)
      forgetNullnessReachable(alternative, state);
    break;
  case ValueOrigin::Kind::Copy:
    // `f(p)`: the callee holds a copy of the pointer, so `p` itself is what
    // it was, but it may have written anything `p` reaches.
    if (origin.place)
      dropBelow(origin.place->place);
    break;
  case ValueOrigin::Kind::Borrow:
    // `f(&s)`: `s` and everything below it may have been written
    // (linenoise's `linenoiseCompletions lc = {0, NULL}; callback(buf,
    // &lc); ... lc.cvec[i]`).
    if (origin.place) {
      drop(origin.place->place);
      dropBelow(origin.place->place);
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
  // make this say "not lost". An escaped alias says the *resource* escaped
  // (`s->next = s->out; keep(s->out); s->next = ...` hands the one
  // allocation to `keep`), so nothing is lost. A move record on an `a[*]`
  // alias releases the resource only if it names the element the alias does
  // (`free(a[0]); a[1] = p;` leaves `a[1]` holding `p`; RFC 0006, *Element
  // witnesses*).
  return llvm::all_of(state.aliases.edgesFrom(place), [&](const auto &entry) {
    const auto &[alias, edge] = entry;
    if (alias == place)
      return true;
    if (state.resources.isEscaped(alias))
      return false;
    if (dying(alias))
      return true;
    const auto moved = state.moves.recordOf(alias);
    return moved && moved->element.matches(edge.element);
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
  if (index == 0 || state.resources.empty() || blockNeverReturns(block))
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
  if (state.resources.empty() || blockNeverReturns(block) ||
      &block == &cfg->getExit() ||
      (successor != nullptr && blockNeverReturns(*successor)))
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
    // The freed expression need not have pointer type: `free(static_array)`
    // decays an array (RFC 0008; reported as `invalid-release`).
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(pointer));
        decl != nullptr && decl->getType()->isPointerType()) {
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

bool FunctionDataflow::isStorageOfVariable(core::PlaceId place) const {
  if (places.innermostDeref(place))
    return false;
  const core::PlaceId root = places.root(place);
  return builder.varForPlace(root) != nullptr || builder.isLiteralPlace(root);
}

void FunctionDataflow::markInterior(core::PlaceId place,
                                    core::AnalysisState &state) {
  const auto record = state.resources.recordOf(place);
  if (!record || record->interior)
    return;
  core::ResourceRecord updated = *record;
  updated.interior = true;
  state.resources.hold(place, updated);
}

void FunctionDataflow::checkInvalidRelease(const Expr &argument,
                                           const std::optional<PlaceRef> &ref,
                                           core::MoveReason reason,
                                           const Expr &at,
                                           const core::AnalysisState &state) {
  if (!recording())
    return;
  const std::string verb =
      reason == core::MoveReason::Freed ? "released" : "passed as owned";
  // `buf[*]` from array decay is the array's storage: name the array.
  const auto shownStorage = [this](core::PlaceId storage) {
    return places.isBase(storage) ||
                   places.step(storage) != core::PathStep::Index
               ? storage
               : *places.parent(storage);
  };
  const auto reportStorage = [&](const std::string &subject,
                                 core::PlaceId storage) {
    const core::PlaceId root = places.root(storage);
    if (builder.isLiteralPlace(root)) {
      report(makeError(core::diag::InvalidRelease,
                       "'" + subject + "' is " + verb +
                           " but points to a string literal",
                       at));
      return;
    }
    const std::string shown = nameOf(shownStorage(storage));
    core::Diagnostic diagnostic =
        makeError(core::diag::InvalidRelease,
                  "'" + subject + "' is " + verb + " but points to '" + shown +
                      "', which is not a heap object",
                  at);
    if (const VarDecl *var = builder.varForPlace(root))
      diagnostic.addNote("'" + shown + "' is declared here",
                         locate(var->getLocation()));
    report(std::move(diagnostic));
  };
  const auto reportInterior = [&](const std::string &subject,
                                  const core::ResourceRecord &record) {
    core::Diagnostic diagnostic =
        makeError(core::diag::InvalidRelease,
                  "'" + subject + "' is " + verb +
                      " but does not point to the start of its allocation",
                  at);
    if (record.location.isValid())
      diagnostic.addNote("allocated here", record.location);
    report(std::move(diagnostic));
  };

  const ValueOrigin origin = builder.classifyValue(argument);
  // `free(buf)`, `free(&x)`, `free(&x.d)`, `free("abc")`: the argument is the
  // storage itself.
  if (origin.kind == ValueOrigin::Kind::Borrow && origin.place) {
    const core::PlaceId storage = origin.place->place;
    if (!isStorageOfVariable(storage))
      return;
    const core::PlaceId root = places.root(storage);
    if (builder.isLiteralPlace(root)) {
      report(makeError(core::diag::InvalidRelease,
                       "a string literal is " + verb, at));
      return;
    }
    const core::PlaceId shown = shownStorage(storage);
    core::Diagnostic diagnostic = makeError(
        core::diag::InvalidRelease,
        "'" + nameOf(shown) + "' is " + verb + " but is not a heap object", at);
    if (const VarDecl *var = builder.varForPlace(root))
      diagnostic.addNote("'" + nameOf(shown) + "' is declared here",
                         locate(var->getLocation()));
    report(std::move(diagnostic));
    return;
  }
  if (!ref || origin.kind != ValueOrigin::Kind::Copy || !origin.place)
    return;
  const core::PlaceId place = ref->place;
  // A place already dead is reported as a double free or use-after-move.
  if (findMoved(place, state, ref->element))
    return;
  // Annotated borrows are RFC 0003's `annotation-mismatch`.
  if (borrowedParamFor(place, state))
    return;
  const std::string subject = nameOf(place);
  // 1, 2: the place holds a loan on a variable's storage or a literal.
  for (const core::Loan &loan : state.loans.heldBy(place)) {
    if (isStorageOfVariable(loan.place)) {
      reportStorage(subject, loan.place);
      return;
    }
  }
  // 3: an offset into an allocation this function knows. `free(s - k)`
  // where `s` is itself interior is the idiom for reaching the start.
  const auto record = state.resources.recordOf(place);
  if (!record)
    return;
  if (origin.interior != record->interior)
    reportInterior(subject, *record);
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
    if (++visits[block->getBlockID()] > MaxVisitsPerBlock) {
      convergenceFailed = true;
      continue;
    }

    core::AnalysisState out = *entryStates[block->getBlockID()];
    transfer(*block, out);
    // A call to a function that never returns ends the path here (RFC 0009,
    // *Inferred `noreturn`*), as a declared `noreturn` does through the CFG.
    if (blockTerminated)
      continue;

    const auto propagate = [&](unsigned succIndex, const CFGBlock &succ,
                               core::AnalysisState edgeState) {
      leaveBlock(*block, succIndex, edgeState);
      // RFC 0009: an edge the state's facts contradict is dead.
      if (edgeInfeasible)
        return;
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
    // exits the function sees the report of everything left (RFC 0007). A
    // block that never hands control back has no edges to check.
    if (blockTerminated || state.resources.empty())
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
  blockTerminated = false;
  for (std::size_t index = 0; index < block.size() && !blockTerminated;
       ++index) {
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

bool FunctionDataflow::blockNeverReturns(const CFGBlock &block) {
  if (block.hasNoReturnElement())
    return true;
  if (neverReturnsCache.size() < cfg->getNumBlockIDs())
    neverReturnsCache.resize(cfg->getNumBlockIDs());
  std::optional<bool> &cached = neverReturnsCache[block.getBlockID()];
  if (cached)
    return *cached;
  // A call to a function inferred never to return (RFC 0009) ends the block
  // as a declared `noreturn` does; the summaries are fixed for this run.
  bool result = false;
  for (const CFGElement &element : block) {
    const auto stmtElement = element.getAs<CFGStmt>();
    if (!stmtElement)
      continue;
    const auto *call = dyn_cast_or_null<CallExpr>(stmtElement->getStmt());
    if (call == nullptr)
      continue;
    const auto effects = classifyCall(*call, summaries);
    if (effects && effects->summary->neverReturns) {
      result = true;
      break;
    }
  }
  cached = result;
  return result;
}

void FunctionDataflow::leaveBlock(const CFGBlock &from, unsigned succIndex,
                                  core::AnalysisState &state) {
  // The condition first: on the null edge of `if (q)` the result owns
  // nothing, and on the other a retracted consumption may hand it back to
  // the argument (RFC 0007, *Acquiring and losing a resource*).
  applyEdge(from, succIndex, state);
  // No real path takes an edge the facts contradict: nothing dies on it.
  if (edgeInfeasible)
    return;
  const CFGBlock *successor = nullptr;
  if (succIndex < from.succ_size())
    successor = (*std::next(from.succ_begin(), succIndex)).getReachableBlock();
  checkBlockEndResources(from, successor, state);
}

// -- Condition facts (RFC 0006) -----------------------------------------------

/// Whether some value in `[lo, hi]` satisfies `v OP k`.
template <typename Int>
static bool rangeSatisfies(BinaryOperatorKind op, Int lo, Int hi, Int k) {
  switch (op) {
  case BO_LT:
    return lo < k;
  case BO_GT:
    return hi > k;
  case BO_LE:
    return lo <= k;
  case BO_GE:
    return hi >= k;
  case BO_EQ:
    return lo <= k && k <= hi;
  case BO_NE:
    return lo != k || hi != k;
  default:
    return false;
  }
}

/// The outcome classes of an integer result that satisfy (`holds`) or
/// falsify (`!holds`) `x OP k` (RFC 0006, *Outcome tests*; RFC 0009,
/// *Assumptions*): a class is selected when some value in its range does.
/// `k` is a mathematical value (`integerConstant`), so it is non-negative
/// when the comparison is unsigned. The comparison is decided in the
/// operands' common type; when that is unsigned of `width` bits, a negative
/// `x` (a signed operand converted up) takes part as `x + 2^width`, above
/// every non-negative value.
static std::set<core::Outcome> classesSatisfying(BinaryOperatorKind op,
                                                 std::int64_t k, bool holds,
                                                 bool unsignedComparison,
                                                 unsigned width) {
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
  if (unsignedComparison) {
    // Unsigned order: zero, then the positives, then the negatives as the
    // top half of the type's range, `2^(N-1) .. 2^N - 1`.
    const unsigned n = std::clamp(width, 1U, 64U);
    const std::uint64_t top = n == 64
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : (std::uint64_t{1} << n) - 1;
    const std::uint64_t half = std::uint64_t{1} << (n - 1);
    const auto ku = static_cast<std::uint64_t>(k);
    if (rangeSatisfies<std::uint64_t>(op, 0, 0, ku))
      result.insert(core::Outcome::Zero);
    if (rangeSatisfies<std::uint64_t>(op, 1, top, ku))
      result.insert(core::Outcome::Positive);
    if (rangeSatisfies<std::uint64_t>(op, half, top, ku))
      result.insert(core::Outcome::Negative);
    return result;
  }
  constexpr std::int64_t Min = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t Max = std::numeric_limits<std::int64_t>::max();
  if (rangeSatisfies<std::int64_t>(op, Min, -1, k))
    result.insert(core::Outcome::Negative);
  if (rangeSatisfies<std::int64_t>(op, 0, 0, k))
    result.insert(core::Outcome::Zero);
  if (rangeSatisfies<std::int64_t>(op, 1, Max, k))
    result.insert(core::Outcome::Positive);
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
  if (expr.isNullPointerConstant(context, Expr::NPC_ValueDependentIsNull) !=
      Expr::NPCK_NotNull)
    return true;
  // `(char *)0` is not a null pointer constant in ISO C's sense (only
  // `(void *)0` is), but Clang converts it with a null-to-pointer cast all
  // the same, and `classifyValue` already reads such a cast as `Null`
  // (zlib's `buf != (charf *)0`).
  for (const Expr *e = expr.IgnoreParens();
       const auto *cast = dyn_cast<CastExpr>(e);
       e = cast->getSubExpr()->IgnoreParens()) {
    if (cast->getCastKind() == CK_NullToPointer)
      return true;
  }
  return false;
}

void FunctionDataflow::applyEdge(const CFGBlock &from, unsigned succIndex,
                                 core::AnalysisState &state) {
  edgeInfeasible = false;
  if (const auto *statement =
          dyn_cast_or_null<SwitchStmt>(from.getTerminatorStmt())) {
    if (succIndex >= from.succ_size())
      return;
    if (const CFGBlock *to =
            (*std::next(from.succ_begin(), succIndex)).getReachableBlock())
      applySwitchEdge(*statement, *to, state);
    return;
  }
  if (from.succ_size() != 2)
    return;
  const auto *condition = dyn_cast_or_null<Expr>(from.getTerminatorCondition());
  if (condition == nullptr)
    return;

  // Successor 0 is the edge taken when the condition holds.
  applyCondition(*condition, succIndex == 0, /*wrapped=*/false, state);
}

void FunctionDataflow::applySwitchEdge(const SwitchStmt &statement,
                                       const CFGBlock &to,
                                       core::AnalysisState &state) {
  const Expr *scrutinee = statement.getCond();
  if (scrutinee == nullptr || !scrutinee->getType()->isIntegerType())
    return;
  // Each `case` label heads a block of its own (an empty one falls through
  // to the next), so the edge into `to` selects the label it carries; the
  // `default` edge, or the one out of a switch without one, says the value
  // is none of the labels.
  // A label is converted to the scrutinee's promoted type (`case -1:` on an
  // `unsigned` selects `UINT_MAX`); one that type does not hold within an
  // `int64_t` decides nothing.
  const auto labelValue =
      [&](const Expr &label) -> std::optional<std::int64_t> {
    const auto value = integerConstant(label, context);
    if (!value)
      return std::nullopt;
    return integerConvertedTo(*value, scrutinee->getType(), context);
  };
  const Stmt *label = to.getLabel();
  if (const auto *caseLabel = dyn_cast_or_null<CaseStmt>(label)) {
    const auto lo = labelValue(*caseLabel->getLHS());
    if (!lo)
      return;
    if (caseLabel->getRHS() == nullptr) {
      applyOutcomeTest(*scrutinee, {core::ValueFact::classOf(*lo)}, state, lo);
      return;
    }
    const auto hi = labelValue(*caseLabel->getRHS());
    if (!hi || *hi < *lo)
      return;
    std::set<core::Outcome> classes;
    if (*lo < 0)
      classes.insert(core::Outcome::Negative);
    if (*lo <= 0 && 0 <= *hi)
      classes.insert(core::Outcome::Zero);
    if (*hi > 0)
      classes.insert(core::Outcome::Positive);
    applyOutcomeTest(*scrutinee, classes, state);
    return;
  }
  if (label != nullptr && !isa<DefaultStmt>(label))
    return;
  // Only the zero class can be covered by labels: `case 0:` somewhere means
  // the default edge carries a non-zero value.
  for (const SwitchCase *sc = statement.getSwitchCaseList(); sc != nullptr;
       sc = sc->getNextSwitchCase()) {
    const auto *caseLabel = dyn_cast<CaseStmt>(sc);
    if (caseLabel == nullptr)
      continue;
    const auto lo = labelValue(*caseLabel->getLHS());
    if (!lo)
      continue;
    std::optional<std::int64_t> hi = lo;
    if (caseLabel->getRHS() != nullptr)
      hi = labelValue(*caseLabel->getRHS());
    if (hi && *lo <= 0 && 0 <= *hi) {
      applyOutcomeTest(*scrutinee,
                       {core::Outcome::Positive, core::Outcome::Negative},
                       state);
      return;
    }
  }
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
      const auto assigned = [](const Expr &operand) -> const Expr * {
        const Expr *stripped = operand.IgnoreParenCasts();
        if (const auto *assign = dyn_cast<BinaryOperator>(stripped);
            assign != nullptr && assign->getOpcode() == BO_Assign)
          return assign->getLHS();
        return &operand;
      };
      // `buf != stack`, `p != &x`: on the unequal edge `buf` does not point
      // at that storage, so releasing it there is not an invalid release
      // (RFC 0008, *Invalid releases*: the stack-or-heap buffer idiom).
      if ((op == BO_NE) == holds) {
        const auto refute = [this, &state, &assigned](const Expr &pointer,
                                                      const Expr &storage) {
          const ValueOrigin borrow = builder.classifyValue(storage);
          if (borrow.kind != ValueOrigin::Kind::Borrow || !borrow.place ||
              !isStorageOfVariable(borrow.place->place))
            return false;
          const auto held = builder.resolvePointerValue(*assigned(pointer));
          if (!held)
            return true;
          for (const core::PlaceId holder : mirrors(held->place, state))
            state.loans.drop(holder, borrow.place->place);
          return true;
        };
        if (refute(lhs, rhs) || refute(rhs, lhs))
          return;
      }
      // `p == q`: on the equal edge the two places hold the same value; on
      // the other they do not, which refutes an exact alias. `(p = f()) ==
      // q` compares what was just stored in `p`.
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

    // `x OP k`, `k OP x` on an integer result. The edge on which `x == k`
    // holds (or `x != k` fails) knows the value exactly (RFC 0009).
    if (lhs.getType()->isIntegerType() && rhs.getType()->isIntegerType()) {
      // Both operands have the comparison's type (the usual arithmetic
      // conversions); `k` is read in it, so `x > ULONG_MAX` has no `k` and
      // decides nothing, and `-1u` is `UINT_MAX`.
      const bool unsignedComparison = lhs.getType()->isUnsignedIntegerType();
      const unsigned width =
          std::clamp(context.getIntWidth(lhs.getType()), 1U, 64U);
      const bool equal = (op == BO_EQ && holds) || (op == BO_NE && !holds);
      const auto test = [&](const Expr &x, BinaryOperatorKind xOp,
                            std::int64_t k) {
        // The value is known exactly on the equal edge unless a negative
        // `x` may have been converted to `k` (a signed operand of an
        // unsigned comparison with `k` in the top half of the range).
        const bool signedOperand =
            !x.IgnoreParenImpCasts()->getType()->isUnsignedIntegerType();
        const bool topHalf =
            unsignedComparison &&
            static_cast<std::uint64_t>(k) >= (std::uint64_t{1} << (width - 1));
        const bool exact = equal && !(signedOperand && topHalf);
        applyOutcomeTest(
            x, classesSatisfying(xOp, k, holds, unsignedComparison, width),
            state, exact ? std::optional(k) : std::nullopt);
      };
      if (const auto k = integerConstant(rhs, context))
        test(lhs, op, *k);
      else if (const auto flipped = integerConstant(lhs, context))
        test(rhs, flipComparison(op), *flipped);
    }
    return;
  }

  // `x` alone: non-null / non-zero on the true edge.
  if (e->getType()->isPointerType()) {
    applyOutcomeTest(*e, {holds ? core::Outcome::NonNull : core::Outcome::Null},
                     state);
  } else if (e->getType()->isIntegerType()) {
    // `!x` is `x == 0`: the zero class is one value, which the fact records
    // so that `switch (x) case 0` and `if (!x)` agree.
    if (holds)
      applyOutcomeTest(*e, {core::Outcome::Positive, core::Outcome::Negative},
                       state);
    else
      applyOutcomeTest(*e, {core::Outcome::Zero}, state, 0);
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
  for (const core::PlaceId place : narrowed.nullInAll()) {
    // `if (grow(l, n) == -1)`: on the failing class the callee stored no
    // buffer, and RFC 0007's relaxation says so through `nullOn`. That
    // retracts the record the store gave; it is not a nullness fact (the
    // place holds whatever it held before), and marking it must-null would
    // make it `Null` for `nullnessAt` and a `null` source for `sourceOf`
    // (RFC 0008, *Implementation notes*).
    if (llvm::is_contained(narrowed.unheldOnly, place)) {
      state.resources.forget(place);
      for (const auto &[alias, edge] : state.aliases.edgesFrom(place)) {
        if (edge.exact)
          state.resources.forget(alias);
      }
      continue;
    }
    markNullWithCopies(place, state);
    setNullness(place,
                core::NullRecord{.state = core::Nullness::Null,
                                 .location = narrowed.location,
                                 .reason = core::NullReason::CalleeStore,
                                 .detail = narrowed.callee},
                state);
  }
  // `if (!make(&p)) return -1; p[0]`: on the classes the caller kept, the
  // callee left the place non-null (RFC 0008, *Per-outcome non-null facts*).
  for (const core::PlaceId place : narrowed.nonNullInAll()) {
    setNullness(place,
                core::NullRecord{.state = core::Nullness::NonNull,
                                 .location = narrowed.location,
                                 .reason = core::NullReason::CalleeStore,
                                 .detail = narrowed.callee},
                state);
  }
}

void FunctionDataflow::applyOutcomeTest(const Expr &operand,
                                        const std::set<core::Outcome> &selected,
                                        core::AnalysisState &state,
                                        std::optional<std::int64_t> constant) {
  if (selected.empty())
    return;
  const Expr *e = operand.IgnoreParenCasts();
  // `(r = f(p)) < 0` tests what was just stored in `r`.
  if (const auto *assign = dyn_cast<BinaryOperator>(e);
      assign != nullptr && assign->getOpcode() == BO_Assign)
    e = assign->getLHS()->IgnoreParenCasts();

  // RFC 0009, *Scalar facts in the state*: an integer test narrows what is
  // known about the tested place, exactly when the edge says `== k`, and
  // whatever it learns refutes the guards that contradict it. A scaled or
  // converted read (`n * 8 > 0`, `(size_t)n == 0`) tests the place's class.
  const auto narrowScalar = [this, &selected, &constant,
                             &state](const PlaceBuilder::ScalarOperand &read) {
    if (!read.place || !read.place->element.isWhole() ||
        !tracksScalar(read.place->place))
      return;
    core::ValueFact fact;
    for (const core::Outcome outcome : selected) {
      if (outcome != core::Outcome::Null && outcome != core::Outcome::NonNull)
        fact.classes.insert(outcome);
    }
    if (fact.classes.empty())
      return;
    if (constant && !read.scaled && fact.classes.size() == 1 &&
        *fact.classes.begin() == core::ValueFact::classOf(*constant))
      fact.constant = constant;
    if (fact.trivial())
      return;
    // An edge the facts contradict is one no path takes (`int c = 0; if
    // (c) free(p);`): its state reaches nobody. Only a variable's own
    // storage is trusted that far; a fact about memory behind a pointer may
    // be stale under an alias the model does not know (RFC 0009, *Bugs
    // deliberately not caught*), so such an edge keeps flowing.
    if (state.scalars.narrow(read.place->place, fact) ==
        core::GuardRefinement::Refuted) {
      if (!places.innermostDeref(read.place->place))
        edgeInfeasible = true;
      return;
    }
    learnFact(read.place->place, fact, state);
  };
  if (!PlaceBuilder::isPlaceExpr(*e) && !isa<CallExpr>(e) &&
      e->getType()->isIntegerType()) {
    narrowScalar(builder.scalarOperand(*e));
    return;
  }

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
  if (e->getType()->isIntegerType()) {
    narrowScalar(PlaceBuilder::ScalarOperand{
        .place = ref, .constant = std::nullopt, .scaled = false});
  }
  // On the edge where the holder is null it owns nothing (RFC 0007, *Null*):
  // `p = malloc(n); if (!p) return -1;` is not a leak. Exact copies hold the
  // same null.
  if (selected == std::set<core::Outcome>{core::Outcome::Null}) {
    markNullWithCopies(ref->place, state);
    forgetBelowNull(ref->place, state);
  }
  // The test decides the pointer's nullness on each edge (RFC 0008,
  // *Nullness*); after the edges merge again it may be null. A place already
  // known non-null keeps that fact on the null edge: the edge is infeasible
  // (`if (!b) return; while (b != NULL && b->n < k) ...`: cJSON's
  // `can_access_at_index` retests on every use), and letting it say `Null`
  // would make the pointer maybe-null once the edges merge (RFC 0008,
  // *Implementation notes*).
  if (ref->element.isWhole() &&
      (selected == std::set<core::Outcome>{core::Outcome::Null} ||
       selected == std::set<core::Outcome>{core::Outcome::NonNull})) {
    const bool selectsNull = selected.contains(core::Outcome::Null);
    const auto known = state.nulls.recordOf(ref->place);
    const bool contradicted =
        selectsNull && known && known->state == core::Nullness::NonNull;
    if (!contradicted) {
      setNullness(ref->place,
                  core::NullRecord{.state = selectsNull
                                                ? core::Nullness::Null
                                                : core::Nullness::NonNull,
                                   .location = locate(*e),
                                   .reason = core::NullReason::Tested,
                                   .detail = {}},
                  state);
      // The guards that spoke about the pointer's nullness are decided (RFC
      // 0009, *Refuting guards in the state*).
      learnFact(ref->place,
                core::ValueFact::of(selectsNull ? core::Outcome::Null
                                                : core::Outcome::NonNull),
                state);
    }
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

// -- Scalar facts and guards (RFC 0009) ---------------------------------------

bool FunctionDataflow::tracksScalar(core::PlaceId place) const {
  // Index places stand for every element, and no single value.
  for (std::optional<core::PlaceId> current = place; current;
       current = places.parent(*current)) {
    if (places.isBase(*current))
      break;
    const core::PathStep step = places.step(*current);
    if (step == core::PathStep::Index)
      return false;
    // Below a dereference: caller memory or a heap object, written only
    // through pointers the model follows (`written` effects forget it).
    if (step == core::PathStep::Deref)
      return true;
  }
  // The storage of a local or parameter. Its address may be taken: a callee
  // that writes through it reports `written` (or, unknown, forgets what it
  // reaches), and a write through a pointer this function holds is applied
  // to what the pointer borrows (`assignScalar`); the nullness tracker
  // makes the same bet (RFC 0008). A global may be written by any callee,
  // which summaries do not report for integers.
  const VarDecl *var = builder.varForPlace(places.root(place));
  return var != nullptr && !var->hasGlobalStorage();
}

void FunctionDataflow::learnFact(core::PlaceId place,
                                 const core::ValueFact &fact,
                                 core::AnalysisState &state) {
  std::vector<core::PlaceId> holders{place};
  for (const auto &[alias, edge] : state.aliases.edgesFrom(place)) {
    if (edge.exact)
      holders.push_back(alias);
  }
  for (const core::PlaceId holder : holders) {
    const core::AnalysisState::Learned learned = state.learn(holder, fact);
    // A reinstated move takes the flow-sensitive consumption it fed with it
    // (RFC 0006, *Inference*), as a retracted pending outcome does.
    for (const core::PlaceId reinstated : learned.reinstated) {
      if (const auto path = builder.summaryPathOf(reinstated))
        state.consumed.erase(*path);
    }
  }
}

std::optional<core::ValueFact>
FunctionDataflow::scalarFactOf(const Expr &expr,
                               const core::AnalysisState &state) {
  const PlaceBuilder::ScalarOperand operand = builder.scalarOperand(expr);
  if (operand.constant)
    return operand.constant;
  if (!operand.place || !operand.place->element.isWhole())
    return std::nullopt;
  auto fact = state.scalars.factOf(operand.place->place);
  if (fact && operand.scaled)
    fact->constant.reset();
  return fact;
}

void FunctionDataflow::assignScalar(core::PlaceId place, const Expr *value,
                                    core::AnalysisState &state) {
  // The old value is gone under every name of the cell (RFC 0009, *Scalar
  // facts in the state*: guards speak about the value that was tested).
  std::vector<core::PlaceId> cells = mirrors(place, state);
  if (!llvm::is_contained(cells, place))
    cells.push_back(place);
  // `*q = 1` or `q->n = 1` where `q` borrows a local: the local's storage is
  // what changed, whatever name it was written under.
  for (const core::PlaceId image : borrowedImages(place, state)) {
    if (!llvm::is_contained(cells, image))
      cells.push_back(image);
  }
  std::optional<core::ValueFact> fact;
  if (value != nullptr && tracksScalar(place))
    fact = scalarFactOf(*value, state);
  for (const core::PlaceId cell : cells) {
    state.dropGuardsOn(cell);
    if (fact && tracksScalar(cell))
      state.scalars.set(cell, *fact);
    else
      state.scalars.forget(cell);
    if (const auto path = builder.summaryPathOf(cell))
      writtenScalarPaths.insert(*path);
  }
}

void FunctionDataflow::forgetScalar(core::PlaceId place,
                                    core::AnalysisState &state) {
  assignScalar(place, nullptr, state);
}

std::vector<core::PlaceId>
FunctionDataflow::borrowedImages(core::PlaceId place,
                                 const core::AnalysisState &state) {
  // `place` is `q->a.b`: for every loan `q` holds on some storage `t`, the
  // image is `t.a.b`. Only the innermost dereference is followed; a pointer
  // read from memory (`q->next->n`) has no loans of its own.
  const auto deref = places.innermostDeref(place);
  if (!deref)
    return {};
  const auto pointer = places.parent(*deref);
  if (!pointer)
    return {};
  std::vector<core::Loan> loans = state.loans.heldBy(*pointer);
  if (loans.empty())
    return {};
  // The steps from the dereference down to `place`, outermost last.
  llvm::SmallVector<core::PlaceId, 4> steps;
  for (core::PlaceId current = place; current != *deref;
       current = *places.parent(current))
    steps.push_back(current);
  std::vector<core::PlaceId> images;
  for (const core::Loan &loan : loans) {
    core::PlaceId image = loan.place;
    for (const core::PlaceId step : llvm::reverse(steps)) {
      switch (places.step(step)) {
      case core::PathStep::Field:
        image = places.field(image, places.fieldName(step));
        break;
      case core::PathStep::Index:
        image = places.index(image);
        break;
      case core::PathStep::Deref:
        image = places.deref(image);
        break;
      }
    }
    if (!llvm::is_contained(images, image))
      images.push_back(image);
  }
  return images;
}

core::PlaceGuard
FunctionDataflow::guardHere(const core::AnalysisState &state,
                            std::optional<core::PlaceId> exclude) {
  core::PlaceGuard guard = state.pathGuard();
  if (!exclude)
    return guard;
  guard.drop(*exclude);
  for (const auto &[alias, edge] : state.aliases.edgesFrom(*exclude)) {
    if (edge.exact)
      guard.drop(alias);
  }
  return guard;
}

core::PathGuard
FunctionDataflow::summaryGuardOf(const core::PlaceGuard &guard) {
  core::PathGuard result;
  for (const auto &[place, fact] : guard.conditions) {
    // A parameter variable that is reassigned no longer holds the argument
    // (`stableSummaryPathOf`); a local names nothing the caller knows.
    if (const auto path = stableSummaryPathOf(place))
      result.require(*path, fact);
  }
  return result;
}

bool FunctionDataflow::pruneGuard(core::PlaceGuard &guard,
                                  const core::AnalysisState &state) {
  for (auto it = guard.conditions.begin(); it != guard.conditions.end();) {
    const auto known = state.factOf(it->first);
    if (!known) {
      ++it;
      continue;
    }
    if (known->disjointFrom(it->second))
      return false;
    if (known->implies(it->second)) {
      it = guard.conditions.erase(it);
      continue;
    }
    ++it;
  }
  return true;
}

bool FunctionDataflow::pruneOrigin(ValueOrigin &origin,
                                   const core::AnalysisState &state) {
  if (!pruneGuard(origin.guard, state))
    return false;
  if (origin.kind != ValueOrigin::Kind::Conditional)
    return true;
  std::erase_if(origin.alternatives, [&state, this](ValueOrigin &alternative) {
    return !pruneOrigin(alternative, state);
  });
  if (origin.alternatives.empty())
    return false;
  if (origin.alternatives.size() == 1) {
    // The survivor is the value, under what is left of both guards.
    ValueOrigin survivor = std::move(origin.alternatives.front());
    if (survivor.call == nullptr)
      survivor.call = origin.call;
    for (const auto &[key, fact] : origin.guard.conditions)
      survivor.guard.require(key, fact);
    origin = std::move(survivor);
  }
  return true;
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
      // so nothing about it changes, except that `p` no longer points at
      // the start of what it owns (RFC 0008, *Invalid releases*).
      doRead(*ref, expr, state, /*includeSelf=*/true);
      doMutationCheck(ref->place, expr, state);
      checkAnnotationOnWrite(*ref, expr, state);
      recordAccess(ref->place, /*write=*/true, state);
      noteVariableWrite(ref->place, state);
      if (expr.getType()->isPointerType() && ref->element.isWhole())
        markInterior(ref->place, state);
      // `n++`, `n += k`: whatever was known of `n` is gone (RFC 0009).
      if (expr.getType()->isIntegerType())
        forgetScalar(ref->place, state);
      break;
    case Role::Write:
    case Role::AddressOf:
      doRead(*ref, expr, state, /*includeSelf=*/false);
      noteVariableWrite(ref->place, state);
      // `&n`: written through the pointer from now on, unseen (RFC 0009).
      if (role == Role::AddressOf && expr.getType()->isIntegerType())
        forgetScalar(ref->place, state);
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
  if (const auto *call = dyn_cast<CallExpr>(&expr)) {
    handleCall(*call, state);
    if (dereferencedCalls.contains(call))
      checkResultDereference(*call, state);
  }
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
      if (init != nullptr && var->getType()->isRecordType()) {
        copyRecord(place, *init, state);
      } else if (init == nullptr && var->getType()->isRecordType() &&
                 isUninitializedLocal(*var)) {
        markUninitializedFields(place, *var->getType()->getAsRecordDecl(),
                                locate(var->getLocation()), state);
      } else {
        // `int n = 0;`: what the initialiser says about the value (RFC 0009).
        if (var->getType()->isIntegerType())
          assignScalar(place, init, state);
        attachOutcome(place, init, state);
      }
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
      // RFC 0008, *Uninitialised pointers*: the place holds garbage until an
      // assignment reaches it, which the move machinery tracks.
      if (isUninitializedLocal(*var))
        state.moves.markMoved(place, core::MoveReason::Uninitialized,
                              locate(var->getLocation()));
      continue;
    }
    applyPointerAssign(place, builder.classifyValue(*init), *init,
                       var->getType()->getPointeeType().isConstQualified(),
                       state);
    attachOutcome(place, init, state);
  }
}

bool FunctionDataflow::isUninitializedLocal(const VarDecl &var) const {
  if (!var.isLocalVarDecl() || var.isStaticLocal() ||
      var.hasExternalStorage() || var.getInit() != nullptr)
    return false;
  // `va_list ap;` is a `char *` on some targets and is initialised by
  // `va_start`/`va_copy`, which take it by value as far as the AST shows.
  // Only the spelling tells it from a `char *`: walk the typedef chain.
  for (QualType type = var.getType();;) {
    const auto *typedefType = type->getAs<TypedefType>();
    if (typedefType == nullptr)
      break;
    if (typedefType->getDecl()->getCanonicalDecl() ==
        context.getBuiltinVaListDecl()->getCanonicalDecl())
      return false;
    type = typedefType->desugar();
  }
  // A local whose address is taken may be written through the pointer at
  // any time (RFC 0008, *Deliberately not caught*).
  return !addressTaken.contains(var.getCanonicalDecl());
}

void FunctionDataflow::markUninitializedFields(
    core::PlaceId place, const RecordDecl &record,
    const core::SourceLocation &declared, core::AnalysisState &state) {
  if (record.isUnion() || !record.isCompleteDefinition() ||
      places.depth(place) >= MaxPlaceDepth)
    return;
  for (const FieldDecl *field : record.fields()) {
    const QualType type = field->getType();
    const core::PlaceId fieldPlace = builder.fieldPlace(place, *field);
    if (type->isPointerType()) {
      state.moves.markMoved(fieldPlace, core::MoveReason::Uninitialized,
                            declared);
    } else if (const RecordDecl *nested = type->getAsRecordDecl()) {
      markUninitializedFields(fieldPlace, *nested, declared, state);
    }
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
  if (outcome.places().empty() && outcome.nullOn.empty() &&
      outcome.nonNullOn.empty())
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
    const ValueOrigin origin = builder.classifyValue(*assign.getRHS());
    // This function's own whole write: the caller's value there is gone on
    // this path (RFC 0008, *Replaced values*). Not `p = p + 1`, which keeps
    // the value (RFC 0004, *Pointer identity*); and not a callee's store,
    // which may have happened on some path only.
    const bool sameValue = origin.kind == ValueOrigin::Kind::Copy &&
                           origin.place && origin.place->place == lhs->place;
    if (!sameValue)
      noteRewritten(lhs->place, state);
    if (lhs->element.isWhole() && !sameValue)
      noteOverwritten(lhs->place, state);
    applyPointerAssign(lhs->place, origin, assign,
                       type->getPointeeType().isConstQualified(), state,
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
  if (type->isIntegerType() && lhs->element.isWhole())
    assignScalar(lhs->place, assign.getRHS(), state);
  else if (type->isIntegerType())
    forgetScalar(lhs->place, state);
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
    noteRewritten(dest, state);
    noteOverwritten(dest, state);
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
    // field is overwritten with values nothing is known about, except what
    // the callee's `result` stores say (RFC 0008, *Struct-by-value
    // results*): each is an assignment to the corresponding field.
    reinit(dest, state);
    if (const auto *call = dyn_cast<CallExpr>(source))
      applyResultStores(dest, *call, state);
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

void FunctionDataflow::applyResultStores(core::PlaceId dest,
                                         const CallExpr &call,
                                         core::AnalysisState &state) {
  const auto effects = classifyCall(call, summaries);
  if (!effects)
    return;
  const core::FunctionSummary &summary = *effects->summary;
  std::map<core::SummaryPath, std::vector<core::ValueSource>> byDest;
  for (const core::Store &store : summary.stores) {
    if (store.dest.isResult())
      byDest[store.dest].push_back(store.value);
  }
  for (const auto &[path, values] : byDest) {
    const auto field = builder.resolveBelow(dest, path);
    if (!field)
      continue;
    std::vector<ValueOrigin> alternatives;
    for (const core::ValueSource &value : values) {
      if (auto alternative = builder.originFromSource(value, call, summary))
        alternatives.push_back(std::move(*alternative));
    }
    if (alternatives.empty())
      continue;
    ValueOrigin origin;
    if (alternatives.size() == 1) {
      origin = std::move(alternatives.front());
    } else {
      origin.kind = ValueOrigin::Kind::Conditional;
      origin.alternatives = std::move(alternatives);
    }
    origin.call = &call;
    applyPointerAssign(*field, origin, call, /*constPointee=*/false, state);
    noteCalleeStore(*field, call, state);
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
  // RFC 0009, *Inferred `noreturn`*: the callee never hands control back,
  // so nothing after it in this block runs and its state reaches nobody.
  // Its effects were still applied: they are what happens before the exit.
  if (effects->summary->neverReturns)
    blockTerminated = true;
  // `strdup(s);`: nobody holds the result (RFC 0007, *Discarded results*).
  // Not when the arguments select a result that is not fresh: `l_alloc(ud,
  // p, n, 0)` returns null (RFC 0009, *Return alternatives*).
  if (discardedCalls.contains(&call) && effects->summary->returnsOnlyFresh() &&
      recording()) {
    ValueOrigin origin = builder.classifyValue(call);
    if (!pruneOrigin(origin, state) || !mayBeFresh(origin))
      return;
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

  //    Arguments the callee dereferences unconditionally must be non-null
  //    (RFC 0008, *Requirements*).
  checkRequiredArguments(call, summary, state);

  // 1. Consumption: the arguments themselves, the caller's memory below them
  //    (`free(b->data)` in the callee) and globals, deepest path first so a
  //    caller's copy of a freed field is marked before the object holding
  //    the field is (RFC 0007, *Applying a summary: deepest paths first*).
  struct Consumed {
    core::SummaryPath path;
    PlaceRef ref;
    bool freed;
    bool replaced;
    std::string family;
    core::PlaceGuard guard;
  };
  std::vector<Consumed> consumed;
  for (const auto &[path, effect] : summary.effects) {
    if (!effect.consumed())
      continue;
    //    An argument-conditional consume (RFC 0009, *Applying a guarded
    //    summary*): its guard, on the arguments, decided by what is passed
    //    and known here; refuted, the consume does not happen at this call.
    core::PlaceGuard guard;
    if (!effect.when.trivial()) {
      auto translated = builder.translateGuard(effect.when, call);
      if (!translated || !pruneGuard(*translated, state))
        continue;
      guard = std::move(*translated);
    }
    std::optional<PlaceRef> ref;
    if (path.isParam() && path.isRoot()) {
      if (path.index >= call.getNumArgs())
        continue;
      checkRawArgument(call, path.index,
                       effect.freed ? "releases" : "takes ownership of", state);
      ref = builder.resolvePointerValue(*call.getArg(path.index));
      // What is released, or handed to a parameter the table or an
      // annotation declares owning, must be a heap allocation (RFC 0008,
      // *Invalid releases*). A body that merely stores its argument is
      // covered by `lifetime-too-short`.
      if (effect.freed || !trustSilence)
        checkInvalidRelease(*call.getArg(path.index), ref,
                            effect.freed ? core::MoveReason::Freed
                                         : core::MoveReason::Moved,
                            call, state);
    } else {
      ref = builder.resolveSummaryPath(path, call);
      // The callee freed some element of the caller's array (`free(a[i])`
      // with its own `i`): a consume applied from a summary is *whole* (RFC
      // 0006) unless the summary says which it is not (RFC 0008, *Element
      // consumes*), so two calls in a loop are not a `double-free`.
      if (ref && effect.element && ref->element.isWhole())
        ref->element = core::ElementWitness::unknown();
    }
    if (ref) {
      consumed.push_back(Consumed{.path = path,
                                  .ref = std::move(*ref),
                                  .freed = effect.freed,
                                  .replaced = effect.replaced,
                                  .family = effect.family,
                                  .guard = std::move(guard)});
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
        state, entry.family, library, entry.replaced, entry.guard);
    llvm::append_range(markedHere, marked);
    if (entry.replaced)
      markedHere.push_back(entry.ref.place);
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
  for (const core::PlaceId place : written.places) {
    forgetBelow(place, state);
    // The written place itself: a pointer's new value arrives through the
    // callee's stores, an integer's is simply unknown now (RFC 0009).
    if (const auto *decl =
            dyn_cast_if_present<ValueDecl>(builder.declFor(place));
        decl != nullptr && decl->getType()->isIntegerType())
      forgetScalar(place, state);
  }

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
    //    A store under a guard the arguments refute did not happen (RFC
    //    0009); with none left the destination keeps what it held.
    std::vector<ValueOrigin> alternatives;
    for (const core::ValueSource &value : values) {
      if (auto alternative = builder.originFromSource(value, call, summary))
        alternatives.push_back(std::move(*alternative));
    }
    if (alternatives.empty())
      continue;
    ValueOrigin origin;
    if (alternatives.size() == 1) {
      origin = std::move(alternatives.front());
    } else {
      origin.kind = ValueOrigin::Kind::Conditional;
      origin.alternatives = std::move(alternatives);
    }
    doMutationCheck(ref->place, call, state);
    checkAnnotationOnWrite(*ref, call, state);
    recordAccess(ref->place, /*write=*/true, state);
    // A resource the caller still holds there is the callee's business (RFC
    // 0007, *Deliberately not caught*: a store without a release).
    if (state.resources.holds(ref->place)) {
      escape(ref->place, state);
      for (const core::PlaceId alias : state.aliases.members(ref->place))
        state.resources.escape(alias);
    }
    // The callee released the value and may have left it there (`free(b->
    // data); if (c) b->data = NULL;`): the store does not clear the record
    // (RFC 0008, *Replaced values*).
    std::optional<core::MoveRecord> kept;
    if (const core::PlaceEffect effect = summary.effectOf(dest);
        effect.consumed() && !effect.replaced)
      kept = state.moves.recordOf(ref->place);
    // A `null` among the stored values is the callee's doing (RFC 0008,
    // *Nullness*: `CalleeStore`).
    origin.call = &call;
    applyPointerAssign(ref->place, origin, call, /*constPointee=*/false, state,
                       ref->element);
    noteCalleeStore(ref->place, call, state);
    if (kept) {
      state.moves.markMoved(ref->place, kept->reason, kept->location, kept->via,
                            kept->element, kept->family, kept->ownValue,
                            kept->guard);
    }
  }
}

void FunctionDataflow::noteCalleeStore(core::PlaceId dest, const CallExpr &call,
                                       core::AnalysisState &state) {
  const auto record = state.nulls.recordOf(dest);
  if (!record || !record->mayBeNull() ||
      record->reason != core::NullReason::CalleeResult)
    return;
  core::NullRecord stored = *record;
  stored.reason = core::NullReason::CalleeStore;
  stored.location = locate(call);
  stored.detail = calleeName(call);
  setNullness(dest, stored, state);
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
      // A `fresh` store's destination the callee never stores null into is
      // in `nullOn` only because the store did not happen on that class
      // (RFC 0007, *Per-outcome null stores*): a record to retract, not a
      // value that is null.
      bool storesFresh = false;
      bool storesNull = false;
      for (const core::Store &store : summary.stores) {
        if (store.dest != path)
          continue;
        storesFresh |= store.value.kind == core::ValueSource::Kind::Fresh;
        storesNull |= store.value.kind == core::ValueSource::Kind::Null;
      }
      if (storesFresh && !storesNull &&
          !llvm::is_contained(conditional.unheldOnly, ref->place))
        conditional.unheldOnly.push_back(ref->place);
    }
  }
  for (const auto &[outcome, paths] : summary.nonNullOn) {
    for (const core::SummaryPath &path : paths) {
      const auto ref = builder.resolveSummaryPath(path, call);
      if (!ref || !ref->element.isWhole())
        continue;
      for (const auto &[cls, effects] : summary.outcomes)
        conditional.consumedBy.try_emplace(cls);
      conditional.nonNullOn[outcome].push_back(ref->place);
    }
  }
  if (conditional.consumedBy.empty())
    return;
  conditional.callee = calleeName(call);
  conditional.location = locate(call);
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
  if (const FunctionDecl *callee = call.getDirectCallee()) {
    // `_FORTIFY_SOURCE` spells `memset` as `__builtin___memset_chk`; the
    // user wrote the former.
    llvm::StringRef name = callee->getName();
    if (name.consume_front("__builtin___"))
      name.consume_back("_chk");
    else
      name = callee->getName();
    return "'" + name.str() + "'";
  }
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
  // Whatever the callee was handed may be kept (RFC 0007, *Escape*), and
  // whatever it reaches through a pointer may have been written: the
  // nullness facts there are gone (RFC 0008). Ownership facts stay: the
  // boundary warning is what covers an unchecked callee's frees and moves
  // (RFC 0003, *Boundaries*).
  for (const Expr *arg : call.arguments()) {
    if (!arg->getType()->isPointerType())
      continue;
    const ValueOrigin origin = builder.classifyValue(*arg);
    escapeValue(origin, /*deep=*/true, state);
    forgetNullnessReachable(origin, state);
  }
  // Nullness annotations say nothing about ownership, so they do not make
  // the callee checked; but what they do say holds (RFC 0008, *Annotation
  // surface*): a `WEAVEC_NONNULL` parameter is a requirement on this call.
  if (callee != nullptr && collectAnnotations(*callee).anyNullness())
    checkRequiredArguments(call, summaryFromAnnotations(*callee), state);

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
        recordResultStores(*ref, state);
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
      if (recording()) {
        // A returned place that is null, or may be, is a `null` alternative
        // for callers (RFC 0008, *Nullness*).
        const std::optional<core::NullRecord> nullness =
            origin.kind == ValueOrigin::Kind::Copy && origin.place
                ? nullnessAt(origin.place->place, state)
                : std::nullopt;
        // The `null` alternative holds under the path's facts and the
        // record's own guard (RFC 0009).
        const auto nullReturn = [this, &nullness, &state] {
          core::PlaceGuard guard = guardHere(state);
          for (const auto &[key, fact] : nullness->guard.conditions)
            guard.require(key, fact);
          core::ValueSource source = core::ValueSource::null();
          source.when = summaryGuardOf(guard);
          return source;
        };
        if (nullness && nullness->state == core::Nullness::Null) {
          inferred.addReturn(nullReturn());
        } else {
          inferred.addReturn(sourceOf(origin, state));
          if (nullness && nullness->state == core::Nullness::MaybeNull)
            inferred.addReturn(nullReturn());
        }
      }
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

void FunctionDataflow::recordResultStores(const PlaceRef &returned,
                                          const core::AnalysisState &state) {
  if (!recording())
    return;
  // Every pointer field path of the returned record's own storage with a
  // known source becomes a store rooted at `result` (RFC 0008,
  // *Struct-by-value results*).
  for (const core::PlaceId place : storageOf(returned.place)) {
    if (place == returned.place)
      continue;
    const auto *field = dyn_cast_if_present<FieldDecl>(builder.declFor(place));
    if (field == nullptr || !field->getType()->isPointerType())
      continue;
    // The path from the record to the field, spelled record-first.
    core::SummaryPath path = core::SummaryPath::result();
    std::vector<core::PlaceId> chain{place};
    for (const core::PlaceId ancestor : places.ancestors(place)) {
      if (ancestor == returned.place)
        break;
      chain.push_back(ancestor);
    }
    for (const core::PlaceId node : llvm::reverse(chain)) {
      if (places.step(node) == core::PathStep::Index)
        path = path.indexed();
      else
        path = path.field(places.fieldName(node));
    }
    ValueOrigin origin;
    if (state.resources.isNull(place)) {
      origin.kind = ValueOrigin::Kind::Null;
    } else {
      origin.kind = ValueOrigin::Kind::Copy;
      origin.place = PlaceRef{.place = place,
                              .derefs = {},
                              .derefExprs = {},
                              .derefElements = {},
                              .element = {}};
    }
    const core::ValueSource source = sourceOf(origin, state);
    if (source.kind == core::ValueSource::Kind::Unknown)
      continue;
    inferred.addStore(core::Store{.dest = path, .value = source});
  }
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
                          survivor->via, survivor->element, survivor->family,
                          survivor->ownValue, survivor->guard);
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
    state.nulls.forget(child);
    state.scalars.forget(child);
    state.dropGuardsOn(child);
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
                            record->family, /*ownValue=*/false, record->guard);
    }
    if (const auto fact = state.scalars.factOf(place))
      state.scalars.set(mirror, *fact);
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
    if (const auto record = state.nulls.recordOf(place))
      state.nulls.set(mirror, *record);
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
    // Dereferencing a pointer that may be null (RFC 0008, *Nullness*).
    checkDereference(ref.derefs[i], where != nullptr ? *where : at, state);
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
                            std::string_view family, bool library,
                            bool replaced, core::PlaceGuard guard) {
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
    // Keep the original record so later diagnostics point at the first
    // site, unless the callee left a new value in the cell (RFC 0008,
    // *Replaced values*): the report stands, but what follows uses the new
    // value, not the twice-freed one, and must not be reported again at
    // every later call (the cascade RFC 0009's guarded stores exposed:
    // `dumpByte(D, tt)` in a loop after one genuine report).
    if (replaced)
      reinit(place, state, ref.element);
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
  // The consume happened on a path with these facts, under the callee's
  // condition if it had one (RFC 0009, *Deriving guards*): a later edge that
  // contradicts one of them reinstates the value.
  for (const auto &[key, fact] : guardHere(state, place).conditions)
    guard.require(key, fact);
  // A callee that released the value and reinitialised the place (RFC 0008,
  // *Replaced values*) leaves the place and its mirrors (the same cell) live
  // and every other name for the old value dead.
  std::vector<core::PlaceId> sameCell;
  if (replaced)
    sameCell = mirrors(place, state);
  for (const ConsumeTarget &target :
       consumeTargets(place, ref.element, state)) {
    const bool isCell =
        target.place == place || llvm::is_contained(sameCell, target.place);
    if (replaced && isCell) {
      // Still this function's consumption of the caller's value.
      recordConsume(target.place, reason, family, target.element, guard, state);
      continue;
    }
    std::optional<core::PlaceId> via;
    if (target.place != place)
      via = place;
    // A place this function has overwritten on every path holds its own
    // value: the record must not reach the summary through the exit state
    // either, however the paths join later (RFC 0008, *Replaced values*).
    const auto path = builder.summaryPathOf(target.place);
    const bool ownValue = path && state.isOverwritten(*path);
    state.moves.markMoved(target.place, reason, here, via, target.element,
                          std::string(family), ownValue, guard);
    recordConsume(target.place, reason, family, target.element, guard, state);
    marked.push_back(target.place);
  }
  if (replaced) {
    // The cell holds a new value (the callee's store says which, or an
    // unknown one): nothing known about the old one applies to it.
    reinit(place, state, ref.element);
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
                                          const ValueOrigin &given,
                                          const Expr &at, bool constPointee,
                                          core::AnalysisState &state,
                                          core::ElementWitness element) {
  // RFC 0009: an alternative whose guard the facts refute is not a value
  // this path can receive (`p = f(n)` after `if (n == 0) return;` with `f`
  // returning null exactly when `n` is zero). A value with nothing left is
  // one the callee never produces here: the destination keeps what it held.
  ValueOrigin pruned;
  const ValueOrigin *chosen = &given;
  if (!given.guard.trivial() ||
      (given.kind == ValueOrigin::Kind::Conditional &&
       llvm::any_of(given.alternatives, [](const ValueOrigin &alternative) {
         return !alternative.guard.trivial();
       }))) {
    pruned = given;
    if (!pruneOrigin(pruned, state))
      return;
    chosen = &pruned;
  }
  const ValueOrigin &origin = *chosen;
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
      arms[0].origin->place && arms[0].origin->place->place == dest) {
    // Except that after `p += 1` the place no longer points at the start
    // of what it owns (RFC 0008, *Invalid releases*).
    if (arms[0].origin->interior && element.isWhole())
      markInterior(dest, state);
    return;
  }

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

  // What the value says about its own nullness, before the destination's
  // facts (which the value may have been read from) are reset (RFC 0008,
  // *Nullness*).
  const std::optional<core::NullRecord> nullness =
      nullnessOf(origin, at, state);

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
    case ValueOrigin::Kind::Alloc: {
      setKind(dest, core::join(state.kindOf(dest), core::OwnershipKind::Owned),
              state);
      // Held when the path's facts hold and, for a callee's argument-
      // conditional result, when its guard does (RFC 0009): `if (n > 0) p =
      // malloc(n); ... if (n > 0) free(p);` leaks nothing on the other edge.
      core::PlaceGuard guard = guardHere(state, dest);
      for (const auto &[key, fact] : arm->guard.conditions)
        guard.require(key, fact);
      state.resources.hold(
          dest,
          core::ResourceRecord{
              .origin = core::ResourceOrigin::Allocated,
              .location = locate(arm->call != nullptr
                                     ? static_cast<const Stmt &>(*arm->call)
                                     : static_cast<const Stmt &>(at)),
              .family = arm->family,
              .escaped = false,
              .interior = false,
              .guard = std::move(guard)});
      break;
    }
    case ValueOrigin::Kind::Copy: {
      if (!source)
        break;
      // The copy holds the same resource; both names now account for it. A
      // copy through an interior edge (`p + 1`, `strchr(p, c)`) does not
      // point at the allocation's start (RFC 0008, *Invalid releases*).
      if (source->resource) {
        core::ResourceRecord record = *source->resource;
        record.interior = record.interior || source->interior;
        state.resources.hold(dest, record);
      }
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
      if (source->moved &&
          source->moved->record.reason != core::MoveReason::Uninitialized) {
        // The read itself was reported at the load; keep the copy moved so
        // uses through it do not cascade into a second report per alias. A
        // copy of an uninitialised pointer is reported once, at the copy:
        // the destination itself is initialised now (RFC 0008).
        state.moves.markMoved(
            dest, source->moved->record.reason, source->moved->record.location,
            source->moved->record.via.value_or(source->moved->target), element,
            source->moved->record.family, /*ownValue=*/false,
            source->moved->record.guard);
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
  if (nullness && element.isWhole())
    setNullness(dest, *nullness, state);
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
  if (var == nullptr) {
    // String literals live for the whole program (RFC 0008).
    return builder.isLiteralPlace(root) ? core::LifetimeId::staticLifetime()
                                        : fnLifetime;
  }
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
  if (hit.record.reason == core::MoveReason::Uninitialized) {
    // RFC 0008, *Uninitialised pointers*: the record was made at the
    // declaration, which is where the note points.
    core::Diagnostic diagnostic = makeError(
        core::diag::UseOfUninitialized,
        "use of '" + nameOf(used) + "' before it was initialized", at);
    const core::PlaceId declared = places.root(hit.target);
    diagnostic.addNote("'" + nameOf(declared) + "' is declared here",
                       hit.record.location);
    report(std::move(diagnostic));
    return;
  }
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

// -- Nullness (RFC 0008) ------------------------------------------------------

std::optional<core::Nullness>
FunctionDataflow::declaredNullness(core::PlaceId place) const {
  const NamedDecl *decl = builder.declFor(place);
  if (decl == nullptr)
    return std::nullopt;
  AnnotationSet set = getAnnotations(*decl);
  if (const auto *param = dyn_cast<ParmVarDecl>(decl)) {
    const unsigned index = param->getFunctionScopeIndex();
    if (index < signature.params.size())
      set.merge(signature.params[index]);
  }
  if (set.nonNull)
    return core::Nullness::NonNull;
  if (set.nullable)
    return core::Nullness::MaybeNull;
  return std::nullopt;
}

std::optional<core::NullRecord>
FunctionDataflow::nullnessAt(core::PlaceId place,
                             const core::AnalysisState &state) const {
  if (const auto record = state.nulls.recordOf(place))
    return record;
  if (state.resources.isNull(place)) {
    return core::NullRecord{.state = core::Nullness::Null,
                            .location = {},
                            .reason = core::NullReason::AssignedNull,
                            .detail = {}};
  }
  const auto declared = declaredNullness(place);
  if (!declared)
    return std::nullopt;
  core::SourceLocation where;
  if (const NamedDecl *decl = builder.declFor(place))
    where = locate(decl->getLocation());
  return core::NullRecord{.state = *declared,
                          .location = where,
                          .reason = core::NullReason::Declared,
                          .detail = {}};
}

std::optional<core::NullRecord>
FunctionDataflow::nullnessOf(const ValueOrigin &origin, const Expr &at,
                             const core::AnalysisState &state) {
  // Flatten the alternatives, remembering the call each came from: the
  // `null` arm of a callee's `returns{fresh, null}` is the callee's doing.
  struct Leaf {
    const ValueOrigin *origin;
    const CallExpr *call;
  };
  std::vector<Leaf> leaves;
  std::vector<Leaf> work{Leaf{.origin = &origin, .call = origin.call}};
  while (!work.empty()) {
    const Leaf leaf = work.back();
    work.pop_back();
    if (leaf.origin->kind == ValueOrigin::Kind::Conditional) {
      for (const ValueOrigin &alternative : leaf.origin->alternatives)
        work.push_back(Leaf{.origin = &alternative,
                            .call = alternative.call != nullptr
                                        ? alternative.call
                                        : leaf.call});
      continue;
    }
    leaves.push_back(leaf);
  }
  if (leaves.empty())
    return std::nullopt;

  std::optional<core::NullRecord> nullSide;
  bool allNull = true;
  bool allNonNull = true;
  bool anyUnknown = false;
  // RFC 0009: the value may be null only when some null arm's guard holds
  // (a callee's `returns{null when n =0, fresh}`); outside every null arm's
  // guard it is one of the other arms, non-null when they all are and each
  // null arm promises the same outside its own guard.
  std::optional<core::PlaceGuard> nullGuard;
  bool promise = true;
  for (const Leaf &leaf : leaves) {
    std::optional<core::NullRecord> record;
    switch (leaf.origin->kind) {
    case ValueOrigin::Kind::Null:
      if (leaf.call != nullptr) {
        record = core::NullRecord{.state = core::Nullness::Null,
                                  .location = locate(*leaf.call),
                                  .reason = core::NullReason::CalleeResult,
                                  .detail = calleeName(*leaf.call)};
      } else {
        record = core::NullRecord{.state = core::Nullness::Null,
                                  .location = locate(at),
                                  .reason = core::NullReason::AssignedNull,
                                  .detail = {}};
      }
      break;
    case ValueOrigin::Kind::Copy:
      if (leaf.origin->place)
        record = nullnessAt(leaf.origin->place->place, state);
      break;
    case ValueOrigin::Kind::Alloc:
    case ValueOrigin::Kind::Borrow:
      // A fresh block or an address: never null on this arm.
      record = core::NullRecord{.state = core::Nullness::NonNull,
                                .location = locate(at),
                                .reason = core::NullReason::AssignedNull,
                                .detail = {}};
      break;
    case ValueOrigin::Kind::Opaque:
      // The result of an unchecked callee: what its declaration says about
      // nullness still holds (RFC 0008, *Annotation surface*).
      if (const FunctionDecl *decl =
              leaf.call != nullptr ? leaf.call->getDirectCallee() : nullptr) {
        const AnnotationSet result = collectAnnotations(*decl).result;
        if (result.nullable || result.nonNull) {
          record = core::NullRecord{.state = result.nonNull
                                                 ? core::Nullness::NonNull
                                                 : core::Nullness::MaybeNull,
                                    .location = locate(decl->getLocation()),
                                    .reason = core::NullReason::Declared,
                                    .detail = decl->getNameAsString()};
        }
      }
      break;
    case ValueOrigin::Kind::Raw:
    case ValueOrigin::Kind::Conditional:
      break;
    }
    if (!record) {
      anyUnknown = true;
      allNull = false;
      allNonNull = false;
      continue;
    }
    if (record->state != core::Nullness::Null)
      allNull = false;
    if (record->state != core::Nullness::NonNull)
      allNonNull = false;
    if (record->mayBeNull()) {
      // The arm's own guard, and the copied record's.
      core::PlaceGuard armGuard = record->guard;
      for (const auto &[key, fact] : leaf.origin->guard.conditions)
        armGuard.require(key, fact);
      if (!nullGuard)
        nullGuard = std::move(armGuard);
      else
        nullGuard->join(armGuard);
      promise &=
          record->state == core::Nullness::Null || record->otherwiseNonNull;
      if (!nullSide)
        nullSide = record;
    }
  }
  if (allNull && nullSide) {
    nullSide->guard = *nullGuard;
    nullSide->otherwiseNonNull = false;
    return nullSide;
  }
  if (nullSide) {
    nullSide->state = core::Nullness::MaybeNull;
    nullSide->guard = *nullGuard;
    nullSide->otherwiseNonNull = promise && !anyUnknown;
    return nullSide;
  }
  if (allNonNull && !anyUnknown)
    return core::NullRecord{.state = core::Nullness::NonNull,
                            .location = locate(at),
                            .reason = core::NullReason::AssignedNull,
                            .detail = {}};
  return std::nullopt;
}

void FunctionDataflow::setNullness(core::PlaceId place,
                                   const core::NullRecord &record,
                                   core::AnalysisState &state) {
  core::NullRecord guarded = record;
  // Every path through here satisfies the facts, so a path that later
  // refutes one never held this record (RFC 0009, *Deriving guards*). A
  // `NonNull` fact is dropped by the join whenever the other side lacks it,
  // so it needs no guard.
  if (guarded.state != core::Nullness::NonNull) {
    for (const auto &[key, fact] : guardHere(state, place).conditions)
      guarded.guard.require(key, fact);
  }
  state.nulls.set(place, guarded);
  for (const auto &[alias, edge] : state.aliases.edgesFrom(place)) {
    if (edge.exact)
      state.nulls.set(alias, guarded);
  }
}

std::string FunctionDataflow::nullNote(const core::NullRecord &record,
                                       std::string_view name) {
  const std::string subject = "'" + std::string(name) + "'";
  switch (record.reason) {
  case core::NullReason::AssignedNull:
    return subject + " is assigned NULL here";
  case core::NullReason::CalleeResult:
    return subject + " may be null: it is the result of " + record.detail +
           " here";
  case core::NullReason::CalleeStore:
    return subject + " may be null: it is set by " + record.detail + " here";
  case core::NullReason::Tested:
    return subject + " may be null: it is compared with NULL here";
  case core::NullReason::Declared:
    if (!record.detail.empty())
      return subject + " may be null: the result of '" + record.detail +
             "' is declared WEAVEC_NULLABLE here";
    return subject + " is declared WEAVEC_NULLABLE here";
  case core::NullReason::Dereferenced:
    break; // never `MaybeNull`, never reported
  }
  return subject + " may be null";
}

void FunctionDataflow::noteRequirement(core::PlaceId place,
                                       const core::AnalysisState &state) {
  if (!recording())
    return;
  const auto require = [this](core::PlaceId candidate) {
    const auto path = stableSummaryPathOf(candidate);
    if (!path || !path->isParam() || !path->isRoot())
      return false;
    // The body of a `WEAVEC_NULLABLE` parameter is checked instead.
    if (declaredNullness(candidate) == core::Nullness::MaybeNull)
      return false;
    inferred.requiresNonNull.insert(path->index);
    return true;
  };
  if (require(place))
    return;
  for (const auto &[alias, edge] : state.aliases.edgesFrom(place)) {
    if (edge.exact && require(alias))
      return;
  }
}

void FunctionDataflow::checkDereference(core::PlaceId pointer, const Expr &at,
                                        core::AnalysisState &state) {
  const auto record = nullnessAt(pointer, state);
  if (!record) {
    noteRequirement(pointer, state);
    markDereferenced(pointer, at, state);
    return;
  }
  if (!record->mayBeNull())
    return;
  const std::string name = nameOf(pointer);
  core::Diagnostic diagnostic = makeError(
      core::diag::NullDereference,
      "dereference of '" + name + "', which " +
          (record->state == core::Nullness::Null ? "is null" : "may be null"),
      at);
  if (record->location.isValid())
    diagnostic.addNote(nullNote(*record, name), record->location);
  report(std::move(diagnostic));
  // One bad pointer reports once: from here on nothing is known about it
  // (in both phases, so the fixpoint is the same).
  state.nulls.forget(pointer);
  if (record->reason == core::NullReason::Declared) {
    // Nothing to forget for a declared place; a positive fact silences it.
    state.nulls.set(pointer,
                    core::NullRecord{.state = core::Nullness::NonNull,
                                     .location = locate(at),
                                     .reason = core::NullReason::Tested,
                                     .detail = {}});
  }
}

void FunctionDataflow::markDereferenced(core::PlaceId pointer, const Expr &at,
                                        core::AnalysisState &state) {
  // The path continued past a dereference with nothing known about the
  // pointer: it was non-null, and stays so until reassigned (RFC 0008,
  // *Implementation notes*). This is what makes cJSON's `if
  // (cannot_access_at_index(input_buffer, 0)) input_buffer->offset--;`
  // clean after `buffer_at_offset(input_buffer)` at the top of the function:
  // the retest's null edge cannot make it maybe-null.
  setNullness(pointer,
              core::NullRecord{.state = core::Nullness::NonNull,
                               .location = locate(at),
                               .reason = core::NullReason::Dereferenced,
                               .detail = {}},
              state);
}

void FunctionDataflow::checkResultDereference(const CallExpr &call,
                                              core::AnalysisState &state) {
  if (!recording() || !emitDiagnostics || inUnsafe)
    return;
  const auto record = nullnessOf(builder.classifyValue(call), call, state);
  if (!record || !record->mayBeNull())
    return;
  const std::string callee = calleeName(call);
  core::Diagnostic diagnostic = makeError(
      core::diag::NullDereference,
      "dereference of the result of " + callee + ", which " +
          (record->state == core::Nullness::Null ? "is null" : "may be null"),
      call);
  if (record->reason == core::NullReason::Declared &&
      record->location.isValid())
    diagnostic.addNote("the result of " + callee +
                           " is declared WEAVEC_NULLABLE here",
                       record->location);
  report(std::move(diagnostic));
}

void FunctionDataflow::checkRequiredArguments(
    const CallExpr &call, const core::FunctionSummary &summary,
    core::AnalysisState &state) {
  for (const std::uint32_t index : summary.requiresNonNull) {
    if (index >= call.getNumArgs())
      continue;
    const Expr &arg = *call.getArg(index);
    if (!arg.getType()->isPointerType())
      continue;
    const ValueOrigin origin = builder.classifyValue(arg);
    std::optional<core::PlaceId> place;
    if (origin.kind == ValueOrigin::Kind::Copy && origin.place)
      place = origin.place->place;
    const auto record = nullnessOf(origin, arg, state);
    if (!record) {
      // `size_t len(const char *s) { return strlen(s); }` requires `s`.
      if (place) {
        noteRequirement(*place, state);
        markDereferenced(*place, arg, state);
      }
      continue;
    }
    if (!record->mayBeNull())
      continue;
    const std::string callee = calleeName(call);
    const std::string which =
        record->state == core::Nullness::Null ? "is null" : "may be null";
    std::string message;
    if (place) {
      message = "'" + nameOf(*place) + "', which ";
      message += which;
      message += ", is passed to ";
    } else {
      message = "a null pointer is passed to ";
    }
    message += callee;
    message += ", which dereferences it";
    core::Diagnostic diagnostic =
        makeError(core::diag::NullDereference, message, arg);
    if (place && record->location.isValid())
      diagnostic.addNote(nullNote(*record, nameOf(*place)), record->location);
    // (Not for an implicitly declared builtin: its "declaration" is here.)
    if (const FunctionDecl *decl = call.getDirectCallee();
        decl != nullptr && !decl->isImplicit())
      diagnostic.addNote(callee + " is declared here",
                         locate(decl->getLocation()));
    report(std::move(diagnostic));
    if (place) {
      state.nulls.forget(*place);
      if (record->reason == core::NullReason::Declared)
        state.nulls.set(*place,
                        core::NullRecord{.state = core::Nullness::NonNull,
                                         .location = locate(arg),
                                         .reason = core::NullReason::Tested,
                                         .detail = {}});
    }
  }
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

  // RFC 0009: ` when[n positive, p nonnull]` after a guarded record or
  // effect; nothing for one that always holds.
  const auto describePlaceGuard = [this](const core::PlaceGuard &guard) {
    std::string text;
    for (const auto &[place, fact] : guard.conditions) {
      text += text.empty() ? " when[" : ", ";
      text += std::string(places.name(place)) + " " + fact.toString();
    }
    return text.empty() ? text : text + "]";
  };
  const auto describePathGuard = [this](const core::PathGuard &guard) {
    std::string text;
    for (const auto &[path, fact] : guard.conditions) {
      text += text.empty() ? " when[" : ", ";
      text += summaryName(path) + " " + fact.toString();
    }
    return text.empty() ? text : text + "]";
  };

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
         << core::toString(record->reason);
      if (!record->family.empty())
        os << "(" << record->family << ")";
      os << describePlaceGuard(record->guard);
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
      if (record->interior)
        os << " interior";
      os << describePlaceGuard(record->guard);
      first = false;
    }
    os << "} nulls{";
    first = true;
    for (const core::PlaceId place : exitState->nulls.places()) {
      const auto record = exitState->nulls.recordOf(place);
      os << (first ? "" : ", ") << places.name(place);
      if (record->location.isValid())
        os << "@" << record->location.line << ":" << record->location.column;
      os << " " << core::toString(record->state);
      os << describePlaceGuard(record->guard);
      // The promise matters once there is a guard to refute.
      if (record->otherwiseNonNull && !record->guard.trivial())
        os << " otherwise-nonnull";
      first = false;
    }
    os << "}";
    // RFC 0009: the integer facts, only when there are any.
    if (!exitState->scalars.empty()) {
      os << " scalars{";
      first = true;
      for (const auto &[place, fact] : exitState->scalars.all()) {
        os << (first ? "" : ", ") << places.name(place) << " "
           << fact.toString();
        first = false;
      }
      os << "}";
    }
    os << "\n";
  }

  os << "  summary:";
  if (inferred.neverReturns)
    os << " never-returns;";
  const auto describeSource =
      [this, &describePathGuard](const core::ValueSource &source) {
        std::string text(source.kind == core::ValueSource::Kind::Copy &&
                                 source.interior
                             ? std::string_view("interior")
                             : core::toString(source.kind));
        if (source.isFresh() && !source.family.empty())
          text += "(" + source.family + ")";
        if (source.path)
          text += " " + summaryName(*source.path);
        return text + describePathGuard(source.when);
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
          std::pair{effect.moved, describeConsume(effect, "moved")},
          std::pair{effect.consumed() && effect.replaced,
                    std::string("replaced")},
          std::pair{effect.consumed() && effect.element,
                    std::string("element")}}) {
      if (flag) {
        os << sep << label;
        sep = "|";
      }
    }
    os << describePathGuard(effect.when) << ";";
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
  if (!inferred.requiresNonNull.empty()) {
    os << " requires{";
    first = true;
    for (const std::uint32_t param : inferred.requiresNonNull) {
      os << (first ? "" : ", ") << summaryName(core::SummaryPath::param(param));
      first = false;
    }
    os << "}";
  }
  const auto describePaths =
      [this, &os, &first](const char *label,
                          const std::set<core::SummaryPath> &paths) {
        os << " " << label << "{";
        first = true;
        for (const core::SummaryPath &path : paths) {
          os << (first ? "" : ", ") << summaryName(path);
          first = false;
        }
        os << "}";
      };
  for (const auto &[outcome, effects] : inferred.outcomes) {
    os << " outcome " << core::toString(outcome) << "{";
    first = true;
    for (const auto &[path, effect] : effects) {
      os << (first ? "" : ", ") << summaryName(path) << ":"
         << (effect.freed ? " " + describeConsume(effect, "freed") : "")
         << (effect.moved ? " " + describeConsume(effect, "moved") : "")
         << (effect.consumed() && effect.replaced ? " replaced" : "")
         << (effect.consumed() && effect.element ? " element" : "")
         << describePathGuard(effect.when);
      first = false;
    }
    os << "}";
    if (const auto nulls = inferred.nullOn.find(outcome);
        nulls != inferred.nullOn.end())
      describePaths("null", nulls->second);
    if (const auto nonNulls = inferred.nonNullOn.find(outcome);
        nonNulls != inferred.nonNullOn.end())
      describePaths("notnull", nonNulls->second);
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
  } else if (path.isGlobal()) {
    root = summaries.globals().nameOf(path.index).str();
  } else {
    root = "result";
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
  // The callee said what it did below the pointee: `written` paths are
  // replayed; a consume alone (`free(L->stack)`, a mutable borrow of `*L`
  // that overwrote nothing) is the caller's consume, recorded elsewhere,
  // and does not make the object written. Only a summary that says nothing
  // at all below the pointee is taken to have overwritten it.
  bool sawAny = false;
  // Paths order by root, then step by step, so those at or below the
  // pointee are one contiguous run.
  for (auto it = summary.effects.lower_bound(pointeePath);
       it != summary.effects.end() &&
       (it->first == pointeePath || pointeePath.isProperPrefixOf(it->first));
       ++it) {
    const auto &[path, effect] = *it;
    sawAny = true;
    if (!effect.written)
      continue;
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
  if (!sawAny) {
    for (const core::SummaryPath &base : bases)
      inferred.addEffect(base, core::PlaceEffect{.written = true});
  }
}

bool FunctionDataflow::isEventBased(const core::SummaryPath &path) const {
  return path.isParam() &&
         (path.isRoot() ||
          (path.index < paramReassigned.size() && paramReassigned[path.index]));
}

/// The summary effect a move record stands for. A consume through an
/// element access is one of an element the caller cannot identify (RFC
/// 0008, *Element consumes*); one under a guard happens only when the
/// caller's arguments satisfy it (RFC 0009).
static core::PlaceEffect effectOfMove(core::MoveReason reason,
                                      std::string_view family,
                                      const core::ElementWitness &element,
                                      core::PathGuard when = {}) {
  core::PlaceEffect effect;
  if (reason == core::MoveReason::Freed)
    effect.freed = true;
  else
    effect.moved = true;
  effect.family = std::string(family);
  effect.element = !element.isWhole();
  effect.when = std::move(when);
  return effect;
}

void FunctionDataflow::recordConsume(core::PlaceId target,
                                     core::MoveReason reason,
                                     std::string_view family,
                                     const core::ElementWitness &element,
                                     const core::PlaceGuard &guard,
                                     core::AnalysisState &state) {
  // Only locals can be uninitialised; the record never reaches a summary
  // (RFC 0008, *Uninitialised pointers*).
  if (reason == core::MoveReason::Uninitialized)
    return;
  const auto path = builder.summaryPathOf(target);
  if (!path)
    return;
  // A place this function has already overwritten on every path holds its
  // own value, not the caller's: releasing it is not the caller's business
  // (RFC 0008, *Replaced values*: consumption is of the value on entry).
  if (state.isOverwritten(*path))
    return;
  const core::PlaceEffect effect =
      effectOfMove(reason, family, element, summaryGuardOf(guard));
  // Every caller-visible consume is recorded as it happens (RFC 0008,
  // *Replaced values*). The flow-sensitive record feeds the outcome classes
  // at each `return` (RFC 0006) and, at the exit, the unconditional
  // effects; it is part of the state so the fixpoint sees it and so a path
  // that never returns contributes nothing.
  state.consumed[*path].join(effect);
}

void FunctionDataflow::noteRewritten(core::PlaceId place,
                                     core::AnalysisState &state) {
  // A write to a place after this function consumed the caller's value there
  // reinitialises it on this path (RFC 0008, *Replaced values*). Any element
  // counts (`free(a[i]); a[i] = strdup(s);` with an index the witnesses
  // cannot follow): RFC 0006 already treats element writes as may-writes of
  // the freed element. Writing an object rewrites its fields, not what its
  // pointers point to.
  const auto path = builder.summaryPathOf(place);
  if (!path)
    return;
  for (auto &[consumedPath, effect] : state.consumed) {
    // A parameter variable, and what lies under a reassigned one, is the
    // callee's private copy: writing it replaces nothing of the caller's
    // (`free(p); p = NULL;` frees the argument for good; RFC 0003).
    if (!effect.consumed() || effect.replaced || isEventBased(consumedPath))
      continue;
    if (consumedPath == *path) {
      effect.replaced = true;
      continue;
    }
    if (!path->isProperPrefixOf(consumedPath))
      continue;
    const bool throughPointer =
        std::any_of(std::next(consumedPath.steps.begin(),
                              static_cast<std::ptrdiff_t>(path->steps.size())),
                    consumedPath.steps.end(), [](const core::PathElem &elem) {
                      return elem.step == core::PathStep::Deref;
                    });
    if (!throughPointer)
      effect.replaced = true;
  }
}

void FunctionDataflow::noteOverwritten(core::PlaceId place,
                                       core::AnalysisState &state) {
  if (const auto path = builder.summaryPathOf(place))
    state.overwritten.insert(*path);
}

core::OutcomeEffects
FunctionDataflow::consumptionAt(const core::AnalysisState &state) {
  // The union of what happened on this path and what the places still hold
  // (RFC 0008, *Replaced values*: event and exit consumption). A record on
  // a place overwritten since entry is about this function's own value.
  core::OutcomeEffects result = state.consumed;
  for (const core::PlaceId place : state.moves.movedPlaces()) {
    const auto record = state.moves.recordOf(place);
    if (record->reason == core::MoveReason::Uninitialized || record->ownValue)
      continue;
    const auto path = builder.summaryPathOf(place);
    if (!path || state.isOverwritten(*path))
      continue;
    result[*path].join(effectOfMove(record->reason, record->family,
                                    record->element,
                                    summaryGuardOf(record->guard)));
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
  // `return rc;` after `if (rc != 0) ...`: the returned integer's fact
  // narrows the classes (RFC 0009, *Scalar facts in the state*).
  if (value.getType()->isIntegerType()) {
    if (const auto fact = scalarFactOf(value, state)) {
      std::set<core::Outcome> narrowed;
      for (const core::Outcome outcome : classes) {
        if (fact->classes.contains(outcome))
          narrowed.insert(outcome);
      }
      if (!narrowed.empty())
        classes = std::move(narrowed);
    }
  }
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
  // The caller memory known non-null here (RFC 0008, *Per-outcome non-null
  // facts*).
  std::set<core::SummaryPath> nonNullHere;
  for (const core::PlaceId place : state.nulls.places()) {
    if (!state.nulls.isNonNull(place))
      continue;
    if (const auto path = callerVisiblePath(place))
      nonNullHere.insert(*path);
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
    // `return *out != NULL`: on every class but the one that means null, the
    // tested place is non-null.
    std::set<core::SummaryPath> nonNullInClass = nonNullHere;
    if (tested && tested->second != outcome)
      nonNullInClass.insert(tested->first);
    for (const core::SummaryPath &path : nullInClass)
      nonNullInClass.erase(path);
    const auto [it, first] = nullAtReturn.try_emplace(
        outcome, NullAtReturn{.null = nullInClass,
                              .held = heldInClass,
                              .nonNull = nonNullInClass});
    if (first)
      continue;
    std::set<core::SummaryPath> both;
    std::ranges::set_intersection(it->second.null, nullInClass,
                                  std::inserter(both, both.end()));
    it->second.null = std::move(both);
    it->second.held.insert(heldInClass.begin(), heldInClass.end());
    std::set<core::SummaryPath> bothNonNull;
    std::ranges::set_intersection(
        it->second.nonNull, nonNullInClass,
        std::inserter(bothNonNull, bothNonNull.end()));
    it->second.nonNull = std::move(bothNonNull);
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
  core::ValueSource source = sourceValueOf(origin, state);
  // The value is handed out on a path with these facts, from an origin
  // that itself came with a condition (RFC 0009, *Deriving guards*):
  // `if (n == 0) return NULL;` is `returns{null when n =0, ...}`.
  core::PlaceGuard guard = guardHere(state);
  for (const auto &[key, fact] : origin.guard.conditions)
    guard.require(key, fact);
  source.when = summaryGuardOf(guard);
  return source;
}

core::ValueSource
FunctionDataflow::sourceValueOf(const ValueOrigin &origin,
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
  // Consumption is recorded as it happens for every caller-visible path
  // (RFC 0008, *Replaced values*, amending RFC 0003), but only what reaches
  // a return is the caller's business: a block that ends in `exit()` never
  // hands control back (RFC 0003, *What a summary describes*), and a null
  // edge retracts the consumption below the pointer (RFC 0007). Lua's
  // `os_exit` does `lua_close(L); exit(status);` and must not tell every
  // caller of a `lua_CFunction` that `L->l_G` is gone. The exit state adds
  // records that reached a place by propagation (a copy of a moved value).
  // Each path still moved at the exit, with the guard under which it is
  // (RFC 0009): a record present at the exit under a guard says the value
  // is gone on the paths the guard holds on and was replaced, or never
  // consumed, on every other path that returns.
  std::map<core::SummaryPath, core::PathGuard> movedAtExit;
  if (exitState != nullptr) {
    for (const auto &[path, effect] : exitState->consumed) {
      if (effect.consumed())
        inferred.addEffect(path, effect);
    }
    for (const core::PlaceId place : exitState->moves.movedPlaces()) {
      const auto record = exitState->moves.recordOf(place);
      if (record->reason == core::MoveReason::Uninitialized || record->ownValue)
        continue;
      const auto path = builder.summaryPathOf(place);
      if (!path || exitState->isOverwritten(*path))
        continue;
      core::PathGuard guard = summaryGuardOf(record->guard);
      inferred.addEffect(*path, effectOfMove(record->reason, record->family,
                                             record->element, guard));
      // Two places with one summary path (an alias and its mirror): the
      // value is gone when either record says so.
      if (const auto [it, inserted] = movedAtExit.emplace(*path, guard);
          !inserted)
        it->second.join(guard);
    }
  }
  // RFC 0009, *Inferred `noreturn`*: a body no path of which reaches the
  // exit never hands control back, provided the states are a fixpoint (a
  // body the iteration gave up on may well return).
  inferred.neverReturns = exitState == nullptr && !convergenceFailed;
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
  // A path consumed on some path through the body but holding the consumed
  // value at no return was reinitialised before returning: the caller's
  // place is replaced, only its other names for the old value are dead (RFC
  // 0008, *Replaced values*). So was a path every consuming path wrote to
  // afterwards (`noteRewritten`), even when an element record the witnesses
  // could not match survives to the exit. Parameter roots and paths under a
  // reassigned parameter describe the callee's private copy, never the
  // argument.
  //
  // A value gone at the exit only under a guard (`if (b == NULL)
  // finish(L); else append(L, b)` where `finish` frees `L->stack` and
  // `append` frees and replaces it) is unreplaced only on the paths the
  // guard holds on; the consume the caller must treat as unreplaced applies
  // under that guard, and the replaced consume of the other paths is not
  // claimed (RFC 0009, *Deriving guards*: *Replaced values under a guard*).
  // Without this the join of the two paths would be an unconditional,
  // unreplaced consume while the store that reinitialises the place keeps
  // its guard, and a caller on the other path would see a value freed that
  // the callee left live.
  if (exitState != nullptr) {
    for (auto &[path, effect] : inferred.effects) {
      if (!effect.consumed() || isEventBased(path))
        continue;
      const auto consumed = exitState->consumed.find(path);
      const bool rewritten = consumed != exitState->consumed.end() &&
                             consumed->second.consumed() &&
                             consumed->second.replaced;
      if (const auto moved = movedAtExit.find(path);
          moved != movedAtExit.end() && !rewritten) {
        for (const auto &[key, fact] : moved->second.conditions)
          effect.when.require(key, fact);
        continue;
      }
      effect.replaced = true;
      for (auto &[outcome, effects] : inferred.outcomes) {
        if (const auto it = effects.find(path); it != effects.end())
          it->second.replaced = true;
      }
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
    if (!paths.empty()) {
      inferred.addOutcome(outcome);
      inferred.nullOn[outcome] = std::move(paths);
      conditional = true;
    }
    // Non-null facts are worth a class only for caller memory the callee
    // wrote (RFC 0008, *Per-outcome non-null facts*): a parameter the
    // callee merely tested is the caller's to know about.
    std::set<core::SummaryPath> nonNull;
    for (const core::SummaryPath &path : nulls.nonNull) {
      if (inferred.storesTo(path))
        nonNull.insert(path);
    }
    if (!nonNull.empty()) {
      inferred.addOutcome(outcome);
      inferred.nonNullOn[outcome] = std::move(nonNull);
      conditional = true;
    }
  }
  // Classes only matter when some consumption depends on them; a summary
  // without conditional consumption stays as small as an RFC 0003 one.
  if (!conditional)
    inferred.outcomes.clear();
  dropUnstableGuards();
}

void FunctionDataflow::dropUnstableGuards() {
  // A guard names the caller's memory as it was on entry (RFC 0009,
  // *Deriving guards*). A path this function writes, itself or through a
  // callee (`written`), or below an object it overwrites, may have held
  // another value when the guard was formed: the conjunct is dropped, which
  // only weakens the guard.
  const auto unstable = [this](const core::SummaryPath &path) {
    if (writtenScalarPaths.contains(path))
      return true;
    return std::ranges::any_of(inferred.effects, [&path](const auto &entry) {
      const auto &[written, effect] = entry;
      return effect.written &&
             (written == path || written.isProperPrefixOf(path));
    });
  };
  const auto clean = [&unstable](core::PathGuard &guard) {
    for (auto it = guard.conditions.begin(); it != guard.conditions.end();) {
      if (unstable(it->first))
        it = guard.conditions.erase(it);
      else
        ++it;
    }
  };
  for (auto &[path, effect] : inferred.effects)
    clean(effect.when);
  for (auto &[outcome, effects] : inferred.outcomes) {
    for (auto &[path, effect] : effects)
      clean(effect.when);
  }
  std::set<core::Store> stores = std::move(inferred.stores);
  inferred.stores.clear();
  for (core::Store store : stores) {
    clean(store.value.when);
    inferred.addStore(std::move(store));
  }
  std::set<core::ValueSource> returns = std::move(inferred.returns);
  inferred.returns.clear();
  for (core::ValueSource source : returns) {
    clean(source.when);
    inferred.addReturn(std::move(source));
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
