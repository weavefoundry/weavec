//===- LedgerAdapter.h - The engine seam (RFC 0030) -------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §14: the only channel from an engine to the ledger. The engine
// publishes, per site and facet, decisions, requirements' decisions and
// boundary facts, and its diagnostics with their certainty; `finish`
// completes the unit ledger (RFC 0035 §8: in memory, for the summary line):
//
//   1. every undecided facet takes its §2.6 default (with reason `budget` in
//      an over-budget function); IntToPtr sites are `unresolved(raw-cast)`
//      (§7.4), facets resting only on system-header attributes
//      `trusted(system-api)` (§5.2);
//   2. the ledger-side rules of `WEAVEC_UNSAFE` (§6.1), the concurrency
//      rules (§5.3) and `setjmp` (§5.4);
//   3. the boundary invariants' decisions and their propagation (§9.4).
//
// Within one authoritative pass, records of one facet merge by rank (§2.5);
// `beginFunction` discards every row an earlier pass recorded for the
// function. Fixpoint and Houdini rounds publish into a discarding adapter,
// which keeps nothing, diagnostics included.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_LEDGERADAPTER_H
#define WEAVEC_ANALYSIS_LEDGERADAPTER_H

#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/Path.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"

#include "llvm/ADT/DenseSet.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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
    /// How the place is spelled here, for the row's detail.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string name = {};
  };
  /// Two owning places that may hold the same object (`second-owner`).
  struct SharedOwners {
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    core::SummaryPath first = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    core::SummaryPath second = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string placeClass = {};
    /// The other place's class, when it differs.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string otherClass = {};
    /// How the two places are spelled here, for the row's detail.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string names = {};
    /// RFC 0031 §8: `first` owns an object its own object is reachable
    /// from through owning cells (an owning cycle; `second` is `first`).
    bool cycle = false;
  };
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Dangling> dangling = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<SharedOwners> sharedOwners = {};
};

/// §9.4: one decision of the boundary invariants: the temporal facet of
/// `site` takes `reason`. A decision the *propagation* made applies only to
/// a facet that would otherwise be proven, because only such a facet rests
/// on the entry assumption the boundary broke.
struct BoundaryDecision {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::SiteId site = {};
  core::UnresolvedReason reason = core::UnresolvedReason::DanglingEscape;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};
  bool propagated = false;
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

struct LedgerAdapterOptions {
  /// The unit's main source as the user named it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  /// §5.3: whether the site's pointer is loaded from a place shared with a
  /// thread or signal entry point (rooted in G).
  std::function<bool(const SiteInfo &)> concurrent = nullptr;
};

