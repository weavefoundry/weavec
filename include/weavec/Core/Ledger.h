//===- Ledger.h - Per-site analysis outcomes (RFC 0030, 0035) --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 records one outcome for every safety facet (spatial, null,
// temporal) of every memory operation the analysis sees, and for the
// assertion facet of every `WEAVEC_ASSUME` (§2). RFC 0035 §8 makes the
// analysis advisory: the outcomes are `proven`, `violation`, `unresolved`
// and `trusted`, they stay in memory, and they feed the summary line. This
// header is the frontend-neutral model of that record:
//
//   Ledger -> units -> functions -> sites -> facets
//          -> diagnostics (a facet links to its diagnostic by index)
//
// with the closed reason lists (§2.3, §2.4), merging by rank (§2.5), the
// undecided defaults (§2.6), and the rollup behind the summary line.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_LEDGER_H
#define WEAVEC_CORE_LEDGER_H

#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/SourceLocation.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// Closed vocabularies
//===----------------------------------------------------------------------===//

/// §2.1: the operation a site stands for.
enum class SiteKind : std::uint8_t {
  Deref,
  Index,
  PtrArith,
  Cast,
  IntToPtr,
  LibCall,
  Release,
  Call,
  Assume,
  Raw,
};

/// §2.1: the obligations of a site. `Assertion` belongs to Assume sites only.
enum class Facet : std::uint8_t { Spatial, Null, Temporal, Assertion };
inline constexpr std::size_t FacetCount = 4;
inline constexpr std::array<Facet, FacetCount> AllFacets{
    Facet::Spatial, Facet::Null, Facet::Temporal, Facet::Assertion};

/// §2.2 as RFC 0035 §8 leaves it. Named `SiteOutcome` because
/// `core::Outcome` is the RFC 0006 result class.
enum class SiteOutcome : std::uint8_t {
  Proven,
  Violation,
  Unresolved,
  Trusted,
};

/// §2.3: why a facet is not proven. Closed: adding a reason requires an RFC.
enum class UnresolvedReason : std::uint8_t {
  UnknownExtent,
  UnknownIndex,
  Inexpressible,
  MayReleased,
  MayMoved,
  MayAliasReleased,
  MayInvalidRelease,
  MayMismatchedRelease,
  MayDangle,
  MayConflict,
  UnknownCallee,
  Callback,
  Setjmp,
  Budget,
  Unanalysed,
  RawCast,
  DanglingEscape,
  SecondOwner,
  NoZeroInit,
  /// RFC 0034 §6.1: a definite finding the replay did not confirm.
  Unconfirmed,
  /// RFC 0035 §8: the facts neither prove nor refute the facet (what RFC
  /// 0030 called `checked`: a runtime check of a bound the code states
  /// would decide it).
  Undecided,
};

/// §2.4: the named trust assumption a trusted facet rests on. Closed.
enum class TrustReason : std::uint8_t {
  Unsafe,
  SystemApi,
  LibrarySpec,
  ExternContract,
  CallerContract,
  ExternalUnit,
  Concurrency,
};

// §3 `Certainty` (definite or possible) is defined in `Diagnostic.h`, since
// `core::Diagnostic` carries it too.

/// §2.1: a Call site is a call boundary or a function exit.
enum class Boundary : std::uint8_t { Call, Exit };

// Spellings, for dumps and tests.
[[nodiscard]] std::string_view toString(SiteKind kind) noexcept;
[[nodiscard]] std::string_view toString(Facet facet) noexcept;
[[nodiscard]] std::string_view toString(SiteOutcome outcome) noexcept;
[[nodiscard]] std::string_view toString(UnresolvedReason reason) noexcept;
[[nodiscard]] std::string_view toString(TrustReason reason) noexcept;

//===----------------------------------------------------------------------===//
// Merging by rank (§2.5)
//===----------------------------------------------------------------------===//

/// violation > unresolved > trusted > proven.
[[nodiscard]] constexpr int outcomeRank(SiteOutcome outcome) noexcept {
  switch (outcome) {
  case SiteOutcome::Proven:
    return 0;
  case SiteOutcome::Trusted:
    return 1;
  case SiteOutcome::Unresolved:
    return 2;
  case SiteOutcome::Violation:
    return 3;
  }
  return 0;
}

/// The higher-ranked of two outcomes.
[[nodiscard]] constexpr SiteOutcome maxByRank(SiteOutcome a,
                                              SiteOutcome b) noexcept {
  return outcomeRank(b) > outcomeRank(a) ? b : a;
}

