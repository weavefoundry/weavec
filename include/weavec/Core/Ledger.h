//===- Ledger.h - Per-site safety outcomes (RFC 0030) ----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 records exactly one outcome for every safety facet (spatial, null,
// temporal) of every memory operation WeaveC compiles, and for the assertion
// facet of every `WEAVEC_ASSUME` (§2). This header is the frontend-neutral
// model of that record:
//
//   Ledger -> units -> functions -> sites -> facets -> requirements
//          -> diagnostics (a facet links to its diagnostic by index)
//
// with the closed reason lists (§2.3, §2.4) and their phrases (RFC 0030,
// *Diagnostics*), merging by rank (§2.5), the undecided defaults (§2.6), the
// rollup behind the JSON `summary` object (§12.1) and the summary line
// (§12.4). Every enumeration has the JSON spelling of §12.1 and a parser for
// it. The JSON and SARIF writers and the fingerprints (§12.2, §12.3) live in
// `weavec/Frontend/LedgerWriter.h`, because Core may not use LLVM.
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
#include <span>
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
inline constexpr std::size_t SiteKindCount = 10;

/// §2.1: the obligations of a site. `Assertion` belongs to Assume sites only.
enum class Facet : std::uint8_t { Spatial, Null, Temporal, Assertion };
inline constexpr std::size_t FacetCount = 4;
inline constexpr std::array<Facet, FacetCount> AllFacets{
    Facet::Spatial, Facet::Null, Facet::Temporal, Facet::Assertion};

/// §2.2. Named `SiteOutcome` because `core::Outcome` is the RFC 0006 result
/// class.
enum class SiteOutcome : std::uint8_t {
  Proven,
  Checked,
  Violation,
  Unresolved,
  Trusted,
};
inline constexpr std::size_t SiteOutcomeCount = 5;
inline constexpr std::array<SiteOutcome, SiteOutcomeCount> AllSiteOutcomes{
    SiteOutcome::Proven, SiteOutcome::Checked, SiteOutcome::Violation,
    SiteOutcome::Unresolved, SiteOutcome::Trusted};

/// §2.3: why a facet is neither proven nor checkable. Closed: adding a
/// reason requires an RFC.
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
};
inline constexpr std::size_t UnresolvedReasonCount = 19;

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
inline constexpr std::size_t TrustReasonCount = 7;

/// §3: every diagnostic is definite (an error) or possible.
enum class Certainty : std::uint8_t { Definite, Possible };

/// §7.6, §14: a unit's verdict on a field-invariant candidate.
enum class Verdict : std::uint8_t { Holds, Violated, Unknown };

/// §2.1: a Call site is a call boundary or a function exit.
enum class Boundary : std::uint8_t { Call, Exit };

/// §12.1 `linkage` of a function.
enum class Linkage : std::uint8_t { External, Internal };

/// §12.1 `check.template`: the six templates of §10.1 plus `violation`, the
/// unconditional trap of a lowered violation (§3.4, §10.2).
enum class CheckTemplate : std::uint8_t {
  Nonnull,
  Index,
  Span,
  Len,
  Disjoint,
  Assert,
  Violation,
};

/// §10.7 `-fweavec-checks=`.
enum class ChecksMode : std::uint8_t { Trap, Report, Verify, None };

/// §6.3 `-fweavec-require=`.
enum class RequireLevel : std::uint8_t { None, Checked, Proven };

/// §12.1 `scope`.
enum class LedgerScope : std::uint8_t { Unit, Program };

// JSON spellings (§12.1) and their parsers. Each parser accepts exactly the
// spelling its `toString` produces.
[[nodiscard]] std::string_view toString(SiteKind kind) noexcept;
[[nodiscard]] std::string_view toString(Facet facet) noexcept;
[[nodiscard]] std::string_view toString(SiteOutcome outcome) noexcept;
[[nodiscard]] std::string_view toString(UnresolvedReason reason) noexcept;
[[nodiscard]] std::string_view toString(TrustReason reason) noexcept;
[[nodiscard]] std::string_view toString(Certainty certainty) noexcept;
[[nodiscard]] std::string_view toString(Verdict verdict) noexcept;
[[nodiscard]] std::string_view toString(Boundary boundary) noexcept;
[[nodiscard]] std::string_view toString(Linkage linkage) noexcept;
[[nodiscard]] std::string_view toString(CheckTemplate kind) noexcept;
[[nodiscard]] std::string_view toString(ChecksMode mode) noexcept;
[[nodiscard]] std::string_view toString(RequireLevel level) noexcept;
[[nodiscard]] std::string_view toString(LedgerScope scope) noexcept;

