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
// by a single final pass that emits diagnostics once per program point and
// records the function's summary (RFC 0003).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_DATAFLOW_H
#define WEAVEC_LIB_ANALYSIS_DATAFLOW_H

#include "PlaceBuilder.h"
#include "weavec/Analysis/Allocators.h"
#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Lifetime.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Summary.h"

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
#include <string_view>
#include <vector>

namespace weavec::analysis {

class FunctionDataflow {
public:
  /// `summaries` supplies callee summaries and receives this function's
  /// global roots. Diagnostics are produced only if `emitDiagnostics`; the
  /// summary is produced either way.
  FunctionDataflow(clang::ASTContext &ctx, const clang::FunctionDecl &fn,
                   core::DiagnosticSink &diagSink,
                   const AnalysisOptions &analysisOptions,
                   SummaryStore &summaryStore, bool emitDiags);

  /// Runs the analysis over `fn`'s body, reports diagnostics to the sink
  /// (if enabled) and computes the summary.
  void run();

  /// The summary inferred by `run` (RFC 0003, *Deriving a summary*).
  [[nodiscard]] const core::FunctionSummary &summary() const noexcept {
    return inferred;
  }

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

  /// The worklist iteration computes states silently; the final pass, run
  /// once per block from the fixpoint states, reports and records.
  enum class Phase : std::uint8_t { Fixpoint, Final };

  clang::ASTContext &context;
  const clang::FunctionDecl &function;
  core::DiagnosticSink &sink;
  const AnalysisOptions &options;
  SummaryStore &summaries;
  const bool emitDiagnostics;

  core::PlaceTable places;
  PlaceBuilder builder;

  core::LifetimeConstraints lifetimes;
  core::LifetimeId callerLifetime;
  core::LifetimeId fnLifetime;
  llvm::DenseMap<const clang::VarDecl *, core::LifetimeId> varLifetimes;
  std::map<std::uint32_t, core::SourceLocation> scopeEnds;
  std::map<std::vector<std::uint32_t>, core::LifetimeId> meetCache;

  /// Statements inside a `WEAVEC_UNSAFE` block (RFC 0004, *Unsafe regions*).
  llvm::DenseSet<const clang::Stmt *> unsafeStmts;
  llvm::DenseMap<const clang::Expr *, Role> roles;
  llvm::DenseSet<const clang::CallExpr *> assignedCalls;
  /// Parameters whose variable is assigned or address-taken in the body.
  std::vector<bool> paramReassigned;
  SignatureAnnotations signature;
  /// The whole body is an unsafe region (`WEAVEC_UNSAFE` on the function).
  bool unsafeBody;
  /// The CFG element being transferred lies in an unsafe region: raw
  /// operations are permitted and nothing is reported.
  bool inUnsafe;
  /// Ownership annotations on local variables, consumed for the assertion
  /// rule (RFC 0004, *Laundering*).
  std::map<core::PlaceId, AnnotationSet> declaredKinds;

  std::unique_ptr<clang::CFG> cfg;
  std::vector<std::optional<core::AnalysisState>> entryStates;

  Phase phase = Phase::Fixpoint;
  std::vector<core::Diagnostic> pending;
  std::map<core::PlaceId, core::OwnershipKind> summaryKinds;

  /// The summary under construction (final pass only).
  core::FunctionSummary inferred;
  /// Consumption recorded as it happens, keyed by summary path; used for
  /// parameter roots and for paths under reassigned parameters.
  std::map<core::SummaryPath, core::PlaceEffect> eventEffects;

  // -- Pre-passes -----------------------------------------------------------

  void collectScopes(const clang::Stmt *stmt, core::LifetimeId current);
  void classifyStmt(const clang::Stmt *stmt);
  void classifyExpr(const clang::Expr *expr, Role role);
  void markPathInterior(const clang::Expr &root);
  void noteAssignedCall(const clang::Expr &rhs);
  void noteParamAccess(const clang::Expr &place, Role role);
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
  /// `dest = value` for a record: field-wise pointer copies when `value` is
  /// a place, field-wise assignments when it is an initializer list, and a
  /// reset otherwise (RFC 0005, *Struct copies*).
  void copyRecord(core::PlaceId dest, const clang::Expr &value,
                  core::AnalysisState &state);
  /// `dest = { ... }`: assigns each initialised field.
  void initRecord(core::PlaceId dest, const clang::InitListExpr &init,
                  core::AnalysisState &state);
  void setKind(core::PlaceId place, core::OwnershipKind kind,
               core::AnalysisState &state);

  // -- Calls (RFC 0003) -----------------------------------------------------

