//===- Ledger.cpp - Per-site safety outcomes (RFC 0030) -------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Ledger.h"

#include <algorithm>
#include <numeric>
#include <tuple>
#include <utility>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// Spellings
//===----------------------------------------------------------------------===//

static constexpr std::array<SiteKind, SiteKindCount> AllSiteKinds{
    SiteKind::Deref,    SiteKind::Index,   SiteKind::PtrArith, SiteKind::Cast,
    SiteKind::IntToPtr, SiteKind::LibCall, SiteKind::Release,  SiteKind::Call,
    SiteKind::Assume,   SiteKind::Raw};

static constexpr std::array<UnresolvedReason, UnresolvedReasonCount>
    AllUnresolvedReasons{UnresolvedReason::UnknownExtent,
                         UnresolvedReason::UnknownIndex,
                         UnresolvedReason::Inexpressible,
                         UnresolvedReason::MayReleased,
                         UnresolvedReason::MayMoved,
                         UnresolvedReason::MayAliasReleased,
                         UnresolvedReason::MayInvalidRelease,
                         UnresolvedReason::MayMismatchedRelease,
                         UnresolvedReason::MayDangle,
                         UnresolvedReason::MayConflict,
                         UnresolvedReason::UnknownCallee,
                         UnresolvedReason::Callback,
                         UnresolvedReason::Setjmp,
                         UnresolvedReason::Budget,
                         UnresolvedReason::Unanalysed,
                         UnresolvedReason::RawCast,
                         UnresolvedReason::DanglingEscape,
                         UnresolvedReason::SecondOwner,
                         UnresolvedReason::NoZeroInit};

static constexpr std::array<TrustReason, TrustReasonCount> AllTrustReasons{
    TrustReason::Unsafe,         TrustReason::SystemApi,
    TrustReason::LibrarySpec,    TrustReason::ExternContract,
    TrustReason::CallerContract, TrustReason::ExternalUnit,
    TrustReason::Concurrency};

std::span<const UnresolvedReason> allUnresolvedReasons() noexcept {
  return AllUnresolvedReasons;
}

std::span<const TrustReason> allTrustReasons() noexcept {
  return AllTrustReasons;
}

std::string_view toString(SiteKind kind) noexcept {
  switch (kind) {
  case SiteKind::Deref:
    return "deref";
  case SiteKind::Index:
    return "index";
  case SiteKind::PtrArith:
    return "ptr-arith";
  case SiteKind::Cast:
    return "cast";
  case SiteKind::IntToPtr:
    return "int-to-ptr";
  case SiteKind::LibCall:
    return "lib-call";
  case SiteKind::Release:
    return "release";
  case SiteKind::Call:
    return "call";
  case SiteKind::Assume:
    return "assume";
  case SiteKind::Raw:
    return "raw";
  }
  return "<invalid>";
}

std::string_view toString(Facet facet) noexcept {
  switch (facet) {
  case Facet::Spatial:
    return "spatial";
  case Facet::Null:
    return "null";
  case Facet::Temporal:
    return "temporal";
  case Facet::Assertion:
    return "assertion";
  }
  return "<invalid>";
}

std::string_view toString(SiteOutcome outcome) noexcept {
  switch (outcome) {
  case SiteOutcome::Proven:
    return "proven";
  case SiteOutcome::Checked:
    return "checked";
  case SiteOutcome::Violation:
    return "violation";
  case SiteOutcome::Unresolved:
    return "unresolved";
  case SiteOutcome::Trusted:
    return "trusted";
  }
  return "<invalid>";
}

std::string_view toString(UnresolvedReason reason) noexcept {
  switch (reason) {
  case UnresolvedReason::UnknownExtent:
    return "unknown-extent";
  case UnresolvedReason::UnknownIndex:
    return "unknown-index";
  case UnresolvedReason::Inexpressible:
    return "inexpressible";
  case UnresolvedReason::MayReleased:
    return "may-released";
  case UnresolvedReason::MayMoved:
    return "may-moved";
  case UnresolvedReason::MayAliasReleased:
    return "may-alias-released";
  case UnresolvedReason::MayInvalidRelease:
    return "may-invalid-release";
  case UnresolvedReason::MayMismatchedRelease:
    return "may-mismatched-release";
  case UnresolvedReason::MayDangle:
    return "may-dangle";
  case UnresolvedReason::MayConflict:
    return "may-conflict";
  case UnresolvedReason::UnknownCallee:
    return "unknown-callee";
  case UnresolvedReason::Callback:
    return "callback";
  case UnresolvedReason::Setjmp:
    return "setjmp";
  case UnresolvedReason::Budget:
    return "budget";
  case UnresolvedReason::Unanalysed:
    return "unanalysed";
  case UnresolvedReason::RawCast:
    return "raw-cast";
  case UnresolvedReason::DanglingEscape:
    return "dangling-escape";
  case UnresolvedReason::SecondOwner:
    return "second-owner";
  case UnresolvedReason::NoZeroInit:
    return "no-zero-init";
  }
  return "<invalid>";
}

