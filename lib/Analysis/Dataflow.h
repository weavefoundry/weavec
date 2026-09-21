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
#include "weavec/Analysis/BypassedDeclarations.h"
#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Lifetime.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Resource.h"
#include "weavec/Core/SourceLocation.h"
#include "weavec/Core/Summary.h"
#include "weavec/Core/Traversal.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

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
  /// summary is produced either way. Everything the analysis publishes goes
  /// through `ledgerAdapter` (RFC 0030 §14): diagnostics with their
  /// certainty, and, when it is authoritative, the decisions of the final
  /// pass.
  FunctionDataflow(clang::ASTContext &ctx, const clang::FunctionDecl &fn,
                   LedgerAdapter &ledgerAdapter,
                   const AnalysisOptions &analysisOptions,
                   SummaryStore &summaryStore, bool emitDiags);

  /// Runs the analysis over `fn`'s body, publishes its diagnostics and
  /// decisions through the adapter (if enabled) and computes the summary.
  void run();
  core::CallbackBindings callbackBindings;
  core::CallContext memoryContext;
  bool validMemoryContext = true;

  /// RFC 0030 §5.5: the CFG blocks `run` transferred, fixpoint and final
  /// pass together, and whether it stopped over its budget (or without a
  /// fixpoint).
  [[nodiscard]] std::uint64_t transfers() const noexcept {
    return blockTransfers;
  }
  [[nodiscard]] bool overBudget() const noexcept { return convergenceFailed; }

  /// The summary inferred by `run` (RFC 0003, *Deriving a summary*).
  [[nodiscard]] const core::FunctionSummary &summary() const & noexcept {
    return inferred;
  }
  [[nodiscard]] core::FunctionSummary summary() && noexcept {
    return std::move(inferred);
  }