[[nodiscard]] std::optional<SiteKind> parseSiteKind(std::string_view text);
[[nodiscard]] std::optional<Facet> parseFacet(std::string_view text);
[[nodiscard]] std::optional<SiteOutcome>
parseSiteOutcome(std::string_view text);
[[nodiscard]] std::optional<UnresolvedReason>
parseUnresolvedReason(std::string_view text);
[[nodiscard]] std::optional<TrustReason>
parseTrustReason(std::string_view text);
[[nodiscard]] std::optional<Certainty> parseCertainty(std::string_view text);
[[nodiscard]] std::optional<Verdict> parseVerdict(std::string_view text);
[[nodiscard]] std::optional<Boundary> parseBoundary(std::string_view text);
[[nodiscard]] std::optional<Linkage> parseLinkage(std::string_view text);
[[nodiscard]] std::optional<CheckTemplate>
parseCheckTemplate(std::string_view text);
[[nodiscard]] std::optional<ChecksMode> parseChecksMode(std::string_view text);
[[nodiscard]] std::optional<RequireLevel>
parseRequireLevel(std::string_view text);
[[nodiscard]] std::optional<LedgerScope>
parseLedgerScope(std::string_view text);

/// Every unresolved reason and trust reason, in the order of §2.3 and §2.4.
/// That order is also the key order of `unresolvedReasons` and
/// `trustedReasons` in the JSON summary.
[[nodiscard]] std::span<const UnresolvedReason> allUnresolvedReasons() noexcept;
[[nodiscard]] std::span<const TrustReason> allTrustReasons() noexcept;

/// The values that fill a reason phrase's placeholders (RFC 0030,
/// *Diagnostics*): the pointer, the callee, the slot, the function, the
/// detail, the place, and the two owners of `second-owner`.
struct PhraseArguments {
  std::string_view pointer;
  std::string_view callee;
  std::string_view slot;
  std::string_view function;
  std::string_view detail;
  std::string_view place;
  std::string_view first;
  std::string_view second;
};

/// The phrase of `reason` with its placeholders spelled literally, for
/// example `the extent of '<p>' is unknown`.
[[nodiscard]] std::string_view phraseTemplate(UnresolvedReason reason) noexcept;
/// The phrase of `reason` with its placeholders filled from `arguments`.
[[nodiscard]] std::string reasonPhrase(UnresolvedReason reason,
                                       const PhraseArguments &arguments);

/// `<operation>` of the require-level messages (RFC 0030, *Diagnostics*).
enum class OperationForm : std::uint8_t {
  /// `access '<text>'`
  Access,
  /// `dereference of '<p>'`
  Dereference,
  /// `call to '<f>'`
  CallTo,
  /// `release of '<p>'`
  Release,
  /// `conversion of '<p>' to '<T>'`
  Conversion,
  /// `boundary of '<fn>'`
  BoundaryOf,
};

/// Spells `<operation>`; `type` is used only by `Conversion`.
[[nodiscard]] std::string operationText(OperationForm form,
                                        std::string_view subject,
                                        std::string_view type = {});
/// `unresolved-operation`: `<operation> is neither proven nor checkable:
/// <reason phrase> [<reason>]`.
[[nodiscard]] std::string
unresolvedOperationMessage(std::string_view operation, UnresolvedReason reason,
                           const PhraseArguments &arguments);
/// `unchecked-operation`: `<operation> relies on a runtime <template> check`.
[[nodiscard]] std::string uncheckedOperationMessage(std::string_view operation,
                                                    CheckTemplate check);

//===----------------------------------------------------------------------===//
// Merging by rank (§2.5)
//===----------------------------------------------------------------------===//

/// violation > unresolved > checked > trusted > proven.
[[nodiscard]] constexpr int outcomeRank(SiteOutcome outcome) noexcept {
  switch (outcome) {
  case SiteOutcome::Proven:
    return 0;
  case SiteOutcome::Trusted:
    return 1;
  case SiteOutcome::Checked:
    return 2;
  case SiteOutcome::Unresolved:
    return 3;
  case SiteOutcome::Violation:
    return 4;
  }
  return 0;
}

