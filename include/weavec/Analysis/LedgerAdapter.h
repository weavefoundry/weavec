//===- LedgerAdapter.h - The engine seam (RFC 0030) -------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §14: the only channel from an engine to the ledger. The engine
// publishes, per site and facet, decisions, requirement records, witnesses
// and boundary facts, and its diagnostics with their certainty;
// `finish` completes the unit ledger:
//
//   1. every undecided facet takes its §2.6 default (with reason `budget` in
//      an over-budget function); IntToPtr sites are `unresolved(raw-cast)`
//      (§7.4), facets resting only on system-header attributes
//      `trusted(system-api)` (§5.2);
//   2. the ledger-side rules of `WEAVEC_UNSAFE` (§6.1) and `setjmp` (§5.4);
//   3. `CheckPlanner::plan` (§10.1), which turns inexpressible checked
//      records `unresolved(inexpressible)`;
//   4. the require-level errors (§6.3), from the planned ledger.
//
// Boundary propagation (§9.4, stage S7), the concurrency rules (§5.3, S4)
// and the §7.6 upgrades (S6) slot in between 2 and 3; until then their
// facts are only recorded.
//
// Within one authoritative pass, records of one facet merge by rank (§2.5);
// `beginFunction` discards every row an earlier pass recorded for the
// function. Fixpoint and Houdini rounds publish into a discarding adapter,
// which keeps nothing, diagnostics included.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_LEDGERADAPTER_H
#define WEAVEC_ANALYSIS_LEDGERADAPTER_H

#include "weavec/Analysis/CheckPlanner.h"
#include "weavec/Analysis/CheckWitness.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/CheckPlan.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"

#include "llvm/ADT/DenseSet.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace weavec::analysis {

/// §9.4: what the engine knows at a boundary about the places reachable
/// from the function's parameters and globals.
struct BoundaryFacts {
  /// A place that may hold a released pointer, or a pointer to storage whose
  /// lifetime has ended (`dangling-escape`).
  struct Dangling {
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    core::SummaryPath place = {};
    /// Where the pointee was released or its storage ended.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    core::SourceLocation released = {};
    /// The place class for propagation: a global `g`, or a field path
    /// `<struct>.<field>...`.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string placeClass = {};
  };
  /// Two owning places that may hold the same object (`second-owner`).
  struct SharedOwners {
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    core::SummaryPath first = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    core::SummaryPath second = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string placeClass = {};
  };
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Dangling> dangling = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<SharedOwners> sharedOwners = {};
};

/// §7.6: a counted-field invariant candidate, `count(d) == f + c` or
/// `bytes(d) == f + c` with `c` 0 or 1.
struct FieldCandidate {
  /// The record type key (`struct buf`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string record = {};
  /// The pointer field `d` and the integer field `f`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string pointer = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string count = {};
  /// `bytes(d)` rather than `count(d)`.
  bool bytes = false;
  /// `c`.
  std::int64_t offset = 0;

  friend auto operator<=>(const FieldCandidate &,
                          const FieldCandidate &) = default;
};

/// §14: the unit's ledger and its check plan.
struct PlannedLedger {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::Ledger ledger = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::CheckPlan plan = {};
  /// Resolves the plan's place and type handles.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PlaceHandleTable handles = {};
  /// Resolves the plan's site ids to statements (`CheckEmitter`, S5).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::shared_ptr<const SiteIndex> sites = {};
};

struct LedgerAdapterOptions {
  /// §12.1 `config`: the checks mode, zero-initialisation, the require
  /// level and the budget.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::LedgerConfig config = {};
  /// The unit's main source as the user named it, and the target triple.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string target = {};
  /// §3.4: whether a facet's violation was lowered to a warning.
  std::function<bool(core::SiteId, core::Facet)> lowered = nullptr;
};

/// A boundary fact or store verdict as published, with its site.
struct PublishedBoundary {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::SiteId site = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  BoundaryFacts facts = {};
};
struct PublishedVerdict {
  const clang::Stmt *store = nullptr;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  FieldCandidate candidate = {};
  core::Verdict verdict = core::Verdict::Unknown;
};

/// The only channel from an engine to the ledger (§14).
class LedgerAdapter {
public:
  enum class Mode : std::uint8_t {
    /// The pass whose rows and diagnostics stand.
    Authoritative,
    /// Fixpoint and Houdini rounds: everything is dropped.
    Discarding,
    /// Context-specialised runs (RFC 0016, §2.6): no row is decided, but
    /// the diagnostics are kept, with their certainty, for the caller to
    /// report at its call.
    Collecting,
  };

  LedgerAdapter(clang::ASTContext &ctx, const SiteIndex &siteIndex,
                LedgerAdapterOptions adapterOptions = {},
                Mode adapterMode = Mode::Authoritative);
  /// A discarding or collecting adapter, which needs no sites.
  LedgerAdapter(clang::ASTContext &ctx, Mode adapterMode);

  /// Whether decisions, requirements, witnesses and the other facts are
  /// dropped: every mode but the authoritative one.
  [[nodiscard]] bool isDiscarding() const noexcept {
    return mode != Mode::Authoritative;
  }
  [[nodiscard]] Mode adapterMode() const noexcept { return mode; }
  [[nodiscard]] const SiteIndex &siteIndex() const noexcept { return sites; }
  [[nodiscard]] clang::ASTContext &astContext() const noexcept {
    return context;
  }
  /// Whether `facet` applies to the site `id` (§2.1).
  [[nodiscard]] bool applies(core::SiteId id, core::Facet facet) const;
  /// The function whose authoritative pass is running, or null.
  [[nodiscard]] const clang::FunctionDecl *currentFunction() const noexcept {
    return current;
  }