std::string_view toString(TrustReason reason) noexcept {
  switch (reason) {
  case TrustReason::Unsafe:
    return "unsafe";
  case TrustReason::SystemApi:
    return "system-api";
  case TrustReason::LibrarySpec:
    return "library-spec";
  case TrustReason::ExternContract:
    return "extern-contract";
  case TrustReason::CallerContract:
    return "caller-contract";
  case TrustReason::ExternalUnit:
    return "external-unit";
  case TrustReason::Concurrency:
    return "concurrency";
  }
  return "<invalid>";
}

std::string_view toString(Certainty certainty) noexcept {
  switch (certainty) {
  case Certainty::Definite:
    return "definite";
  case Certainty::Possible:
    return "possible";
  }
  return "<invalid>";
}

std::string_view toString(Verdict verdict) noexcept {
  switch (verdict) {
  case Verdict::Holds:
    return "holds";
  case Verdict::Violated:
    return "violated";
  case Verdict::Unknown:
    return "unknown";
  }
  return "<invalid>";
}

std::string_view toString(Boundary boundary) noexcept {
  switch (boundary) {
  case Boundary::Call:
    return "call";
  case Boundary::Exit:
    return "exit";
  }
  return "<invalid>";
}

std::string_view toString(Linkage linkage) noexcept {
  switch (linkage) {
  case Linkage::External:
    return "external";
  case Linkage::Internal:
    return "internal";
  }
  return "<invalid>";
}

std::string_view toString(CheckTemplate kind) noexcept {
  switch (kind) {
  case CheckTemplate::Nonnull:
    return "nonnull";
  case CheckTemplate::Index:
    return "index";
  case CheckTemplate::Span:
    return "span";
  case CheckTemplate::Len:
    return "len";
  case CheckTemplate::Disjoint:
    return "disjoint";
  case CheckTemplate::Assert:
    return "assert";
  case CheckTemplate::Violation:
    return "violation";
  }
  return "<invalid>";
}

std::string_view toString(ChecksMode mode) noexcept {
  switch (mode) {
  case ChecksMode::Trap:
    return "trap";
  case ChecksMode::Report:
    return "report";
  case ChecksMode::Verify:
    return "verify";
  case ChecksMode::None:
    return "none";
  }
  return "<invalid>";
}

std::string_view toString(RequireLevel level) noexcept {
  switch (level) {
  case RequireLevel::None:
    return "none";
  case RequireLevel::Checked:
    return "checked";
  case RequireLevel::Proven:
    return "proven";
  }
  return "<invalid>";
}

std::string_view toString(LedgerScope scope) noexcept {
  switch (scope) {
  case LedgerScope::Unit:
    return "unit";
  case LedgerScope::Program:
    return "program";
  }
  return "<invalid>";
}

/// The enumerator of `values` whose spelling is `text`.
template <typename Enum, std::size_t N>
static std::optional<Enum> parseSpelling(const std::array<Enum, N> &values,
                                         std::string_view text) {
  for (const Enum value : values) {
    if (toString(value) == text)
      return value;
  }
  return std::nullopt;
}

std::optional<SiteKind> parseSiteKind(std::string_view text) {
  return parseSpelling(AllSiteKinds, text);
}

std::optional<Facet> parseFacet(std::string_view text) {
  return parseSpelling(AllFacets, text);
}

std::optional<SiteOutcome> parseSiteOutcome(std::string_view text) {
  return parseSpelling(AllSiteOutcomes, text);
}

std::optional<UnresolvedReason> parseUnresolvedReason(std::string_view text) {
  return parseSpelling(AllUnresolvedReasons, text);
}

std::optional<TrustReason> parseTrustReason(std::string_view text) {
  return parseSpelling(AllTrustReasons, text);
}