private:
  // RFC 0027: most mirror queries return one place. Larger results grow
  // normally; inline capacity never limits alias expansion.
  using MirrorPlaces = llvm::SmallVector<core::PlaceId, 4>;

  [[nodiscard]] std::vector<core::PlaceId>
  scalarArrayOverlaps(core::PlaceId place,
                      const core::AnalysisState &state) const;
  [[nodiscard]] std::optional<core::CallContext>
  captureCallContext(const clang::CallExpr &call,
                     const core::FunctionSummary &summary,
                     core::AnalysisState &state);
  void initializeCallContext(core::AnalysisState &state);
  [[nodiscard]] std::optional<std::pair<core::PlaceId, clang::QualType>>
  contextPlace(const core::SummaryPath &path, const core::AnalysisState &state);
  std::map<const clang::CallExpr *, core::CallContext> memoryContexts;
  std::map<core::SummaryPath, core::PointerOffset> contextEntryOffsets;
  core::PointerOffset contextOffsetOf(core::PlaceId place,
                                      const core::AnalysisState &state);
  // RFC 0015: complete array cells share the ordinary pointer/heap domains.
  [[nodiscard]] std::optional<core::PlaceId>
  boundedArrayCell(core::PlaceId storage, const core::ArrayIndex &index,
                   const clang::Expr &at, core::AnalysisState &state);
  [[nodiscard]] PlaceRef selectArrayElement(PlaceRef storage,
                                            std::optional<core::Affine> index,
                                            clang::QualType type,
                                            const clang::Expr &at);
  [[nodiscard]] std::optional<std::string>
  summaryArrayIndex(std::string_view selector);
  void snapshotArrayIndex(core::PlaceId place, const clang::Expr *at,
                          core::AnalysisState &state);
  void initializeArray(core::PlaceId storage, clang::QualType type,
                       const clang::Expr *init, const clang::VarDecl &decl,
                       core::AnalysisState &state, bool zeroInitialize = false);
  void initializeArrayValue(core::PlaceId cell, clang::QualType type,
                            const clang::Expr *value,
                            const clang::VarDecl &decl,
                            core::AnalysisState &state, bool zeroInitialize);
  std::map<core::PlaceId, clang::QualType> arrayTypes;
  std::map<std::pair<core::PlaceId, const clang::Expr *>, core::PlaceId>
      arrayIndexSnapshots;
  // RFC 0020: places are append-only within this run; parse each selector once.
  std::size_t indexedArrayPlaces = 0;
  std::map<core::PlaceId,
           std::vector<std::pair<core::PlaceId, core::ArrayIndex>>>
      arrayCellsByIndex;
  struct ArrayBuffer {
    core::PlaceId storage;
    clang::QualType element;
    core::Affine start;
    bool explicitArray = false;
  };
  [[nodiscard]] std::optional<ArrayBuffer>
  arrayBuffer(const clang::Expr &expr, core::AnalysisState &state);
  [[nodiscard]] bool handleArrayCopy(const clang::CallExpr &call,
                                     const CallEffects &effects,
                                     core::AnalysisState &state);
  void copyArrayCell(core::PlaceId dest, core::PlaceId source,
                     clang::QualType type, const clang::CallExpr &at,
                     core::AnalysisState &state);
  std::map<std::pair<const clang::CallExpr *, std::int64_t>, core::PlaceId>
      arrayCopySnapshots;
  void materializeArrayCell(core::PlaceId storage, core::PlaceId cell,
                            const core::ArrayIndex &index, clang::QualType type,
                            const clang::Expr &at, core::AnalysisState &state);
  bool installArrayRange(const ArrayBuffer &dest, const ArrayBuffer &source,
                         core::Affine count, const clang::CallExpr &call,
                         core::AnalysisState &state, std::size_t ordinal = 0,
                         bool definite = true);
  void applyArrayRanges(const clang::CallExpr &call,
                        const core::FunctionSummary &summary,
                        core::AnalysisState &state);
  void recordArrayOutputs(const core::AnalysisState &state);
  void recordArrayResult(core::PlaceId result,
                         const core::AnalysisState &state);
  void applyArrayResult(core::PlaceId result, const clang::CallExpr &call,
                        core::AnalysisState &state);
  std::map<const clang::CallExpr *, std::map<core::SummaryPath, core::PlaceId>>
      arrayResultOutputs;
  [[nodiscard]] clang::QualType arrayElementType(core::PlaceId storage);
  std::map<std::pair<const clang::CallExpr *, std::size_t>, core::PlaceId>
      arrayRangeSnapshots;
  std::map<core::PlaceId, const clang::CallExpr *> arrayRangeSites;
  void snapshotArrayCell(core::PlaceId source, core::PlaceId target,
                         clang::QualType type, core::AnalysisState &state);
  void captureArrayReallocation(const clang::CallExpr &call,
                                const CallEffects &effects,
                                core::AnalysisState &state);
  void applyArrayReallocation(core::PlaceId dest, const clang::CallExpr &call,
                              core::AnalysisState &state);
  std::map<const clang::CallExpr *, core::PlaceId> arrayReallocInputs;
  struct ArrayCleanupLoop {
    const clang::CallExpr *release;
    const clang::ArraySubscriptExpr *element;
    const clang::Expr *count;
    bool cleared;
  };
  std::map<const clang::ForStmt *, ArrayCleanupLoop> arrayCleanupLoops;
  struct ArrayFillLoop {
    const clang::BinaryOperator *assignment = nullptr;
    const clang::ArraySubscriptExpr *element = nullptr;
    const clang::Expr *count = nullptr;
    std::optional<std::int64_t> bytes;
  };
  std::map<const clang::ForStmt *, ArrayFillLoop> arrayFillLoops;
  std::map<std::pair<const clang::Expr *, std::size_t>, core::PlaceId>
      arrayFillSites;
  std::map<core::PlaceId, const clang::Expr *> arrayFillExpressions;
  void fillArrayRange(core::PlaceId storage, core::Affine count,
                      std::optional<std::int64_t> bytes, const clang::Expr &at,
                      core::AnalysisState &state, std::size_t ordinal = 0,
                      bool definite = true);
  void materializeArrayFill(core::PlaceId storage, core::PlaceId cell,
                            const core::ArrayIndex &index,
                            const clang::Expr &at, core::AnalysisState &state);
  void applyArrayFills(const clang::CallExpr &call,
                       const core::FunctionSummary &summary,
                       core::AnalysisState &state);
  std::set<const clang::CallExpr *> arrayCleanupCalls;
  /// The expressions of the bodies of those loops, which the loop's model
  /// replaces (`Role::Ignore`); RFC 0030 §15 item 4 still decides their
  /// sites (`decideLoopBodySite`).
  std::set<const clang::Expr *> arrayLoopExprs;
  /// §15 item 4: the facets of a site in the body of an array fill or
  /// cleanup loop, from the state inside the loop, without its effects.
  void decideLoopBodySite(const clang::Expr &expr, core::AnalysisState &state);
  std::set<const clang::BinaryOperator *> arrayCleanupStores;
  std::map<std::pair<const clang::Expr *, std::size_t>, core::PlaceId>
      arrayReleaseSites;
  std::map<core::PlaceId, const clang::Expr *> arrayReleaseExpressions;
  void collectArrayCleanupLoops(const clang::Stmt *stmt);
  void completeArrayCleanupLoop(const clang::CFGBlock &from, unsigned succIndex,
                                core::AnalysisState &state);
  void releaseArrayRange(core::PlaceId storage, core::ArraySpan span,
                         bool cleared, const clang::Expr &at,
                         core::AnalysisState &state, std::size_t ordinal = 0);
  void materializeArrayRelease(core::PlaceId storage, core::PlaceId cell,
                               const core::ArrayIndex &index,
                               const clang::Expr &at,
                               core::AnalysisState &state);
  void applyArrayReleases(const clang::CallExpr &call,
                          const core::FunctionSummary &summary,
                          core::AnalysisState &state);
  void weakenOverlappingArrayWrites(core::PlaceId dest,
                                    const ValueOrigin &origin,
                                    const clang::Expr &at, bool constPointee,
                                    core::AnalysisState &state);
  void forgetArrayStorage(core::PlaceId place, core::AnalysisState &state);
  void checkArrayTraversal(core::PlaceId storage, const core::Affine &count,
                           const clang::Expr &at, core::AnalysisState &state);
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

  /// RFC 0030 §8.2: `retain`, `reads` and `invalidates` of the row.
  void applyLibraryState(const clang::CallExpr &call,
                         const core::LibraryMatch &library,
                         core::AnalysisState &state);
  /// §5.3: `sync` callbacks as may-effects; false when a target is unknown.
  bool applyLibraryCallbacks(const clang::CallExpr &call,
                             const core::LibraryMatch &library,
                             core::AnalysisState &state);
  /// §8.2: `pointer` is a hidden state slot or a copy of one.
  [[nodiscard]] bool fromLibraryState(core::PlaceId pointer,
                                      const core::AnalysisState &state);
  /// Consumes applied now are may-effects: conditional, never settled.
  bool mayEffects = false;
  /// RFC 0030 §8: the `LibrarySpec` row that governs `call` (directly, or
  /// through its one known target), once `resolveCall` has resolved it.
  [[nodiscard]] const core::LibraryMatch *
  resolvedLibrary(const clang::CallExpr &call) const;
  [[nodiscard]] core::DifferenceConstraints
  differenceConstraints(const core::AnalysisState &state);
  [[nodiscard]] bool provedAtMost(const core::Affine &lhs,
                                  const core::Affine &rhs,
                                  const core::AnalysisState &state);

  clang::ASTContext &context;
  const clang::FunctionDecl &function;
  LedgerAdapter &ledger;
  const AnalysisOptions &options;
  SummaryStore &summaries;
  bool materializingArray = false;
  bool materializingArrayFill = false;
  bool materializingArrayRelease = false;
  /// Inside the alternative state of `weakenOverlappingArrayWrites`: a store
  /// that may not have gone to this cell (§7.4).
  bool weakeningArrayWrite = false;
  const bool emitDiagnostics;

  [[nodiscard]] std::optional<core::PlaceGuard>
  translateIntegerGuard(const core::PathGuard &guard,
                        const clang::CallExpr &call,
                        const core::AnalysisState &state);
  void recordIntegerCondition(const clang::Expr &lhs, core::IntegerOp op,
                              const clang::Expr &rhs,
                              core::AnalysisState &state);
  void handleIntegerCompound(const clang::CompoundAssignOperator &expr,
                             core::AnalysisState &state);
  void specializeIntegerBuiltin(const clang::CallExpr &call,
                                const core::LibraryMatch &library,
                                core::FunctionSummary &summary,
                                core::AnalysisState &state);
  bool handleCheckedIntegerCall(const clang::CallExpr &call,
                                core::AnalysisState &state);
  void applyIntegerRange(const clang::Expr &expr,
                         const core::IntegerRange &allowed,
                         core::AnalysisState &state);
  std::map<const clang::Expr *, std::optional<core::PlaceId>>
      integerStatementResults;
  void recordNumericOutputs(const clang::Expr *value,
                            const core::AnalysisState &state);
  void prepareNumericCall(const clang::CallExpr &call,
                          const core::FunctionSummary &summary,
                          core::AnalysisState &state);
  void finishNumericCall(const clang::CallExpr &call,
                         core::AnalysisState &state);
  std::map<
      const clang::CallExpr *,
      std::map<core::SummaryPath, std::map<core::Outcome, core::ValueFact>>>
      numericCallOutcomeFacts;
  std::map<const clang::CallExpr *, std::map<core::SummaryPath, core::PlaceId>>
      numericCallOutputs;
  [[nodiscard]] std::optional<core::PlaceId>
  numericCallResult(const clang::CallExpr &call) const;
  [[nodiscard]] std::optional<core::Affine>
  byteSum(const core::Affine &lhs, const core::Affine &rhs,
          const core::AnalysisState &state);
  [[nodiscard]] std::optional<core::IntegerExpression<core::PlaceId>>
  byteExpression(const core::Affine &value, const core::AnalysisState &state);
  [[nodiscard]] core::SpatialRecord
  subobjectRecord(const core::SpatialRecord &record, core::PlaceId source,
                  const core::PointerOffset &step);
  void captureVariableArray(core::PlaceId place, const clang::VarDecl &var,
                            core::AnalysisState &state);
  void captureVariableArrayType(clang::TypeSourceInfo *info,
                                core::AnalysisState &state,
                                const clang::Expr *initializer = nullptr);
  std::map<const clang::VariableArrayType *, core::PlaceId> variableArrayCounts;
  bool checkVariableArray(const clang::Expr &expr, core::AnalysisState &state);
  [[nodiscard]] std::optional<core::IntegerExpression<core::PlaceId>>
  variableArraySize(clang::QualType type, const core::AnalysisState &state);
  std::map<const clang::Expr *, core::SpatialCheck> spatialChecks;
  void recordSpatialCheck(const clang::Expr &at, core::SpatialCheck check);
  [[nodiscard]] std::pair<std::optional<std::int64_t>,
                          std::optional<std::int64_t>>
  integerBounds(core::PlaceId place, const core::AnalysisState &state);
  using NumericExpression = core::IntegerExpression<core::PlaceId>;
  // RFC 0017: values read at call entry, independent of the post-state
  // paths. Slots are bounded by call site, interface path and integer type.
  using NumericInputKey = std::pair<core::SummaryPath, core::IntegerType>;
  std::map<const clang::CallExpr *, std::map<NumericInputKey, core::PlaceId>>
      numericInputs;
  std::set<const clang::CallExpr *> numericInputsReady;
  void captureNumericInputs(const clang::CallExpr &call,
                            const core::FunctionSummary &summary,
                            core::AnalysisState &state);
  [[nodiscard]] std::optional<NumericExpression>
  numericInput(const clang::CallExpr &call, const core::SummaryPath &path,
               core::IntegerType type, const core::AnalysisState &state);
  std::map<core::PlaceId, NumericExpression> numericExpressions;
  std::map<NumericExpression, core::PlaceId> expressionPlaces;
  std::map<core::PlaceId, core::PlaceId> numericEntryValues;
  [[nodiscard]] core::IntegerRangeEvaluation
  evaluateNumericExpression(const NumericExpression &expression,
                            const core::AnalysisState &state);
  [[nodiscard]] bool operationDoesNotOverflow(core::IntegerOp op,
                                              const NumericExpression &lhs,
                                              const NumericExpression &rhs,
                                              core::IntegerType type,
                                              const core::AnalysisState &state);
  std::map<core::PlaceId, core::IntegerExpression<core::SummaryPath>>
      numericSnapshotExpressions;
  [[nodiscard]] std::optional<NumericExpression>
  integerExpressionOf(const clang::Expr &expr, const core::AnalysisState &state,
                      unsigned depth = 0);
  [[nodiscard]] std::optional<core::Affine>
  integerAffineOf(const clang::Expr &expr, core::AnalysisState &state);
  [[nodiscard]] std::optional<core::Affine>
  linearIntegerExpression(const NumericExpression &expression,
                          const core::AnalysisState &state,
                          bool upperEnvelope = false);
  [[nodiscard]] core::Affine
  internIntegerExpression(const NumericExpression &expression,
                          core::AnalysisState &state);
  [[nodiscard]] std::optional<core::Affine>
  instantiateIntegerExpression(const core::PathAffine &value,
                               const clang::CallExpr &call,
                               core::AnalysisState &state);
  [[nodiscard]] std::optional<core::IntegerExpression<core::SummaryPath>>
  summaryIntegerExpression(const NumericExpression &expression);
  void snapshotIntegerDependencies(core::PlaceId place, const clang::Expr *at,
                                   core::AnalysisState &state);
  core::AnalysisState *currentState = nullptr;
  std::map<const clang::CallExpr *,
           std::shared_ptr<const core::FunctionSummary>>
      callSummaries;
  core::CallMemoryFootprintCache callFootprints;
  llvm::DenseMap<std::pair<const void *, const std::string *>,
                 std::weak_ptr<const core::FunctionSummary>>
      validatedObjectViews;
  std::map<
      const clang::CallExpr *,
      std::map<core::SummaryPath, std::weak_ptr<const core::FunctionSummary>>>
      validatedObjectPaths;
  std::size_t validatedObjectPathCount = 0;
  std::map<const clang::CallExpr *, SummarySource> callSources;
  std::map<const clang::CallExpr *, core::LibraryMatch> callLibraries;
  std::map<const clang::CallExpr *, core::CallTargets> callTargetsSeen;
  /// RFC 0030 §9.3: how the solved slots resolve each indirect call.
  std::map<const clang::CallExpr *, core::CallResolution> callResolutions;
  std::map<const clang::CallExpr *, core::CallbackBindings> callbackContexts;
  [[nodiscard]] core::CallTargets functionTargets(const clang::Expr &expr,
                                                  core::AnalysisState &state,
                                                  unsigned depth = 0);
  [[nodiscard]] core::CallTargets
  originTargets(const ValueOrigin &origin, const core::AnalysisState &state);
  /// RFC 0030 §9.3: the solution the indirect calls are resolved through:
  /// the program's when the link step or `--whole-program` solved it, the
  /// unit's own otherwise. Null when the engine was given none.
  [[nodiscard]] const core::SlotSolution *solvedSlots() const;
  /// RFC 0030 §9.3: what the solved slot says about an indirect call. A
  /// direct call, or a unit without slots, gets `ClosedEmpty` and no
  /// targets, which the caller reads as "the slots say nothing".
  [[nodiscard]] core::CallResolution
  slotResolutionOf(const clang::CallExpr &call) const;
  /// §9.3: the targets the solved slots give a function pointer loaded from
  /// the global or field `place` names; none when it has no slot key. An
  /// open slot's targets are joined with the unknown target, so a caller
  /// keeps the §5.1 alternative.
  [[nodiscard]] std::optional<core::CallTargets>
  slotTargetsOf(core::PlaceId place) const;
  [[nodiscard]] std::optional<ResolvedSummary>
  resolveCall(const clang::CallExpr &call);
  [[nodiscard]] static std::string
  objectEvidenceView(core::PlaceId holder, const core::AnalysisState &state);
  [[nodiscard]] bool validateObjectPath(const core::SummaryPath &path,
                                        const clang::CallExpr &call);
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
  std::map<const clang::Expr *, std::pair<const clang::CallExpr *, unsigned>>
      dynamicArguments;
  /// Parameters whose variable is assigned or address-taken in the body.
  std::vector<bool> paramReassigned;
  /// Calls whose result is discarded (a statement expression): a fresh
  /// result is leaked on the spot (RFC 0007).
  llvm::DenseSet<const clang::CallExpr *> discardedCalls;
  /// Place expressions whose pointer value is converted to an integer: the
  /// resource escapes the model (RFC 0007, *Escape*).
  llvm::DenseSet<const clang::Expr *> escapingExprs;
  /// Lvalues whose address (or decayed array) is passed to a consuming
  /// parameter: `release(&o->in)`, `release(o->payload)`. The pointer they
  /// derive from is what is consumed (RFC 0011, *Deriving a pointer*), so
  /// walking through it is not a separate use: a second release is one
  /// `double-free`, not that and a `use-after-free`.
  llvm::DenseSet<const clang::Expr *> consumedDerivations;
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
  /// operations are permitted (RFC 0004), and a dereference there is
  /// trusted and refines nothing (RFC 0030 §6.1).
  bool inUnsafe;
  /// Ownership annotations on local variables, consumed for the assertion
  /// rule (RFC 0004, *Laundering*).
  std::map<core::PlaceId, AnnotationSet> declaredKinds;

  std::shared_ptr<clang::CFG> cfg;
  std::vector<std::optional<core::AnalysisState>> entryStates;

  Phase phase = Phase::Fixpoint;
  /// RFC 0013: final reachable heap state and caller materialization.
  bool materializingHeap = false;
  /// A diagnostic of the final pass, flushed through the adapter at its end
  /// (RFC 0030 §14): with its certainty and the site and facet it is about.
  struct PendingReport {
    core::Diagnostic diagnostic;
    core::Certainty certainty = core::Certainty::Definite;
    const clang::Stmt *site = nullptr;
    std::optional<core::Facet> facet = std::nullopt;
  };
  std::vector<PendingReport> pending;
  std::map<core::PlaceId, core::OwnershipKind> summaryKinds;

  void mirrorHeapWrite(core::PlaceId place, core::AnalysisState &state);
  [[nodiscard]] core::PathGuard
  heapEntryGuard(const core::PlaceGuard &guard,
                 const core::AnalysisState &state);
  [[nodiscard]] core::PathGuard
  heapWriteGuard(core::PlaceId place, const core::AnalysisState &state);
  std::map<core::PlaceId, core::SummaryPath> snapshotInputPaths;
  [[nodiscard]] MirrorPlaces definiteMirrors(core::PlaceId place,
                                             const core::AnalysisState &state);
  std::map<std::pair<const clang::CallExpr *, core::SummaryPath>, core::PlaceId>
      heapInputs;
  std::map<std::pair<const clang::CallExpr *, core::SummaryPath>, bool>
      heapInputEscaped;
  std::set<core::PlaceId> pointerSnapshots;
  std::set<core::PlaceId> resultHeapInputs;
  void retireHeapInputs(core::AnalysisState &state);
  void restoreHeapInput(const core::PendingOutcome::PendingStore &store,
                        core::AnalysisState &state);
  void copyHeapValue(core::PlaceId source, core::PlaceId target,
                     core::AnalysisState &state);
  void captureHeapInputs(const clang::CallExpr &call,
                         const core::FunctionSummary &summary,
                         core::AnalysisState &state);
  [[nodiscard]] std::optional<ValueOrigin>
  heapOrigin(const core::ValueSource &value, const clang::CallExpr &call,
             const core::FunctionSummary &summary);
  [[nodiscard]] core::HeapDescription
  describeHeap(core::PlaceId root, bool pointer,
               const core::AnalysisState &state, const clang::Expr *at);
  void recordHeapResult(const ValueOrigin &origin, const clang::Expr &at,
                        const core::AnalysisState &state);
  void recordHeapOutputs(const core::AnalysisState &state);
  [[nodiscard]] bool isHeapOutputPath(const core::SummaryPath &path) const;
  void applyHeap(core::PlaceId dest, const core::HeapDescription &graph,
                 const clang::CallExpr &call,
                 const core::FunctionSummary &summary,
                 core::AnalysisState &state);
  void applyHeapValue(core::PlaceId dest, const ValueOrigin &origin,
                      core::AnalysisState &state);
  void applyHeapResult(core::PlaceId dest, const clang::CallExpr &call,
                       core::AnalysisState &state);
  void applyHeapOutputs(const clang::CallExpr &call,
                        const core::FunctionSummary &summary,
                        core::AnalysisState &state);

  /// RFC 0013: bounded names for integer values before a write.
  std::map<std::pair<core::PlaceId, const clang::Expr *>, core::PlaceId>
      valueSnapshots;
  std::set<core::PlaceId> snapshotPlaces;
  [[nodiscard]] std::optional<core::Affine>
  foldAffine(std::optional<core::Affine> value,
             const core::AnalysisState &state);
  void snapshotScalar(core::PlaceId place, const clang::Expr *at,
                      core::AnalysisState &state);

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
    /// Those of `places` the callee wrote without a store saying what it
    /// left there: a pointer among them holds a value the caller cannot
    /// name, and no longer the one it held on entry.
    std::vector<core::PlaceId> unnamedValue;
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
  /// RFC 0030 §9.2: per pointer parameter, the classes of the returns at
  /// which it is not proven non-null; and every class some return produces.
  std::map<unsigned, std::set<core::Outcome>> paramNullClasses;
  std::set<core::Outcome> returnClasses;

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
  /// RFC 0030 §5.5: the run went over its budget of block transfers, or some
  /// block hit `MaxVisitsPerBlock` (no fixpoint): it stopped, and the
  /// function takes the defaults.
  bool convergenceFailed = false;
  /// RFC 0030 §5.5: the block transfers so far.
  std::uint64_t blockTransfers = 0;
  /// §5.5: the defaults and the unknown-callee summary of a function over
  /// its budget.
  void finishOverBudget();
  /// The edge being applied contradicts a must-fact of the state (`if (c)`
  /// with `c` known zero): no real path takes it, so its state reaches
  /// nobody and nothing dies on it (RFC 0009, *Scalar facts in the state*).
  bool edgeInfeasible = false;
  /// Whether `sitesByOperand` is built (RFC 0030 §14).
  bool sitesByOperandBuilt = false;
  /// Set while `decidePathBounds` (and the other decision-only bounds
  /// passes) run: no requirement, dump count or incompleteness is recorded.
  bool boundsDecisionOnly = false;
  /// Per block: whether it calls a function that never returns, declared
  /// (`hasNoReturnElement`) or inferred (RFC 0009); computed on first use.
  std::vector<std::optional<bool>> neverReturnsCache;
  [[nodiscard]] bool blockNeverReturns(const clang::CFGBlock &block);
  /// Caller-visible integer paths this function writes anywhere (flow
  /// insensitive): a guard on one speaks about a value the caller cannot
  /// see, so it is dropped from the summary (RFC 0009, *Deriving guards*).
  std::set<core::SummaryPath> writtenScalarPaths;

  // -- Sized fields (RFC 0012) ----------------------------------------------

  /// A place that is a field of a named record: the place of the object it
  /// belongs to, the field, and the field's count-field key (`struct
  /// buf.data`; empty when the record has no stable spelling).
  struct FieldPlace {
    core::PlaceId object;
    const clang::FieldDecl *field = nullptr;
    std::string key;
  };
  /// One store of a pointer into a field of a named record (`o->f = v`),
  /// flow insensitive, and what it says for the inference: a null store
  /// says nothing; a store whose extent some sibling count is (or comes
  /// to be, at that count's write) equal to witnesses that pair; any other
  /// store refutes the field (RFC 0012, *Sized fields*, "Inference").
  struct FieldPointerStore {
    core::PlaceId place;
    FieldPlace field;
    core::SourceLocation location;
    bool null = false;
    /// The stored value's extent, when it is `{X, s, 0}` for a place `X`.
    std::optional<core::Affine> extent;
    /// The sibling count's key and the scale, when the store itself decided
    /// it (the count was already equal to `X`).
    std::optional<std::pair<std::string, std::int64_t>> witnessed;
    std::optional<core::IntegerType> productType = std::nullopt;
  };
  std::vector<FieldPointerStore> fieldPointerStores;
  /// A write of a count `o->g` that found the pointer sibling `o->f`
  /// holding an object of `{X, s, 0}` bytes with `X` now equal to the count:
  /// the store of that object into `o->f` is witnessed by `g`.
  struct CountWitness {
    core::PlaceId pointer;
    core::Affine extent;
    std::string count;
    std::optional<core::IntegerType> productType = std::nullopt;
  };
  std::vector<CountWitness> countWitnesses;
  /// Integer fields of named records this function writes (`o->g = e`,
  /// `o->g++`), with the object's place: a pointer sibling not stored here
  /// is not counted by them.
  struct FieldScalarWrite {
    core::PlaceId place;
    FieldPlace field;
    core::SourceLocation location;
  };
  std::vector<FieldScalarWrite> fieldScalarWrites;
  /// Whether the two are being recorded (the final pass only: the
  /// fixpoint's passes would record every store several times).
  [[nodiscard]] bool recordsSizedFields() const noexcept;

  // -- Shares (RFC 0010) ----------------------------------------------------

  /// Operands of an adjustment (`x` in `x++`, `x += 1`): the adjustment
  /// itself updates the scalar fact, so the operand's read-write role must
  /// not forget it first.
  llvm::DenseSet<const clang::Expr *> adjustedOperands;
  /// RFC 0011: pointer operands of `++p`, `p--`, `p += k`, with the offset
  /// the expression moves them by.
  llvm::DenseMap<const clang::Expr *, core::PointerOffset> pointerSteps;
  /// Integer places this function subtracts one from, itself or through a
  /// callee (flow insensitive): a `free` of the object such a place lies in,
  /// on a path whose facts say it is zero, releases a share rather than the
  /// object (RFC 0010, *Releasing a share*).
  std::set<core::PlaceId> decrementedPlaces;
  /// Per outcome class, the caller-visible stores and integer facts at the
  /// returns of the class seen so far (RFC 0010, *Per-outcome stores* and
  /// *Per-outcome integer facts*): stores are a may-fact per class (the
  /// union over its returns), facts a must-fact (joined over its returns;
  /// a path with a fact at only some returns is dropped).
  struct StoredAtReturn {
    std::set<core::SummaryPath> stored;
    std::map<core::SummaryPath, core::ValueFact> facts;
    bool anyReturn = false;
  };
  std::map<core::Outcome, StoredAtReturn> storedAtReturn;

  /// RFC 0010, *Recognising increments and decrements*: `x` was adjusted by
  /// `delta` at `at`. Updates the scalar fact, records the adjustment for
  /// the summary and, for an increment of a field of `*o`, retains `o`.
  void handleAdjustment(const PlaceBuilder::Adjustment &adjustment,
                        const clang::Expr &at, core::AnalysisState &state);
  /// The pointer whose object the integer place `count` is a field of
  /// (`o` for `o->rc` and `o->base.refs`), if the steps from the dereference
  /// down to `count` are all fields; with the count-field key of the field
  /// (empty when the type has no stable spelling).
  struct CountedObject {
    core::PlaceId pointer;
    std::string key;
  };
  [[nodiscard]] std::optional<CountedObject>
  countedObjectOf(core::PlaceId count);
  /// The count-field key for the field path `steps` of the object of type
  /// `pointee`; `struct obj` alone when `steps` is empty.
  [[nodiscard]] std::string
  countKeyFor(clang::QualType pointee,
              llvm::ArrayRef<core::PathElem> steps) const;
  /// `pointer` retains its object through the count `key` at `at` (RFC 0010,
  /// *Retaining*): its record gains a share or a `Retained` one is made.
  static void retain(core::PlaceId pointer, std::string key,
                     const core::SourceLocation &at,
                     core::AnalysisState &state);
  /// Applies the `increments` and `decrements` of a callee's summary at
  /// `call`: the argument places are retained and the adjustments are this
  /// function's too (wrappers compose).
  void applyAdjustments(const clang::CallExpr &call,
                        const core::FunctionSummary &summary,
                        core::AnalysisState &state);
  /// RFC 0010, *Releasing a share*: a `free` of `pointer` on a path whose
  /// facts (and `guard`) say a decremented integer field of `*pointer` is
  /// zero is a share release; returns the count place.
  [[nodiscard]] std::optional<core::PlaceId>
  zeroCountBelow(core::PlaceId pointer, const core::PlaceGuard &guard,
                 const core::AnalysisState &state);
  /// RFC 0010, *Recognising an `unref` body*: a consume of a parameter root
  /// guarded by a decremented count of its object being zero is a share
  /// release; marks it `share`, records the count path and drops the
  /// consumes of the object's contents that carry the same conjunct.
  void recogniseShareReleases();
  /// Whether `key` is a known count (RFC 0010, *Leaks of shares*): a
  /// `WEAVEC_REFCOUNT` field or one some analysed function releases through.
  [[nodiscard]] bool isKnownCount(std::string_view key) const;

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
  /// RFC 0030 §8.4: `block` (null: the function's end) returns from `main`.
  [[nodiscard]] bool returnsFromMain(const clang::CFGBlock *block) const;
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
  /// `x OP k` on an integer, decided in a type of `width` bits, on the edge
  /// where it `holds` (RFC 0009); `x` may be an adjustment whose value is
  /// the place at an offset (RFC 0010).
  void testInteger(const clang::Expr &x, clang::BinaryOperatorKind op,
                   std::int64_t k, bool holds, bool unsignedComparison,
                   unsigned width, core::AnalysisState &state);
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
                    core::AnalysisState &state,
                    const clang::Expr *at = nullptr);
  /// The integer place `place` was written in a way the model does not
  /// follow (`n++`, `n += k`, through its address).
  void forgetScalar(core::PlaceId place, core::AnalysisState &state,
                    const clang::Expr *at = nullptr);
  /// What is known of the integer rvalue `expr`: a constant, the fact of the
  /// place it reads (its class only through a scale), or nothing.
  [[nodiscard]] std::optional<core::ValueFact>
  scalarFactOf(const clang::Expr &expr, const core::AnalysisState &state);
  [[nodiscard]] std::optional<core::IntegerRangeEvaluation>
  integerRangeOf(const clang::Expr &expr, const core::AnalysisState &state,
                 unsigned depth = 0);
  [[nodiscard]] core::IntegerRange
  integerRangeAt(core::PlaceId place, core::IntegerType type,
                 const core::AnalysisState &state);
  [[nodiscard]] bool preservesInteger(const clang::Expr &expr,
                                      const core::AnalysisState &state);
  void checkIntegerOperation(const clang::Expr &expr,
                             core::AnalysisState &state);
  bool refineIntegerComparison(const clang::Expr &lhs,
                               clang::BinaryOperatorKind op,
                               const clang::Expr &rhs, bool holds,
                               core::AnalysisState &state);

  /// True if a fact about the integer place `place` is worth keeping: the
  /// storage of a local or parameter, or memory behind a pointer; not a
  /// global (any callee may write it) or an array element.
  [[nodiscard]] bool tracksScalar(core::PlaceId place) const;
  void stepPointer(core::PlaceId place, const core::PointerOffset &step,
                   core::AnalysisState &state, const clang::Expr &at);
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
  /// RFC 0017: every numeric premise must survive the combined guard limit
  /// or follow from scalar facts retained in the guard itself.
  [[nodiscard]] static bool
  integerGuardComplete(const core::PlaceGuard &guard,
                       const core::AnalysisState &state,
                       std::optional<core::PlaceId> exclude = std::nullopt);
  /// `guard` translated to this function's summary paths for a `when`
  /// clause: conjuncts on places with no stable path are dropped, which only
  /// weakens the guard (RFC 0009, *Deriving guards*).
  [[nodiscard]] bool
  summaryGuardComplete(const core::PlaceGuard &guard,
                       const core::PathGuard &projectedGuard);
  [[nodiscard]] core::PathGuard summaryGuardOf(const core::PlaceGuard &guard);
  /// `guard` with what the current facts decide taken out: false if some
  /// conjunct is refuted (what it protects does not happen here).
  [[nodiscard]] bool pruneGuard(core::PlaceGuard &guard,
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
  /// RFC 0030 §3.1: the records of the places every class still possible
  /// consumes, whatever the arguments, are no longer conditional.
  static void settleConsumed(const core::PendingOutcome &narrowed,
                             core::AnalysisState &state);
  /// RFC 0009, *Guards*: a place the classes still possible consume only
  /// under a guard on the arguments keeps that guard on its move record, or
  /// is reinstated (through `reinstate`) where the facts here refute it.
  void applyOutcomeGuards(
      const core::PendingOutcome &narrowed, core::AnalysisState &state,
      const std::function<void(const std::vector<core::PlaceId> &)> &reinstate);
  /// Retracts the stores `narrowed` says did not happen on the classes
  /// still possible and applies the integer facts that hold on all of them
  /// (RFC 0010, *Per-outcome stores* and *Per-outcome integer facts*).
  void applyOutcomeStores(core::PendingOutcome &narrowed,
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
  /// A callee kept a copy of the value at `place` out of the summary's sight
  /// (RFC 0010, *Stores out of sight*): escapes it and its storage, and
  /// records the same for this function's caller.
  void escapeOutOfSight(core::PlaceId place, core::AnalysisState &state);
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
  [[nodiscard]] bool
  resourceLost(core::PlaceId place, const core::ResourceRecord &record,
               const std::function<bool(core::PlaceId)> &dying,
               const core::AnalysisState &state);
  /// Reports and forgets every resource among `candidates` that is lost when
  /// the places `dying` says so go away. A `Retained` record (RFC 0010) is
  /// reported only when its count field is a known count.
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
  /// `calleeOffset` (RFC 0011) is where the callee releases relative to
  /// what it is passed (`free_container(&o->in)`: `-outer.in`, composing
  /// with the argument's `+outer.in` to the start).
  void checkInvalidRelease(const clang::Expr &argument,
                           const std::optional<PlaceRef> &ref,
                           core::MoveReason reason, const clang::Expr &at,
                           const core::AnalysisState &state,
                           const core::PointerOffset &calleeOffset = {},
                           bool certain = true);
  /// RFC 0011: the spatial record of a borrow of `storage` at `offset`: the
  /// size of the variable, array or field borrowed, when it is complete.
  [[nodiscard]] std::optional<core::SpatialRecord>
  storageRecordOf(const PlaceRef &storage, const core::PointerOffset &offset);
  /// RFC 0030 §7.4: the record of a pointer into a variable's sub-object
  /// (`&s.f`, `&m[i][j]`, `s.arr` decayed): the complete variable's extent,
  /// and where in it the pointer points, in elements of what it points to
  /// (somewhere inside when a subscript on the way is not known).
  [[nodiscard]] std::optional<core::SpatialRecord>
  completeStorageRecordOf(const PlaceRef &storage,
                          const core::PointerOffset &offset);
  /// True if `place` is the storage of a local variable or parameter (not
  /// memory behind a pointer, a global or the literal place): what RFC
  /// 0011's deferred lifetime check watches die.
  [[nodiscard]] bool isLocalStorage(core::PlaceId place) const;
  /// RFC 0011: an extent over this function's places as one over its
  /// interface, when its place has a stable summary path.
  [[nodiscard]] std::optional<core::PathAffine>
  summaryAffineOf(const std::optional<core::Affine> &affine);
  /// RFC 0011, *Deferred lifetime checks*: the storage of `dying` locals is
  /// going; every loan on it whose holder survives it and whose holder's
  /// lifetime the loan does not outlive is `lifetime-too-short`, reported at
  /// the store that created it.
  void checkOutlivedLoans(const std::function<bool(core::PlaceId)> &dying,
                          const core::AnalysisState &state);
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

  /// With `reportMoved` off, a pointer walked through that was freed is not
  /// a use (the consume that walks it reports the double free).
  void doRead(const PlaceRef &ref, const clang::Expr &at,
              core::AnalysisState &state, bool includeSelf,
              bool reportMoved = true);
  /// Returns the places marked moved (the place, its mirrors and aliases);
  /// empty if the place was already moved (reported, not re-marked). With
  /// `replaced` (RFC 0008, *Replaced values*) only the aliases are marked
  /// and the place itself is reinitialised.
  /// `guard` is what a callee's argument-conditional effect requires of the
  /// caller's places, already translated and pruned (RFC 0009); the path's
  /// own facts are added to it.
  /// With `share` (RFC 0010, *Releasing a share*) one share of the object
  /// is released rather than the object: a holder with a surplus keeps its
  /// name, a `Retained` holder stays valid, any other is `Released`.
  /// `offset` (RFC 0011) is where the released pointer points relative to
  /// the value at `ref`: what the summary records as the consume's `at`.
  std::vector<core::PlaceId>
  doConsume(const PlaceRef &ref, core::MoveReason reason, const clang::Expr &at,
            core::AnalysisState &state, std::string_view family = {},
            bool library = false, bool replaced = false,
            core::PlaceGuard guard = {}, bool share = false,
            const core::PointerOffset &offset = {},
            core::MoveOrigin origin = {});
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
  /// The loan part of `applyBorrow`: the loans on `target` (and its mirrors)
  /// held by `dest` (and its mirrors).
  /// Also what a derived copy `&p->f` gives its holder on `(*p).f` (RFC
  /// 0011, *Derived pointers*).
  void lend(core::PlaceId dest, core::PlaceId target, core::BorrowKind kind,
            core::LifetimeId loanLifetime, const clang::Expr &at,
            core::AnalysisState &state);
  /// True if `holder` is a plain local whose loans liveness retires (RFC
  /// 0006): not address-taken, not memory behind a pointer, not a global.
  [[nodiscard]] bool isLivenessTracked(core::PlaceId holder) const;
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
                     const core::PointerOffset &offset,
                     core::AnalysisState &state);
  /// `dest = value` for a record: field-wise pointer copies when `value` is
  /// a place, field-wise assignments when it is an initializer list, and a
  /// reset otherwise (RFC 0005, *Struct copies*).
  void copyRecordPlaces(core::PlaceId dest, core::PlaceId source,
                        core::AnalysisState &state);
  bool handleMemoryCopy(const clang::CallExpr &call, const CallEffects &effects,
                        core::AnalysisState &state);
  /// RFC 0030 §2.3 `raw-cast`: a non-pointer store to `lvalue` (resolved to
  /// `written`) that rewrites bytes of a pointer object, or a union member
  /// beside a pointer member, leaves those pointers reinterpreted.
  void noteReinterpretingStore(const clang::Expr &lvalue,
                               const PlaceRef &written,
                               core::AnalysisState &state);
  /// RFC 0030 §15 item 3: the engine could not model `at`. The summary
  /// records `reason` as an incompleteness, and the facet the construct
  /// feeds (spatial for integer and extent modelling, temporal otherwise)
  /// of the site `at` stands for, or of the innermost site around it, is
  /// `unresolved` with the reason `core::incompletenessReason` gives and
  /// `reason` as the detail. Nothing is reported.
  void decideIncomplete(const std::string &reason, const clang::Stmt &at);
  /// The facet an incompleteness leaves undecidable: what the construct the
  /// engine could not model feeds.
  [[nodiscard]] static core::Facet incompleteFacet(llvm::StringRef reason);
  std::map<const clang::CallExpr *, core::PlaceId> memorySnapshots;

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
      const std::vector<std::pair<core::SummaryPath,
                                  std::vector<core::PlaceId>>> &consumedTargets,
      std::vector<std::pair<core::PlaceId, std::optional<core::PlaceEffect>>>
          localEvents);

  // -- Code WeaveC cannot see (RFC 0030 §5; DataflowUnknown.cpp) ------------

  /// The declared type of `place`: its variable's or field's, or what its
  /// parent points to or holds; none when the builder cannot name it.
  [[nodiscard]] std::optional<clang::QualType>
  placeType(core::PlaceId place) const;
  /// Whether `place` holds a pointer, from its declaration; unknown for a
  /// place whose type the builder cannot name.
  [[nodiscard]] std::optional<bool> holdsPointer(core::PlaceId place) const;
  /// §3.1, §9.4: `place` is a field or global some function of the unit
  /// releases a value loaded from.
  [[nodiscard]] bool isOwningPlace(core::PlaceId place);
  /// §3.1: the key of the type `pointer` points to, for the effective-type
  /// rule (`AnalysisState::AnyType` for a character, `void` or unknown one).
  [[nodiscard]] std::uint64_t pointeeTypeKey(core::PlaceId pointer) const;
  /// §3.1: the object `released` points to was released on this path.
  void noteRelease(core::PlaceId released, core::AnalysisState &state);
  /// §3.1, *Aliases of a released object*: an access through `pointer`,
  /// which has no move record, may reach an object released earlier on some
  /// path: `pointer` comes from a parameter- or global-rooted place and is
  /// not provably distinct from every object released so far.
  [[nodiscard]] bool mayAliasReleased(core::PlaceId pointer,
                                      const core::AnalysisState &state);
  /// §3.1: a value of `origin` stored now is not a released object: it is
  /// not a copy of a value older than the last release.
  [[nodiscard]] bool storedSinceRelease(const ValueOrigin &origin,
                                        const core::AnalysisState &state);
  /// §5.1: `place` and every name holding the same value get a release
  /// record of unknown origin, unless they have a record. `reached`: the
  /// callee reached the place through a pointee, so an uninitialised record
  /// gives way (the callee may have written it).
  void markUnknown(core::PlaceId place, const core::SourceLocation &here,
                   bool reached, core::AnalysisState &state);
  /// §5.1: nothing known about `object` and what lies below it holds: its
  /// nullness, extents, scalar facts and call targets.
  void forgetReachableFacts(core::PlaceId object, core::AnalysisState &state);
  /// §5.1 (the lazy default): everything below `object` is now unknown.
  /// One entry in `AnalysisState::unknownBelow` stands for the release
  /// record every place below it would have had, and covers the places this
  /// function names only after the call.
  void markUnknownBelow(core::PlaceId object, core::AnalysisState &state);
  /// §5.1: the record a place below an unknown object inherits, unless this
  /// function has given it, or a place above it, a value since. None when
  /// nothing covers `place`, or when it holds no pointer (the record only
  /// ever says a pointer's object may be gone).
  [[nodiscard]] std::optional<core::MoveRecord>
  inheritedUnknown(core::PlaceId place, const core::AnalysisState &state) const;
  /// §5.1: `place` holds a value this function gave it, so it and what lies
  /// below it no longer inherit an unknown object's record.
  void noteEstablished(core::PlaceId place, core::AnalysisState &state) const;
  /// Whether `object` lies above `place` in the place tree.
  [[nodiscard]] bool isBelow(core::PlaceId place, core::PlaceId object) const;
  /// §5.1: the places one call's unknown effects mark and the objects whose
  /// facts they forget, collected while `unknownBatch` is set and applied
  /// once by `flushUnknown`: marking first (the mirrors of a subtree are
  /// shared), then one forgetting pass over the outermost objects.
  struct UnknownBatch {
    std::vector<std::pair<core::PlaceId, bool>> marks;
    std::vector<core::PlaceId> objects;
  };
  UnknownBatch *unknownBatch = nullptr;
  void flushUnknown(UnknownBatch &batch, const core::SourceLocation &here,
                    core::AnalysisState &state);
  /// `forgetReachableFacts` for the places `reached` (an object and what
  /// lies below it), with one scan of the guards.
  void forgetFactsOf(std::vector<core::PlaceId> reached,
                     core::AnalysisState &state);
  /// Every place a numeric expression of this function, or a numeric value,
  /// condition or extent of `state` names, sorted: only for such a place can
  /// `snapshotIntegerDependencies` or `snapshotScalar` do anything.
  [[nodiscard]] std::vector<core::PlaceId>
  numericNames(const core::AnalysisState &state) const;
  /// §5.1: the unknown-callee default for one pointer handed to unknown
  /// code: its holders get unknown-origin release records, and through a
  /// pointee that is not `readOnly`, so does every pointer cached there and
  /// the facts there are gone. The value's own nullness and extent stay.
  void applyUnknownToValue(const ValueOrigin &value, bool readOnly,
                           const core::SourceLocation &here,
                           core::AnalysisState &state);
  /// §5.1: every escaped place and every pointer global the unknown code
  /// can reach.
  void applyUnknownToReachable(const core::SourceLocation &here,
                               core::AnalysisState &state);
  /// §5.1: the Call site's temporal facet `unresolved(unknown-callee)`, with
  /// the suggestion for the first argument `uncovered` by a contract.
  void decideUnknownCall(const clang::CallExpr &call,
                         std::optional<unsigned> uncovered);
  /// §5.1: a direct callee with no body, program summary, table entry, and
  /// not declared in a platform header.
  [[nodiscard]] bool isExternCallee(const clang::FunctionDecl &callee) const;
  /// §5.1, §5.5, after a known summary is applied: its `unknown` effects,
  /// the default for the pointer parameters of an external callee without
  /// an ownership contract (else `trusted(extern-contract)`), and an
  /// incomplete summary's may-effects on every argument.
  void applyUnknownEffects(const clang::CallExpr &call,
                           const CallEffects &effects,
                           core::AnalysisState &state);
  /// §5.1: the code the unknown-callee default being applied stands for
  /// (the callee's name, `inline assembly`), which its records keep.
  std::string unknownCode;
  /// RFC 0030 §9.3: the unknown code named by `unknownCode` is reached
  /// through a function pointer, so the records it makes say `callback`.
  bool unknownIsCallback = false;
  /// `calleeName` without its quotes.
  [[nodiscard]] std::string unquotedCalleeName(const clang::CallExpr &call);
  /// §5.7: an `asm` statement's pointer operands get the unknown-callee
  /// default, and a `"memory"` clobber reaches every escaped place and
  /// reachable global.
  void handleAsm(const clang::GCCAsmStmt &stmt, core::AnalysisState &state);
  /// RFC 0030 §6.2: when `call` is `WEAVEC_ASSUME(e)`, decides its assertion
  /// facet (proven when the facts refute `!e`; a violation, the
  /// `contradicted-assumption` error, when they refute `e`; checked
  /// otherwise) and assumes `e` from here on. False for any other call.
  bool handleAssumption(const clang::CallExpr &call,
                        core::AnalysisState &state);
  /// Handles a call across the checking boundary (no summary): §5.2 for a
  /// platform function, else the unknown-callee default (§5.1).
  void handleUncheckedCall(const clang::CallExpr &call,
                           core::AnalysisState &state);
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
  /// RFC 0030 §3.2, §8.4: an allocation's result, `record`, is used at `at`
  /// without a null test (`allocation-failure`, off by default).
  void reportAllocationFailure(const core::NullRecord &record,
                               const clang::Expr &at, const SiteInfo *site);
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

  // -- Bounds (RFC 0011, *Bounds checks*) -----------------------------------

  /// What an lvalue expression touches: the bytes from `start` to `end`
  /// past the value of `base` (a pointer-valued expression, or null for the
  /// storage of variable `storage`), each affine in one integer place at
  /// most. `index` is the subscript or arithmetic operand spelled, for the
  /// message.
  struct Access {
    const clang::Expr *base = nullptr;
    const clang::VarDecl *storage = nullptr;
    core::Affine start;
    core::Affine end;
    const clang::Expr *index = nullptr;
  };
  /// The access `lvalue` makes (`p[i]`, `*(p + i)`, `p->f`, `s.a[i]`,
  /// `q->buf[i]`), or nothing when its shape is not one the check reads.
  [[nodiscard]] std::optional<Access> accessOf(const clang::Expr &lvalue);
  /// What a pointer argument points at, and where in it (`buf`, `&buf[2]`,
  /// `&s.f`, `p`, `p + 1`), as an access of no bytes yet.
  [[nodiscard]] std::optional<Access>
  argumentAccessOf(const clang::Expr &argument);
  /// Reports `out-of-bounds` when the object `lvalue` reads or writes is
  /// known to be too small (RFC 0011, *Bounds checks*), and records the
  /// requirement when it is a parameter's of unknown extent.
  void checkBounds(const clang::Expr &lvalue, core::AnalysisState &state);
  /// The extent and its origin the object behind `base` has, if known:
  /// the spatial record of the pointer's place, or a variable's size.
  struct KnownExtent {
    core::Affine have;
    core::SourceLocation origin;
    /// The pointer place the record belongs to, when there is one.
    std::optional<core::PlaceId> pointer;
    /// The pointer's offset from the start, in elements of `unit` bytes.
    core::PointerOffset offset;
    /// The size of what the base pointer points to, set by the caller.
    std::optional<std::int64_t> unit;
    /// Whether `origin` is a declaration (a variable, a `WEAVEC_SIZED_BY`
    /// parameter) rather than an allocation, for the note.
    bool declared = false;
    /// RFC 0030 §7.1: exact, declared or a lower bound
    /// (`SpatialRecord::extentClass`).
    core::ExtentClass extentClass = core::ExtentClass::Exact;
    /// The pointer expression the access measured from (`Access::base`),
    /// when it is one.
    const clang::Expr *base = nullptr;
    /// The pointer was made from a member of the object (`p->data`), whose
    /// own start a message measures from (RFC 0030 §7.4 bounds it by the
    /// whole object).
    bool fromMember = false;

    [[nodiscard]] bool exact() const noexcept {
      return extentClass == core::ExtentClass::Exact;
    }
  };
  [[nodiscard]] std::optional<KnownExtent>
  knownExtentOf(const Access &access, const core::AnalysisState &state);
  /// `affine` with a constant the facts know substituted for its place.
  [[nodiscard]] static core::Affine
  foldAffine(const core::Affine &affine, const core::AnalysisState &state);
  /// What comparing an access's need with a known extent found.
  struct BoundsEvaluation {
    core::SpatialCheck check;
    std::optional<core::BoundsVerdict> verdict;
    /// The need and the have as compared (folded; the need shifted by an
    /// offset relation), and the need as written, for messages.
    core::Affine need;
    core::Affine have;
    core::Affine spelled;
    std::optional<core::Relation> between;
    std::int64_t relationOffset = 0;
    /// The have was an allocation's size expression, bounded above.
    bool convertedUpperBound = false;
    /// Bytes from the object's start to where the pointer points.
    std::int64_t shift = 0;
    /// A call that needs nothing at all.
    bool nothingNeeded = false;
  };
  [[nodiscard]] std::optional<std::int64_t>
  memberArrayOffset(const clang::Expr &at) const;
  /// Compares `need` bytes (from the start of the access, `accessStart` for
  /// a call) against `known`, without reporting or deciding anything.
  /// Nothing when the pointer's offset takes the access out of the check.
  [[nodiscard]] std::optional<BoundsEvaluation>
  evaluateBounds(const core::Affine &need, const KnownExtent &known,
                 const clang::Expr &at, const clang::CallExpr *call,
                 const core::AnalysisState &state,
                 std::optional<core::Affine> accessStart = std::nullopt);
  /// Compares `need` against `known.have` under the facts and reports with
  /// `subject` (`'p[i]'`, `'memcpy' accesses`) when they decide against it.
  /// Returns whether something was reported.
  bool reportBounds(const core::Affine &need, const KnownExtent &known,
                    const clang::Expr &at, std::string_view subject,
                    std::string_view accessed, const clang::Expr *index,
                    const clang::CallExpr *call,
                    const core::AnalysisState &state, bool lowerBound = false,
                    std::optional<core::Affine> accessStart = std::nullopt);
  /// `need` in a local index that a relation puts at or below a parameter
  /// (`i < n`), restated at the boundary in that parameter; nothing when no
  /// such relation holds.
  [[nodiscard]] std::optional<core::PathAffine>
  boundaryRequirement(const core::Affine &need,
                      const core::AnalysisState &state);
  /// RFC 0017: only canonical loops that reach their boundary can project a
  /// local index into a caller requirement. Cached independently of CFG facts.
  [[nodiscard]] bool loopBoundaryEligible(const core::Affine &need);
  std::map<const clang::VarDecl *, bool> loopBoundaryEligibility;
  /// Records that this function requires `need` bytes behind `pointer`
  /// when it is a parameter root (RFC 0011, *Extents in summaries*).
  void noteExtentRequirement(core::PlaceId pointer, const core::Affine &need,
                             const core::AnalysisState &state,
                             const core::PlaceGuard *extra = nullptr,
                             std::optional<core::Affine> start = std::nullopt);
  /// The arguments `summary.requiresExtent` names must be large enough.
  void checkRequiredExtents(const clang::CallExpr &call,
                            const core::FunctionSummary &summary,
                            const core::AnalysisState &state);
  /// Learns `lhs OP rhs` (`holds` says which edge) about two integer places
  /// (RFC 0011, *Relations*).
  void learnRelation(const clang::Expr &lhs, clang::BinaryOperatorKind op,
                     const clang::Expr &rhs, bool holds,
                     core::AnalysisState &state);
  /// The name of an integer place for a bounds message: the variable, or
  /// the expression as written.
  [[nodiscard]] std::string spellIndex(const clang::Expr *index,
                                       const core::Affine &affine);

  // -- Sized fields (RFC 0012, *Sized fields*) --------------------------------
  // Implemented in DataflowSizedFields.cpp.

  /// The `FieldPlace` `place` is, if it is a field of a named record.
  [[nodiscard]] std::optional<FieldPlace> fieldPlaceOf(core::PlaceId place);
  /// What counts the pointer field `place`: the place of the sibling count
  /// (`o->cap` for `o->data`), the bytes per element, and whether it comes
  /// from `WEAVEC_SIZED_BY` (else from a confirmed inference). Nothing for
  /// a field nothing counts. A malformed annotation is reported here, once
  /// per unit.
  struct SizedFieldPlace {
    core::PlaceId count;
    std::int64_t unit = 1;
    bool annotated = false;
    std::optional<core::IntegerType> productType = std::nullopt;
  };
  [[nodiscard]] std::optional<SizedFieldPlace>
  sizedFieldPlaceOf(core::PlaceId place);
  [[nodiscard]] std::pair<std::optional<core::Affine>,
                          std::optional<core::IntegerType>>
  sizedFieldExtent(const core::Affine &extent,
                   const core::AnalysisState &state);
  /// The spatial record of `place`: the state's, or, for a sized field with
  /// none, the record its count implies (RFC 0012, *Sized fields*, "Loads").
  [[nodiscard]] std::optional<core::SpatialRecord>
  spatialRecordAt(core::PlaceId place, const core::AnalysisState &state);
  /// `o->f = v` with `f` a pointer field of a named record: remembers the
  /// store for the inference and, for an annotated field, checks the
  /// value's extent against the count when it is decided here ("Stores").
  void noteFieldPointerStore(core::PlaceId dest, const clang::Expr &at,
                             const core::AnalysisState &state);
  /// `o->g = e` with `g` an integer field: remembers the write for the
  /// inference, decides the pending stores of the sibling pointer fields
  /// whose extent the new value now counts, and checks the annotated ones.
  void noteFieldScalarWrite(core::PlaceId place, const clang::Expr *at,
                            const core::AnalysisState &state);
  /// At the end of the analysis: the witnesses and refutations of RFC
  /// 0012's inference, from the stores and writes remembered.
  void finalizeSizedFields(const core::AnalysisState *exitState);
  /// `'b->data' is declared WEAVEC_SIZED_BY(cap) but is given 4 bytes where
  /// 'b->cap' says 8` when `have` (the stored value's extent) is decided
  /// smaller than the count's; returns whether it reported.
  bool checkSizedFieldStore(core::PlaceId dest, const SizedFieldPlace &sized,
                            const core::SpatialRecord &record,
                            const core::SourceLocation &at,
                            const core::AnalysisState &state);
  /// `8 bytes`, `'n' bytes`, `'n' * 4 + 4 bytes`.
  [[nodiscard]] std::string spellBytes(const core::Affine &amount);

  // -- Strings (RFC 0012, *String facts*) -----------------------------------
  // Implemented in DataflowStrings.cpp.

  /// The object a `char *` argument points into, as the string tracker sees
  /// it: the place whose spatial record carries the object's string fact,
  /// the argument's byte offset into it, and what is known of its extent.
  struct StringSubject {
    /// The pointer place, or an array's storage place.
    core::PlaceId key;
    /// Bytes from the object's start to where the argument points.
    std::int64_t offset = 0;
    /// The object's extent in bytes, when known, and its origin (for
    /// `reportBounds`).
    std::optional<KnownExtent> extent;
    /// The name of the object for a message (`buf`, `p`).
    std::string name;
  };
  /// `stringSubjectOf(E)` for a pointer-valued expression at an element
  /// offset the tracker follows (zero or a constant number of bytes);
  /// nothing for a literal, a field offset, or an unknown one.
  [[nodiscard]] std::optional<StringSubject>
  stringSubjectOf(const clang::Expr &arg, const core::AnalysisState &state);
  /// The bytes before the terminator of the string `arg` points at, when
  /// known: a literal's, or the subject's length seen from its offset.
  [[nodiscard]] std::optional<core::Affine>
  stringLengthOf(const clang::Expr &arg, const core::AnalysisState &state);
  /// True if the object `arg` points into is known to hold no terminator.
  [[nodiscard]] std::optional<core::StringFact>
  stringFactOf(const clang::Expr &arg, const core::AnalysisState &state);
  /// Every name of the object behind `key` the fact is set on: the place,
  /// its exact aliases, the storage it borrows, and the holders of loans on
  /// that storage.
  [[nodiscard]] std::vector<core::PlaceId>
  stringTargets(core::PlaceId key, const core::AnalysisState &state);
  /// Sets (or, with nothing, drops) the string fact of the object behind
  /// `key` under every name.
  void setStringFact(core::PlaceId key,
                     const std::optional<core::StringFact> &fact,
                     core::AnalysisState &state);
  /// `s`'s string changed in a way the tracker does not follow: its length
  /// place (if any) is forgotten with it.
  void dropStringFact(core::PlaceId key, core::AnalysisState &state);
  /// RFC 0012, *Sources of string facts*: what a library call establishes
  /// about the strings behind its arguments, applied after the call's other
  /// effects (`strcpy`, `strcat`, `sprintf`, `strncpy`, `memset`, `strlen`,
  /// ...); every `w` argument not listed loses its facts.
  void applyStringEffects(const clang::CallExpr &call,
                          const core::FunctionSummary &summary,
                          core::AnalysisState &state);
  /// RFC 0012, *String checks*: the needs of `strcpy`, `strcat` and
  /// `sprintf` against the destination's extent, and terminator-seeking
  /// reads of an unterminated object.
  void checkStringArguments(const clang::CallExpr &call,
                            const core::FunctionSummary &summary,
                            const core::AnalysisState &state);
  /// `d[i] = c`: a byte store into an object the tracker follows.
  void noteByteStore(const clang::Expr &lvalue, const clang::Expr *value,
                     core::AnalysisState &state);
  /// `char a[N] = "..."`, `char a[] = {...}`: the initialiser's string.
  void initStringStorage(core::PlaceId storage, const clang::VarDecl &var,
                         core::AnalysisState &state);
  /// RFC 0030 §8: a row term over the string facts before `call`: `strlen`
  /// from the facts, `fmtlen` from a literal format (at least its value
  /// when `lowerBound` is set), the rest as `libraryValue` evaluates it.
  [[nodiscard]] std::optional<core::Affine>
  stringTermValue(const core::LibTerm &term, const clang::CallExpr &call,
                  const core::LibraryMatch &library,
                  const core::AnalysisState &state, bool &lowerBound);
  /// RFC 0012: `strdup(s)`'s result has `s`'s length and one more byte, when
  /// the length is known; the extent and string fact of the fresh object.
  [[nodiscard]] std::optional<std::pair<core::Affine, core::StringFact>>
  duplicatedStringOf(const clang::CallExpr &call,
                     const core::AnalysisState &state);
  /// Whether `a >= b` is decided by the facts (true, false, or nothing):
  /// constants, one place against a bound, two places against a relation.
  [[nodiscard]] static std::optional<bool>
  decideAtLeast(const core::Affine &a, const core::Affine &b,
                const core::AnalysisState &state);
  /// What a `printf` format, with the arguments it is given, is known to
  /// produce: at least `lower` bytes (exactly, when `exact`), and which
  /// arguments `%s` reads to their terminators.
  struct FormatNeed {
    core::Affine lower = core::Affine::ofConstant(0);
    bool exact = true;
    std::vector<unsigned> stringArguments;
  };
  /// Reads the literal format at `formatIndex` of `call`; nothing when it
  /// is not a literal.
  [[nodiscard]] std::optional<FormatNeed>
  formatNeedOf(const clang::CallExpr &call, unsigned formatIndex,
               const core::AnalysisState &state);

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
                     const core::PlaceGuard &guard,
                     const core::PointerOffset &offset,
                     core::AnalysisState &state, bool widened = false);
  /// RFC 0011: where the pointer value of `argument` points relative to the
  /// start of the object `ref` names: the holder's own offset composed with
  /// the argument's derivation (`free(p + 1)` with `p` at `+2` is `+3`).
  [[nodiscard]] core::PointerOffset
  valueOffsetOf(const clang::Expr &argument, const PlaceRef &ref,
                const core::AnalysisState &state);
  /// For `return --*r == 0`, `return !--*r`, `return *r`: the integer place
  /// the result speaks about and, per integer class of the result, the fact
  /// the place satisfies when the result is in it (RFC 0010, *Per-outcome
  /// integer facts*).
  struct ScalarReturnTest {
    core::PlaceId place;
    std::map<core::Outcome, core::ValueFact> factOn;
  };
  [[nodiscard]] std::optional<ScalarReturnTest>
  scalarTestReturn(const clang::Expr &value);
  /// Records into `storedAtReturn` what holds at a `return` of `outcome`:
  /// the stores of the path and the facts about the caller's integer memory
  /// this function wrote, plus what the returned test says (RFC 0010).
  void recordStoredAtReturn(core::Outcome outcome,
                            const core::AnalysisState &state,
                            const std::optional<ScalarReturnTest> &tested);
  /// This function overwrote `place` outright on the current path: what the
  /// caller's memory held there on entry is gone (RFC 0008, *Replaced
  /// values*; `state.overwritten`).
  void noteOverwritten(core::PlaceId place, core::AnalysisState &state);
  /// This function wrote `place` (any element) after consuming the caller's
  /// value there on the current path: the consume is `replaced` on this
  /// path (RFC 0008, *Replaced values*; `state.consumed[path].replaced`).
  void noteRewritten(core::PlaceId place, core::AnalysisState &state);
  /// `noteRewritten` for one name of the written cell.
  void noteRewrittenAt(core::PlaceId place, core::AnalysisState &state);
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
  /// RFC 0030 §9.1: how the consumption in force at a `return` is keyed by
  /// the result. A path listed in `classes` is consumed only on those
  /// result classes; one that is not is consumed on every class. A path in
  /// `widened` had a conjunct its guard could not export dropped, so the
  /// consume is claimed on paths the body does not consume on.
  struct ResultCases {
    std::map<core::SummaryPath, core::OutcomeSet> classes;
    std::set<core::SummaryPath> widened;
  };
  /// The cases the consumes in force at `state` fall in when the `return`
  /// names the local place `returned` (§9.1, *Derivation rule*): a conjunct
  /// on that local becomes a result class, a conjunct on a parameter stays
  /// a guard, and anything else is dropped and makes the case `widened`.
  [[nodiscard]] ResultCases
  resultCasesAt(const core::AnalysisState &state,
                std::optional<core::PlaceId> returned);
  /// Records the outcome classes of `return value` and the consumption on
  /// this path for each (final pass).
  void recordOutcomes(const clang::Expr &value, const ValueOrigin &origin,
                      const core::AnalysisState &state);
  /// The stable summary path of `place` if it names caller memory (below a
  /// dereference of a parameter, or a global).
  [[nodiscard]] std::optional<core::SummaryPath>
  callerVisiblePath(core::PlaceId place);
  /// `callerVisiblePath` for a count this function adjusts, bounded to the
  /// place depth the summary keeps (RFC 0010, *Recognising increments and
  /// decrements*).
  [[nodiscard]] std::optional<core::SummaryPath>
  countPathFor(core::PlaceId count);
  /// For `return p != NULL`, `return !p` and their negations: the tested
  /// place and the integer class the function returns when it is null.
  [[nodiscard]] std::optional<std::pair<const clang::Expr *, core::Outcome>>
  nullTestReturn(const clang::Expr &value) const;
  /// Records a pointer value written into caller-visible memory, or, for a
  /// caller-visible value written below a pointer the summary cannot name,
  /// that the value escaped (RFC 0010, *Stores out of sight*).
  void recordStore(core::PlaceId dest, const core::ValueSource &value,
                   const core::AnalysisState &state);
  void recordStoreOutOfSight(core::PlaceId dest, const core::ValueSource &value,
                             const core::AnalysisState &state);
  /// `return s` for a record: one `result`-rooted store per pointer field
  /// path of `s`'s storage with a known source (RFC 0008, *Struct-by-value
  /// results*).
  void recordResultStores(const PlaceRef &returned,
                          const core::AnalysisState &state);
  /// Classifies a value the callee hands out (stores or returns), guarded by
  /// the path's facts and the origin's own (RFC 0009).
  /// Entry identities are used for final heap/return facts. Historical
  /// stores retain their interface-cell interpretation (RFC 0013).
  [[nodiscard]] core::ValueSource sourceOf(const ValueOrigin &origin,
                                           const core::AnalysisState &state,
                                           bool entryValue = false);
  /// `sourceOf` without the guard.
  [[nodiscard]] core::ValueSource
  sourceValueOf(const ValueOrigin &origin, const core::AnalysisState &state,
                bool entryValue = false);
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
    /// RFC 0030 §9.4: the target holds a pointer *into* the released
    /// object, not the object: it is not an owning place, so the consume
    /// is not exported for it (`interiorConsume`).
    bool interior = false;
    /// RFC 0030 §9.4: the target points *before* the released pointer, so
    /// it names an object that **contains** the released one. Releasing a
    /// position inside an object does not release the object (RFC 0011
    /// reports that as `invalid-release` at the release itself), so a use
    /// through the container is possible, never definite.
    bool container = false;
    /// RFC 0030 §3.1, *Aliases of a released object*: the target is a second
    /// name for the released value on an exact alias edge that neither a
    /// copy nor a test of this path established — a join left it in the
    /// may-relation without the fact that made it. It holds the released
    /// value exactly when the two are equal, so the consume is recorded
    /// under that identity and can never be definite.
    bool unproved = false;
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
  [[nodiscard]] MirrorPlaces mirrors(core::PlaceId place,
                                     const core::AnalysisState &state,
                                     bool definite = false);
  [[nodiscard]] MirrorPlaces computeMirrors(core::PlaceId place,
                                            const core::AnalysisState &state,
                                            bool definite);
  /// RFC 0030 §5.1: while the unknown-callee default marks the places of a
  /// subtree, which changes no alias, the (non-definite) mirrors of each
  /// place, shared by its descendants.
  llvm::DenseMap<std::uint32_t, MirrorPlaces> *mirrorCache = nullptr;
  [[nodiscard]] MirrorPlaces scalarMirrors(core::PlaceId place,
                                           const core::AnalysisState &state);

  struct MovedHit {
    core::PlaceId target;
    core::MoveRecord record;
    /// The record is the accessed element's (or the whole array's), not
    /// another cell's whose index may equal it (RFC 0030 §3.1).
    bool sameElement = true;
  };
  /// The move record of `place` if it is moved and the record's element
  /// witness matches the access's (`Whole` matches everything).
  [[nodiscard]] std::optional<MovedHit>
  findMoved(core::PlaceId place, const core::AnalysisState &state,
            core::ElementWitness element = core::ElementWitness::whole());
  /// A loan that conflicts with a move or mutation of `place` (`kind`
  /// unset) or with a new borrow of it. Loans on the place's ancestors count
  /// unless `ancestors` is false: freeing what `s.buf` points to leaves a
  /// borrow of `s` intact.
  /// Loans whose holder `ignoreHolder` accepts do not count.
  /// With `storageOnly`, only loans on `place`'s own storage and its
  /// object's (`storageOf`) count: freeing an object releases that, not the
  /// objects the pointers stored in it refer to (RFC 0011, *Derived
  /// pointers*: a hash table's `parents->buckets->last` points at a pair
  /// the intrusive list owns; freeing the bucket array is no conflict with a
  /// borrow of the pair). What the object's pointers own is released by a
  /// consume of its own, with its own check.
  [[nodiscard]] std::optional<core::Loan>
  findLoanConflict(core::PlaceId place, std::optional<core::BorrowKind> kind,
                   const core::AnalysisState &state, bool ancestors = true,
                   const std::function<bool(core::PlaceId)> &ignoreHolder = {},
                   bool storageOnly = false);

  [[nodiscard]] std::vector<core::LifetimeId>
  lifetimesOfPlace(core::PlaceId place, const core::AnalysisState &state);
  [[nodiscard]] core::LifetimeId meet(std::vector<core::LifetimeId> ids);
  [[nodiscard]] core::LifetimeId rootLifetime(core::PlaceId place);

  // -- Diagnostics and decisions (RFC 0030 §3, §14) --------------------------

  [[nodiscard]] core::SourceLocation locate(const clang::Stmt &stmt) const;
  [[nodiscard]] core::SourceLocation locate(clang::SourceLocation loc) const;
  /// A diagnostic whose id is about no facet (`leak`, `invalid-annotation`,
  /// ...): definite when it is an error.
  void report(core::Diagnostic diagnostic);
  /// A diagnostic with its certainty, linked to `site` and `facet` when
  /// given. Its severity is `diag::defaultSeverity(id, certainty)`.
  void report(core::Diagnostic diagnostic, core::Certainty certainty,
              const SiteInfo *site, std::optional<core::Facet> facet);
  /// Whether this run publishes decisions: the final pass of the
  /// authoritative run, outside an unsafe region (whose rules are §6.1's).
  [[nodiscard]] bool publishing() const noexcept;
  /// The site the engine's check at `at` is about, with `facet`: the
  /// statement itself, the site `at` is the pointer operand of (tried first
  /// when `operand`: `p->f` is itself a site, and the operand of `p->f[i]`),
  /// or the innermost enclosing site (an argument's call, a returned value's
  /// exit). Null when there is none, or when this run does not publish.
  [[nodiscard]] const SiteInfo *
  siteFor(const clang::Stmt &at, core::Facet facet, bool operand = false);
  /// The site the access `access` itself stands for, with `facet`: no
  /// operand or enclosing site (a member read `s.f` inside `g(s.f)` decides
  /// nothing about the call). Null when there is none, or when this run
  /// does not publish.
  [[nodiscard]] const SiteInfo *accessSite(const clang::Expr &access,
                                           core::Facet facet);
  /// RFC 0030 §15 item 4: decides the spatial facets of the accesses on the
  /// path of `root` that the engine handles at the root: the interior
  /// loads (`v->items` in `v->items[i]`, `a[i]` in `a[i]->f`) and the
  /// subscripts of array lvalues (`m[i]` in `m[i][j]`), and `root` itself
  /// when `self` (a consumed argument, `free(a[i])`). Decision only: the
  /// summary's requirements and the dump's counts are not touched.
  void decidePathBounds(const clang::Expr &root, bool self,
                        core::AnalysisState &state);
  /// §15 item 4: the facets of an access whose place the builder cannot
  /// name (its pointer is a conversion or arithmetic over one): spatial by
  /// the bounds check, temporal and null by the pointer it derives from.
  void decideUnplacedAccess(const clang::Expr &access, Role role,
                            core::AnalysisState &state);
  /// One decision about one facet of `site` (§2.5: records merge by rank).
  void decide(const SiteInfo *site, core::Facet facet,
              const core::FacetDecision &decision);
  /// The same for the exit site `stmt` stands for: a `return`, the function
  /// body, or a call that does not return.
  void decideExit(const clang::Stmt &stmt, const core::FacetDecision &decision);
  /// §9.4: the place class a boundary row and its propagation name: the
  /// nearest enclosing field (`struct s.buf`), else the name of the global
  /// the place is rooted in; empty when the place is neither.
  [[nodiscard]] std::string placeClassOf(core::PlaceId place) const;
  /// §9.4: what the places the other side of a boundary can reach hold.
  /// `call` is the call the boundary is at; null at the end of the body,
  /// the one exit that returns to the caller, whose releases the summary
  /// exports for it and which therefore reports only escaped storage. A
  /// call, and a call that does not return, excuse nothing: the other side
  /// assumes A1 and A3 and is told nothing.
  void publishBoundary(const clang::Stmt &at, const clang::CallExpr *call,
                       const core::AnalysisState &state);
  /// §9.4: the summary path to name `place` by at a boundary whose other
  /// side reaches the objects `reachable`, or none when it cannot reach it.
  [[nodiscard]] std::optional<core::SummaryPath>
  boundaryPathOf(core::PlaceId place, llvm::ArrayRef<core::PlaceId> reachable);
  /// §9.4: the caller-visible holders of storage whose lifetime ended, as
  /// the lifetime rules found them; published at the exit.
  std::vector<BoundaryFacts::Dangling> escapedStorage;
  /// §7.4: the width a dereference of a `T *` needs and a Single pointer to
  /// `T` guarantees: `sizeof(T)`, or for a struct with a flexible trailing
  /// array member (every trailing array at `-fstrict-flex-arrays=0`), the
  /// member's offset. Nothing for an incomplete or variably sized type.
  [[nodiscard]] std::optional<std::int64_t>
  objectWidthOf(clang::QualType type) const;
  /// §14: `have` bytes as a term over C names here: a constant, or a place
  /// (or the quantity the program computed into one, RFC 0017) scaled and
  /// shifted. A field read through a pointer names that pointer in
  /// `readsThrough` (§10.3 rule 5).
  [[nodiscard]] std::optional<WitnessTerm>
  extentTerm(const core::Affine &have,
             std::optional<core::PlaceId> &readsThrough);
  /// §7.4 *Arithmetic*: `have` bytes as whole elements of `unit` bytes:
  /// exactly when the facts divide it (`n` for `malloc(n * sizeof *p)`),
  /// else the byte value rounded down (`bytes / 4`).
  [[nodiscard]] std::optional<WitnessTerm>
  countTerm(const core::Affine &have, std::int64_t unit,
            std::optional<core::PlaceId> &readsThrough);
  /// A place that holds the start of the object `known.pointer` points
  /// into, unchanged since (a definite alias at its start), for a span
  /// check's base.
  [[nodiscard]] std::optional<WitnessTerm>
  objectBaseTerm(const KnownExtent &known,
                 std::optional<core::PlaceId> &readsThrough);
  /// §14 `witness`: the check of the access `site` against `known`: an
  /// index below the whole elements of an object the pointer points to the
  /// start of, or a span inside the object a cursor points into, when the
  /// terms have C names here.
  [[nodiscard]] std::optional<CheckWitness>
  accessWitness(const SiteInfo &site, const KnownExtent &known);
  /// `place` as a term: a variable, or a field below at most one
  /// dereference (the pointer it reads through goes to `readsThrough`).
  [[nodiscard]] std::optional<WitnessTerm>
  placeTerm(core::PlaceId place, std::optional<core::PlaceId> &readsThrough);
  [[nodiscard]] std::optional<WitnessTerm>
  expressionTerm(const core::IntegerExpression<core::PlaceId> &expression,
                 std::optional<core::PlaceId> &readsThrough);
  /// §3.3: the spatial decision of an access against `known` (whose check,
  /// `check`, already compared the access's need with it), with the witness
  /// a check needs when it is checked (published with it). §7.1: a lower
  /// bound decides only what it covers.
  void decideSpatial(const SiteInfo *site, const core::SpatialCheck &check,
                     const KnownExtent *known);
  /// Whether the pointer `pointer` points into a string literal: on every
  /// path (true), on some (false), or nothing known of one (nothing).
  [[nodiscard]] std::optional<bool>
  pointsToLiteral(const clang::Expr &pointer, const core::AnalysisState &state);
  /// RFC 0030 *Diagnostics*: a write through a pointer into a string literal
  /// has no writable byte. Decides `site`'s spatial facet (a violation with
  /// `out-of-bounds` when on every path, `unknown-extent` otherwise) and
  /// returns whether the pointer may point into one.
  bool checkLiteralWrite(const clang::Expr &pointer, const clang::Expr &at,
                         const SiteInfo *site,
                         const core::AnalysisState &state);
  /// One spatial requirement of a call on one of its arguments (§2.5).
  struct ArgumentRequirement {
    unsigned argument = 0;
    /// `Bytes`: `need` bytes behind the argument; `String`: a terminator
    /// within its object (§10.3 rule 2).
    enum class Kind : std::uint8_t { Bytes, String };
    Kind kind = Kind::Bytes;
    /// The bytes needed, when the facts give them, and as a C term.
    std::optional<core::Affine> need = std::nullopt;
    std::optional<WitnessTerm> needTerm = std::nullopt;
    /// The call writes through the argument.
    bool writes = false;
    /// §7.4: a `str` destination is bounded by its member.
    bool memberBound = false;
    /// An object only the library makes and reads (`FILE`).
    bool libraryObject = false;
    /// §7.3's Call-site row: proven or `unresolved(unknown-extent)` only.
    bool rowOnly = false;
    /// A declared kind or an enforced §7.5 requirement: a definite
    /// shortfall against an exact extent is the call's violation ...
    bool enforced = false;
    /// ... when no guard term (§7.5) is left open by the facts.
    std::optional<WitnessTerm> guard = std::nullopt;
  };
  /// §2.5, §3.3: one spatial requirement record per requirement of `call`
  /// (whose site is `site`), each decided from the facts before the call,
  /// with the length or string witness its check needs.
  void
  decideArgumentRequirements(const clang::CallExpr &call, const SiteInfo &site,
                             llvm::ArrayRef<ArgumentRequirement> requirements,
                             const core::AnalysisState &state);
  /// §15 item 4: the requirements of the `LibrarySpec` row that governs
  /// `call` (a LibCall site), and its `disjoint` clauses.
  void decideLibraryRequirements(const clang::CallExpr &call,
                                 const core::AnalysisState &state);
  /// §15 item 12: the declared requirements (§7.2) of a Call site's
  /// callee on its arguments.
  void decideDeclaredRequirements(const clang::CallExpr &call,
                                  const core::AnalysisState &state);
  // -- RFC 0030 §7, §15 item 14: kinds in the engine (KindSeeding.cpp) ----
  void seedParameter(const clang::ParmVarDecl &param, core::PlaceId place,
                     core::AnalysisState &state);
  [[nodiscard]] std::optional<core::SpatialRecord>
  slotRecordAt(core::PlaceId place);
  void seedCallResult(core::PlaceId dest, const clang::Expr &value,
                      core::AnalysisState &state);
  [[nodiscard]] std::optional<KnownExtent>
  resultExtentOf(const clang::Expr &base);
  [[nodiscard]] std::optional<core::Nullness>
  kindNullness(const clang::NamedDecl &decl) const;
  void decideCallKinds(const clang::CallExpr &call,
                       const core::AnalysisState &state);
  [[nodiscard]] core::FacetDecision
  coveredDecision(const SiteInfo &site, core::Facet facet,
                  core::FacetDecision decision, unsigned argument = ~0U);
  [[nodiscard]] bool isArgvElement(const clang::Expr &pointer) const;
  void decideSlotStores(const clang::Stmt &stmt,
                        const core::AnalysisState &state);
  /// §7.4, §10.3 rule 4: before the places below `place` are forgotten, an
  /// extent that names one of them (the count a loaded pointer carries from
  /// its object) keeps its value as a snapshot no check can name.
  void snapshotExtentsBelow(core::PlaceId place, core::AnalysisState &state);
  /// The row term `term` of `match` as a value known at `call` (bytes,
  /// elements or a length as the term says), or nothing.
  [[nodiscard]] std::optional<core::Affine>
  libraryValue(const core::LibTerm &term, const clang::CallExpr &call,
               const core::LibraryMatch &match,
               const core::AnalysisState &state);
  /// §8.3: the `null-if-zero` length of call argument `argument` is a
  /// non-zero constant here, so the argument's `nonnull_n` check refines it.
  [[nodiscard]] bool lengthKnownNonZero(const clang::CallExpr &call,
                                        const SiteInfo &site,
                                        std::uint32_t argument,
                                        const core::AnalysisState &state);
  /// §8.3: whether the zero term of a `null-if-zero` argument is non-zero
  /// (true), zero (false), or not known.
  [[nodiscard]] std::optional<bool> libraryLengthNonZero(
      const clang::CallExpr &call, const core::LibraryMatch &library,
      std::uint32_t argument, const core::AnalysisState &state);
  /// Per pointer operand (stripped of transparent casts), the sites of this
  /// function it is the operand of; built on first use.
  llvm::DenseMap<const clang::Expr *, llvm::SmallVector<const SiteInfo *, 1>>
      sitesByOperand;
  std::unique_ptr<clang::ParentMap> parentMap;
  /// RFC 0030 §3.1: the certainty of a move record hit at a use: definite
  /// when it holds on every path from an unconditional consume.
  [[nodiscard]] static core::Certainty
  certaintyOf(const core::MoveRecord &record);
  /// RFC 0030 §2.6: the findings of a context-specialised run requested at
  /// `call`, reported where the run found them, with `note` naming the call
  /// (when given), and linked to the call's site, whose temporal facet a
  /// temporal finding decides.
  void reportContextFindings(const clang::CallExpr &call,
                             std::vector<core::Diagnostic> found,
                             std::string_view note);
  /// RFC 0030 §5.1, §9.3: the reason a use of a place with a record of
  /// unknown origin takes: `callback` through a function pointer,
  /// `unknown-callee` otherwise.
  [[nodiscard]] static core::UnresolvedReason
  unknownReasonOf(const core::MoveRecord &record);
  /// §3.1: the temporal decision a use that hits `record` gets.
  [[nodiscard]] static core::FacetDecision
  temporalDecisionFor(const core::MoveRecord &record,
                      core::Certainty certainty);
  void reportUseOfMoved(core::PlaceId used, const MovedHit &hit,
                        const clang::Expr &at);
  /// RFC 0030 §11: whether `place`'s declaration is one a jump of this
  /// function can bypass, which zero-initialisation does not reach.
  [[nodiscard]] bool declarationBypassed(core::PlaceId place);
  /// `bypassedDeclarations` of this function's body, filled by
  /// `initialState`.
  llvm::DenseSet<const clang::VarDecl *> bypassedDecls;
  /// RFC 0030 §3.4: definite when the escaping value exactly aliases the
  /// dying storage (on every path); a returned one is about the exit's
  /// temporal facet (`may-dangle` when possible).
  void
  reportLifetimeTooShort(core::PlaceId holder, core::PlaceId borrowed,
                         const clang::Expr &at, bool returned,
                         core::Certainty certainty = core::Certainty::Definite);
  void
  reportLifetimeTooShort(core::PlaceId holder, core::PlaceId borrowed,
                         const core::SourceLocation &at, bool returned,
                         core::Certainty certainty = core::Certainty::Definite,
                         const SiteInfo *site = nullptr);
  /// The summary side of a dangling holder: a caller-visible holder's value
  /// is `unknown` to callers.
  void noteDanglingHolder(core::PlaceId holder, bool returned = false);
  /// §5.1: the caller can trust nothing about the value held there.
  void noteUnknownHolder(core::PlaceId holder);
  [[nodiscard]] std::string nameOf(core::PlaceId place) const;
  [[nodiscard]] std::string summaryName(const core::SummaryPath &path) const;
  [[nodiscard]] core::Diagnostic makeError(std::string_view id,
                                           std::string message,
                                           const clang::Expr &at) const;
};

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_DATAFLOW_H
