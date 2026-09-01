//===- FunctionAnalysis.cpp - Per-function ownership analysis -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/FunctionAnalysis.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Core/Moves.h"
#include "weavec/Core/Place.h"

#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Casting.h"

#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

namespace {

/// Path-insensitive checker for locally-owned heap pointers.
///
/// Walks a function body in evaluation order, treating `free(p)` as moving
/// ownership out of `p`, and flags subsequent reads of `p` until it is
/// reassigned. Branches are analysed independently and joined conservatively
/// ("may be freed"); loop bodies are analysed once.
class LocalOwnershipChecker {
public:
  LocalOwnershipChecker(ASTContext &ctx, core::DiagnosticSink &diagSink)
      : context(ctx), sink(diagSink) {}

  void run(const FunctionDecl &function) {
    for (const ParmVarDecl *param : function.parameters())
      track(*param);
    visit(function.getBody(), tracker);
  }

private:
  ASTContext &context;
  core::DiagnosticSink &sink;
  core::PlaceTable places;
  core::MoveTracker tracker;
  llvm::DenseMap<const VarDecl *, core::PlaceId> placeOf;

  void track(const VarDecl &var) {
    if (!var.getType()->isPointerType())
      return;
    placeOf.try_emplace(&var, places.create(var.getNameAsString()));
  }

  std::optional<core::PlaceId> placeFor(const Expr *expr) const {
    const auto *ref = dyn_cast<DeclRefExpr>(expr->IgnoreParenCasts());
    if (ref == nullptr)
      return std::nullopt;
    const auto *var = dyn_cast<VarDecl>(ref->getDecl());
    if (var == nullptr)
      return std::nullopt;
    const auto it = placeOf.find(var);
    if (it == placeOf.end())
      return std::nullopt;
    return it->second;
  }

  core::SourceLocation locate(const Stmt &stmt) const {
    return toCoreLocation(context.getSourceManager(), stmt.getBeginLoc());
  }

  static bool isFreeCall(const CallExpr &call) {
    const FunctionDecl *callee = call.getDirectCallee();
    if (callee == nullptr || call.getNumArgs() != 1)
      return false;
    const IdentifierInfo *ident = callee->getIdentifier();
    return ident != nullptr && ident->getName() == "free" && callee->isGlobal();
  }

  // -- Traversal ----------------------------------------------------------

  void visit(const Stmt *stmt, core::MoveTracker &state) {
    if (stmt == nullptr)
      return;

    if (isUnsafeBlock(*stmt))
      return;

    if (const auto *declStmt = dyn_cast<DeclStmt>(stmt)) {
      visitDeclStmt(*declStmt, state);
      return;
    }
    if (const auto *binOp = dyn_cast<BinaryOperator>(stmt);
        binOp != nullptr && binOp->isAssignmentOp()) {
      visitAssignment(*binOp, state);
      return;
    }
    if (const auto *call = dyn_cast<CallExpr>(stmt);
        call && isFreeCall(*call)) {
      visitFree(*call, state);
      return;
    }
    if (const auto *ref = dyn_cast<DeclRefExpr>(stmt)) {
      visitUse(*ref, state);
      return;
    }
    if (const auto *ifStmt = dyn_cast<IfStmt>(stmt)) {
      visitIf(*ifStmt, state);
      return;
    }
    if (isa<WhileStmt, ForStmt, DoStmt>(stmt)) {
      visitLoop(*stmt, state);
      return;
    }

    for (const Stmt *child : stmt->children())
      visit(child, state);
  }

  void visitDeclStmt(const DeclStmt &declStmt, core::MoveTracker &state) {
    for (const Decl *decl : declStmt.decls()) {
      const auto *var = dyn_cast<VarDecl>(decl);
      if (var == nullptr)
        continue;
      if (const Expr *init = var->getInit())
        visit(init, state);
      track(*var);
    }
  }

  void visitAssignment(const BinaryOperator &assign, core::MoveTracker &state) {
    // Evaluate the right-hand side first, then treat a plain assignment to a
    // tracked pointer as re-initialising it.
    visit(assign.getRHS(), state);
    const auto place = placeFor(assign.getLHS());
    if (!place) {
      visit(assign.getLHS(), state);
      return;
    }
    if (assign.getOpcode() == BO_Assign)
      state.reinitialize(*place);
    else
      checkUse(*place, *assign.getLHS(), state);
  }