std::optional<Certainty> parseCertainty(std::string_view text) {
  return parseSpelling(
      std::array<Certainty, 2>{Certainty::Definite, Certainty::Possible}, text);
}

std::optional<Verdict> parseVerdict(std::string_view text) {
  return parseSpelling(std::array<Verdict, 3>{Verdict::Holds, Verdict::Violated,
                                              Verdict::Unknown},
                       text);
}

std::optional<Boundary> parseBoundary(std::string_view text) {
  return parseSpelling(std::array<Boundary, 2>{Boundary::Call, Boundary::Exit},
                       text);
}

std::optional<Linkage> parseLinkage(std::string_view text) {
  return parseSpelling(
      std::array<Linkage, 2>{Linkage::External, Linkage::Internal}, text);
}

std::optional<CheckTemplate> parseCheckTemplate(std::string_view text) {
  return parseSpelling(
      std::array<CheckTemplate, 7>{
          CheckTemplate::Nonnull, CheckTemplate::Index, CheckTemplate::Span,
          CheckTemplate::Len, CheckTemplate::Disjoint, CheckTemplate::Assert,
          CheckTemplate::Violation},
      text);
}

std::optional<ChecksMode> parseChecksMode(std::string_view text) {
  return parseSpelling(
      std::array<ChecksMode, 4>{ChecksMode::Trap, ChecksMode::Report,
                                ChecksMode::Verify, ChecksMode::None},
      text);
}

std::optional<RequireLevel> parseRequireLevel(std::string_view text) {
  return parseSpelling(std::array<RequireLevel, 3>{RequireLevel::None,
                                                   RequireLevel::Checked,
                                                   RequireLevel::Proven},
                       text);
}

std::optional<LedgerScope> parseLedgerScope(std::string_view text) {
  return parseSpelling(
      std::array<LedgerScope, 2>{LedgerScope::Unit, LedgerScope::Program},
      text);
}

//===----------------------------------------------------------------------===//
// Reason phrases and require-level messages
//===----------------------------------------------------------------------===//

std::string_view phraseTemplate(UnresolvedReason reason) noexcept {
  switch (reason) {
  case UnresolvedReason::UnknownExtent:
    return "the extent of '<p>' is unknown";
  case UnresolvedReason::UnknownIndex:
    return "the position of '<p>' in its object is unknown";
  case UnresolvedReason::Inexpressible:
    return "its bound has no name here";
  case UnresolvedReason::MayReleased:
    return "'<p>' may have been freed";
  case UnresolvedReason::MayMoved:
    return "'<p>' may have been moved";
  case UnresolvedReason::MayAliasReleased:
    return "'<p>' may point into an object freed earlier";
  case UnresolvedReason::MayInvalidRelease:
    return "'<p>' may not point to the start of a heap object";
  case UnresolvedReason::MayMismatchedRelease:
    return "'<p>' may belong to another allocator";
  case UnresolvedReason::MayDangle:
    return "'<p>' may outlive its storage";
  case UnresolvedReason::MayConflict:
    return "'<p>' may still be borrowed";
  case UnresolvedReason::UnknownCallee:
    return "'<f>' may have freed or kept '<p>'";
  case UnresolvedReason::Callback:
    return "the target of '<slot>' is unknown";
  case UnresolvedReason::Setjmp:
    return "'<fn>' calls setjmp";
  case UnresolvedReason::Budget:
    return "'<fn>' exceeded the analysis budget";
  case UnresolvedReason::Unanalysed:
    return "WeaveC does not model this (<detail>)";
  case UnresolvedReason::RawCast:
    return "'<p>' was made from a non-pointer value";
  case UnresolvedReason::DanglingEscape:
    return "'<place>' may hold a freed pointer here";
  case UnresolvedReason::SecondOwner:
    return "'<a>' and '<b>' may own the same object here";
  case UnresolvedReason::NoZeroInit:
    return "'<p>' may be uninitialised";
  }
  return "<invalid>";
}

/// `'<value>'`.
static std::string quoted(std::string_view value) {
  std::string text;
  text.reserve(value.size() + 2);
  text += '\'';
  text += value;
  text += '\'';
  return text;
}