/// The higher-ranked of two outcomes.
[[nodiscard]] constexpr SiteOutcome maxByRank(SiteOutcome a,
                                              SiteOutcome b) noexcept {
  return outcomeRank(b) > outcomeRank(a) ? b : a;
}

/// One record about one facet: an outcome and, for `unresolved` and
/// `trusted`, its reason, with a free-text detail (§12.1 `detail`).
struct FacetDecision {
  SiteOutcome outcome = SiteOutcome::Proven;
  /// Set exactly when `outcome` is `Unresolved`.
  std::optional<UnresolvedReason> unresolved = std::nullopt;
  /// Set exactly when `outcome` is `Trusted`.
  std::optional<TrustReason> trusted = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};

  [[nodiscard]] static FacetDecision proven();
  [[nodiscard]] static FacetDecision checked();
  [[nodiscard]] static FacetDecision violation(std::string detail = {});
  [[nodiscard]] static FacetDecision unresolvedFor(UnresolvedReason reason,
                                                   std::string detail = {});
  [[nodiscard]] static FacetDecision trustedFor(TrustReason reason,
                                                std::string detail = {});

  /// A reason is present exactly for the outcomes that carry one.
  [[nodiscard]] bool isWellFormed() const noexcept;
  /// The reason's spelling, or empty for outcomes without one.
  [[nodiscard]] std::string_view reasonText() const noexcept;
  /// `<outcome>[/<reason>]`: the facet cells of a unit record's `sites`
  /// rows (§13.1).
  [[nodiscard]] std::string compact() const;

  /// Whether `other` wins a merge against `this` (§2.5): a strictly higher
  /// rank. Records of equal rank keep the earlier one, so the merge is
  /// deterministic for a deterministic engine.
  [[nodiscard]] bool losesTo(const FacetDecision &other) const noexcept;

  friend bool operator==(const FacetDecision &,
                         const FacetDecision &) = default;
};

/// Parses `<outcome>[/<reason>]`; rejects a missing, spurious or unknown
/// reason.
[[nodiscard]] std::optional<FacetDecision>
parseCompactFacet(std::string_view text);

/// §12.1 `check`: the template a planned check uses. `proven` marks a
/// verify-mode check of a proven facet (§10.7).
struct FacetCheck {
  CheckTemplate kind = CheckTemplate::Nonnull;
  bool proven = false;

  friend bool operator==(const FacetCheck &, const FacetCheck &) = default;
};

/// §12.1 `requirements`: one record that merged into a LibCall, Release or
/// call-site facet. A requirement with its own `check` is enforced even when
/// the merged facet is unresolved (§2.5).
struct Requirement {
  /// `arg`: the argument the requirement is about; absent for the callee
  /// operand or the call as a whole.
  std::optional<std::uint32_t> argument = std::nullopt;
  /// `need` and `have`: the required and the available quantity, spelled as
  /// terms; absent when unknown.
  std::optional<std::string> need = std::nullopt;
  std::optional<std::string> have = std::nullopt;
  /// `outcome` and `reason`. The detail is not serialised.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  FacetDecision decision = {};
  std::optional<FacetCheck> check = std::nullopt;

  friend bool operator==(const Requirement &, const Requirement &) = default;
};

/// One facet of one site: the merged decision and everything §12.1 records
/// about it.
struct FacetRecord {
  /// False until the first record. `LedgerAdapter::finish` fills every
  /// undecided facet with its default (§2.6, `defaultDecision`).
  bool decided = false;
  /// The merged outcome, reason and detail.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  FacetDecision decision = {};
  /// A suggested annotation (`fixit`).
  std::optional<FixItHint> fixit = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Requirement> requirements = {};
  /// Set by `CheckPlanner` on checked facets, and in verify mode on proven
  /// facets that received a check.
  std::optional<FacetCheck> check = std::nullopt;
  /// Index of the linked diagnostic in `Ledger::diagnostics`.
  std::optional<std::uint32_t> diagnostic = std::nullopt;
  /// §12.3, filled by the writer.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string fingerprint = {};