/// One record about one facet: an outcome and, for `unresolved` and
/// `trusted`, its reason, with a free-text detail.
struct FacetDecision {
  SiteOutcome outcome = SiteOutcome::Proven;
  /// Set exactly when `outcome` is `Unresolved`.
  std::optional<UnresolvedReason> unresolved = std::nullopt;
  /// Set exactly when `outcome` is `Trusted`.
  std::optional<TrustReason> trusted = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};

  [[nodiscard]] static FacetDecision proven();
  [[nodiscard]] static FacetDecision violation(std::string detail = {});
  [[nodiscard]] static FacetDecision unresolvedFor(UnresolvedReason reason,
                                                   std::string detail = {});
  [[nodiscard]] static FacetDecision trustedFor(TrustReason reason,
                                                std::string detail = {});

  /// A reason is present exactly for the outcomes that carry one.
  [[nodiscard]] bool isWellFormed() const noexcept;
  /// The reason's spelling, or empty for outcomes without one.
  [[nodiscard]] std::string_view reasonText() const noexcept;

  /// Whether `other` wins a merge against `this` (§2.5): a strictly higher
  /// rank. Records of equal rank keep the earlier one, so the merge is
  /// deterministic for a deterministic engine.
  [[nodiscard]] bool losesTo(const FacetDecision &other) const noexcept;

  friend bool operator==(const FacetDecision &,
                         const FacetDecision &) = default;
};

/// One facet of one site: the merged decision and what else is known about
/// it.
struct FacetRecord {
  /// False until the first record. `LedgerAdapter::finish` fills every
  /// undecided facet with its default (§2.6, `defaultDecision`).
  bool decided = false;
  /// The merged outcome, reason and detail.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  FacetDecision decision = {};
  /// A suggested annotation.
  std::optional<FixItHint> fixit = std::nullopt;
  /// Index of the linked diagnostic in `Ledger::diagnostics`.
  std::optional<std::uint32_t> diagnostic = std::nullopt;

  [[nodiscard]] SiteOutcome outcome() const noexcept {
    return decision.outcome;
  }
  /// Merges one record by rank (§2.5).
  void decide(const FacetDecision &record);
  /// Forgets every decision (a new authoritative pass, §2.6), keeping only
  /// the fact that the facet applies.
  void reset();

  friend bool operator==(const FacetRecord &, const FacetRecord &) = default;
};

/// §2.6: the outcome of a facet no record decided: `unresolved(budget)` in
/// a function over its budget, `unresolved(unanalysed)` otherwise.
[[nodiscard]] FacetDecision defaultDecision(bool overBudget);

//===----------------------------------------------------------------------===//
// Sites, functions, units
//===----------------------------------------------------------------------===//

/// A site within one unit: the index of its function in
/// `UnitLedger::functions` and its ordinal there.
struct SiteId {
  std::uint32_t function = 0;
  std::uint32_t ordinal = 0;

  friend auto operator<=>(const SiteId &, const SiteId &) = default;
};

/// A site's `text` is at most this many bytes.
inline constexpr std::size_t MaxSiteTextBytes = 80;

/// A site's `text`: `source` with every ASCII whitespace character removed,
/// truncated to `MaxSiteTextBytes` without splitting a UTF-8 sequence.
[[nodiscard]] std::string siteText(std::string_view source);

/// §2.1: one operation in one emitted function.
struct Site {
  /// 0-based position in source order within the function.
  std::uint32_t ordinal = 0;
  SiteKind kind = SiteKind::Deref;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SourceLocation location = {};
  /// Normalised by `siteText`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string text = {};
  /// Call sites only.
  std::optional<Boundary> boundary = std::nullopt;
  /// The callee's name for call-like sites.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string callee = {};
  /// Indexed by `Facet`; empty for facets that do not apply (§2.1).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::array<std::optional<FacetRecord>, FacetCount> facets = {};

  /// Makes `facet` apply to the site (idempotent) and returns its record.
  FacetRecord &addFacet(Facet facet);
  [[nodiscard]] FacetRecord *facet(Facet facet);
  [[nodiscard]] const FacetRecord *facet(Facet facet) const;
  [[nodiscard]] bool hasFacet(Facet facet) const {
    return facets.at(static_cast<std::size_t>(facet)).has_value();
  }
  /// The site outcome of the summary line: the highest-ranked outcome among
  /// its facets. A site without facets is vacuously proven.
  [[nodiscard]] SiteOutcome outcome() const noexcept;

  friend bool operator==(const Site &, const Site &) = default;
};

/// One emitted function and its sites in source order.
struct FunctionLedger {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string name = {};
  /// The file that holds the definition, which differs from the unit's
  /// source for a function defined in a header. Empty means the unit's
  /// source.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string file = {};
  std::uint32_t line = 0;
  bool overBudget = false;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Site> sites = {};

  /// The site with `ordinal`, or null.
  [[nodiscard]] Site *site(std::uint32_t ordinal) noexcept;
  [[nodiscard]] const Site *site(std::uint32_t ordinal) const noexcept;
  /// `LedgerAdapter::beginFunction`: forgets every decision an earlier pass
  /// recorded (§2.5), keeping the sites and the facets that apply.
  void resetDecisions();