std::string reasonPhrase(UnresolvedReason reason,
                         const PhraseArguments &arguments) {
  const std::string pointer = quoted(arguments.pointer);
  switch (reason) {
  case UnresolvedReason::UnknownExtent:
    return "the extent of " + pointer + " is unknown";
  case UnresolvedReason::UnknownIndex:
    return "the position of " + pointer + " in its object is unknown";
  case UnresolvedReason::Inexpressible:
    return "its bound has no name here";
  case UnresolvedReason::MayReleased:
    return pointer + " may have been freed";
  case UnresolvedReason::MayMoved:
    return pointer + " may have been moved";
  case UnresolvedReason::MayAliasReleased:
    return pointer + " may point into an object freed earlier";
  case UnresolvedReason::MayInvalidRelease:
    return pointer + " may not point to the start of a heap object";
  case UnresolvedReason::MayMismatchedRelease:
    return pointer + " may belong to another allocator";
  case UnresolvedReason::MayDangle:
    return pointer + " may outlive its storage";
  case UnresolvedReason::MayConflict:
    return pointer + " may still be borrowed";
  case UnresolvedReason::UnknownCallee:
    return quoted(arguments.callee) + " may have freed or kept " + pointer;
  case UnresolvedReason::Callback:
    return "the target of " + quoted(arguments.slot) + " is unknown";
  case UnresolvedReason::Setjmp:
    return quoted(arguments.function) + " calls setjmp";
  case UnresolvedReason::Budget:
    return quoted(arguments.function) + " exceeded the analysis budget";
  case UnresolvedReason::Unanalysed:
    return "WeaveC does not model this (" + std::string(arguments.detail) + ")";
  case UnresolvedReason::RawCast:
    return pointer + " was made from a non-pointer value";
  case UnresolvedReason::DanglingEscape:
    return quoted(arguments.place) + " may hold a freed pointer here";
  case UnresolvedReason::SecondOwner:
    return quoted(arguments.first) + " and " + quoted(arguments.second) +
           " may own the same object here";
  case UnresolvedReason::NoZeroInit:
    return pointer + " may be uninitialised";
  }
  return "<invalid>";
}

std::string operationText(OperationForm form, std::string_view subject,
                          std::string_view type) {
  switch (form) {
  case OperationForm::Access:
    return "access " + quoted(subject);
  case OperationForm::Dereference:
    return "dereference of " + quoted(subject);
  case OperationForm::CallTo:
    return "call to " + quoted(subject);
  case OperationForm::Release:
    return "release of " + quoted(subject);
  case OperationForm::Conversion:
    return "conversion of " + quoted(subject) + " to " + quoted(type);
  case OperationForm::BoundaryOf:
    return "boundary of " + quoted(subject);
  }
  return "<invalid>";
}

std::string unresolvedOperationMessage(std::string_view operation,
                                       UnresolvedReason reason,
                                       const PhraseArguments &arguments) {
  std::string text(operation);
  text += " is neither proven nor checkable: ";
  text += reasonPhrase(reason, arguments);
  text += " [";
  text += toString(reason);
  text += ']';
  return text;
}

std::string uncheckedOperationMessage(std::string_view operation,
                                      CheckTemplate check) {
  std::string text(operation);
  text += " relies on a runtime ";
  text += toString(check);
  text += " check";
  return text;
}

//===----------------------------------------------------------------------===//
// Decisions and merging
//===----------------------------------------------------------------------===//

FacetDecision FacetDecision::proven() {
  return FacetDecision{.outcome = SiteOutcome::Proven};
}

FacetDecision FacetDecision::checked() {
  return FacetDecision{.outcome = SiteOutcome::Checked};
}

FacetDecision FacetDecision::violation(std::string detail) {
  return FacetDecision{.outcome = SiteOutcome::Violation,
                       .detail = std::move(detail)};
}

FacetDecision FacetDecision::unresolvedFor(UnresolvedReason reason,
                                           std::string detail) {
  return FacetDecision{.outcome = SiteOutcome::Unresolved,
                       .unresolved = reason,
                       .detail = std::move(detail)};
}

FacetDecision FacetDecision::trustedFor(TrustReason reason,
                                        std::string detail) {
  return FacetDecision{.outcome = SiteOutcome::Trusted,
                       .trusted = reason,
                       .detail = std::move(detail)};
}

bool FacetDecision::isWellFormed() const noexcept {
  return (outcome == SiteOutcome::Unresolved) == unresolved.has_value() &&
         (outcome == SiteOutcome::Trusted) == trusted.has_value();
}