  [[nodiscard]] SiteOutcome outcome() const noexcept {
    return decision.outcome;
  }
  /// Merges one record by rank (§2.5).
  void decide(const FacetDecision &record);
  /// Appends a requirement record and merges its outcome and reason (§2.5).
  void addRequirement(Requirement requirement);
  /// Forgets every decision (a new authoritative pass, §2.6), keeping only
  /// the fact that the facet applies.
  void reset();

  friend bool operator==(const FacetRecord &, const FacetRecord &) = default;
};

/// §2.6: the outcome of a facet no record decided. `spatialCheckable` says
/// whether the site's extent is exact from the type or declared, with terms
/// that are constants, `const` locals or parameters never assigned or
/// address-taken.
[[nodiscard]] FacetDecision defaultDecision(Facet facet, bool overBudget,
                                            bool spatialCheckable);

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

/// §12.1 `text` is at most this many bytes.
inline constexpr std::size_t MaxSiteTextBytes = 80;

/// §12.1 `text`: `source` with every ASCII whitespace character removed,
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
  /// The callee's name for call-like sites; empty serialises as null.
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

/// §12.1 `functions[]`: one emitted function and its sites in source order.
struct FunctionLedger {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string name = {};
  std::uint32_t line = 0;
  Linkage linkage = Linkage::External;
  bool overBudget = false;
  bool requireSafe = false;
  /// JSON `setjmp`.
  bool callsSetjmp = false;
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

/// §12.1 unit `summary.a5`: the calls to allocation rows that are not
/// lowered, and the pointer locals whose declaration a jump can bypass (§11).
struct A5Counts {
  std::uint64_t nonLoweredAllocations = 0;
  std::uint64_t bypassedDeclarations = 0;

  A5Counts &operator+=(const A5Counts &other) noexcept {
    nonLoweredAllocations += other.nonLoweredAllocations;
    bypassedDeclarations += other.bypassedDeclarations;
    return *this;
  }
  friend bool operator==(const A5Counts &, const A5Counts &) = default;
};

/// §12.1 `units[]`: one translation unit.
struct UnitLedger {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string object = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string target = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<FunctionLedger> functions = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  A5Counts a5 = {};

  [[nodiscard]] Site *site(SiteId id) noexcept;
  [[nodiscard]] const Site *site(SiteId id) const noexcept;

  friend bool operator==(const UnitLedger &, const UnitLedger &) = default;
};

//===----------------------------------------------------------------------===//
// Diagnostics, assumptions, the ledger
//===----------------------------------------------------------------------===//

/// §12.1 `diagnostics[].notes[]`.
struct LedgerNote {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string message = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SourceLocation location = {};

  friend bool operator==(const LedgerNote &, const LedgerNote &) = default;
};

/// §12.1 `diagnostics[]`.
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
  /// The enclosing function; empty at file scope (JSON null; the
  /// fingerprint spells it `<file-scope>`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
  /// The linked site's ordinal within `function`, and its facet.
  std::optional<std::uint32_t> site = std::nullopt;
  std::optional<Facet> facet = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LedgerNote> notes = {};
  /// §12.3, filled by the writer.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string fingerprint = {};
  /// The unit the diagnostic belongs to (index into `Ledger::units`), for
  /// the per-unit summaries. Not serialised. When unset, the diagnostic
  /// belongs to the only unit of a one-unit ledger, and otherwise to no
  /// unit (it still counts in the ledger's own summary).
  std::optional<std::uint32_t> unit = std::nullopt;

  friend bool operator==(const LedgerDiagnostic &,
                         const LedgerDiagnostic &) = default;
};

/// §12.1 `producer`.
struct Producer {
  std::string name = "weavec";
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string version = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string revision = {};

  friend bool operator==(const Producer &, const Producer &) = default;
};

/// The default of `-fweavec-budget` before S3 calibrates it (§5.5).
inline constexpr std::uint64_t DefaultBudget = 200000;

/// §12.1 `config`.
struct LedgerConfig {
  ChecksMode checks = ChecksMode::Trap;
  bool zeroInit = true;
  RequireLevel require = RequireLevel::None;
  /// Block transfers per function; 0 means unlimited.
  std::uint64_t budget = DefaultBudget;

  friend bool operator==(const LedgerConfig &, const LedgerConfig &) = default;
};

