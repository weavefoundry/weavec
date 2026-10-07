//===- Engine.h - The object engine (RFC 0031) ------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031's engine behind the RFC 0030 seam. `ObjectEngine` runs a unit:
// the call graph bottom up, summaries (`core::FunctionEffects`) to a
// fixpoint per strongly connected component, then one authoritative pass
// per emitted function. `FunctionRun` analyses one function body over the
// always-add CFG with `core::HeapState` as the lattice (§4), and in its
// final pass decides every site `SiteCollector` enumerated (§5).
//
// Only `ObjectEngine.cpp` and the `Engine*.cpp` files include this header
// (hygiene gate H2).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_ENGINE_H
#define WEAVEC_LIB_ANALYSIS_ENGINE_H

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/ObjectEngine.h"
#include "weavec/Analysis/SafetyEngine.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Analysis/SourceTerm.h"
#include "weavec/Core/Effects.h"
#include "weavec/Core/Heap.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SparseBitVector.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace weavec::analysis::engine {

/// Opaque handles for Clang entities (RFC 0031 §2).
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr):
// a handle is the address of the node it names.
[[nodiscard]] inline core::Handle handleOf(const void *pointer) noexcept {
  return static_cast<core::Handle>(reinterpret_cast<std::uintptr_t>(pointer));
}
template <typename T>
[[nodiscard]] inline const T *fromHandle(core::Handle handle) noexcept {
  return reinterpret_cast<const T *>(static_cast<std::uintptr_t>(handle));
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
/// The variable a `Local` or `Global` object is; null for any other, and
/// for a local an expression makes (`ObjectKey::expression`), whose handle
/// is no declaration.
[[nodiscard]] inline const clang::VarDecl *
variableOf(const core::ObjectInfo &info) noexcept {
  if ((info.key.kind != core::ObjectKind::Local &&
       info.key.kind != core::ObjectKind::Global) ||
      info.key.expression)
    return nullptr;
  return llvm::dyn_cast_or_null<clang::VarDecl>(
      fromHandle<clang::Decl>(info.key.handle));
}
[[nodiscard]] core::Handle typeHandle(clang::QualType type) noexcept;
[[nodiscard]] clang::QualType typeOfHandle(core::Handle handle) noexcept;

class UnitRun;

/// RFC 0030 §9.1 `param N =0|!=0`: whether `value`, a parameter's or an
/// argument's, is zero (a null pointer) for certain, non-zero for certain,
/// or either.
[[nodiscard]] std::optional<bool> isZeroValue(const core::Heap &heap,
                                              const core::HeapState &state,
                                              core::Sym value);

/// A C place as a diagnostic spells it (`b->cap`, where a check's text
/// says `(*b).cap`); any other term as the check does.
[[nodiscard]] std::string messageSpelling(const SourceTerm &term);

/// `path[*]`: the elements below `path`. Unlike `SummaryPath::indexed`,
/// never collapsed onto a trailing dereference, so `a[*]` and `a[0]`
/// (`*a`) stay different paths (RFC 0031 §4.2 *Amendment (arrays)*).
[[nodiscard]] inline core::SummaryPath elementsOf(core::SummaryPath path) {
  path.steps.pushBack(
      core::PathElem{.step = core::PathStep::Index, .field = {}});
  return path;
}

/// `path` extended to the scalar cell at byte `offset` of an object of
/// `type`: the member names down through nested records (`box.data`), an
/// anonymous member by its offset, and `#<offset>` where no member names
/// it. At offset zero of a scalar object `path` itself, unless `named`
/// (then `#0`). EngineSummary.cpp.
[[nodiscard]] core::SummaryPath cellPath(const clang::ASTContext &context,
                                         clang::QualType type,
                                         core::SummaryPath path,
                                         std::int64_t offset, bool named);

/// What one analysis of a function produced.
struct RunResult {
  core::FunctionEffects effects;
  bool overBudget = false;
  /// Over the per-function budget, not just the run's share of the unit's.
  bool spentBudget = false;
  std::uint64_t transfers = 0;
  /// RFC 0034 §7.1.
  std::uint64_t work = 0;
};

/// A place where a value is stored or read: every object and offset an
/// lvalue may designate.
struct Address {
  std::vector<core::Target> targets;
  /// Any object (an unknown or raw pointer).
  bool top = false;
  /// The pointer the lvalue dereferences, when it dereferences one.
  core::Sym base = core::ZeroSym;
};

/// The per-expression result of evaluation.
struct ExprResult {
  core::Sym value = core::ZeroSym;
  std::optional<Address> address = std::nullopt;
};

/// RFC 0031 §6.6: which of a callee's entry objects a caller made the same
/// object. Each parameter maps to the lowest-numbered parameter it shares an
/// object with (itself when none) and its byte offset from it; each global
/// listed points into that parameter's object.
struct AliasContext {
  std::vector<std::pair<unsigned, std::int64_t>> params;
  std::vector<std::tuple<const clang::VarDecl *, unsigned, std::int64_t>>
      globals;
  /// Integer arguments the caller knows the value of.
  std::vector<std::optional<std::int64_t>> constants;
  /// Globals whose value points into the object another global's value
  /// points to (the first), at an offset from it.
  std::vector<
      std::tuple<const clang::VarDecl *, const clang::VarDecl *, std::int64_t>>
      globalAliases;
  /// Pointer cells of argument objects that point into one object: (param,
  /// cell offset) holds the value of (rep param, rep cell offset), shifted.
  struct CellAlias {
    unsigned param;
    std::int64_t cell;
    unsigned repParam;
    std::int64_t repCell;
    std::int64_t shift;
    friend auto operator<=>(const CellAlias &, const CellAlias &) = default;
  };
  std::vector<CellAlias> cellAliases;
  /// §6.6 *Amendment (numeric contexts)*: integer cells of the objects
  /// pointer arguments point to that the caller knows the value of, as
  /// `(parameter, byte offset, value)`.
  std::vector<std::tuple<unsigned, std::int64_t, std::int64_t>> cells;
  /// The same for the object a pointer global the callee reads points to,
  /// as `(global, byte offset, value)`.
  std::vector<std::tuple<const clang::VarDecl *, std::int64_t, std::int64_t>>
      globalCells;
  /// §7 *Amendment (cross-unit contexts)*: the functions a function-pointer
  /// argument holds, by portable name.
  std::vector<std::pair<unsigned, std::vector<std::string>>> callbacks;

  /// No parameter shares another's object and no global points into one.
  [[nodiscard]] bool trivial() const {
    for (unsigned i = 0; i < params.size(); ++i)
      if (params[i].first != i)
        return false;
    return globals.empty() && globalAliases.empty() && cellAliases.empty();
  }
  friend bool operator<(const AliasContext &a, const AliasContext &b) {
    return std::tie(a.params, a.globals, a.constants, a.globalAliases,
                    a.cellAliases, a.cells, a.globalCells, a.callbacks) <
           std::tie(b.params, b.globals, b.constants, b.globalAliases,
                    b.cellAliases, b.cells, b.globalCells, b.callbacks);
  }
};

/// §7 *Amendment (cross-unit contexts)*: a context's portable spelling (its
/// parameters' aliases, constants, integer cells and callbacks; not the
/// globals, which another unit numbers differently), and back.
[[nodiscard]] std::string contextKeyText(
    const AliasContext &context,
    const std::function<std::string(const clang::VarDecl &)> &globalName);
[[nodiscard]] std::optional<AliasContext> parseContextKey(
    llvm::StringRef text, unsigned params,
    const std::function<const clang::VarDecl *(const std::string &)> &global);

/// How one function body is analysed.
enum class RunMode : std::uint8_t {
  /// A summary round: publishes into a discarding adapter.
  Summary,
  /// The authoritative pass (§3 step 3).
  Authoritative,
  /// An alias-context run: diagnostics only (§6.6).
  Context,
};

/// The object an argument points into, as written (`buf` for `&buf[3]`
/// and `buf + 2`).
const clang::Expr &pointedObject(const clang::Expr &argument);

class FunctionRun;

/// RFC 0030 §9.4: the class of a cell for propagation: `struct s.f` for a
/// field, the global's name for a global's own storage; empty when neither.
std::string cellClass(const FunctionRun &run, core::ObjectId object,
                      core::CellKey key);

/// Every object reachable from `start` through the state's memory, `start`
/// included.
std::vector<core::ObjectId> reachableFrom(const core::Heap &heap,
                                          const core::HeapState &state,
                                          std::vector<core::ObjectId> start);

/// One analysis of one function body.
class FunctionRun final : public core::HeapOracle {
public:
  FunctionRun(UnitRun &unitRun, const clang::FunctionDecl &fn,
              LedgerAdapter &adapter, RunMode runMode,
              const AliasContext *alias = nullptr, unsigned depth = 0);
  ~FunctionRun() override;
  FunctionRun(const FunctionRun &) = delete;
  FunctionRun &operator=(const FunctionRun &) = delete;
  FunctionRun(FunctionRun &&) = delete;
  FunctionRun &operator=(FunctionRun &&) = delete;

  RunResult run();
  /// RFC 0030 §7.6 (RFC 0031 *Implementation amendments*): a checking run
  /// refutes the standing counted-field invariants its writes break, at
  /// its calls and exits.
  bool checkingInvariants = false;
  /// Refutes the standing invariants `object` breaks in `state`.
  void checkInvariants(core::HeapState state, core::ObjectId object) const;
  /// `--dump-analysis`: the states at block entries and the objects.
  void dump(llvm::raw_ostream &os);

  // core::HeapOracle
  [[nodiscard]] bool typesMayAlias(core::Handle first,
                                   core::Handle second) const override;
  core::Sym unwritten(core::HeapState &state, core::ObjectId object,
                      core::CellKey key,
                      const core::SymInfo &hint) const override;

private:
  friend class Transfer;
  UnitRun &unit;
  const clang::FunctionDecl &function;
  const AliasContext *aliasContext;
  unsigned contextDepth;
  clang::ASTContext &context;
  LedgerAdapter &out;
  /// Where the run publishes: `out`, or the replay's witness adapter.
  LedgerAdapter *publishTo;
  RunMode mode;
  mutable core::ObjectTable objects;
  core::Heap heap;
  std::unique_ptr<clang::CFG> cfg;
  std::vector<std::optional<core::HeapState>> entryStates;
  std::vector<unsigned> visits;
  /// Joins at each loop head (§12's cost bound).
  std::vector<unsigned> joinsAt;
  std::vector<bool> loopHead;
  /// The back edges (block, loop head) of the depth-first order.
  std::set<std::pair<unsigned, unsigned>> backEdges;
  /// Each loop head's natural loop: the blocks that reach one of its back
  /// edges without passing through it (the order of iteration, §12).
  llvm::DenseMap<unsigned, llvm::BitVector> loopBody;
  /// Blocks in reverse post-order.
  std::vector<const clang::CFGBlock *> order;
  /// Expressions whose value a later block reads (§2), and the ones each
  /// block reads.
  llvm::DenseSet<const clang::Expr *> crossBlock;
  llvm::DenseMap<unsigned, std::vector<core::Handle>> consumedBy;
  /// Parameters the body never assigns (their cells hold the entry value).
  std::set<unsigned> unmodifiedParams;
  std::vector<const clang::VarDecl *> fixedLocals;
  /// Liveness of locals after each CFG element (for leaks).
  llvm::DenseMap<const clang::VarDecl *, unsigned> localIndex;
  llvm::DenseMap<const clang::Stmt *, llvm::BitVector> liveAfterStmt;
  llvm::DenseMap<const clang::Stmt *, llvm::BitVector> liveBeforeStmt;
  /// The locals a block may reference before they are assigned again, or
  /// at all, from its entry on (by block id).
  std::vector<llvm::BitVector> liveInBlock;
  /// Where each block is (its first statement's, or its terminator's,
  /// expansion location), and the compound statement each local is
  /// declared in: a local stays while a block inside its scope may name it
  /// in a check (`dropDeadLocals`).
  std::vector<clang::SourceLocation> blockLocation;
  llvm::DenseMap<const clang::VarDecl *, clang::SourceRange> localScope;
  llvm::BitVector addressTaken;
  /// Drops from `state`, entering block `block`, the locals no path from it
  /// references whose address is not taken (§4.6): what they hold is dead.
  void dropDeadLocals(core::HeapState &state, unsigned block) const;
  /// The expression values a block's state carries for a later block
  /// (`exprs`: cross-block operands, a conditional's arm values), by index,
  /// and the ones some path from each block's entry reads before it
  /// evaluates them again.
  llvm::DenseMap<core::Handle, unsigned> carriedIndex;
  std::vector<llvm::SparseBitVector<>> carriedLiveIn;
  /// Drops from `state`, entering block `block`, the carried expression
  /// values no path from it reads.
  void dropDeadValues(core::HeapState &state, unsigned block) const;
  /// Statement-level elements: whose parent is not an expression.
  llvm::DenseSet<const clang::Stmt *> statementLevel;
  /// RFC 0017: declarations whose variably modified type the CFG does not
  /// evaluate (a pointer to a variable-length array), by the first element
  /// of their initializer: the dimensions are taken before it runs.
  llvm::DenseMap<const clang::Stmt *, std::vector<const clang::VarDecl *>>
      vlaCaptureBefore;
  llvm::DenseSet<const clang::VarDecl *> vlaCapturedEarly;
  /// §6.6: a context this run needed was not run (its depth or count limit),
  /// here or in a context it ran: the call that requested it is not proven.
  bool contextIncomplete = false;
  /// Calls with a spatial violation reported (RFC 0030 §7.5, or a store past
  /// the caller's object): one finding per call.
  llvm::DenseSet<const clang::Stmt *> requirementViolated;
  /// The block that evaluates each expression element.
  llvm::DenseMap<const clang::Stmt *, unsigned> evaluatedIn;
  /// The block being transferred.
  unsigned currentBlock = ~0U;
  /// The current block ends the program (a call that does not return).
  bool currentBlockNoReturn = false;
  /// The arms of conditional operators, to their operator.
  llvm::DenseMap<const clang::Expr *, const clang::Expr *> arms;
  /// Exit states, for the summary.
  std::vector<core::HeapState> exits;
  std::uint64_t transfers = 0;
  /// RFC 0034 §7.1: the run's work, and the states it keeps at entries.
  std::uint64_t work = 0;
  std::uint64_t retained = 0;
  /// §12 `--analysis-stats`: the joins and materialised cells of the run.
  std::uint64_t joins = 0;
  mutable std::uint64_t materialisations = 0;
  /// Stopped by the per-function budget (`RunResult::spentBudget`).
  bool spentBudget = false;
  bool overBudget = false;
  bool publishing = false;
  bool inFinalPass = false;
  /// Objects already reported leaked in the publishing pass.
  std::set<core::ObjectId> reportedLeaks;
  /// ... and in which blocks: a loss downstream of a reported one is the
  /// same path's (RFC 0007: once per path).
  std::vector<std::pair<core::ObjectId, unsigned>> leakBlocks;
  /// Whether the CFG block `to` is reachable from `from` (or is it).
  [[nodiscard]] bool blockReaches(unsigned from, unsigned to) const;
  /// RFC 0008: uninitialised values already reported where they were read
  /// (a copy reports at the copy, once).
  std::set<core::Sym> reportedUninit;
  /// Summary derivation: the new objects already described (§6.1).
  std::set<core::ObjectId> visitedFresh;
  /// RFC 0007: entry objects a `WEAVEC_OWNED` field declares owned, with
  /// the field's name and declaration (a leak's note).
  std::map<core::ObjectId, std::pair<std::string, core::SourceLocation>>
      declaredOwner;
  /// Diagnostics already reported, keyed by site and id.
  std::set<std::pair<const clang::Stmt *, std::string>> reported;
  /// RFC 0034 §6.1: the definite errors of the authoritative pass, and
  /// what the replay saw of each: a path that reports it (`witnessed`), a
  /// path that reaches its site and decides it otherwise (`countered`).
  struct Candidate {
    const clang::Stmt *site = nullptr;
    core::Facet facet = core::Facet::Temporal;
    std::string id;
    core::SourceLocation where;
    bool witnessed = false;
    bool countered = false;
    /// In the block being replayed: reported definitely, decided otherwise.
    bool reportedHere = false;
    bool otherwiseHere = false;
  };
  std::vector<Candidate> candidates;
  bool witnessing = false;
  /// Replays the function along single paths and unconfirms the
  /// candidates no path confirms.
  void confirmCandidates();
  /// §4.1: the symbol an integer operation on two symbols last produced, so
  /// the same operation on the same values finds the same value again
  /// (checked against the state before use, EngineExpr.cpp).
  /// Summary derivation (§6.2): the cells this run stored into, anywhere
  /// (an integer cell's value alone does not tell a store from the entry
  /// value).
  std::set<std::pair<core::ObjectId, core::CellKey>> writtenCells;
  using OperationKey = std::tuple<core::IntegerOp, core::Sym, core::Sym,
                                  std::optional<std::int64_t>, core::Handle>;
  std::map<OperationKey, core::Sym> operations;

  // Object helpers (EngineRun.cpp).
  mutable llvm::DenseMap<const clang::Decl *, core::ObjectId> localObjects;
  const clang::Stmt *currentElement = nullptr;
  mutable std::unique_ptr<clang::ParentMap> parentMap;
  mutable std::optional<llvm::DenseSet<const clang::VarDecl *>> passedToCalls;
  mutable std::optional<llvm::DenseSet<const clang::VarDecl *>> bypassed;

public:
  // Services for the transfer functions.
  [[nodiscard]] core::Heap &domain() noexcept { return heap; }
  [[nodiscard]] core::ObjectTable &table() const noexcept { return objects; }
  [[nodiscard]] clang::ASTContext &ast() const noexcept { return context; }
  [[nodiscard]] UnitRun &unitRun() const noexcept { return unit; }
  [[nodiscard]] const clang::FunctionDecl &decl() const noexcept {
    return function;
  }
  [[nodiscard]] bool isPublishing() const noexcept { return publishing; }
  [[nodiscard]] unsigned depth() const noexcept { return contextDepth; }
  [[nodiscard]] bool isUnmodifiedParam(unsigned index) const {
    return unmodifiedParams.contains(index);
  }
  /// Pointer locals of the body's outermost block, assigned only where they
  /// are declared: one value for the whole run.
  [[nodiscard]] const std::vector<const clang::VarDecl *> &
  fixedPointerLocals() const noexcept {
    return fixedLocals;
  }
  [[nodiscard]] RunMode runMode() const noexcept { return mode; }
  [[nodiscard]] LedgerAdapter &ledger() const noexcept { return *publishTo; }
  /// The unit's sites, whatever adapter this run publishes into (a context
  /// run collects into its own, §6.6).
  [[nodiscard]] const SiteIndex &sites() const;
  /// The function calls a returns-twice function (RFC 0030 §5.4).
  [[nodiscard]] bool callsSetjmp() const;
  [[nodiscard]] bool applies(core::SiteId id, core::Facet facet) const;
  [[nodiscard]] bool isCrossBlock(const clang::Expr &expr) const {
    return crossBlock.contains(&expr);
  }
  /// Whether `expr` is evaluated as an element of the current block (so a
  /// carried value of it is from an earlier visit and stale).
  [[nodiscard]] bool evaluatedHere(const clang::Expr &expr) const {
    auto it = evaluatedIn.find(&expr);
    return it != evaluatedIn.end() && it->second == currentBlock;
  }
  /// The conditional operator `expr` is an arm of, if any.
  [[nodiscard]] const clang::Expr *armOf(const clang::Expr &expr) const {
    auto it = arms.find(&expr);
    return it == arms.end() ? nullptr : it->second;
  }
  [[nodiscard]] bool isStatementLevel(const clang::Stmt &stmt) const {
    return statementLevel.contains(&stmt);
  }
  /// Records an exit state (a `return` or the end of the body).
  void noteExit(const core::HeapState &state) { exits.push_back(state); }

  /// The object of a local, parameter or global variable.
  core::ObjectId variableObject(const clang::VarDecl &var) const;
  /// The object of a string literal or compound literal.
  core::ObjectId literalObject(const clang::Expr &literal) const;
  /// An allocation site's recent object (§4.2).
  core::ObjectId allocationObject(const clang::Expr &site,
                                  clang::QualType pointee,
                                  const std::string &name,
                                  const core::SummaryPath &path = {}) const;
  /// RFC 0013: the caller's temporary a record-valued call returns into.
  core::ObjectId recordResultObject(const clang::CallExpr &call) const;
  /// The object a callee without a body returned a pointer to.
  core::ObjectId callResultObject(const clang::Expr &site,
                                  clang::QualType pointee,
                                  const std::string &name) const;
  /// The unknown object.
  core::ObjectId unknownObject() const;
  /// RFC 0030 §8.2: the library's hidden state slot `<slot>` (`strtok`'s
  /// saved string, `getenv`'s environment), one object per slot.
  core::ObjectId stateObject(const std::string &slot) const;
  /// Whether `id` is a hidden state slot's object.
  [[nodiscard]] bool isStateObject(core::ObjectId id) const;
  /// The entry object below `parent`'s cell `key` (§4.6).
  core::ObjectId childEntryObject(const core::HeapState &state,
                                  core::ObjectId parent, core::CellKey key,
                                  clang::QualType pointee,
                                  const clang::FieldDecl *field) const;
  /// A fresh value of C type `type` reached from an object nobody in this
  /// activation wrote: an entry pointer, an unknown integer.
  core::Sym entryValue(core::HeapState &state, clang::QualType type,
                       core::ObjectId parent, core::CellKey key,
                       const clang::FieldDecl *field) const;
  /// §7.3–§7.5: a pointer parameter's entry extent and nullness from its
  /// kind (EngineKinds.cpp); `values` are the integer parameters' symbols.
  std::optional<core::Extent> paramExtent(unsigned index,
                                          const std::vector<core::Sym> &values,
                                          bool &nonnull) const;
  /// The roots of the state for collection (§4.6).
  [[nodiscard]] std::vector<core::ObjectId>
  roots(const core::HeapState &state) const;

  /// RFC 0007: owned allocations nothing live reaches after `at` are leaked.
  /// `ExitEdge`: an edge that reaches the exit through blocks with no
  /// statements, reported at the branch that takes it. `Scope`: the end of
  /// a block with no statements, where only the locals whose lifetime ended
  /// are dead.
  enum class LeakPoint : std::uint8_t {
    Statement,
    BlockEnd,
    Release,
    Exit,
    ExitEdge,
    Scope
  };
  void checkLeaks(core::HeapState &state, const clang::Stmt &at,
                  LeakPoint point, const std::string &released = {});
  /// Whether the local `var` may be read after `at`.
  [[nodiscard]] bool liveAfter(const clang::Stmt &at,
                               const clang::VarDecl &var) const;
  [[nodiscard]] bool liveBefore(const clang::Stmt &at,
                                const clang::VarDecl &var) const;
  /// Reports a diagnostic once per (site, id) in the publishing pass.
  void report(core::Diagnostic diagnostic, core::Certainty certainty,
              const clang::Stmt *site, std::optional<core::Facet> facet);

  // Lifetimes (EngineLifetimes.cpp, §5.7).
  /// Where a pointer to frame storage was last stored into a cell, in the
  /// publishing pass, and how the program spelled the cell there.
  struct FrameStore {
    const clang::Stmt *at = nullptr;
    std::string holder;
  };
  /// Storage this activation's frame owns: a local, a parameter, a compound
  /// literal or temporary, an `alloca` block.
  [[nodiscard]] bool isFrameObject(const core::HeapState &state,
                                   core::ObjectId id) const;
  /// How messages name frame storage (`x`, `<alloca>`).
  [[nodiscard]] std::string frameName(core::ObjectId id) const;
  void noteFrameStore(core::ObjectId holder, core::CellKey key,
                      core::ObjectId frame, std::string holderSpelling);
  [[nodiscard]] const FrameStore *frameStore(core::ObjectId holder,
                                             core::CellKey key,
                                             core::ObjectId frame) const;
  /// §5.5: where a derived pointer (a borrow) was last stored into a cell.
  void noteBorrowStore(core::ObjectId holder, core::CellKey key,
                       std::string holderSpelling);
  [[nodiscard]] const FrameStore *borrowStore(core::ObjectId holder,
                                              core::CellKey key) const;
  /// The local variable (or parameter) an object is the storage of.
  [[nodiscard]] const clang::VarDecl *localVariable(core::ObjectId id) const;
  /// Whether the address of the local `var` is taken (or it is an
  /// aggregate), so a pointer it holds may be read through another name.
  [[nodiscard]] bool isAddressTaken(const clang::VarDecl &var) const;
  /// The conditions `Transfer::refineCondition` is refining, outermost
  /// first.
  std::vector<core::Sym> openConditions;
  /// A call ran code the analysis does not see (`FunctionEffects::
  /// unknownGlobals`).
  bool ranUnknownCode = false;
  /// The CFG element being transferred.
  void noteElement(const clang::Stmt &stmt) { currentElement = &stmt; }
  /// RFC 0030 §11: whether `operand` reads a local whose declaration a
  /// jump can bypass, so zero-initialisation does not reach it.
  [[nodiscard]] bool isBypassed(const clang::Expr &operand) const;
  /// RFC 0033 §1: whether `expr`'s value is discarded by a `(void)` cast.
  [[nodiscard]] bool isDiscarded(const clang::Expr &expr) const;
  /// `stmt`'s parent in the body (the unit's parent map costs too much).
  [[nodiscard]] const clang::Stmt *parentOf(const clang::Stmt &stmt) const;
  /// RFC 0033 §1: whether the function passes the address of `var` (or of
  /// a member or an element of it) to a call.
  [[nodiscard]] bool isPassedToCall(const clang::VarDecl &var) const;
  /// RFC 0030 §6.1: whether `stmt` is inside a `WEAVEC_UNSAFE` block or
  /// function.
  [[nodiscard]] bool inUnsafeRegion(const clang::Stmt &stmt) const;
  /// Frame objects a global held at a call (§5.7: handed to the callee
  /// through the global, not left behind by mistake).
  std::set<core::ObjectId> exposedFrames;
  /// RFC 0012: where the program last wrote an object's bytes wholesale (a
  /// library call's copy, fill or string write, an array's initialiser), for
  /// the note on a read of an object left without a terminator. Messages
  /// only.
  std::map<core::ObjectId, core::SourceLocation> byteWrites;

private:
  /// §5.7: frame stores by (holder, cell, frame object).
  std::map<std::tuple<core::ObjectId, core::CellKey, core::ObjectId>,
           FrameStore>
      frameStores;
  std::map<std::pair<core::ObjectId, core::CellKey>, FrameStore> borrowStores;

public:
  /// Declaration-level annotation diagnostics (EngineAnnotations.cpp).
  void validateAnnotations();
  /// The constants a bound growing at loop head `head` may stop at (§4.8):
  /// those the loop's conditions test.
  [[nodiscard]] std::vector<std::int64_t>
  wideningThresholds(unsigned head) const;

private:
  void buildCfg();
  void initialState(core::HeapState &state);
  bool transferBlock(const clang::CFGBlock &block, core::HeapState state,
                     std::vector<std::pair<unsigned, core::HeapState>> &outs);
  void finalPass();
  /// The summary from the exit states (EngineSummary.cpp).
  core::FunctionEffects deriveEffects();
  /// The cells of a fresh object as stores below `path` (§6.1).
  template <typename Describe>
  void describeContents(const core::HeapState &state, core::ObjectId object,
                        const core::SummaryPath &path,
                        std::map<core::SummaryPath, core::ValueDesc> &stores,
                        int depth, Describe &describe);
  /// RFC 0015 §5: an entry object's ranges, selected cells and summary
  /// cells at an exit as element releases and stores (EngineSummary.cpp).
  using ElementKey =
      std::pair<core::SummaryPath, std::optional<core::ElementRange>>;
  template <typename Describe>
  void describeElements(const core::HeapState &state, core::ObjectId id,
                        const core::ObjectState &contents,
                        const core::SummaryPath &objectPath, Describe &describe,
                        std::map<ElementKey, std::string> &releases,
                        std::map<ElementKey, core::ValueDesc> &stores);
  /// `term` over the values the integer parameters still hold (§6.2), when
  /// the zone relates it to one of them.
  [[nodiscard]] std::optional<core::PathTerm>
  parameterTerm(const core::HeapState &state, const core::Term &term) const;
  /// Integer parameters the body never assigns or takes the address of.
  mutable std::optional<std::vector<bool>> unchangedParams;
};

/// One requirement on an argument of a call (RFC 0030 §7.5, §8).
struct ArgRequirement {
  enum class Kind : std::uint8_t { Bytes, String };
  Kind kind = Kind::Bytes;
  unsigned argument = 0;
  /// The bytes needed behind the argument ...
  core::Term need = core::Term::unknown();
  /// ... exactly, or a bound on them (a string length known only to be at
  /// most, a format's least output): a bound proves or violates one way.
  enum class Bound : std::uint8_t { Exact, AtMost, AtLeast };
  Bound bound = Bound::Exact;
  /// §7.5: it binds only under a guard not known to hold, so a shortfall is
  /// no violation.
  bool guarded = false;
  /// A shortfall against an exact extent is the call's violation.
  bool enforced = false;
  /// Proven or unresolved, never checked (§7.3 reliance).
  bool rowOnly = false;
  /// A `str` destination bounded by its member (§7.4).
  bool memberBound = false;
  bool writes = false;
  /// A `printf`-family writer, checked through its bounded writer.
  bool format = false;
  /// An element of `main`'s argv: nul-terminated by the system.
  bool argvElement = false;
  /// A `%s` argument of a literal format: only a read past an object with
  /// no terminator is decided (RFC 0012).
  bool formatArgument = false;
};

/// The transfer functions over one state (§5): evaluation of the CFG
/// elements of one block, and the decisions of their sites in the
/// publishing pass.
class Transfer {
public:
  Transfer(FunctionRun &run, core::HeapState &state);

  /// One CFG element.
  void element(const clang::CFGElement &element);
  /// The value of a branch condition evaluated in this block, if any.
  [[nodiscard]] core::Sym conditionValue(const clang::Expr &condition);
  /// Refines `state` for the edge on which `condition` is `truth`; returns
  /// false when the edge is infeasible.
  static bool refine(FunctionRun &run, core::HeapState &state,
                     core::Sym condition, bool truth);
  static bool refineCondition(FunctionRun &run, core::HeapState &state,
                              core::Sym condition, bool truth);
  /// Refines `state` for the edge of `switchStmt` to `successor` (the
  /// default edge when `isDefault`), on which `value` of `type` matched its
  /// case label or none of them (RFC 0017: labels converted to the
  /// promoted type); false when the edge is infeasible.
  static bool refineSwitchEdge(FunctionRun &run, core::HeapState &state,
                               core::Sym value, clang::QualType type,
                               const clang::SwitchStmt &switchStmt,
                               const clang::CFGBlock &successor,
                               bool isDefault);
  /// Refines `state` to the paths on which `sym` is in `resultClass` and
  /// applies the pending cases that decides; false when none are.
  static bool selectClass(FunctionRun &run, core::HeapState &state,
                          core::Sym sym, core::ResultClass resultClass);
  /// Applies the pending cases of `sym` its value already decides.
  static void settlePending(FunctionRun &run, core::HeapState &state,
                            core::Sym sym);
  /// RFC 0017: the dimensions of the variable-length arrays a declaration
  /// (or typedef) of `type` spells, evaluated where it runs; a later
  /// `sizeof` or extent uses these values, not the expressions' current
  /// ones.
  void captureVlas(clang::QualType type);
  /// The element count a variable-length array type was declared with.
  core::Sym vlaCount(const clang::VariableArrayType &vla);
  /// The bytes an object of `type` takes, when its size is known or made of
  /// captured dimensions.
  std::optional<core::Sym> bytesOf(clang::QualType type, const clang::Expr &at);
  /// The block's end: exits and expression values later blocks read.
  void finishBlock(const clang::CFGBlock &block);

  // Evaluation (EngineExpr.cpp).
  ExprResult evaluate(const clang::Expr &expr);
  core::Sym valueOf(const clang::Expr &expr);
  Address addressOf(const clang::Expr &expr);
  core::Sym load(const Address &address, clang::QualType type,
                 const clang::Expr *at);
  void store(const Address &address, core::Sym value, clang::QualType type,
             const clang::Expr *at, const std::string &holderName = {});
  core::Sym constant(std::int64_t value, clang::QualType type);
  /// RFC 0017: an integer constant of `type` whatever its width, kept as
  /// the symbol's interval when the zone's 64-bit bounds cannot hold it.
  core::Sym constant(const llvm::APSInt &value, clang::QualType type);
  core::Sym unknownValue(clang::QualType type);
  core::Sym pointerTo(const Address &address, clang::QualType pointee,
                      std::string name);
  core::Sym nullPointer(clang::QualType type);
  [[nodiscard]] std::optional<core::IntegerType>
  integerType(clang::QualType type) const;
  /// A definite store of a summary's `store` to the bytes `[start, end)`
  /// of `target`'s object that lie outside it: an error at the call, which
  /// counts the bytes behind the argument (at `pointer` in the object).
  void storePastObject(const clang::CallExpr &call,
                       const core::StoreEffect &store,
                       const core::Target &target, const core::Term &pointer,
                       __int128 start, __int128 end);
  /// A summary's integer (`desc`) as `value`'s interval, where its `[lo, hi]`
  /// cannot say it.
  void boundByRange(core::Sym value, clang::QualType type,
                    const core::ValueDesc &desc);
  /// RFC 0017 §3: the size `left * right` of a checked-product allocation
  /// (`calloc`, `reallocarray`): none when the product overflows `type` for
  /// every value (the allocation fails); else the C product, which the
  /// allocation's success makes the mathematical one.
  std::optional<core::Sym> checkedProduct(core::Sym left, core::Sym right,
                                          clang::QualType type,
                                          const clang::Expr &at);
  /// `p + i * size` (or minus).
  core::Sym pointerAdd(core::Sym pointer, core::Sym index, std::int64_t size,
                       bool subtract, const clang::Expr &at);
  /// The byte width of `type`, if complete.
  [[nodiscard]] std::optional<std::int64_t> sizeOf(clang::QualType type) const;
  /// RFC 0015 §4: the scalar cells (pointer and integer leaves) of a value
  /// of `type`, by offset; false when they are not all known (a union, a
  /// bit-field, a large or incomplete member).
  bool recordLeaves(
      clang::QualType type,
      std::vector<std::pair<std::int64_t, clang::QualType>> &out) const;
  /// A record's value copied leaf by leaf from `from` to `to`, every leaf
  /// read before any is written; false when its leaves are not known.
  bool copyRecord(const Address &to, const Address &from, clang::QualType type);
  /// Names the allocations a record's pointer members own after them.
  void nameHeldByMembers(core::ObjectId object, clang::QualType type,
                         const std::string &prefix);
  /// The spelling of an expression for messages (`q->a`, `buf`).
  [[nodiscard]] std::string spell(const clang::Expr &expr) const;
  /// The integer term a value stands for (its linear form).
  [[nodiscard]] core::Term termOf(core::Sym sym) const;

  // Calls (EngineCalls.cpp).
  core::Sym call(const clang::CallExpr &call);
  /// RFC 0031 §6.6: re-analyses a callee whose arguments share objects,
  /// and reports what it finds at the call.
  void checkAliasContext(const clang::CallExpr &call,
                         const clang::FunctionDecl &callee,
                         const std::vector<core::Sym> &args);
  /// §6.6 *Amendment (numeric contexts)*: the summary of `callee`, a
  /// function of the unit outside the caller's component, derived for this
  /// call's context (the objects its arguments share, the integers it knows)
  /// when `general` depends on them; none when the general one stands.
  const core::FunctionEffects *
  contextSummary(const clang::CallExpr &call, const clang::FunctionDecl &callee,
                 const std::vector<core::Sym> &args,
                 const core::FunctionEffects &general);
  /// §7 *Amendment (cross-unit contexts)*: for a call into another unit,
  /// the callee's summary in the call's context when its unit ran it;
  /// otherwise none, and the context is asked for.
  const core::FunctionEffects *
  remoteContext(const clang::CallExpr &call, const clang::FunctionDecl &callee,
                const std::vector<core::Sym> &args,
                const core::FunctionEffects &general);
  /// The same for a function of another unit known by its portable name,
  /// over parameters of the types `shape`.
  const core::FunctionEffects *
  remoteContext(const clang::CallExpr &call, const std::string &callee,
                const std::vector<clang::QualType> &shape,
                const std::vector<core::Sym> &args,
                const core::FunctionEffects &general);
  /// Applies a callee's summary at `call` (EngineSummary.cpp, §6.3).
  /// Code the analysis does not see may write any global: each forgets
  /// what it holds, and what it reaches may have been released by `record`
  /// (the C library's own globals keep their values).
  void forgetGlobals(const core::ReleaseRecord &record);
  /// RFC 0030 §5.1 for one pointer argument `pointer` of `call`: code the
  /// analysis does not see may write what it reaches (not the first object
  /// when `constPointee`) and release what may be released.
  void havocArgument(const clang::CallExpr &call, core::Sym pointer,
                     bool constPointee, bool callback);
  core::Sym pathValue(core::Sym at, const core::ValueDesc &desc,
                      clang::QualType type);
  core::Sym instantiate(const clang::CallExpr &call,
                        const core::FunctionEffects &effects,
                        const std::vector<core::Sym> &args);

  // Decisions (EngineDecide.cpp).
  void decideSites(const clang::Stmt &stmt);
  /// What an access leaves known of its pointer (every pass).
  void accessed(const clang::Stmt &stmt);
  void decideExitSite(const clang::Stmt &stmt, bool isReturn);
  /// A call's own sites, from the state before its effects.
  void decideCall(const clang::CallExpr &call,
                  const std::vector<core::Sym> &args, core::Sym calleeValue);
  /// The requirement records of a call's arguments (EngineLibrary.cpp).
  void decideArguments(const clang::CallExpr &call, const SiteInfo &site,
                       const std::vector<ArgRequirement> &requirements,
                       const std::vector<core::Sym> &args, bool library);
  /// The kinds' requirements at a call (EngineKinds.cpp, §7.3–§7.5).
  void decideCallKinds(const clang::CallExpr &call, const SiteInfo &site,
                       const std::vector<core::Sym> &args);
  /// An element of `main`'s argv.
  [[nodiscard]] bool isArgvElement(const clang::Expr &pointer) const;
  /// §7.5 covered accesses and the argv contract: a decision a kind
  /// strengthens (EngineKinds.cpp).
  [[nodiscard]] core::FacetDecision covered(const SiteInfo &site,
                                            core::Facet facet,
                                            core::FacetDecision decision) const;
  /// §7.5 for a call's argument: the parameter's inferred requirement
  /// covers what the call needs of it (EngineKinds.cpp).
  [[nodiscard]] std::optional<core::FacetDecision>
  coveredArgument(const clang::CallExpr &call, core::Sym pointer,
                  bool string) const;
  /// A library call's requirement records (EngineLibrary.cpp).
  void decideLibraryCall(const clang::CallExpr &call, const SiteInfo &site,
                         const std::vector<core::Sym> &args);
  /// RFC 0030 §9.3: the slot solution's answer for an indirect call.
  [[nodiscard]] std::optional<core::CallResolution>
  slotResolution(const clang::CallExpr &call) const;
  /// The functions `call` may reach: its direct callee, the functions its
  /// callee value names, or a solved slot's (RFC 0030 §9.3).
  [[nodiscard]] std::vector<const clang::FunctionDecl *>
  callTargets(const clang::CallExpr &call, core::Sym calleeValue) const;
  /// Whether a function `call` may reach reads or writes through its
  /// argument `index` (the summary's `reads` and `writes`), or cannot say.
  /// RFC 0033 §1: whether every function the call may reach has a
  /// complete summary that neither reads, writes nor releases through
  /// argument `index`.
  [[nodiscard]] bool usesValueOnly(const clang::CallExpr &call,
                                   core::Sym calleeValue,
                                   const std::vector<core::Sym> &args,
                                   unsigned index) const;
  [[nodiscard]] bool calleeTouches(const clang::CallExpr &call,
                                   core::Sym calleeValue, unsigned index) const;
  /// Whether the call's callee releases the object argument `index` points
  /// to on every path (its summary), so a use of a released one there is a
  /// double release.
  [[nodiscard]] bool calleeReleases(const clang::CallExpr &call,
                                    core::Sym calleeValue,
                                    const std::vector<core::Sym> &args,
                                    unsigned index,
                                    bool possibly = false) const;
  /// The functions a resolution names.
  [[nodiscard]] std::vector<const clang::FunctionDecl *>
  slotTargets(const core::CallResolution &resolution) const;
  /// RFC 0030 §9.1: what the state knows of the function's unmodified
  /// integer parameters (zero or not), for a release's record.
  [[nodiscard]] std::vector<std::pair<std::uint32_t, bool>> paramGuard() const;
  /// The fixed pointer locals that hold a non-null value here (sorted).
  [[nodiscard]] std::vector<core::Handle> nonNullLocals() const;
  [[nodiscard]] std::vector<core::ParamPairTest> pairGuard() const;
  /// Whether `address` names two objects of which each path has exactly
  /// one (an entry object and the object made where its pointer was null).
  [[nodiscard]] bool complementary(const Address &address) const;
  /// RFC 0014: whether the arguments a pair test names compare equal, when
  /// the state decides it.
  [[nodiscard]] std::optional<bool>
  argumentsEqual(const std::vector<core::Sym> &args,
                 const core::ParamPairTest &test) const;
  /// §5.7: a pointer a callee left to its own frame storage: the object
  /// may have ended, and a use of it is not proven (EngineLifetimes.cpp).
  core::Sym danglingValue(const clang::CallExpr &call, clang::QualType type,
                          const core::SummaryPath &path);
  /// RFC 0008: a callee's summary releases `value` (the caller's value at
  /// `path`): a pointer to storage that is no heap object is an
  /// `invalid-release` at the call (EngineLifetimes.cpp).
  void calleeRelease(const clang::CallExpr &call, core::Sym value,
                     const core::SummaryPath &path, bool certain);
  /// A callee's summary releases `value` (the caller's value at `path`,
  /// below an argument or a global) that the caller already released: a
  /// double release at the call.
  /// §7 *Amendment (cross-unit contexts)*: a function value a summary
  /// describes.
  core::Sym functionValue(const core::ValueDesc &desc);
  /// Whether an earlier release of `value` is one still pending on a null
  /// result of the call that made the argument `path` goes through.
  [[nodiscard]] bool
  pendingOnNullArgument(const std::vector<core::Sym> &args, core::Sym value,
                        const core::SummaryPath &path,
                        const core::SourceLocation &where) const;
  // Strings (EngineStrings.cpp, RFC 0012 *String facts*).
  enum class Byte : std::uint8_t { Zero, NonZero, Unknown };
  struct StringFacts {
    enum class Length : std::uint8_t { Unknown, Exact, AtMost };
    Length length = Length::Unknown;
    /// `strlen` of the pointer, exactly or at most.
    core::Term term = core::Term::unknown();
    /// The offset in the object of the NUL that ends it.
    core::Term nulAt = core::Term::unknown();
    /// No byte from the pointer to the object's constant end is NUL.
    bool unterminated = false;
  };
  /// A `printf`-family call's literal format (RFC 0030 §8.2).
  struct FormatFacts {
    bool literal = false;
    std::optional<unsigned> reads = std::nullopt;
    unsigned passed = 0;
    /// The least output without its NUL, and whether it is exact.
    std::int64_t lower = 0;
    bool exact = false;
    /// The call arguments `%s` reads to their terminator.
    std::vector<unsigned> strings;
    /// RFC 0033 §5: the call arguments a `%.Ns` reads at most N bytes of;
    /// they need no terminator.
    std::vector<unsigned> bounded;
  };
  /// The byte a value stored as `width` bytes puts first in memory.
  [[nodiscard]] Byte valueByte(core::Sym sym,
                               std::optional<std::int64_t> width) const;
  [[nodiscard]] Byte byteAt(core::ObjectId id, std::int64_t offset) const;
  [[nodiscard]] StringFacts stringFacts(core::Sym pointer) const;
  /// `strlen(pointer)` after a call measured it: the known length, or a new
  /// length symbol the object's string fact records.
  core::Term measureString(core::Sym pointer, const clang::Expr *argument);
  /// A row's `writes-str`: the string at `pointer` has `length` characters.
  void noteStringWritten(core::Sym pointer, const core::Term &length);
  /// A store of `value` (`width` bytes) at `offset` into `object`.
  void stringStored(core::ObjectId id, const core::Term &offset,
                    std::int64_t width, core::Sym value);
  [[nodiscard]] FormatFacts formatFacts(const clang::CallExpr &call,
                                        const core::LibraryMatch &match) const;

  /// RFC 0030 §3.4, RFC 0031 §5.5: whether releasing `pointer` releases
  /// the start of heap objects, with the message when it may not.
  struct ReleaseCheck {
    enum class Kind : std::uint8_t {
      Proven,
      UnknownIndex,
      Possible,
      Violation
    };
    Kind kind = Kind::Proven;
    std::string message;
    std::string note;
    core::SourceLocation noteAt = {};
  };
  /// `anywhere`: the release takes a range (`munmap`, RFC 0034 §6.3), so
  /// a pointer into a heap object need not point to its start.
  [[nodiscard]] ReleaseCheck releaseCheck(core::Sym pointer,
                                          const std::string &subject,
                                          const clang::Expr &operand,
                                          const std::string &verb,
                                          bool anywhere = false) const;
  /// A path below a call's argument as the caller spells it (`b.data` for
  /// `param0->data` with `&b`, `b->data` with `b`), when it can.
  [[nodiscard]] std::optional<std::string>
  spellArgumentPath(const clang::CallExpr &call,
                    const core::SummaryPath &path) const;
  /// RFC 0030 §5.3: the functions a callback argument's value may be; empty
  /// when it is not known.
  [[nodiscard]] std::vector<const clang::FunctionDecl *>
  syncTargets(core::Sym function) const;
  /// RFC 0031 §6.3: a callee's release through `valuePath` that the
  /// caller's memory cannot follow; applies the unknown-callee default to
  /// what the argument reaches and says so on the call. False when the path
  /// does not start at a pointer argument.
  bool lostView(const clang::CallExpr &call, const std::vector<core::Sym> &args,
                const core::SummaryPath &valuePath);
  /// RFC 0008: `value`, read from `lvalue` at `address`, is a local
  /// pointer no path assigned.
  void uninitialisedRead(core::Sym value, const clang::Expr &lvalue,
                         const Address &address);
  // Null findings (EngineDecide.cpp, RFC 0008, RFC 0030 §3.2).
  /// Adds the note saying why `value` (spelled `name`) is null.
  void addNullNote(core::Diagnostic &diagnostic, const core::SymInfo &value,
                   const std::string &name) const;
  /// The `null-dereference` diagnostic for a null argument `arg` a callee
  /// (spelled `callee`, declared at `declared` when valid) dereferences.
  [[nodiscard]] core::Diagnostic
  nullArgument(const clang::Expr &arg, const core::SymInfo &value,
               const std::string &callee,
               const clang::FunctionDecl *declared) const;
  /// RFC 0030 §3.2: a use of an allocation's result that may be null (the
  /// off-by-default `allocation-failure`), reported at `at`.
  void allocationFailure(const core::SymInfo &value, const clang::Expr &at,
                         const clang::Stmt &site);
  // Annotation mismatches (EngineAnnotations.cpp, RFC 0003, RFC 0012).
  void reportMismatch(std::string message, clang::SourceLocation at,
                      std::string note, clang::SourceLocation noteAt,
                      std::string copy, const clang::Stmt &site,
                      std::optional<core::Facet> facet = core::Facet::Temporal);
  void checkConsumeAnnotation(core::Sym value, const clang::Expr &operand,
                              const clang::Stmt &site, bool moved);
  void checkWriteAnnotation(const Address &address, const clang::Stmt &site);
  void checkCallAnnotations(const clang::CallExpr &call,
                            const std::vector<core::Sym> &args);
  void checkReturnAnnotation(core::Sym value, const clang::Expr &returned);
  void checkSizedFieldStore(const clang::MemberExpr &lhs,
                            const Address &address);
  /// An amount of bytes for messages: `6 bytes`, `'strlen(s)' + 1 bytes`;
  /// `own` prefers the name a value was made under to the place holding it.
  [[nodiscard]] std::optional<std::string> spellAmount(core::Term bytes,
                                                       bool own) const;
  /// A C place holding `sym` at this point, for messages (§5.3); for an
  /// `extent` (a have), also its defining operation.
  [[nodiscard]] std::optional<SourceTerm>
  nameOf(core::Sym sym, bool extent = false, int depth = 0) const;

  // Lifetimes, boundaries and assumptions (EngineLifetimes.cpp).
  /// §5.7, §5.6: the exit of the function at `exit` (a `return` or the end
  /// of the body): frame storage left where the caller can find it.
  void exitLifetimes(const clang::Stmt &exit, const clang::ReturnStmt *ret);
  /// §5.6: the boundary facts of a call, from the state before its effects.
  void callBoundary(const clang::CallExpr &call,
                    const std::vector<core::Sym> &args);
  /// RFC 0030 §5.7: an `asm` statement applies the unknown-callee default
  /// to its pointer operands.
  void inlineAssembly(const clang::GCCAsmStmt &assembly);
  /// The call a `cleanup` attribute makes when `var`'s scope ends.
  void cleanupFunction(const clang::VarDecl &var);
  /// What an unknown callee may do to the objects reachable from `start`;
  /// with `mayOwn`, it may have taken over what they own.
  void unknownEffect(const std::vector<core::ObjectId> &start,
                     const core::ReleaseRecord &record, bool mayOwn);
  /// A truth value (a comparison, a logical operator, `!`, a scalar) with a
  /// condition refinement can decide, evaluated afresh here; none when the
  /// expression is none of these.
  std::optional<core::Sym> truthValue(const clang::Expr &expr);
  /// A comparison evaluated afresh from its operands' values here.
  core::Sym comparison(const clang::BinaryOperator &op) {
    return compare(op.getOpcode(), valueOf(*op.getLHS()), valueOf(*op.getRHS()),
                   op.getLHS()->getType());
  }
  /// RFC 0030 §6.2: a `WEAVEC_ASSUME` call; false when `call` is not one.
  bool assumption(const clang::CallExpr &call);
  /// §5.2, §5.7: a use of `operand` whose value may point to storage whose
  /// lifetime ended: `lifetime-too-short`, reported where the pointer was
  /// stored.
  /// RFC 0004 *Laundering*: a raw `value` stored into a place declared
  /// with a safe kind (or returned from a function whose result is) asserts
  /// that kind, which needs an unsafe region; the place holds a value that
  /// is no longer raw. `target` names the place, or is empty for a return.
  core::Sym launder(core::Sym value, const AnnotationSet &declared,
                    const std::string &target, const clang::Expr &source);
  void danglingUse(const clang::Expr &operand,
                   const core::TemporalVerdict &verdict,
                   const clang::Stmt *site, bool definite);

  [[nodiscard]] core::HeapState &heapState() noexcept { return state; }
  [[nodiscard]] FunctionRun &functionRun() noexcept { return run; }
  [[nodiscard]] core::Heap &domain() noexcept { return heap; }

private:
  FunctionRun &run;
  core::Heap &heap;
  core::HeapState &state;
  clang::ASTContext &context;
  llvm::DenseMap<const clang::Expr *, ExprResult> memo;
  /// The next element starts a statement.
  bool statementStart = true;

  ExprResult evaluateUncached(const clang::Expr &expr);
  core::Sym evaluateCast(const clang::CastExpr &cast);
  core::Sym evaluateUnary(const clang::UnaryOperator &op);
  core::Sym evaluateBinary(const clang::BinaryOperator &op);
  core::Sym evaluateAssign(const clang::BinaryOperator &op);
  core::Sym arithmetic(clang::BinaryOperatorKind kind, core::Sym left,
                       core::Sym right, clang::QualType type,
                       const clang::Expr &at);
  /// RFC 0017: `value` of C type `from` converted to `to` (modular).
  core::Sym convertInteger(core::Sym value, clang::QualType from,
                           clang::QualType to);
  /// The same, to the integer type `target` (a bit-field's width) of a
  /// value of C type `to`.
  core::Sym convertInteger(core::Sym value, clang::QualType from,
                           const core::IntegerType &target, clang::QualType to);
  /// RFC 0017: a bit-field holds its declared width. One whose bytes no
  /// other member shares is a cell of its own; the others are not tracked
  /// (their stores forget the bytes, their loads are unknown in the width).
  [[nodiscard]] std::optional<core::IntegerType>
  bitFieldType(const clang::FieldDecl &field) const;
  core::Sym loadBitField(const Address &address, clang::QualType type,
                         const clang::FieldDecl &field, const clang::Expr *at);
  /// Stores `value` converted to the bit-field's width; returns the value
  /// stored (the assignment's value).
  core::Sym storeBitField(const Address &address, core::Sym value,
                          clang::QualType type, const clang::FieldDecl &field,
                          const clang::Expr *at);
  /// RFC 0017: `__builtin_*_overflow(a, b, out)`: the value stored through
  /// `out` and the overflow flag (the call's `result`, refined).
  core::Sym checkedArithmetic(const clang::CallExpr &call, core::IntegerOp op,
                              const std::vector<core::Sym> &args,
                              core::Sym result);
  /// RFC 0017, RFC 0031 §5.10: an operation invalid for every value.
  void reportInteger(core::IntegerError error, const clang::Expr &at);
  core::Sym compare(clang::BinaryOperatorKind kind, core::Sym left,
                    core::Sym right, clang::QualType operandType);
  void declare(const clang::VarDecl &var);
  /// RFC 0007 *Owned fields*: at a release of `operand`, what the entry
  /// container's `WEAVEC_OWNED` fields hold is owned (so leaked unless
  /// released or moved first).
  void declaredOwners(const clang::Expr &operand, const std::string &released);
  void initialize(const Address &address, clang::QualType type,
                  const clang::Expr *init);
  void zeroFill(const Address &address, clang::QualType type);
  void lifetimeEnds(const clang::VarDecl &var);
  void returned(const clang::ReturnStmt &ret);
  void checkLeaks(core::Sym overwritten, const clang::Stmt &at);
  /// §5.7: notes where a pointer to frame storage was stored into a cell
  /// (EngineLifetimes.cpp).
  void recordFrameStore(core::ObjectId holder, core::CellKey key,
                        core::Sym value, const clang::Expr *at,
                        const std::string &holderName);
};

/// Everything the unit's functions share: summaries, the owning slots, the
/// library, the kinds, the options.
class UnitRun {
public:
  UnitRun(const EngineInput &engineInput, LedgerAdapter &adapter);

  void analyzeAll(
      const std::function<bool(const clang::FunctionDecl &)> &shouldReport);
  [[nodiscard]] UnitExports exports();
  void dump(const clang::FunctionDecl &function, llvm::raw_ostream &os);

  const EngineInput &input;
  LedgerAdapter &authoritative;
  /// A discarding adapter for summary rounds.
  LedgerAdapter discarding;
  /// Alias contexts already run, per callee (§6.6), and their count.
  std::map<const clang::FunctionDecl *, std::set<AliasContext>> contextsRun;
  /// RFC 0034 §6.1: what each context run found, for the replay of a
  /// caller, which does not run a context twice.
  std::map<std::pair<const clang::FunctionDecl *, AliasContext>,
           std::vector<core::Diagnostic>>
      contextFindings;
  /// §6.6 *Amendment (numeric contexts)*: the summaries derived per callee
  /// and context (none: the run was over budget or incomplete).
  std::map<const clang::FunctionDecl *,
           std::map<AliasContext, std::optional<core::FunctionEffects>>>
      contextSummaries;
  /// The members of the recursive component being summarised, whose
  /// summaries are not final.
  std::set<const clang::FunctionDecl *> unsettled;
  /// The block transfers each function's last run took (what a context
  /// run of it may cost).
  std::map<const clang::FunctionDecl *, std::uint64_t> transfersOf;
  /// RFC 0034 §7.2: the work each function's last run took.
  std::map<const clang::FunctionDecl *, std::uint64_t> workOf;
  /// RFC 0033 §1: `onlyValueUses` by definition, parameter and depth.
  std::map<std::tuple<const clang::FunctionDecl *, unsigned, unsigned>, bool>
      valueOnly;
  /// RFC 0033 §9, RFC 0034 §7.1: the unit's budget of work (0:
  /// unlimited), what every run in it has spent, and the definitions whose
  /// authoritative run is still to come.
  std::uint64_t unitBudget = 0;
  std::uint64_t unitWork = 0;
  std::uint64_t functionsLeft = 1;
  /// A run's share of the work left: eight fair shares (0 unlimited).
  [[nodiscard]] std::uint64_t runShare() const {
    static constexpr std::uint64_t MinRunShare = 100000;
    const std::uint64_t left = unitBudget - std::min(unitBudget, unitWork);
    return unitBudget == 0
               ? 0
               : std::max(MinRunShare,
                          8 * left / std::max<std::uint64_t>(1, functionsLeft));
  }
  /// Summaries of the unit's definitions, by canonical declaration.
  std::map<const clang::FunctionDecl *, core::FunctionEffects> summaries;
  std::set<const clang::FunctionDecl *> incomplete;
  /// Members of a recursive component whose summary round spent the
  /// per-function budget (RFC 0033 *Implementation amendments*).
  std::set<const clang::FunctionDecl *> exhausted;
  /// §9.4, §4.5 D2: fields and globals some function releases a value
  /// loaded from.
  llvm::DenseSet<const clang::Decl *> owningSlots;
  /// Over-budget functions.
  std::set<const clang::FunctionDecl *> overBudget;

  [[nodiscard]] clang::ASTContext &context() const { return input.context; }
  [[nodiscard]] const core::LibrarySpec &library() const {
    return input.library;
  }
  /// The summary of `callee` from this unit or the program database.
  [[nodiscard]] const core::FunctionEffects *
  summaryOf(const clang::FunctionDecl &callee) const;
  /// Whether `callee` has a body in this unit.
  [[nodiscard]] bool hasBody(const clang::FunctionDecl &callee) const;
  /// The summary id of a global variable, and back (RFC 0005 `global(g)`).
  std::uint32_t globalId(const clang::VarDecl &var);
  [[nodiscard]] const clang::VarDecl *globalDecl(std::uint32_t id) const;
  [[nodiscard]] const std::vector<const clang::VarDecl *> &globals() const {
    return globalList;
  }

  /// The name another unit knows a global by: its own for external
  /// linkage, `<source>#<name>` otherwise (which no other unit has).
  [[nodiscard]] std::string portableName(const clang::VarDecl &var) const;
  /// RFC 0005: the unit's functions of `typeKey` whose address is taken,
  /// the candidates of an indirect call nothing else resolves in the
  /// program.
  [[nodiscard]] const std::vector<const clang::FunctionDecl *> &
  localCandidates(const std::string &typeKey) const;
  /// §4.6: a variable with static storage and internal linkage that the
  /// unit only reads scalars of (its address is never taken, no store
  /// names it): every cell holds its initializer's value.
  [[nodiscard]] bool keepsInitializer(const clang::VarDecl &var) const;

  /// §7 *Amendment (cross-unit contexts)*: the name another unit knows a
  /// function by, and the unit's function of that name.
  [[nodiscard]] std::string portableName(const clang::FunctionDecl &fn) const;
  [[nodiscard]] const clang::FunctionDecl *
  functionNamed(const std::string &portable) const;
  /// The unit's global of a portable name.
  [[nodiscard]] const clang::VarDecl *
  globalNamed(const std::string &portable) const;
  /// A summary of the program database (`key` names it in the cache), in
  /// this unit's global numbering.
  [[nodiscard]] const core::FunctionEffects *
  importEffects(const std::string &key,
                const core::FunctionEffects &effects) const;
  /// The contexts this unit's calls asked of other units' functions, and
  /// the summaries of its own functions in the contexts others asked for.
  std::set<ContextRequest> contextRequests;
  std::map<ContextRequest, core::FunctionEffects> servedContexts;
  /// Runs the contexts other units asked of this unit's functions: their
  /// summaries, and (for aliased arguments) their diagnostics.
  void serveContextRequests(
      const std::function<bool(const clang::FunctionDecl &)> &shouldReport);

  /// RFC 0030 §7.6 (RFC 0031 *Implementation amendments*, "Counted-field
  /// invariants"): the candidates standing, the kinds they give their
  /// pointer fields while assumed, and those a checking run refuted.
  std::vector<const ResolvedCandidate *> standing;
  std::map<const clang::FieldDecl *, KindEntry> assumedFields;
  std::map<const clang::FieldDecl *, const ResolvedCandidate *>
      assumedCandidate;
  std::set<const ResolvedCandidate *> refuted;
  std::set<const ResolvedCandidate *> witnessed;
  /// The kind of `field`: its table entry, or an assumed invariant's when
  /// the entry gives no extent.
  [[nodiscard]] const KindEntry *fieldKind(const clang::FieldDecl &field) const;
  /// Houdini over the candidates (checking runs of the functions that write
  /// their fields), then the functions that read a standing invariant's
  /// pointer field analysed again with it.
  void inferInvariants(
      const std::vector<const clang::FunctionDecl *> &definitions,
      const std::function<bool(const clang::FunctionDecl &)> &shouldReport);

private:
  mutable std::optional<std::map<std::string, const clang::FunctionDecl *>>
      functionsByName;
  mutable std::optional<llvm::DenseSet<const clang::VarDecl *>> initializerOnly;
  // Interned lazily, also while a const lookup imports a summary.
  mutable std::vector<const clang::VarDecl *> globalList;
  mutable llvm::DenseMap<const clang::VarDecl *, std::uint32_t> globalIds;
  std::uint32_t internGlobal(const clang::VarDecl &var) const;
  void computeOwningSlots();
  /// §7: summaries imported from the program database, by callee name, in
  /// this unit's global numbering.
  mutable std::map<std::string, core::FunctionEffects> imported;
  /// The unit's global variables by portable name.
  mutable std::optional<std::map<std::string, const clang::VarDecl *>>
      globalsByName;
  mutable std::optional<
      std::map<std::string, std::vector<const clang::FunctionDecl *>>>
      candidatesByType;
};

} // namespace weavec::analysis::engine

#endif // WEAVEC_LIB_ANALYSIS_ENGINE_H