std::string_view FacetDecision::reasonText() const noexcept {
  if (unresolved)
    return toString(*unresolved);
  if (trusted)
    return toString(*trusted);
  return {};
}

std::string FacetDecision::compact() const {
  std::string text(toString(outcome));
  if (const std::string_view reason = reasonText(); !reason.empty()) {
    text += '/';
    text += reason;
  }
  return text;
}

bool FacetDecision::losesTo(const FacetDecision &other) const noexcept {
  return outcomeRank(other.outcome) > outcomeRank(outcome);
}

std::optional<FacetDecision> parseCompactFacet(std::string_view text) {
  const std::size_t slash = text.find('/');
  const std::optional<SiteOutcome> outcome =
      parseSiteOutcome(text.substr(0, slash));
  if (!outcome)
    return std::nullopt;
  const bool hasReason = slash != std::string_view::npos;
  const std::string_view reason =
      hasReason ? text.substr(slash + 1) : std::string_view{};
  switch (*outcome) {
  case SiteOutcome::Unresolved:
    if (const auto parsed = parseUnresolvedReason(reason))
      return FacetDecision::unresolvedFor(*parsed);
    return std::nullopt;
  case SiteOutcome::Trusted:
    if (const auto parsed = parseTrustReason(reason))
      return FacetDecision::trustedFor(*parsed);
    return std::nullopt;
  case SiteOutcome::Proven:
  case SiteOutcome::Checked:
  case SiteOutcome::Violation:
    if (hasReason)
      return std::nullopt;
    return FacetDecision{.outcome = *outcome};
  }
  return std::nullopt;
}

void FacetRecord::decide(const FacetDecision &record) {
  if (!decided) {
    decision = record;
    decided = true;
    return;
  }
  if (decision.losesTo(record)) {
    decision = record;
    return;
  }
  // Equal rank keeps the earlier record; a matching later record may still
  // supply the detail the earlier one lacked.
  if (decision.detail.empty() && record.outcome == decision.outcome &&
      record.unresolved == decision.unresolved &&
      record.trusted == decision.trusted)
    decision.detail = record.detail;
}

void FacetRecord::addRequirement(Requirement requirement) {
  decide(requirement.decision);
  requirements.push_back(std::move(requirement));
}

void FacetRecord::reset() {
  *this = FacetRecord{};
}

FacetDecision defaultDecision(Facet facet, bool overBudget,
                              bool spatialCheckable) {
  const UnresolvedReason reason =
      overBudget ? UnresolvedReason::Budget : UnresolvedReason::Unanalysed;
  switch (facet) {
  case Facet::Null:
  case Facet::Assertion:
    return FacetDecision::checked();
  case Facet::Spatial:
    return spatialCheckable ? FacetDecision::checked()
                            : FacetDecision::unresolvedFor(reason);
  case Facet::Temporal:
    return FacetDecision::unresolvedFor(reason);
  }
  return FacetDecision::unresolvedFor(reason);
}

//===----------------------------------------------------------------------===//
// Sites, functions, units
//===----------------------------------------------------------------------===//

static bool isAsciiSpace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

