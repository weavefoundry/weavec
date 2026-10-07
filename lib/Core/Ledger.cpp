//===- Ledger.cpp - Per-site analysis outcomes (RFC 0030, 0035) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Ledger.h"

#include <algorithm>
#include <numeric>
#include <span>
#include <tuple>
#include <utility>

namespace weavec::core {

//===----------------------------------------------------------------------===//
// Spellings
//===----------------------------------------------------------------------===//

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
  case UnresolvedReason::Unconfirmed:
    return "unconfirmed";
  case UnresolvedReason::Undecided:
    return "undecided";
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

//===----------------------------------------------------------------------===//
// Decisions and merging
//===----------------------------------------------------------------------===//

FacetDecision FacetDecision::proven() {
  return FacetDecision{.outcome = SiteOutcome::Proven};
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

bool FacetDecision::losesTo(const FacetDecision &other) const noexcept {
  return outcomeRank(other.outcome) > outcomeRank(outcome);
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

void FacetRecord::reset() {
  *this = FacetRecord{};
}

FacetDecision defaultDecision(bool overBudget) {
  return FacetDecision::unresolvedFor(
      overBudget ? UnresolvedReason::Budget : UnresolvedReason::Unanalysed);
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

void LedgerSummary::addUnit(const UnitLedger &unit) {
  for (const FunctionLedger &function : unit.functions) {
    if (function.overBudget)
      overBudget.push_back(function.name);
    for (const Site &site : function.sites) {
      ++sites;
      outcomes.add(site.outcome());
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

/// `3,050 proven, 1,130 not proven, 0 violations, 30 trusted`.
static std::string outcomesText(const OutcomeCounts &outcomes) {
  return formatThousands(outcomes.proven) + " proven, " +
         formatThousands(outcomes.unresolved) + " not proven, " +
         counted(outcomes.violation, "violation") + ", " +
         formatThousands(outcomes.trusted) + " trusted";
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
                            const LedgerSummary &summary) {
  std::string text = "weavec: ";
  text += source;
  text += ": " + counted(summary.sites, "site") + ": " +
          outcomesText(summary.outcomes) + findingsText(summary);
  return text;
}

std::string programSummaryLine(std::string_view program, std::size_t units,
                               const LedgerSummary &summary) {
  std::string text = "weavec: program ";
  text += program;
  text += ": " + counted(summary.sites, "site") + " in " +
          counted(units, "unit") + ": " + outcomesText(summary.outcomes) +
          findingsText(summary);
  return text;
}

} // namespace weavec::core
