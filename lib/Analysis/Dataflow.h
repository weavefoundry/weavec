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
#include "weavec/Core/Resource.h"
#include "weavec/Core/SourceLocation.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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
  /// Parameters whose variable is assigned or address-taken in the body.
  std::vector<bool> paramReassigned;
  /// Calls whose result is discarded (a statement expression): a fresh
  /// result is leaked on the spot (RFC 0007).
  llvm::DenseSet<const clang::CallExpr *> discardedCalls;
  /// Place expressions whose pointer value is converted to an integer: the
  /// resource escapes the model (RFC 0007, *Escape*).
  llvm::DenseSet<const clang::Expr *> escapingExprs;
  /// Call expressions whose result is dereferenced on the spot (`f()->x`,
  /// `*g()`, `h()[i]`): the value lives in no place, so its nullness is
  /// checked as the call completes (RFC 0008, *Nullness*).
  llvm::DenseSet<const clang::CallExpr *> dereferencedCalls;

  // -- Liveness (RFC 0006, *Loans end at the last use of their holder*) -----

  /// Index of every local variable (parameters included) in the liveness
  /// bit vectors.
  llvm::DenseMap<const clang::VarDecl *, unsigned> liveIndex;
  /// Locals whose address is taken somewhere in the body: they may be read
  /// through the pointer at any time, so they are treated as always live.
  llvm::DenseSet<const clang::VarDecl *> addressTaken;
  /// Per block, per element: the locals live *before* that element.
  std::vector<std::vector<llvm::BitVector>> liveBefore;
  /// Per block: the locals live at its end (the union over its successors).
  std::vector<llvm::BitVector> liveOut;
  /// Per block: the locals live at its entry; what is live on the edge into
  /// it, which is narrower than the predecessor's `liveOut`.
  std::vector<llvm::BitVector> liveIn;
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
  /// The (call, pointee path) pairs whose callee writes `replayWrites` has
  /// already copied into `inferred`; a block visited again adds nothing.
  std::set<std::pair<const clang::CallExpr *, core::SummaryPath>> replayed;
  /// Per call, the places this function knows below the callee's written
  /// paths, valid while the place table has `placesSeen` entries.
  struct WrittenPlaces {
    std::optional<std::size_t> placesSeen;
    std::vector<core::PlaceId> places;
  };
  llvm::DenseMap<const clang::CallExpr *, WrittenPlaces> writtenAt;
  /// Per outcome class (RFC 0007, *Per-outcome null stores*): the caller
  /// memory null at every `return` of that class seen so far, and the
  /// caller memory holding a resource at some return of it. A `fresh` store
  /// destination held at no return of a class did not take effect on it
  /// (`if (strm == NULL) return Z_STREAM_ERROR;` before the store).
  struct NullAtReturn {
    std::set<core::SummaryPath> null;
    std::set<core::SummaryPath> held;
    /// The caller memory known non-null at every return of the class (RFC
    /// 0008, *Per-outcome non-null facts*).
    std::set<core::SummaryPath> nonNull;
  };
  std::map<core::Outcome, NullAtReturn> nullAtReturn;

  /// The consumption a call in the block being transferred performed that
  /// depends on its result (RFC 0006, *Pending outcomes*), until the result
  /// is stored somewhere or tested directly by the block's terminator.
  struct CallOutcome {
    const clang::CallExpr *call = nullptr;
    core::PendingOutcome pending;
  };
  std::optional<CallOutcome> lastCall;

  /// The block being transferred called a function that never returns (RFC
  /// 0009, *Inferred `noreturn`*): the rest of the block is dead and its
  /// state reaches no successor.
  bool blockTerminated = false;
  /// Some block hit `MaxVisitsPerBlock`: the exit state is not a fixpoint,
  /// so an unreachable exit proves nothing about termination.
  bool convergenceFailed = false;
  /// The edge being applied contradicts a must-fact of the state (`if (c)`
  /// with `c` known zero): no real path takes it, so its state reaches
  /// nobody and nothing dies on it (RFC 0009, *Scalar facts in the state*).
  bool edgeInfeasible = false;
  /// Per block: whether it calls a function that never returns, declared
  /// (`hasNoReturnElement`) or inferred (RFC 0009); computed on first use.
  std::vector<std::optional<bool>> neverReturnsCache;
  [[nodiscard]] bool blockNeverReturns(const clang::CFGBlock &block);
  /// Caller-visible integer paths this function writes anywhere (flow
  /// insensitive): a guard on one speaks about a value the caller cannot
  /// see, so it is dropped from the summary (RFC 0009, *Deriving guards*).
  std::set<core::SummaryPath> writtenScalarPaths;

  // -- Pre-passes -----------------------------------------------------------

  void collectScopes(const clang::Stmt *stmt, core::LifetimeId current);
  void classifyStmt(const clang::Stmt *stmt);
  void classifyExpr(const clang::Expr *expr, Role role);
  void markPathInterior(const clang::Expr &root);
  void noteParamAccess(const clang::Expr &place, Role role);
  void collectUnsafe(const clang::Stmt &stmt);
  void collectDiscardedCalls(const clang::Stmt *stmt);
  core::AnalysisState initialState();
  /// Backward liveness of the function's locals over the CFG, filling
  /// `liveBefore` (RFC 0006).
  void computeLiveness();

  // -- Engine ---------------------------------------------------------------

  void transfer(const clang::CFGBlock &block, core::AnalysisState &state);
  /// Drops the loans whose holder is a local that is dead before element
  /// `index` of `block`.
  void expireDeadLoans(const clang::CFGBlock &block, std::size_t index,
                       core::AnalysisState &state);
  /// Reports the resources held by locals that die before element `index`
  /// of `block` (RFC 0007, *Death points*); runs before the dead locals'
  /// alias edges go.
  void checkDeadResources(const clang::CFGBlock &block, std::size_t index,
                          core::AnalysisState &state);
  /// The liveness bit of the local that `element` writes outright (a
  /// declaration with an initialiser, `p = ...`), if any.
  [[nodiscard]] std::optional<unsigned>
  localWrittenBy(const clang::CFGElement &element) const;
  /// The same at the end of `block`, on the edge to `successor`: what is
  /// live at the block's end but not read by `successor`, and everything
  /// local when `successor` is the exit (or null).
  void checkBlockEndResources(const clang::CFGBlock &block,
                              const clang::CFGBlock *successor,
                              core::AnalysisState &state);
  /// Refines `state` with the condition of the edge from `from` to its
  /// successor `succIndex`, then checks what dies on it.
  void leaveBlock(const clang::CFGBlock &from, unsigned succIndex,
                  core::AnalysisState &state);
  void applyEdge(const clang::CFGBlock &from, unsigned succIndex,
                 core::AnalysisState &state);
  /// Refines `state` with `condition` being true (`holds`) or false.
  /// `wrapped` says the condition's value was computed before the branch
  /// (`!(c)`, `__builtin_expect(c, k)`, `(c) != 0`) rather than the branch
  /// being on the operand Clang's short-circuit CFG evaluated last.
  void applyCondition(const clang::Expr &condition, bool holds, bool wrapped,
                      core::AnalysisState &state);
  /// The edge out of a `switch` into `to`: the scrutinee equals one of the
  /// block's `case` labels, or none of them on the `default` edge (RFC
  /// 0009, *Scalar facts in the state*).
  void applySwitchEdge(const clang::SwitchStmt &statement,
                       const clang::CFGBlock &to, core::AnalysisState &state);
  /// A test of a call result on a conditional edge (RFC 0006, *Outcome
  /// tests*): the classes the edge selects for the pending outcome of the
  /// tested operand. For an integer operand the edge also narrows its
  /// scalar fact, to `constant` when the test is an equality that holds
  /// (RFC 0009), and every fact learnt refutes the guards it contradicts.
  void applyOutcomeTest(const clang::Expr &operand,
                        const std::set<core::Outcome> &selected,
                        core::AnalysisState &state,
                        std::optional<std::int64_t> constant = std::nullopt);

  // -- Scalar facts and guards (RFC 0009) -----------------------------------

  /// `place` (and its exact copies) satisfies `fact` from here on: every
  /// guarded record learns it, and the moves whose guard is refuted are
  /// reinstated, with the flow-sensitive consumption they fed.
  void learnFact(core::PlaceId place, const core::ValueFact &fact,
                 core::AnalysisState &state);
  /// The integer place `place` takes the value of `value` (unknown when
  /// null): its fact is replaced and the guards that spoke about its old
  /// value drop that conjunct.
  void assignScalar(core::PlaceId place, const clang::Expr *value,
                    core::AnalysisState &state);
  /// The integer place `place` was written in a way the model does not
  /// follow (`n++`, `n += k`, through its address).
  void forgetScalar(core::PlaceId place, core::AnalysisState &state);
  /// What is known of the integer rvalue `expr`: a constant, the fact of the
  /// place it reads (its class only through a scale), or nothing.
  [[nodiscard]] std::optional<core::ValueFact>
  scalarFactOf(const clang::Expr &expr, const core::AnalysisState &state);
  /// True if a fact about the integer place `place` is worth keeping: the
  /// storage of a local or parameter, or memory behind a pointer; not a
  /// global (any callee may write it) or an array element.
  [[nodiscard]] bool tracksScalar(core::PlaceId place) const;
  /// The storage a write to `place` lands in when `place` is below a
  /// pointer that borrows a local (`q->n` with `q = &s` is `s.n`).
  [[nodiscard]] std::vector<core::PlaceId>
  borrowedImages(core::PlaceId place, const core::AnalysisState &state);
  /// The facts of the current path as the guard of a record created here
  /// (RFC 0009, *Deriving guards*), less any conjunct on `exclude` or its
  /// exact copies (the record is about that place's new value).
  [[nodiscard]] static core::PlaceGuard
  guardHere(const core::AnalysisState &state,
            std::optional<core::PlaceId> exclude = std::nullopt);
  /// `guard` translated to this function's summary paths for a `when`
  /// clause: conjuncts on places with no stable path are dropped, which only
  /// weakens the guard (RFC 0009, *Deriving guards*).
  [[nodiscard]] core::PathGuard summaryGuardOf(const core::PlaceGuard &guard);
  /// `guard` with what the current facts decide taken out: false if some
  /// conjunct is refuted (what it protects does not happen here).
  [[nodiscard]] static bool pruneGuard(core::PlaceGuard &guard,
                                       const core::AnalysisState &state);
  /// Drops the alternatives of `origin` whose guard the facts refute and
  /// collapses a single survivor; `origin.guard` itself is pruned too.
  /// Returns false if nothing survives.
  [[nodiscard]] bool pruneOrigin(ValueOrigin &origin,
                                 const core::AnalysisState &state);
  /// Drops, from every guard in `inferred`, the conjuncts on paths this
  /// function writes: they spoke about a value the caller never saw.
  void dropUnstableGuards();
  /// `place` and its exact copies hold null (RFC 0007, *Null*).
  static void markNullWithCopies(core::PlaceId place,
                                 core::AnalysisState &state);
  /// Drops every fact below `place` and its exact copies on the edge where
  /// they are null: nothing lies below a null pointer (RFC 0006, *Null
  /// edges*).
  void forgetBelowNull(core::PlaceId place, core::AnalysisState &state);
  /// Marks null what `narrowed` says is null in every class still possible,
  /// and non-null what it says is non-null (RFC 0008).
  static void markNullOutcomes(const core::PendingOutcome &narrowed,
                               core::AnalysisState &state);
  void flushDiagnostics();
  void dump(const core::AnalysisState *exitState);

  // -- Element handlers -----------------------------------------------------

  void handleExpr(const clang::Expr &expr, core::AnalysisState &state);
  void handleDecl(const clang::DeclStmt &decl, core::AnalysisState &state);
  /// RFC 0008, *Uninitialised pointers*: a local declared without an
  /// initialiser whose address is never taken in this body, so every write
  /// to it is one this function sees.
  [[nodiscard]] bool isUninitializedLocal(const clang::VarDecl &var) const;
  /// Marks every pointer-typed field path of the record at `place`
  /// (through nested records, not arrays, unions or pointers) as
  /// uninitialised, `declared` being the declaration.
  void markUninitializedFields(core::PlaceId place,
                               const clang::RecordDecl &record,
                               const core::SourceLocation &declared,
                               core::AnalysisState &state);
  void handleAssign(const clang::BinaryOperator &assign,
                    core::AnalysisState &state);
  void handleCall(const clang::CallExpr &call, core::AnalysisState &state);
  void handleReturn(const clang::ReturnStmt &ret, core::AnalysisState &state);
  void handleLifetimeEnd(const clang::VarDecl &var,
                         const core::SourceLocation &at,
                         core::AnalysisState &state);

  // -- Resources (RFC 0007) -------------------------------------------------

  /// The forms a `leak` report takes.
  enum class LeakForm : std::uint8_t {
    /// `'p' is leaked`: its holder went out of reach.
    Lost,
    /// `'p' is leaked: it is overwritten without being released`.
    Overwritten,
    /// `'b->p' is leaked when 'b' is freed`.
    Container,
  };
  /// `place` and every descendant reachable without crossing a dereference:
  /// the memory the place's own storage holds.
  [[nodiscard]] std::vector<core::PlaceId> storageOf(core::PlaceId place);
  /// Memory below a dereference of a parameter: the caller's, never a leak
  /// candidate here, and its records outlive the parameter name's last use.
  [[nodiscard]] bool isCallerMemory(core::PlaceId place) const;
  /// The resource at `place` escapes the model: its loss is not a leak.
  void escape(core::PlaceId place, core::AnalysisState &state);
  /// Escapes the places a value names: the copied place of a copy (and the
  /// storage below it when `deep`), the storage of a borrowed object.
  void escapeValue(const ValueOrigin &origin, bool deep,
                   core::AnalysisState &state);
  /// Drops the nullness facts unchecked code handed `origin` may have
  /// changed: everything below a copied pointer, the borrowed object and
  /// everything below it (RFC 0008, *Implementation notes*).
  void forgetNullnessReachable(const ValueOrigin &origin,
                               core::AnalysisState &state);
  /// True if the resource at `place` (with `record`) is lost when every
  /// place `dying` says so goes away: nothing else reaches it.
  [[nodiscard]] static bool
  resourceLost(core::PlaceId place, const core::ResourceRecord &record,
               const std::function<bool(core::PlaceId)> &dying,
               const core::AnalysisState &state);
  /// Reports and forgets every resource among `candidates` that is lost when
  /// the places `dying` says so go away.
  void checkLeaks(const std::vector<core::PlaceId> &candidates,
                  const std::function<bool(core::PlaceId)> &dying,
                  LeakForm form, const core::SourceLocation &at,
                  core::AnalysisState &state,
                  std::optional<core::PlaceId> container = std::nullopt);
  /// A whole-place assignment to `dest`: what it held is lost unless
  /// something else reaches it.
  void checkOverwrite(core::PlaceId dest, const clang::Expr &at,
                      core::AnalysisState &state);
  /// The object `*pointer` is being freed by a shipped-table release: the
  /// resources its storage holds go with it (RFC 0007, *Owned fields*).
  void checkContainerFree(core::PlaceId pointer, const clang::Expr &at,
                          core::AnalysisState &state);
  /// The object `*pointer` is being freed by a defined or annotated
  /// destructor: what its storage holds is the destructor's to release, so
  /// the records below escape rather than being reported.
  void releaseStorageBelow(core::PlaceId pointer, core::AnalysisState &state);
  /// `'p' is released with 'free' but must be released with 'fclose'`.
  void checkReleaseFamily(core::PlaceId place, std::string_view family,
                          const clang::Expr &at,
                          const core::AnalysisState &state);
  /// RFC 0008, *Invalid releases*: the value `argument` hands to a consuming
  /// parameter of `call` (the place `ref`, when it is one) is known not to
  /// be the start of a heap allocation: the storage of a variable, a string
  /// literal, or an offset into an allocation.
  void checkInvalidRelease(const clang::Expr &argument,
                           const std::optional<PlaceRef> &ref,
                           core::MoveReason reason, const clang::Expr &at,
                           const core::AnalysisState &state);
  /// `place` now points into what it owns rather than at its start
  /// (`p++`, `p += k`).
  static void markInterior(core::PlaceId place, core::AnalysisState &state);
  /// True if `place` is the storage of a variable or the string-literal
  /// place: its root is not dereferenced on the way (`x`, `x.d`, `buf[*]`,
  /// but not `*p` or `p->f`).
  [[nodiscard]] bool isStorageOfVariable(core::PlaceId place) const;
  void reportLeak(core::PlaceId place, const core::ResourceRecord &record,
                  std::string message, const core::SourceLocation &at);
  /// The source location of element `index` of `block` (the statement, or
  /// the block's terminator / the function's end for anything else).
  [[nodiscard]] core::SourceLocation locateElement(const clang::CFGBlock &block,
                                                   std::size_t index) const;

  // -- Semantic actions -----------------------------------------------------

  void doRead(const PlaceRef &ref, const clang::Expr &at,
              core::AnalysisState &state, bool includeSelf);
  /// Returns the places marked moved (the place, its mirrors and aliases);
  /// empty if the place was already moved (reported, not re-marked). With
  /// `replaced` (RFC 0008, *Replaced values*) only the aliases are marked
  /// and the place itself is reinitialised.
  /// `guard` is what a callee's argument-conditional effect requires of the
  /// caller's places, already translated and pruned (RFC 0009); the path's
  /// own facts are added to it.
  std::vector<core::PlaceId>
  doConsume(const PlaceRef &ref, core::MoveReason reason, const clang::Expr &at,
            core::AnalysisState &state, std::string_view family = {},
            bool library = false, bool replaced = false,
            core::PlaceGuard guard = {});
  void doMutationCheck(core::PlaceId place, const clang::Expr &at,
                       core::AnalysisState &state);
  /// The variable `place` names (if it is a base place) was assigned or had
  /// its address taken: element witnesses on it are no longer reliable
  /// (RFC 0006, *Element witnesses*).
  void noteVariableWrite(core::PlaceId place, core::AnalysisState &state);
  /// `dest` (its element `element` when it is a summarised array place)
  /// receives a pointer value of the given origin.
  void applyPointerAssign(
      core::PlaceId dest, const ValueOrigin &given, const clang::Expr &at,
      bool constPointee, core::AnalysisState &state,
      core::ElementWitness element = core::ElementWitness::whole());
  /// `dest` received the result of `call`; the call's pending outcome, if
  /// any, now belongs to `dest` (RFC 0006, *Pending outcomes*).
  void attachOutcome(core::PlaceId dest, const clang::Expr *init,
                     core::AnalysisState &state);
  void applyBorrow(core::PlaceId dest, const PlaceRef &borrowed,
                   core::BorrowKind kind, const clang::Expr &at,
                   core::AnalysisState &state);
  void checkTemporaryBorrow(const PlaceRef &borrowed, core::BorrowKind kind,
                            const clang::Expr &at,
                            const core::AnalysisState &state);
  /// Forgets every fact about `place` and the places below it. With a
  /// non-whole `element`, a move record of another element of `place`
  /// survives (an element write does not reinitialise its neighbours).
  void reinit(core::PlaceId place, core::AnalysisState &state,
              core::ElementWitness element = core::ElementWitness::whole());
  /// Forgets every fact about the places strictly below `place` (the object
  /// was overwritten; RFC 0006, *`written` forgets what lies below*).
  void forgetBelow(core::PlaceId place, core::AnalysisState &state);
  /// `pointer` is about to be forgotten (reassigned or dead) while an alias
  /// still reaches its object: the resources the object refers to escape,
  /// since the references below `*pointer` go with it (RFC 0007, *Escape*).
  void loseTrackBelow(core::PlaceId pointer, core::AnalysisState &state);
  /// Drops the move records of the mirrors of `place` (the same cell under
  /// an aliased pointer): a whole write to `place` replaces what that cell
  /// held under every name (RFC 0002, aliases).
  void reinitMirrors(core::PlaceId place, core::AnalysisState &state);
  /// True if `dest` lies below a dereference of a pointer whose object
  /// nobody here owns, borrows or names (a local holding a value of unknown
  /// origin): memory reached that way belongs to whoever handed the pointer
  /// out, so a value stored there escapes (RFC 0007, *Escape*).
  [[nodiscard]] bool isBelowOpaquePointer(core::PlaceId dest,
                                          const core::AnalysisState &state);
  /// True if `dest` has no summary path but lies below a dereference of a
  /// local that borrows caller memory or a global (`tb = &L->strt;
  /// tb->hash = p`): the store landed in an object that outlives this
  /// function, under a name the summary cannot report (RFC 0007, *Escape*).
  [[nodiscard]] bool
  isBelowBorrowOfCallerMemory(core::PlaceId dest,
                              const core::AnalysisState &state);
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
  /// `dest = f(...)` for a record: the callee's `result` stores become
  /// assignments to the fields of `dest` (RFC 0008, *Struct-by-value
  /// results*).
  void applyResultStores(core::PlaceId dest, const clang::CallExpr &call,
                         core::AnalysisState &state);
  void setKind(core::PlaceId place, core::OwnershipKind kind,
               core::AnalysisState &state);

  // -- Calls (RFC 0003) -----------------------------------------------------

  /// Applies a resolved callee summary: consumption, borrows for the call,
  /// stores through arguments and into globals.
  void applySummary(const clang::CallExpr &call, const CallEffects &effects,
                    core::AnalysisState &state);
  /// Records into `lastCall` the consumption `applySummary` performed for
  /// `call` that the callee's outcomes make conditional on its result.
  void notePendingOutcome(
      const clang::CallExpr &call, const core::FunctionSummary &summary,
      const std::vector<
          std::pair<core::SummaryPath, std::vector<core::PlaceId>>>
          &consumedTargets);
  /// Handles a call across the checking boundary (no summary): the RFC 0003
  /// warning by default, a raw operation under `--strict-externs` (RFC
  /// 0004, *Boundaries*).
  void handleUncheckedCall(const clang::CallExpr &call,
                           core::AnalysisState &state);
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

  // -- Nullness (RFC 0008) --------------------------------------------------

  /// What `WEAVEC_NULLABLE` / `WEAVEC_NONNULL` on the variable, parameter or
  /// field `place` names declares, if anything.
  [[nodiscard]] std::optional<core::Nullness>
  declaredNullness(core::PlaceId place) const;
  /// The nullness fact for `place`: its record, or what its declaration
  /// says when it has none.
  [[nodiscard]] std::optional<core::NullRecord>
  nullnessAt(core::PlaceId place, const core::AnalysisState &state) const;
  /// The nullness a value with `origin` gives its destination, if any: a
  /// null constant, a callee result or store with a `null` alternative, or a
  /// copy of a place with a fact (RFC 0008, *Sources of facts*).
  [[nodiscard]] std::optional<core::NullRecord>
  nullnessOf(const ValueOrigin &origin, const clang::Expr &at,
             const core::AnalysisState &state);
  /// Records `record` for `place` and its exact copies. A record that may be
  /// null and carries no guard of its own gets the path's facts as one (RFC
  /// 0009, *Deriving guards*).
  static void setNullness(core::PlaceId place, const core::NullRecord &record,
                          core::AnalysisState &state);
  /// A callee's store into `dest` at `call` may have left it null: the
  /// record says so in the callee's name (`CalleeStore`).
  void noteCalleeStore(core::PlaceId dest, const clang::CallExpr &call,
                       core::AnalysisState &state);
  /// `pointer` is dereferenced at `at`: reports a null or possibly-null
  /// pointer (once per path), and records the requirement when the pointer
  /// is a parameter about which nothing is known.
  void checkDereference(core::PlaceId pointer, const clang::Expr &at,
                        core::AnalysisState &state);
  /// `pointer` was dereferenced at `at` with nothing known about it: from
  /// here on it is non-null.
  void markDereferenced(core::PlaceId pointer, const clang::Expr &at,
                        core::AnalysisState &state);
  /// The result of `call` is dereferenced without being stored first.
  void checkResultDereference(const clang::CallExpr &call,
                              core::AnalysisState &state);
  /// The arguments `summary.requiresNonNull` names must be non-null at
  /// `call`.
  void checkRequiredArguments(const clang::CallExpr &call,
                              const core::FunctionSummary &summary,
                              core::AnalysisState &state);
  /// Records that this function requires `place` (a parameter root or an
  /// exact copy of one) to be non-null.
  void noteRequirement(core::PlaceId place, const core::AnalysisState &state);
  /// `'p' may be null: it is the result of 'f' here`, for the note.
  [[nodiscard]] static std::string nullNote(const core::NullRecord &record,
                                            std::string_view name);

  // -- Summary recording (RFC 0003) -----------------------------------------

  [[nodiscard]] bool recording() const noexcept {
    return phase == Phase::Final;
  }
  /// Marks the summary path of `place` (and of its mirrors) as read or
  /// written, when it names caller memory.
  void recordAccess(core::PlaceId place, bool write,
                    const core::AnalysisState &state);
  /// Records what a callee wrote below `pointee`, the object argument
  /// `argument` points to, as this function's writes: the callee's written
  /// paths below `param(argument)*`, or the pointee itself when the summary
  /// has none (an annotation).
  void replayWrites(const clang::CallExpr &call, const PlaceRef &pointee,
                    std::uint32_t argument,
                    const core::FunctionSummary &summary,
                    const core::AnalysisState &state);
  /// Records a release/move of `target` as it happens in the state's
  /// flow-sensitive `consumed` map, which the outcome classes read at each
  /// `return` and the unconditional effects at the exit.
  void recordConsume(core::PlaceId target, core::MoveReason reason,
                     std::string_view family,
                     const core::ElementWitness &element,
                     const core::PlaceGuard &guard, core::AnalysisState &state);
  /// This function overwrote `place` outright on the current path: what the
  /// caller's memory held there on entry is gone (RFC 0008, *Replaced
  /// values*; `state.overwritten`).
  void noteOverwritten(core::PlaceId place, core::AnalysisState &state);
  /// This function wrote `place` (any element) after consuming the caller's
  /// value there on the current path: the consume is `replaced` on this
  /// path (RFC 0008, *Replaced values*; `state.consumed[path].replaced`).
  void noteRewritten(core::PlaceId place, core::AnalysisState &state);
  /// True if `path` describes the callee's own copy of an argument rather
  /// than the caller's memory (RFC 0003, *Deriving a summary*): parameter
  /// roots and paths under reassigned parameters. Such a path is never
  /// `replaced` (RFC 0008).
  [[nodiscard]] bool isEventBased(const core::SummaryPath &path) const;
  /// The consumption in force at `state`, by summary path: the union of
  /// `state.consumed` (as it happened) and `state.moves` (what the places
  /// still hold); RFC 0006 *Outcome-conditional summaries*, RFC 0008
  /// *Replaced values*.
  [[nodiscard]] core::OutcomeEffects
  consumptionAt(const core::AnalysisState &state);
  /// Records the outcome classes of `return value` and the consumption on
  /// this path for each (final pass).
  void recordOutcomes(const clang::Expr &value, const ValueOrigin &origin,
                      const core::AnalysisState &state);
  /// The stable summary path of `place` if it names caller memory (below a
  /// dereference of a parameter, or a global).
  [[nodiscard]] std::optional<core::SummaryPath>
  callerVisiblePath(core::PlaceId place);
  /// For `return p != NULL`, `return !p` and their negations: the tested
  /// place and the integer class the function returns when it is null.
  [[nodiscard]] std::optional<std::pair<const clang::Expr *, core::Outcome>>
  nullTestReturn(const clang::Expr &value) const;
  /// Records a pointer value written into caller-visible memory.
  void recordStore(core::PlaceId dest, const core::ValueSource &value);
  /// `return s` for a record: one `result`-rooted store per pointer field
  /// path of `s`'s storage with a known source (RFC 0008, *Struct-by-value
  /// results*).
  void recordResultStores(const PlaceRef &returned,
                          const core::AnalysisState &state);
  /// Classifies a value the callee hands out (stores or returns), guarded by
  /// the path's facts and the origin's own (RFC 0009).
  [[nodiscard]] core::ValueSource sourceOf(const ValueOrigin &origin,
                                           const core::AnalysisState &state);
  /// `sourceOf` without the guard.
  [[nodiscard]] core::ValueSource
  sourceValueOf(const ValueOrigin &origin, const core::AnalysisState &state);
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
  /// The targets of a consume of element `element` of `place`, each with
  /// the element witness the record on it gets (RFC 0006, *Element
  /// witnesses*): the place and its mirrors keep `element`; an alias gets
  /// the element of it the alias edge names, and is skipped when the edge
  /// names another element of `place` than the access did.
  struct ConsumeTarget {
    core::PlaceId place;
    core::ElementWitness element;
  };
  [[nodiscard]] std::vector<ConsumeTarget>
  consumeTargets(core::PlaceId place, core::ElementWitness element,
                 const core::AnalysisState &state);
  /// Whether this function has said anything about `place` (or a mirror or
  /// alias of it): named it, aliased it, or recorded a resource, move or
  /// null fact there. RFC 0007, *Applying a summary: deepest paths first*.
  [[nodiscard]] bool knowsPlace(core::PlaceId place,
                                core::ElementWitness element,
                                const core::AnalysisState &state);
  [[nodiscard]] std::vector<core::PlaceId>
  mirrors(core::PlaceId place, const core::AnalysisState &state);
  /// `place` together with its ancestors and descendants.
  [[nodiscard]] std::vector<core::PlaceId> related(core::PlaceId place);

  struct MovedHit {
    core::PlaceId target;
    core::MoveRecord record;
  };
  /// The move record of `place` if it is moved and the record's element
  /// witness matches the access's (`Whole` matches everything).
  [[nodiscard]] static std::optional<MovedHit>
  findMoved(core::PlaceId place, const core::AnalysisState &state,
            core::ElementWitness element = core::ElementWitness::whole());
  /// A loan that conflicts with a move or mutation of `place` (`kind`
  /// unset) or with a new borrow of it. Loans on the place's ancestors count
  /// unless `ancestors` is false: freeing what `s.buf` points to leaves a
  /// borrow of `s` intact.
  [[nodiscard]] std::optional<core::Loan>
  findLoanConflict(core::PlaceId place, std::optional<core::BorrowKind> kind,
                   const core::AnalysisState &state, bool ancestors = true);

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