  /// Starts the authoritative pass over `function` and discards every row,
  /// witness and budget mark an earlier pass recorded for it (§2.5, §2.6).
  void beginFunction(const clang::FunctionDecl &function);
  /// One decision for one facet of a known site; may be called more than
  /// once within a pass, and records merge by rank (§2.5). For a `return`
  /// or a function body, the decision is about its exit.
  void decide(const clang::Stmt &site, core::Facet facet,
              core::SiteOutcome outcome,
              std::optional<core::UnresolvedReason> unresolved = {},
              std::optional<core::TrustReason> trusted = {},
              std::string detail = {});
  /// As `decide`, for the site of `stmt` of kind `kind` (and boundary):
  /// when one statement stands for several sites, such as a call to a
  /// function that does not return and its exit.
  void decideAs(const clang::Stmt &site, core::SiteKind kind,
                std::optional<core::Boundary> boundary, core::Facet facet,
                const core::FacetDecision &decision);
  /// One requirement record of a LibCall, Release or Call facet: kept in
  /// the row's `requirements` with its own outcome, and merged into the
  /// facet by rank (§2.5). A witness for it may come along.
  void requirement(const clang::Stmt &site, core::Facet facet,
                   core::Requirement record,
                   std::optional<CheckWitness> witness = std::nullopt);
  /// A diagnostic with its certainty; `site` and `facet` link it to its row.
  /// The diagnostic's own `certainty` is set to `certainty`. The same
  /// diagnostic (id, location and message) is reported once per unit, as
  /// the engine's passes may find it more than once.
  void report(core::Diagnostic diagnostic, core::Certainty certainty,
              const clang::Stmt *site = nullptr,
              std::optional<core::Facet> facet = {});
  /// What the check of a checked (or, in verify mode, proven) facet needs.
  void witness(const clang::Stmt &site, core::Facet facet,
               CheckWitness checkWitness);
  /// Caller-visible places at a call or exit that may hold released
  /// pointers or aliased owners (§9.4).
  void boundary(const clang::Stmt &site, BoundaryFacts facts);
  /// A function body exceeded its budget (§5.5).
  void overBudget(const clang::FunctionDecl &function);
  /// A store verdict for a field-invariant candidate (§7.6).
  void storeVerdict(const clang::Stmt &store, FieldCandidate candidate,
                    core::Verdict verdict);

  /// Completes the unit's ledger and plans its checks; see the file
  /// comment. Call once, after the engine is done.
  [[nodiscard]] PlannedLedger finish();

  /// The diagnostics to report, in publication order; `finish` appends the
  /// require-level errors.
  [[nodiscard]] const std::vector<core::Diagnostic> &
  diagnostics() const noexcept {
    return emitted;
  }
  /// Decisions about statements `SiteCollector` did not enumerate (§2.6
  /// item 2: an internal error; each becomes an `unresolved(unanalysed)`
  /// row of its function).
  [[nodiscard]] std::size_t orphanDecisions() const noexcept {
    return orphans.size();
  }
  [[nodiscard]] const std::vector<PublishedBoundary> &
  boundaries() const noexcept {
    return boundaryList;
  }
  [[nodiscard]] const std::vector<PublishedVerdict> &
  storeVerdicts() const noexcept {
    return verdictList;
  }
  /// The unit's rows as published so far (tests, dumps).
  [[nodiscard]] const core::UnitLedger &unitLedger() const noexcept {
    return unit;
  }

private:
  struct Orphan {
    const clang::Stmt *stmt = nullptr;
    std::uint32_t function = 0;
    core::Facet facet = core::Facet::Temporal;
  };

  clang::ASTContext &context;
  const SiteIndex &sites;
  LedgerAdapterOptions options;
  Mode mode;
  core::UnitLedger unit;
  WitnessTable witnesses;
  const clang::FunctionDecl *current = nullptr;
  llvm::DenseSet<std::uint32_t> overBudgetFunctions;
  std::vector<core::Diagnostic> emitted;
  /// (id, file, line, column, message) of what `emitted` holds.
  std::set<std::tuple<std::string, std::string, std::uint32_t, std::uint32_t,
                      std::string>>
      emittedKeys;
  std::vector<core::LedgerDiagnostic> ledgerDiagnostics;
  std::vector<Orphan> orphans;
  std::vector<PublishedBoundary> boundaryList;
  std::vector<PublishedVerdict> verdictList;
  bool finished = false;

  /// The record of `facet` at `id`, or null when the facet does not apply.
  core::FacetRecord *record(core::SiteId id, core::Facet facet);
  void noteOrphan(const clang::Stmt &stmt, core::Facet facet);
  void decideAt(std::optional<core::SiteId> id, const clang::Stmt &stmt,
                core::Facet facet, const core::FacetDecision &decision);
  /// Records a diagnostic, linked to a facet of a site when given.
  void publish(core::Diagnostic diagnostic, core::Certainty certainty,
               std::optional<core::SiteId> id,
               std::optional<core::Facet> facet);
  /// The emitted function whose definition holds `loc`, or the current one.
  [[nodiscard]] std::string
  functionNameAt(const core::SourceLocation &loc) const;
  void fillDefaults(PlannerOptions &planner);
  void applyOverrides(PlannerOptions &planner);
  void appendOrphanRows();
  void reportRequireLevel();
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_LEDGERADAPTER_H