/// §12.1 `assumptions` (program scope only): what the link step could not
/// verify under A1 and A3, and the A4 and A5 counts (§Soundness).
struct Assumptions {
  struct Callers {
    std::uint64_t exportedRequirements = 0;
    std::uint64_t verified = 0;
    std::uint64_t reliesOnSingle = 0;
    std::uint64_t unverifiedCallers = 0;

    friend bool operator==(const Callers &, const Callers &) = default;
  };
  struct OtherCode {
    std::uint64_t headerInvariants = 0;
    std::uint64_t unverified = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): designated-init
    std::vector<std::string> inputsWithoutRecords = {};

    friend bool operator==(const OtherCode &, const OtherCode &) = default;
  };
  struct Concurrency {
    std::uint64_t concurrencySites = 0;

    friend bool operator==(const Concurrency &, const Concurrency &) = default;
  };
  struct Initialisation {
    std::uint64_t nonLoweredAllocations = 0;
    std::uint64_t bypassedDeclarations = 0;
    /// A unit that defines the allocator (§13.2); JSON null when none.
    std::optional<std::string> allocatorDefinedBy = std::nullopt;

    friend bool operator==(const Initialisation &,
                           const Initialisation &) = default;
  };

  /// A1 — callers.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  Callers a1 = {};
  /// A3 — other code maintains the heap invariants.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  OtherCode a3 = {};
  /// A4 — no concurrency outside trust.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  Concurrency a4 = {};
  /// A5 — initialisation.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  Initialisation a5 = {};

  /// A1 exported requirements that are not verified: the count the program
  /// summary line lists.
  [[nodiscard]] std::uint64_t unverifiedRequirements() const noexcept {
    return a1.exportedRequirements > a1.verified
               ? a1.exportedRequirements - a1.verified
               : 0;
  }

  friend bool operator==(const Assumptions &, const Assumptions &) = default;
};

/// §12.1: one unit ledger (`weavec-cc -c`, `weavec` per source) or the
/// program ledger (link, `weavec --whole-program`).
struct Ledger {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  Producer producer = {};
  LedgerScope scope = LedgerScope::Unit;
  /// The fingerprint root (§12.3), absolute.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string root = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  LedgerConfig config = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<UnitLedger> units = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LedgerDiagnostic> diagnostics = {};
  /// Program scope only.
  std::optional<Assumptions> assumptions = std::nullopt;

  /// Whether `diagnostic` belongs to unit `unitIndex` (see
  /// `LedgerDiagnostic::unit`).
  [[nodiscard]] bool belongsTo(const LedgerDiagnostic &diagnostic,
                               std::size_t unitIndex) const noexcept;

  friend bool operator==(const Ledger &, const Ledger &) = default;
};

/// Fills the assumption counts that follow from the rows: A4's
/// `concurrencySites` (sites with a facet trusted for concurrency) and the
/// A5 sums of the units' `a5` counts. Creates `assumptions` if absent.
void deriveAssumptionCounts(Ledger &ledger);

/// Why a ledger is not complete (§2.6): an undecided facet, a decision
/// whose reason does not fit its outcome, a dangling diagnostic link, or
/// ordinals that are not 0, 1, 2, ... Empty when complete.
[[nodiscard]] std::vector<std::string>
completenessProblems(const Ledger &ledger);

/// §12.1: puts the diagnostics in source order (file, line and column, then
/// id and message; stable otherwise) and updates the facets' links to them.
void sortDiagnostics(Ledger &ledger);

//===----------------------------------------------------------------------===//
// Rollup (§12.1 `summary`) and the summary line (§12.4)
//===----------------------------------------------------------------------===//

/// Outcome counts, in the key order of §12.1.
struct OutcomeCounts {
  std::uint64_t proven = 0;
  std::uint64_t checked = 0;
  std::uint64_t violation = 0;
  std::uint64_t unresolved = 0;
  std::uint64_t trusted = 0;

  void add(SiteOutcome outcome, std::uint64_t count = 1) noexcept;
  [[nodiscard]] std::uint64_t of(SiteOutcome outcome) const noexcept;
  [[nodiscard]] std::uint64_t total() const noexcept {
    return proven + checked + violation + unresolved + trusted;
  }
  OutcomeCounts &operator+=(const OutcomeCounts &other) noexcept;
  friend bool operator==(const OutcomeCounts &,
                         const OutcomeCounts &) = default;
};

