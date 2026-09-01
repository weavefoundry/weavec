//===- Dataflow.h - CFG dataflow driving the core model --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The intra-procedural engine specified by RFC 0002: a forward worklist
// iteration over `clang::CFG` whose state is `core::AnalysisState`, followed
// by a single reporting pass that emits diagnostics once per program point.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_DATAFLOW_H
#define WEAVEC_LIB_ANALYSIS_DATAFLOW_H

#include "PlaceBuilder.h"
#include "weavec/Analysis/Allocators.h"
#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Lifetime.h"
#include "weavec/Core/Place.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weavec::analysis {

class FunctionDataflow {
public:
  FunctionDataflow(clang::ASTContext &ctx, const clang::FunctionDecl &fn,
                   core::DiagnosticSink &diagSink,
                   const AnalysisOptions &analysisOptions);

  /// Runs the analysis over `fn`'s body and reports diagnostics to the sink.
  void run();

private:
  /// How a place expression is used at its position in the tree, decided by
  /// a pre-pass over the AST so that each CFG element can be handled locally.
  enum class Role : std::uint8_t {
    /// The place's value is read (a load or a dereference on the way to
    /// another place).
    Read,
    /// Left-hand side of a plain assignment: dereferences on the path are
    /// read, the place itself is written.
    Write,
    /// `x++`, `x += ...`: read and written.
    ReadWrite,
    /// Argument whose ownership a call takes; handled at the call.
    Consume,
    /// Operand of `&`: not read (though dereferences on the path are).
    AddressOf,
    /// Interior node of a longer place path; handled at the root.
    Ignore,
  };

  clang::ASTContext &context;
  const clang::FunctionDecl &function;
  core::DiagnosticSink &sink;
  const AnalysisOptions &options;

  core::PlaceTable places;
  PlaceBuilder builder;

  core::LifetimeConstraints lifetimes;
  core::LifetimeId callerLifetime;
  core::LifetimeId fnLifetime;
  llvm::DenseMap<const clang::VarDecl *, core::LifetimeId> varLifetimes;
  std::map<std::uint32_t, core::SourceLocation> scopeEnds;
  std::map<std::vector<std::uint32_t>, core::LifetimeId> meetCache;

  llvm::DenseSet<const clang::Stmt *> unsafeStmts;
  llvm::DenseMap<const clang::Expr *, Role> roles;
  llvm::DenseSet<const clang::CallExpr *> assignedCalls;

  std::unique_ptr<clang::CFG> cfg;
  std::vector<std::optional<core::AnalysisState>> entryStates;

  bool reporting = false;
  std::vector<core::Diagnostic> pending;
  std::map<core::PlaceId, core::OwnershipKind> summaryKinds;

  // -- Pre-passes -----------------------------------------------------------

  void collectScopes(const clang::Stmt *stmt, core::LifetimeId current);
  void classifyStmt(const clang::Stmt *stmt);
  void classifyExpr(const clang::Expr *expr, Role role);
  void markPathInterior(const clang::Expr &root);
  void noteAssignedCall(const clang::Expr &rhs);
  void collectUnsafe(const clang::Stmt &stmt);
  core::AnalysisState initialState();

  // -- Engine ---------------------------------------------------------------

  void transfer(const clang::CFGBlock &block, core::AnalysisState &state);
  void applyEdge(const clang::CFGBlock &from, unsigned succIndex,
                 core::AnalysisState &state);
  void flushDiagnostics();
  void dump(const core::AnalysisState *exitState);

  // -- Element handlers -----------------------------------------------------

  void handleExpr(const clang::Expr &expr, core::AnalysisState &state);
  void handleDecl(const clang::DeclStmt &decl, core::AnalysisState &state);
  void handleAssign(const clang::BinaryOperator &assign,
                    core::AnalysisState &state);
  void handleCall(const clang::CallExpr &call, core::AnalysisState &state);
  void handleReturn(const clang::ReturnStmt &ret, core::AnalysisState &state);
  void handleLifetimeEnd(const clang::VarDecl &var, core::AnalysisState &state);

  // -- Semantic actions -----------------------------------------------------

  void doRead(const PlaceRef &ref, const clang::Expr &at,
              core::AnalysisState &state, bool includeSelf);
  /// Returns false if the place was already moved (reported, not re-marked).
  bool doConsume(const PlaceRef &ref, core::MoveReason reason,
                 const clang::Expr &at, core::AnalysisState &state);
  void doMutationCheck(core::PlaceId place, const clang::Expr &at,
                       core::AnalysisState &state);
  void applyPointerAssign(core::PlaceId dest, const ValueOrigin &origin,
                          const clang::Expr &at, bool constPointee,
                          core::AnalysisState &state);
  void applyBorrow(core::PlaceId dest, const PlaceRef &borrowed,
                   core::BorrowKind kind, const clang::Expr &at,
                   core::AnalysisState &state);
  void checkTemporaryBorrow(const PlaceRef &borrowed, core::BorrowKind kind,
                            const clang::Expr &at,
                            const core::AnalysisState &state);
  void reinit(core::PlaceId place, core::AnalysisState &state);
  /// Copies every fact about the objects below `*src` onto `*dest`.
  void mirrorSubtree(core::PlaceId src, core::PlaceId dest,
                     core::AnalysisState &state);
  void setKind(core::PlaceId place, core::OwnershipKind kind,
               core::AnalysisState &state);

  // -- Queries --------------------------------------------------------------

  /// Every place a fact about `place` also applies to: the direct aliases of
  /// `place` and of each of its mirrors (the same path under every alias of
  /// a dereferenced pointer). Deliberately not transitive; see the
  /// definition.
  [[nodiscard]] std::vector<core::PlaceId>
  targets(core::PlaceId place, const core::AnalysisState &state);
  [[nodiscard]] std::vector<core::PlaceId>
  mirrors(core::PlaceId place, const core::AnalysisState &state);
  /// `place` together with its ancestors and descendants.
  [[nodiscard]] std::vector<core::PlaceId> related(core::PlaceId place);

  struct MovedHit {
    core::PlaceId target;
    core::MoveRecord record;
  };
  [[nodiscard]] static std::optional<MovedHit>
  findMoved(core::PlaceId place, const core::AnalysisState &state);
  [[nodiscard]] std::optional<core::Loan>
  findLoanConflict(core::PlaceId place, std::optional<core::BorrowKind> kind,
                   const core::AnalysisState &state);

  [[nodiscard]] std::vector<core::LifetimeId>
  lifetimesOfPlace(core::PlaceId place, const core::AnalysisState &state);
  [[nodiscard]] core::LifetimeId meet(std::vector<core::LifetimeId> ids);
  [[nodiscard]] core::LifetimeId rootLifetime(core::PlaceId place);

  // -- Diagnostics ----------------------------------------------------------

  [[nodiscard]] core::SourceLocation locate(const clang::Stmt &stmt) const;
  [[nodiscard]] core::SourceLocation locate(clang::SourceLocation loc) const;
  void report(core::Diagnostic diagnostic);
  void reportUseOfMoved(core::PlaceId used, const MovedHit &hit,
                        const clang::Expr &at);
  void reportLifetimeTooShort(core::PlaceId holder, core::PlaceId borrowed,
                              const clang::Expr &at, bool returned);
  [[nodiscard]] std::string nameOf(core::PlaceId place) const;
};

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_DATAFLOW_H