  void visitFree(const CallExpr &call, core::MoveTracker &state) {
    const Expr *arg = call.getArg(0);
    const auto place = placeFor(arg);
    if (!place) {
      visit(arg, state);
      return;
    }
    const core::SourceLocation here = locate(call);
    if (const auto previous =
            state.markMoved(*place, core::MoveReason::Freed, here)) {
      core::Diagnostic diagnostic{
          .severity = core::Severity::Error,
          .id = core::diag::DoubleFree,
          .message =
              "'" + std::string(places.name(*place)) + "' is freed twice",
          .location = here,
          .notes = {},
      };
      diagnostic.addNote("previously freed here", previous->location);
      sink.report(diagnostic);
    }
  }

  void visitUse(const DeclRefExpr &ref, core::MoveTracker &state) {
    if (const auto place = placeFor(&ref))
      checkUse(*place, ref, state);
  }

  void checkUse(core::PlaceId place, const Expr &use,
                core::MoveTracker &state) {
    const auto moved = state.movedAt(place);
    if (!moved)
      return;
    const bool freed = moved->reason == core::MoveReason::Freed;
    core::Diagnostic diagnostic{
        .severity = core::Severity::Error,
        .id = freed ? core::diag::UseAfterFree : core::diag::UseAfterMove,
        .message = "use of '" + std::string(places.name(place)) +
                   "' after it " + (freed ? "was freed" : "was moved"),
        .location = locate(use),
        .notes = {},
    };
    diagnostic.addNote(freed ? "freed here" : "moved here", moved->location);
    sink.report(diagnostic);
  }

  void visitIf(const IfStmt &ifStmt, core::MoveTracker &state) {
    visit(ifStmt.getInit(), state);
    visit(ifStmt.getCond(), state);

    core::MoveTracker thenState = state;
    visit(ifStmt.getThen(), thenState);

    core::MoveTracker elseState = state;
    visit(ifStmt.getElse(), elseState);

    // Conservative join: a place freed on either path may be freed afterwards.
    state = std::move(thenState);
    state.join(elseState);
  }

  void visitLoop(const Stmt &loop, core::MoveTracker &state) {
    // Analyse the loop as if executed once, then join with the "not executed"
    // state. A real fixpoint over the CFG replaces this later.
    core::MoveTracker bodyState = state;
    for (const Stmt *child : loop.children())
      visit(child, bodyState);
    state.join(bodyState);
  }
};

} // namespace

FunctionAnalyzer::FunctionAnalyzer(ASTContext &ctx,
                                   core::DiagnosticSink &diagSink,
                                   AnalysisOptions analysisOptions)
    : context(ctx), sink(diagSink), options(analysisOptions) {}

void FunctionAnalyzer::analyze(const FunctionDecl &function) {
  if (!function.doesThisDeclarationHaveABody())
    return;

  const SourceManager &sm = context.getSourceManager();

  const AnnotationSet annotations = getAnnotations(function);
  if (annotations.invalid) {
    sink.report(core::Diagnostic{
        .severity = core::Severity::Warning,
        .id = core::diag::InvalidAnnotation,
        .message = "unrecognised weavec annotation on '" +
                   function.getNameAsString() + "'",
        .location = toCoreLocation(sm, function.getLocation()),
        .notes = {},
    });
  }
  if (annotations.unsafe)
    return;

  if (options.reportUnannotated) {
    for (const ParmVarDecl *param : function.parameters()) {
      if (!param->getType()->isPointerType() || getAnnotations(*param).any())
        continue;
      sink.report(core::Diagnostic{
          .severity = core::Severity::Warning,
          .id = core::diag::AnnotationRequired,
          .message = "pointer parameter '" + param->getNameAsString() +
                     "' has no inferable ownership; annotate it with "
                     "WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT",
          .location = toCoreLocation(sm, param->getLocation()),
          .notes = {},
      });
    }
  }

  LocalOwnershipChecker checker(context, sink);
  checker.run(function);
}

} // namespace weavec::analysis