static bool isUtf8Continuation(char c) noexcept {
  return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

std::string siteText(std::string_view source) {
  std::string text;
  text.reserve(std::min(source.size(), MaxSiteTextBytes + 4));
  for (const char c : source) {
    if (!isAsciiSpace(c))
      text += c;
  }
  if (text.size() > MaxSiteTextBytes) {
    std::size_t cut = MaxSiteTextBytes;
    // Never split a UTF-8 sequence: back up to the start of the sequence
    // the first dropped byte continues.
    while (cut > 0 && isUtf8Continuation(text[cut]))
      --cut;
    text.resize(cut);
  }
  return text;
}

FacetRecord &Site::addFacet(Facet facet) {
  std::optional<FacetRecord> &slot = facets.at(static_cast<std::size_t>(facet));
  if (!slot)
    slot.emplace();
  return *slot;
}

FacetRecord *Site::facet(Facet facet) {
  std::optional<FacetRecord> &slot = facets.at(static_cast<std::size_t>(facet));
  return slot ? &*slot : nullptr;
}

const FacetRecord *Site::facet(Facet facet) const {
  const std::optional<FacetRecord> &slot =
      facets.at(static_cast<std::size_t>(facet));
  return slot ? &*slot : nullptr;
}

SiteOutcome Site::outcome() const noexcept {
  SiteOutcome result = SiteOutcome::Proven;
  for (const std::optional<FacetRecord> &record : facets) {
    if (record)
      result = maxByRank(result, record->outcome());
  }
  return result;
}

Site *FunctionLedger::site(std::uint32_t ordinal) noexcept {
  if (ordinal < sites.size() && sites[ordinal].ordinal == ordinal)
    return &sites[ordinal];
  const auto found = std::ranges::find(sites, ordinal, &Site::ordinal);
  return found == sites.end() ? nullptr : &*found;
}

const Site *FunctionLedger::site(std::uint32_t ordinal) const noexcept {
  if (ordinal < sites.size() && sites[ordinal].ordinal == ordinal)
    return &sites[ordinal];
  const auto found = std::ranges::find(sites, ordinal, &Site::ordinal);
  return found == sites.end() ? nullptr : &*found;
}

void FunctionLedger::resetDecisions() {
  for (Site &site : sites) {
    for (std::optional<FacetRecord> &record : site.facets) {
      if (record)
        record->reset();
    }
  }
}

Site *UnitLedger::site(SiteId id) noexcept {
  return id.function < functions.size()
             ? functions[id.function].site(id.ordinal)
             : nullptr;
}

const Site *UnitLedger::site(SiteId id) const noexcept {
  return id.function < functions.size()
             ? functions[id.function].site(id.ordinal)
             : nullptr;
}

bool Ledger::belongsTo(const LedgerDiagnostic &diagnostic,
                       std::size_t unitIndex) const noexcept {
  if (diagnostic.unit)
    return *diagnostic.unit == unitIndex;
  return units.size() == 1 && unitIndex == 0;
}

void deriveAssumptionCounts(Ledger &ledger) {
  Assumptions &assumptions =
      ledger.assumptions ? *ledger.assumptions : ledger.assumptions.emplace();
  assumptions.a4.concurrencySites = 0;
  A5Counts a5;
  for (const UnitLedger &unit : ledger.units) {
    a5 += unit.a5;
    for (const FunctionLedger &function : unit.functions) {
      for (const Site &site : function.sites) {
        const bool concurrent = std::ranges::any_of(
            site.facets, [](const std::optional<FacetRecord> &record) {
              return record && record->decision.trusted ==
                                   std::optional(TrustReason::Concurrency);
            });
        if (concurrent)
          ++assumptions.a4.concurrencySites;
      }
    }
  }
  assumptions.a5.nonLoweredAllocations = a5.nonLoweredAllocations;
  assumptions.a5.bypassedDeclarations = a5.bypassedDeclarations;
}

std::vector<std::string> completenessProblems(const Ledger &ledger) {
  std::vector<std::string> problems;
  for (std::size_t u = 0; u < ledger.units.size(); ++u) {
    const UnitLedger &unit = ledger.units[u];
    for (const FunctionLedger &function : unit.functions) {
      const std::string where = "unit " + std::to_string(u) + " ('" +
                                unit.source + "'), function '" + function.name +
                                "'";
      for (std::size_t s = 0; s < function.sites.size(); ++s) {
        const Site &site = function.sites[s];
        const std::string at = where + ", site " + std::to_string(s);
        if (site.ordinal != s)
          problems.push_back(at + ": ordinal " + std::to_string(site.ordinal));
        for (const Facet facet : AllFacets) {
          const FacetRecord *record = site.facet(facet);
          if (record == nullptr)
            continue;
          const std::string facetAt =
              at + ": " + std::string(toString(facet)) + " facet";
          if (!record->decided)
            problems.push_back(facetAt + " is undecided");
          else if (!record->decision.isWellFormed())
            problems.push_back(facetAt + " has a reason that does not fit '" +
                               std::string(toString(record->outcome())) + "'");
          for (const Requirement &requirement : record->requirements) {
            if (!requirement.decision.isWellFormed())
              problems.push_back(facetAt + " has a malformed requirement");
          }
          if (record->diagnostic &&
              *record->diagnostic >= ledger.diagnostics.size())
            problems.push_back(facetAt + " links to missing diagnostic " +
                               std::to_string(*record->diagnostic));
        }
      }
    }
  }
  return problems;
}

void sortDiagnostics(Ledger &ledger) {
  std::vector<std::uint32_t> order(ledger.diagnostics.size());
  std::iota(order.begin(), order.end(), 0U);
  const auto key = [&](std::uint32_t index) {
    const LedgerDiagnostic &diagnostic = ledger.diagnostics[index];
    return std::tie(diagnostic.location.file, diagnostic.location.line,
                    diagnostic.location.column, diagnostic.id,
                    diagnostic.message);
  };
  std::ranges::stable_sort(
      order, [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });
  std::vector<std::uint32_t> position(order.size());
  std::vector<LedgerDiagnostic> sorted;
  sorted.reserve(order.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    position[order[i]] = static_cast<std::uint32_t>(i);
    sorted.push_back(std::move(ledger.diagnostics[order[i]]));
  }
  ledger.diagnostics = std::move(sorted);
  for (UnitLedger &unit : ledger.units) {
    for (FunctionLedger &function : unit.functions) {
      for (Site &site : function.sites) {
        for (std::optional<FacetRecord> &record : site.facets) {
          if (record && record->diagnostic &&
              *record->diagnostic < position.size())
            record->diagnostic = position[*record->diagnostic];
        }
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Rollup
//===----------------------------------------------------------------------===//

void OutcomeCounts::add(SiteOutcome outcome, std::uint64_t count) noexcept {
  switch (outcome) {
  case SiteOutcome::Proven:
    proven += count;
    return;
  case SiteOutcome::Checked:
    checked += count;
    return;
  case SiteOutcome::Violation:
    violation += count;
    return;
  case SiteOutcome::Unresolved:
    unresolved += count;
    return;
  case SiteOutcome::Trusted:
    trusted += count;
    return;
  }
}

std::uint64_t OutcomeCounts::of(SiteOutcome outcome) const noexcept {
  switch (outcome) {
  case SiteOutcome::Proven:
    return proven;
  case SiteOutcome::Checked:
    return checked;
  case SiteOutcome::Violation:
    return violation;
  case SiteOutcome::Unresolved:
    return unresolved;
  case SiteOutcome::Trusted:
    return trusted;
  }
  return 0;
}

OutcomeCounts &OutcomeCounts::operator+=(const OutcomeCounts &other) noexcept {
  proven += other.proven;
  checked += other.checked;
  violation += other.violation;
  unresolved += other.unresolved;
  trusted += other.trusted;
  return *this;
}

double LedgerSummary::spatialNullShare() const noexcept {
  const OutcomeCounts &spatial =
      facets[static_cast<std::size_t>(Facet::Spatial)];
  const OutcomeCounts &null = facets[static_cast<std::size_t>(Facet::Null)];
  const std::uint64_t all = spatial.total() + null.total();
  if (all == 0)
    return 0.0;
  return static_cast<double>(spatial.unresolved + null.unresolved) /
         static_cast<double>(all);
}

void LedgerSummary::addUnit(const UnitLedger &unit) {
  functions += unit.functions.size();
  for (const FunctionLedger &function : unit.functions) {
    if (function.overBudget)
      overBudget.push_back(function.name);
    for (const Site &site : function.sites) {
      ++sites;
      outcomes.add(site.outcome());
      for (const Facet facet : AllFacets) {
        const FacetRecord *record = site.facet(facet);
        if (record == nullptr)
          continue;
        facets.at(static_cast<std::size_t>(facet)).add(record->outcome());
        if (record->outcome() == SiteOutcome::Unresolved &&
            record->decision.unresolved)
          ++unresolvedReasons.at(
              static_cast<std::size_t>(*record->decision.unresolved));
        if (record->outcome() == SiteOutcome::Trusted &&
            record->decision.trusted)
          ++trustedReasons.at(
              static_cast<std::size_t>(*record->decision.trusted));
        if (record->outcome() == SiteOutcome::Proven && record->check &&
            record->check->proven)
          ++verifyChecks;
      }
    }
  }
}

void LedgerSummary::addDiagnostic(const LedgerDiagnostic &diagnostic) noexcept {
  if (diagnostic.severity == Severity::Error)
    ++errors;
  else if (diagnostic.severity == Severity::Warning)
    ++warnings;
}

LedgerSummary summarize(const Ledger &ledger) {
  LedgerSummary summary;
  for (const UnitLedger &unit : ledger.units)
    summary.addUnit(unit);
  for (const LedgerDiagnostic &diagnostic : ledger.diagnostics)
    summary.addDiagnostic(diagnostic);
  return summary;
}

LedgerSummary summarizeUnit(const Ledger &ledger, std::size_t unitIndex) {
  LedgerSummary summary;
  if (unitIndex < ledger.units.size())
    summary.addUnit(ledger.units[unitIndex]);
  for (const LedgerDiagnostic &diagnostic : ledger.diagnostics) {
    if (ledger.belongsTo(diagnostic, unitIndex))
      summary.addDiagnostic(diagnostic);
  }
  return summary;
}

//===----------------------------------------------------------------------===//
// The summary line
//===----------------------------------------------------------------------===//

std::string formatThousands(std::uint64_t value) {
  const std::string digits = std::to_string(value);
  std::string text;
  text.reserve(digits.size() + (digits.size() / 3));
  for (std::size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (digits.size() - i) % 3 == 0)
      text += ',';
    text += digits[i];
  }
  return text;
}

/// `1 site`, `4,210 sites`.
static std::string counted(std::uint64_t count, std::string_view noun) {
  std::string text = formatThousands(count);
  text += ' ';
  text += noun;
  if (count != 1)
    text += 's';
  return text;
}

static std::string joined(std::span<const std::string> items) {
  std::string text;
  for (const std::string &item : items) {
    if (!text.empty())
      text += ", ";
    text += item;
  }
  return text;
}

/// `3,050 proven, 980 checked, 150 unresolved, 30 trusted`.
static std::string outcomesText(const OutcomeCounts &outcomes,
                                const SummaryLineOptions &options) {
  std::string text =
      formatThousands(outcomes.proven) + " proven, " +
      formatThousands(outcomes.checked) +
      (options.checksEnforced ? " checked" : " checkable (not enforced)");
  if (outcomes.violation != 0)
    text += ", " + counted(outcomes.violation, "violation");
  text += ", " + formatThousands(outcomes.unresolved) + " unresolved, " +
          formatThousands(outcomes.trusted) + " trusted";
  return text;
}

/// `; 0 errors, 2 warnings[; 1 function over budget (<name>)]`.
static std::string findingsText(const LedgerSummary &summary) {
  std::string text = "; " + counted(summary.errors, "error") + ", " +
                     counted(summary.warnings, "warning");
  if (!summary.overBudget.empty())
    text += "; " + counted(summary.overBudget.size(), "function") +
            " over budget (" + joined(summary.overBudget) + ")";
  return text;
}

std::string unitSummaryLine(std::string_view source,
                            const LedgerSummary &summary,
                            const SummaryLineOptions &options) {
  std::string text = "weavec: ";
  text += source;
  text += ": " + counted(summary.sites, "site") + ": " +
          outcomesText(summary.outcomes, options) + findingsText(summary);
  return text;
}

std::string programSummaryLine(const ProgramLineFacts &facts,
                               const LedgerSummary &summary,
                               const SummaryLineOptions &options) {
  std::string text = "weavec: program ";
  text += facts.program;
  text += ": " + counted(summary.sites, "site") + " in " +
          counted(facts.units, "unit") + ": " +
          outcomesText(summary.outcomes, options) + findingsText(summary);
  if (!facts.inputsWithoutRecords.empty())
    text += "; " + counted(facts.inputsWithoutRecords.size(), "input") +
            " without a WeaveC record (" + joined(facts.inputsWithoutRecords) +
            ")";
  text += "; unverified: " +
          counted(facts.unverifiedRequirements, "exported requirement") +
          " (A1), " + counted(facts.unverifiedInvariants, "header invariant") +
          " (A3)";
  return text;
}

std::string summaryLine(const Ledger &ledger, std::string_view name,
                        const SummaryLineOptions &options) {
  const LedgerSummary summary = summarize(ledger);
  if (ledger.scope == LedgerScope::Unit)
    return unitSummaryLine(name, summary, options);
  const Assumptions assumptions =
      ledger.assumptions ? *ledger.assumptions : Assumptions{};
  const ProgramLineFacts facts{
      .program = name,
      .units = ledger.units.size(),
      .inputsWithoutRecords = assumptions.a3.inputsWithoutRecords,
      .unverifiedRequirements = assumptions.unverifiedRequirements(),
      .unverifiedInvariants = assumptions.a3.unverified};
  return programSummaryLine(facts, summary, options);
}

std::string facetRowKey(SiteKind kind, Facet facet,
                        const FacetDecision &decision) {
  std::string key(toString(kind));
  key += '/';
  key += toString(facet);
  key += '/';
  key += decision.compact();
  return key;
}

} // namespace weavec::core