/// A boundary fact or store verdict as published, with its site.
struct PublishedBoundary {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::SiteId site = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  BoundaryFacts facts = {};
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
    /// RFC 0034 §6.1: the replay of one function along single paths. The
    /// unit's sites apply as in the authoritative pass, nothing is
    /// published, and every decision goes to `observer`.
    Witness,
  };

  LedgerAdapter(clang::ASTContext &ctx, const SiteIndex &siteIndex,
                LedgerAdapterOptions adapterOptions = {},
                Mode adapterMode = Mode::Authoritative);
  /// A discarding or collecting adapter, which needs no sites.
  LedgerAdapter(clang::ASTContext &ctx, Mode adapterMode);

  /// Whether decisions, requirements and the other facts are dropped: every
  /// mode but the authoritative one.
  [[nodiscard]] bool isDiscarding() const noexcept {
    return mode != Mode::Authoritative;
  }
  [[nodiscard]] Mode adapterMode() const noexcept { return mode; }
  [[nodiscard]] const SiteIndex &siteIndex() const noexcept { return sites; }
  /// Whether `facet` applies to the site `id` (§2.1).
  [[nodiscard]] bool applies(core::SiteId id, core::Facet facet) const;

  /// Starts the authoritative pass over `function` and discards every row
  /// and budget mark an earlier pass recorded for it (§2.5, §2.6).
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
  /// A suggested annotation (§12.1 `fixit`) for `facet` of the site of
  /// `stmt` of kind `kind`: §5.1's suggestion at an unknown callee's call.
  /// The first suggestion of a pass stands.
  void suggest(const clang::Stmt &site, core::SiteKind kind,
               std::optional<core::Boundary> boundary, core::Facet facet,
               core::FixItHint fixit);
  /// One requirement's decision for a LibCall, Release or Call facet,
  /// merged into the facet by rank (§2.5).
  void requirement(const clang::Stmt &site, core::Facet facet,
                   const core::FacetDecision &decision);
  /// A diagnostic with its certainty; `site` and `facet` link it to its row.
  /// The diagnostic's own `certainty` is set to `certainty`. The same
  /// diagnostic (id, location and message) is reported once per unit, as
  /// the engine's passes may find it more than once.
  void report(core::Diagnostic diagnostic, core::Certainty certainty,
              const clang::Stmt *site = nullptr,
              std::optional<core::Facet> facet = {});
  /// Caller-visible places at a call or exit that may hold released
  /// pointers or aliased owners (§9.4).
  void boundary(const clang::Stmt &site, BoundaryFacts facts);
  /// §9.4 as RFC 0031 amends it: the proven temporal facet of `site` rests
  /// on the entry assumption of a place of `placeClass` (its pointer is a
  /// value that place held at entry, or derived from one).
  void reliesOn(core::SiteId site, std::string placeClass);
  /// A function body exceeded its budget (§5.5).
  void overBudget(const clang::FunctionDecl &function);
  /// §9.4: what `BoundaryInvariants` made of `boundaries()`; `finish`
  /// records the broken boundaries and applies the propagation, after the
  /// defaults.
  void boundaryDecisions(std::vector<BoundaryDecision> decisions);

  /// RFC 0034 §6.1: a definite error of the authoritative pass that the
  /// replay did not confirm. Its facet's violation becomes
  /// `unresolved(unconfirmed)`, and each of its diagnostics at the site a
  /// warning with the note `not confirmed on a feasible path`.
  void unconfirm(const clang::Stmt &site, core::Facet facet,
                 std::string_view id);
  /// Witness mode: called with every decision of a facet of a site.
  std::function<void(const clang::Stmt &, core::Facet,
                     const core::FacetDecision &)>
      observer;

  /// Completes the unit's ledger; see the file comment. Call once, after
  /// the engine is done.
  [[nodiscard]] core::Ledger finish();

  /// The diagnostics to report, in publication order.
  [[nodiscard]] const std::vector<core::Diagnostic> &
  diagnostics() const noexcept {
    return emitted;
  }
  [[nodiscard]] const std::vector<PublishedBoundary> &
  boundaries() const noexcept {
    return boundaryList;
  }
  [[nodiscard]] const std::map<core::SiteId, std::set<std::string>> &
  reliances() const noexcept {
    return relied;
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
  const clang::FunctionDecl *current = nullptr;
  llvm::DenseSet<std::uint32_t> overBudgetFunctions;
  std::vector<core::Diagnostic> emitted;
  /// (id, file, line, column, message) of what `emitted` holds, with its
  /// index among the ledger's diagnostics when it was published there.
  std::map<std::tuple<std::string, std::string, std::uint32_t, std::uint32_t,
                      std::string>,
           std::optional<std::uint32_t>>
      emittedKeys;
  std::vector<core::LedgerDiagnostic> ledgerDiagnostics;
  std::vector<Orphan> orphans;
  std::vector<PublishedBoundary> boundaryList;
  std::map<core::SiteId, std::set<std::string>> relied;
  std::vector<BoundaryDecision> boundaryRows;
  bool finished = false;

  /// The record of `facet` at `id`, or null when the facet does not apply.
  core::FacetRecord *record(core::SiteId id, core::Facet facet);
  void noteOrphan(const clang::Stmt &stmt, core::Facet facet);
  void decideAt(std::optional<core::SiteId> id, const clang::Stmt &stmt,
                core::Facet facet, const core::FacetDecision &decision);
  /// Records a diagnostic, linked to a facet of a site when given.
  /// Links the ledger diagnostic `index` to its site's facet (a violation
  /// when it is a definite error).
  void link(std::uint32_t index, const core::Diagnostic &diagnostic,
            core::Certainty certainty, std::optional<core::SiteId> id,
            std::optional<core::Facet> facet);
  void publish(core::Diagnostic diagnostic, core::Certainty certainty,
               std::optional<core::SiteId> id,
               std::optional<core::Facet> facet);
  /// The emitted function whose definition holds `loc`, or the current one.
  [[nodiscard]] std::string
  functionNameAt(const core::SourceLocation &loc) const;
  void fillDefaults();
  void applyOverrides();
  /// §9.4: records the broken boundaries and their propagation.
  void applyBoundaries();
  void appendOrphanRows();
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_LEDGERADAPTER_H