  /// Applies a resolved callee summary: consumption, borrows for the call,
  /// stores through arguments and into globals.
  void applySummary(const clang::CallExpr &call, const CallEffects &effects,
                    core::AnalysisState &state);
  /// Handles a call across the checking boundary (no summary): the RFC 0003
  /// warning by default, a raw operation under `--strict-externs` (RFC
  /// 0004, *Boundaries*).
  void handleUncheckedCall(const clang::CallExpr &call);
  /// Reports `annotation-required` the first time an unresolvable callee
  /// with pointer parameters or result is called from reported code.
  void noteUnknownCallee(const clang::CallExpr &call);
  /// True if `call` has a pointer argument or result worth reporting on.
  [[nodiscard]] static bool callInvolvesPointers(const clang::CallExpr &call);
  /// `'free'`, `'o.drop'`, or `a function pointer`, for messages.
  [[nodiscard]] std::string calleeName(const clang::CallExpr &call);

  // -- Raw pointers (RFC 0004) ----------------------------------------------

  /// The raw record for `place`: from the state, or synthesised if the
  /// place's variable or field is declared `WEAVEC_RAW`.
  [[nodiscard]] std::optional<core::RawRecord>
  rawAt(core::PlaceId place, const core::AnalysisState &state) const;
  /// The raw record a value with `origin` would give its destination, if
  /// any: a raw origin, a copy of a raw place, or a value reached through a
  /// raw pointer.
  [[nodiscard]] std::optional<core::RawRecord>
  rawRecordOf(const ValueOrigin &origin, const clang::Expr &at,
              const core::AnalysisState &state);
  /// The ownership annotations on the variable `place` names (a local's
  /// own, a parameter's signature), if any.
  [[nodiscard]] std::optional<AnnotationSet>
  declaredAnnotations(core::PlaceId place) const;
  void markRaw(core::PlaceId place, const core::RawRecord &record,
               core::AnalysisState &state);
  /// Reports a raw operation on the pointer `name` (empty for a value with
  /// no place) unless inside an unsafe region.
  void reportRawOperation(std::string message, std::string_view name,
                          const core::RawRecord &record, const clang::Expr &at);
  /// A raw pointer passed where the callee dereferences, releases or takes
  /// ownership of it.
  void checkRawArgument(const clang::CallExpr &call, unsigned index,
                        const char *verb, const core::AnalysisState &state);
  /// `'p' is raw: cast from an integer here (through 'q')`.
  [[nodiscard]] std::string rawNote(const core::RawRecord &record,
                                    std::string_view name) const;
  /// The name of the place a pointer value names, if it names one.
  [[nodiscard]] std::optional<std::string>
  pointerName(const clang::Expr &value);

  // -- Summary recording (RFC 0003) -----------------------------------------

  [[nodiscard]] bool recording() const noexcept {
    return phase == Phase::Final;
  }
  /// Marks the summary path of `place` (and of its mirrors) as read or
  /// written, when it names caller memory.
  void recordAccess(core::PlaceId place, bool write,
                    const core::AnalysisState &state);
  /// Records a release/move of `target` as it happens.
  void recordConsume(core::PlaceId target, core::MoveReason reason);
  /// Records a pointer value written into caller-visible memory.
  void recordStore(core::PlaceId dest, const core::ValueSource &value);
  /// Classifies a value the callee hands out (stores or returns).
  [[nodiscard]] core::ValueSource sourceOf(const ValueOrigin &origin,
                                           const core::AnalysisState &state);
  /// Summary path for `place`, ignoring parameters that were reassigned
  /// (their variable no longer holds the argument).
  [[nodiscard]] std::optional<core::SummaryPath>
  stableSummaryPathOf(core::PlaceId place);
  void finalizeSummary(const core::AnalysisState *exitState);

  // -- Reconciliation (RFC 0003) --------------------------------------------

  struct AnnotatedParam {
    core::PlaceId place;
    Annotation annotation = Annotation::Borrowed;
  };
  /// The borrowed/mutably-borrowed parameter that `place` is, or aliases.
  [[nodiscard]] std::optional<AnnotatedParam>
  borrowedParamFor(core::PlaceId place, const core::AnalysisState &state);
  void checkAnnotationOnConsume(const PlaceRef &ref, core::MoveReason reason,
                                const clang::Expr &at,
                                const core::AnalysisState &state);
  void checkAnnotationOnWrite(const PlaceRef &ref, const clang::Expr &at,
                              const core::AnalysisState &state);
  void checkAnnotationOnReturn(const ValueOrigin &origin, const clang::Expr &at,
                               const core::AnalysisState &state);
  void reportMismatch(const AnnotatedParam &param, const std::string &what,
                      core::PlaceId through, const clang::Expr &at);

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
  [[nodiscard]] std::string summaryName(const core::SummaryPath &path) const;
  [[nodiscard]] core::Diagnostic makeError(std::string_view id,
                                           std::string message,
                                           const clang::Expr &at) const;
};

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_DATAFLOW_H