/// §12.1 `summary`: the rollup of a unit or of the whole ledger.
struct LedgerSummary {
  std::uint64_t sites = 0;
  /// Sites by site outcome (the highest-ranked facet).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  OutcomeCounts outcomes = {};
  /// Facets by facet and outcome, indexed by `Facet`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::array<OutcomeCounts, FacetCount> facets = {};
  /// Unresolved and trusted facets by reason, indexed by the enumerators.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::array<std::uint64_t, UnresolvedReasonCount> unresolvedReasons = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::array<std::uint64_t, TrustReasonCount> trustedReasons = {};
  std::uint64_t errors = 0;
  std::uint64_t warnings = 0;
  std::uint64_t functions = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> overBudget = {};
  /// Proven facets that received a verify-mode check (§10.7).
  std::uint64_t verifyChecks = 0;

  /// §12.1 `unresolvedShare.spatialNull`: unresolved spatial and null facets
  /// over all spatial and null facets; 0 when there are none.
  [[nodiscard]] double spatialNullShare() const noexcept;

  /// Adds every function, site and facet of `unit`.
  void addUnit(const UnitLedger &unit);
  /// Counts `diagnostic` under `errors` or `warnings`.
  void addDiagnostic(const LedgerDiagnostic &diagnostic) noexcept;

  friend bool operator==(const LedgerSummary &,
                         const LedgerSummary &) = default;
};

/// The summary of the whole ledger: every unit and every diagnostic.
[[nodiscard]] LedgerSummary summarize(const Ledger &ledger);
/// The summary of one unit and the diagnostics that belong to it.
[[nodiscard]] LedgerSummary summarizeUnit(const Ledger &ledger,
                                          std::size_t unitIndex);

/// `4210` -> `4,210`.
[[nodiscard]] std::string formatThousands(std::uint64_t value);

struct SummaryLineOptions {
  /// False with `-fweavec-checks=none` and in `weavec`, where "checked"
  /// reads `checkable (not enforced)`.
  bool checksEnforced = true;
};

/// §12.4, unit form:
/// `weavec: cJSON.c: 4,210 sites: 3,050 proven, 980 checked, 150
/// unresolved, 30 trusted; 0 errors, 2 warnings`, with `; 1 function over
/// budget (<name>)` appended when a function is over budget. A violation
/// count, which the RFC's example (with none) does not show, is listed
/// after the checked count when it is not zero.
[[nodiscard]] std::string
unitSummaryLine(std::string_view source, const LedgerSummary &summary,
                const SummaryLineOptions &options = {});

/// The link-only facts of the program form of the summary line.
struct ProgramLineFacts {
  /// The link output's name (`minigzip`).
  std::string_view program;
  std::size_t units = 0;
  /// Link inputs without a (valid) WeaveC record.
  std::span<const std::string> inputsWithoutRecords;
  /// Exported requirements not verified (A1).
  std::uint64_t unverifiedRequirements = 0;
  /// Header invariants not verified (A3).
  std::uint64_t unverifiedInvariants = 0;
};

/// §12.4, link form: `weavec: program minigzip: 9,876 sites in 3 units:
/// <outcomes>; 0 errors, 1 warning[; <n> functions over budget (...)][; 1
/// input without a WeaveC record (libz.a)]; unverified: 12 exported
/// requirements (A1), 0 header invariants (A3)`.
[[nodiscard]] std::string
programSummaryLine(const ProgramLineFacts &facts, const LedgerSummary &summary,
                   const SummaryLineOptions &options = {});

/// The summary line of `ledger`: the unit form for a unit ledger, with
/// `name` the source as the user named it, or the program form for a
/// program ledger, with `name` the program and the facts taken from
/// `ledger.assumptions`.
[[nodiscard]] std::string summaryLine(const Ledger &ledger,
                                      std::string_view name,
                                      const SummaryLineOptions &options = {});

/// §12.3 `key` of a facet row: `<kind>/<facet>/<outcome>[/<reason>]`.
[[nodiscard]] std::string facetRowKey(SiteKind kind, Facet facet,
                                      const FacetDecision &decision);

} // namespace weavec::core

#endif // WEAVEC_CORE_LEDGER_H