  friend bool operator==(const FunctionLedger &,
                         const FunctionLedger &) = default;
};

/// One translation unit.
struct UnitLedger {
  /// The main source as the user named it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<FunctionLedger> functions = {};

  [[nodiscard]] Site *site(SiteId id) noexcept;
  [[nodiscard]] const Site *site(SiteId id) const noexcept;

  friend bool operator==(const UnitLedger &, const UnitLedger &) = default;
};

//===----------------------------------------------------------------------===//
// Diagnostics, the ledger
//===----------------------------------------------------------------------===//

/// A note of a ledger diagnostic.
struct LedgerNote {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string message = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SourceLocation location = {};

  friend bool operator==(const LedgerNote &, const LedgerNote &) = default;
};

/// A diagnostic the analysis reported, with the site it is about.
struct LedgerDiagnostic {
  /// One of the ids in `weavec::core::diag`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string id = {};
  Severity severity = Severity::Error;
  Certainty certainty = Certainty::Definite;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string message = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SourceLocation location = {};
  /// The enclosing function; empty at file scope.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
  /// The linked site's ordinal within `function`, and its facet.
  std::optional<std::uint32_t> site = std::nullopt;
  std::optional<Facet> facet = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LedgerNote> notes = {};

  friend bool operator==(const LedgerDiagnostic &,
                         const LedgerDiagnostic &) = default;
};

/// The default of `-fweavec-budget` (§5.5, RFC 0034 §7.1): the work of one
/// function's run, the sizes of the states it transfers and joins summed.
inline constexpr std::uint64_t DefaultBudget = 20000000;

/// One unit's ledger (a run of the analysis), or a program's (`weavec
/// --whole-program`: every unit's).
struct Ledger {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<UnitLedger> units = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LedgerDiagnostic> diagnostics = {};

  friend bool operator==(const Ledger &, const Ledger &) = default;
};

/// Why a ledger is not complete (§2.6): an undecided facet, a decision
/// whose reason does not fit its outcome, a dangling diagnostic link, or
/// ordinals that are not 0, 1, 2, ... Empty when complete.
[[nodiscard]] std::vector<std::string>
completenessProblems(const Ledger &ledger);

/// Puts the diagnostics in source order (file, line and column, then id and
/// message; stable otherwise) and updates the facets' links to them.
void sortDiagnostics(Ledger &ledger);

//===----------------------------------------------------------------------===//
// Rollup and the summary line (RFC 0035 §8)
//===----------------------------------------------------------------------===//

/// Outcome counts.
struct OutcomeCounts {
  std::uint64_t proven = 0;
  std::uint64_t violation = 0;
  std::uint64_t unresolved = 0;
  std::uint64_t trusted = 0;

  void add(SiteOutcome outcome, std::uint64_t count = 1) noexcept;
  [[nodiscard]] std::uint64_t total() const noexcept {
    return proven + violation + unresolved + trusted;
  }
  friend bool operator==(const OutcomeCounts &,
                         const OutcomeCounts &) = default;
};

/// The rollup of a ledger.
struct LedgerSummary {
  std::uint64_t sites = 0;
  /// Sites by site outcome (the highest-ranked facet).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  OutcomeCounts outcomes = {};
  std::uint64_t errors = 0;
  std::uint64_t warnings = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> overBudget = {};

  /// Adds every site of `unit` and its functions over budget.
  void addUnit(const UnitLedger &unit);
  /// Counts `diagnostic` under `errors` or `warnings`.
  void addDiagnostic(const LedgerDiagnostic &diagnostic) noexcept;

  friend bool operator==(const LedgerSummary &,
                         const LedgerSummary &) = default;
};

/// The summary of the whole ledger: every unit and every diagnostic.
[[nodiscard]] LedgerSummary summarize(const Ledger &ledger);

/// `4210` -> `4,210`.
[[nodiscard]] std::string formatThousands(std::uint64_t value);

/// RFC 0035 §8, unit form: `weavec: cJSON.c: 4,210 sites: 3,050 proven,
/// 1,130 not proven, 0 violations, 30 trusted; 0 errors, 2 warnings`, with
/// `; 1 function over budget (<name>)` appended when a function is over
/// budget.
[[nodiscard]] std::string unitSummaryLine(std::string_view source,
                                          const LedgerSummary &summary);

/// Program form: `weavec: program minigzip: 9,876 sites in 3 units:
/// <outcomes>; 0 errors, 1 warning[; <n> functions over budget (...)]`.
[[nodiscard]] std::string programSummaryLine(std::string_view program,
                                             std::size_t units,
                                             const LedgerSummary &summary);

} // namespace weavec::core

#endif // WEAVEC_CORE_LEDGER_H
