//===- LedgerAdapter.cpp - The engine seam (RFC 0030) ---------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/LedgerAdapter.h"

#include "weavec/Analysis/ClangLocation.h"

#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

#include <cassert>
#include <utility>

namespace weavec::analysis {

LedgerAdapter::LedgerAdapter(clang::ASTContext &ctx, const SiteIndex &siteIndex,
                             LedgerAdapterOptions adapterOptions,
                             Mode adapterMode)
    : context(ctx), sites(siteIndex), options(std::move(adapterOptions)),
      mode(adapterMode) {
  if (mode == Mode::Authoritative || mode == Mode::Witness)
    unit.functions = sites.ledgers();
  unit.source = options.source;
}

/// The sites of a unit nothing is decided about.
static const SiteIndex &noSites() {
  static const SiteIndex None;
  return None;
}

LedgerAdapter::LedgerAdapter(clang::ASTContext &ctx, Mode adapterMode)
    : LedgerAdapter(ctx, noSites(), {}, adapterMode) {
  assert(adapterMode != Mode::Authoritative &&
         "an authoritative adapter needs the unit's sites");
}

core::FacetRecord *LedgerAdapter::record(core::SiteId id, core::Facet facet) {
  core::Site *site = unit.site(id);
  return site != nullptr ? site->facet(facet) : nullptr;
}

bool LedgerAdapter::applies(core::SiteId id, core::Facet facet) const {
  const core::Site *site = unit.site(id);
  return site != nullptr && site->hasFacet(facet);
}

void LedgerAdapter::beginFunction(const clang::FunctionDecl &function) {
  if (isDiscarding())
    return;
  current = &function;
  const SiteIndex::FunctionSites *analysed = sites.function(function);
  if (analysed == nullptr)
    return;
  unit.functions[analysed->index].resetDecisions();
  overBudgetFunctions.erase(analysed->index);
}

void LedgerAdapter::noteOrphan(const clang::Stmt &stmt, core::Facet facet) {
  // §2.6 item 2: an internal error. Release builds record an
  // `unresolved(unanalysed)` row in the enclosing function.
  const SiteIndex::FunctionSites *function =
      current != nullptr ? sites.function(*current) : nullptr;
  if (function == nullptr)
    return;
  orphans.push_back(
      Orphan{.stmt = &stmt, .function = function->index, .facet = facet});
}

void LedgerAdapter::decideAt(std::optional<core::SiteId> id,
                             const clang::Stmt &stmt, core::Facet facet,
                             const core::FacetDecision &decision) {
  assert(id.has_value() &&
         "a decision about a statement SiteCollector did not enumerate");
  assert(decision.isWellFormed() &&
         "a decision whose reason does not fit its outcome");
  if (!id) {
    noteOrphan(stmt, facet);
    return;
  }
  // A facet that does not apply to the site is vacuous (§2.1): the
  // decision says nothing the ledger records.
  if (core::FacetRecord *facetRecord = record(*id, facet))
    facetRecord->decide(decision);
}

void LedgerAdapter::decide(const clang::Stmt &site, core::Facet facet,
                           core::SiteOutcome outcome,
                           std::optional<core::UnresolvedReason> unresolved,
                           std::optional<core::TrustReason> trusted,
                           std::string detail) {
  if (observer)
    observer(site, facet,
             core::FacetDecision{.outcome = outcome,
                                 .unresolved = unresolved,
                                 .trusted = trusted,
                                 .detail = detail});
  if (isDiscarding())
    return;
  decideAt(sites.find(site), site, facet,
           core::FacetDecision{.outcome = outcome,
                               .unresolved = unresolved,
                               .trusted = trusted,
                               .detail = std::move(detail)});
}

void LedgerAdapter::decideAs(const clang::Stmt &site, core::SiteKind kind,
                             std::optional<core::Boundary> boundary,
                             core::Facet facet,
                             const core::FacetDecision &decision) {
  if (observer)
    observer(site, facet, decision);
  if (isDiscarding())
    return;
  decideAt(sites.find(site, kind, boundary), site, facet, decision);
}

void LedgerAdapter::suggest(const clang::Stmt &site, core::SiteKind kind,
                            std::optional<core::Boundary> boundary,
                            core::Facet facet, core::FixItHint fixit) {
  if (isDiscarding())
    return;
  const std::optional<core::SiteId> id = sites.find(site, kind, boundary);
  if (!id)
    return;
  core::FacetRecord *facetRecord = record(*id, facet);
  if (facetRecord != nullptr && !facetRecord->fixit)
    facetRecord->fixit = std::move(fixit);
}

void LedgerAdapter::requirement(const clang::Stmt &site, core::Facet facet,
                                const core::FacetDecision &decision) {
  if (isDiscarding())
    return;
  const std::optional<core::SiteId> id = sites.find(site);
  assert(id.has_value() &&
         "a requirement of a statement SiteCollector did not enumerate");
  if (!id) {
    noteOrphan(site, facet);
    return;
  }
  if (core::FacetRecord *facetRecord = this->record(*id, facet))
    facetRecord->decide(decision);
}

void LedgerAdapter::boundary(const clang::Stmt &site, BoundaryFacts facts) {
  if (isDiscarding())
    return;
  std::optional<core::SiteId> id = sites.findExit(site);
  if (!id)
    id = sites.find(site);
  if (id)
    boundaryList.push_back(
        PublishedBoundary{.site = *id, .facts = std::move(facts)});
}

void LedgerAdapter::reliesOn(core::SiteId site, std::string placeClass) {
  if (isDiscarding() || placeClass.empty())
    return;
  relied[site].insert(std::move(placeClass));
}

void LedgerAdapter::boundaryDecisions(std::vector<BoundaryDecision> decisions) {
  if (isDiscarding())
    return;
  boundaryRows = std::move(decisions);
}

void LedgerAdapter::applyBoundaries() {
  for (const BoundaryDecision &row : boundaryRows) {
    core::FacetRecord *facet = record(row.site, core::Facet::Temporal);
    if (facet == nullptr)
      continue;
    // §9.4 *Propagation*: only a proven facet rests on the assumption the
    // boundary broke. A trusted one rests on its own reason, so it is not
    // downgraded.
    if (row.propagated &&
        (!facet->decided || facet->outcome() != core::SiteOutcome::Proven))
      continue;
    facet->decide(core::FacetDecision::unresolvedFor(row.reason, row.detail));
  }
}

void LedgerAdapter::overBudget(const clang::FunctionDecl &function) {
  if (isDiscarding())
    return;
  if (const SiteIndex::FunctionSites *analysed = sites.function(function))
    overBudgetFunctions.insert(analysed->index);
}

static bool contains(const clang::SourceManager &sm, clang::SourceRange range,
                     clang::SourceLocation loc) {
  const clang::CharSourceRange expanded = sm.getExpansionRange(range);
  const clang::SourceLocation begin = expanded.getBegin();
  const clang::SourceLocation end = expanded.getEnd();
  if (begin.isInvalid() || end.isInvalid() || loc.isInvalid())
    return false;
  return !sm.isBeforeInTranslationUnit(loc, begin) &&
         !sm.isBeforeInTranslationUnit(end, loc);
}

std::string
LedgerAdapter::functionNameAt(const core::SourceLocation &loc) const {
  const clang::SourceManager &sm = context.getSourceManager();
  const clang::SourceLocation at = toClangLocation(loc);
  if (current != nullptr &&
      (at.isInvalid() ||
       contains(sm, current->getSourceRange(), sm.getExpansionLoc(at))))
    return current->getNameAsString();
  if (at.isInvalid())
    return {};
  if (const SiteIndex::FunctionSites *function = sites.functionAt(at, sm))
    return function->decl->getNameAsString();
  return {};
}

void LedgerAdapter::publish(core::Diagnostic diagnostic,
                            core::Certainty certainty,
                            std::optional<core::SiteId> id,
                            std::optional<core::Facet> facet) {
  core::LedgerDiagnostic entry;
  entry.id = std::string(diagnostic.id);
  entry.severity = diagnostic.severity;
  entry.certainty = certainty;
  entry.message = diagnostic.message;
  entry.location = diagnostic.location;
  for (const core::Diagnostic &note : diagnostic.notes)
    entry.notes.push_back(
        core::LedgerNote{.message = note.message, .location = note.location});
  const auto index = static_cast<std::uint32_t>(ledgerDiagnostics.size());
  if (id) {
    entry.function = unit.functions[id->function].name;
    if (facet && record(*id, *facet) != nullptr) {
      entry.site = id->ordinal;
      entry.facet = facet;
    }
  } else {
    entry.function = functionNameAt(diagnostic.location);
  }
  ledgerDiagnostics.push_back(std::move(entry));
  link(index, diagnostic, certainty, id, facet);
  emittedKeys[{std::string(diagnostic.id), diagnostic.location.file,
               diagnostic.location.line, diagnostic.location.column,
               diagnostic.message}] = index;
  emitted.push_back(std::move(diagnostic));
}

void LedgerAdapter::link(std::uint32_t index,
                         const core::Diagnostic &diagnostic,
                         core::Certainty certainty,
                         std::optional<core::SiteId> id,
                         std::optional<core::Facet> facet) {
  if (!id)
    return;
  // §3.4 and (V): a definite error is a violation of its facet, whatever
  // path reported it. The facet is made to apply when the site kind would
  // not otherwise carry it (a callee's requirement at a Call site).
  if (facet && certainty == core::Certainty::Definite &&
      diagnostic.severity == core::Severity::Error) {
    if (core::Site *site = unit.site(*id))
      site->addFacet(*facet).decide(
          core::FacetDecision::violation(diagnostic.message));
  }
  core::FacetRecord *linked = facet ? record(*id, *facet) : nullptr;
  if (linked != nullptr && !linked->diagnostic)
    linked->diagnostic = index;
}

/// The facet a definite error of `id` is a violation of (§3, §17.3's
/// matching facets), or none for ids that are about no facet.
static std::optional<core::Facet> facetOfDiagnostic(std::string_view id) {
  namespace diag = core::diag;
  if (id == diag::OutOfBounds || id == diag::InvalidRelease)
    return core::Facet::Spatial;
  if (id == diag::NullDereference || id == diag::UseOfUninitialized)
    return core::Facet::Null;
  if (id == diag::UseAfterFree || id == diag::DoubleFree ||
      id == diag::UseAfterMove || id == diag::ConflictingBorrow ||
      id == diag::LifetimeTooShort || id == diag::MismatchedRelease)
    return core::Facet::Temporal;
  if (id == diag::ContradictedAssumption)
    return core::Facet::Assertion;
  return std::nullopt;
}

void LedgerAdapter::report(core::Diagnostic diagnostic,
                           core::Certainty certainty, const clang::Stmt *site,
                           std::optional<core::Facet> facet) {
  if (mode == Mode::Discarding)
    return;
  diagnostic.certainty = certainty;
  auto [seen, fresh] = emittedKeys.try_emplace(
      {std::string(diagnostic.id), diagnostic.location.file,
       diagnostic.location.line, diagnostic.location.column,
       diagnostic.message},
      std::nullopt);
  // Reported again (a function analysed once more, §7.6): the rows the new
  // run published link to the diagnostic the first one made.
  const std::optional<std::uint32_t> known =
      fresh ? std::nullopt : seen->second;
  if (!fresh && !known)
    return;
  if (mode == Mode::Collecting || mode == Mode::Witness) {
    emitted.push_back(std::move(diagnostic));
    return;
  }
  std::optional<core::SiteId> id;
  if (site != nullptr) {
    id = sites.find(*site);
    if (!id)
      id = sites.findExit(*site);
  }
  // (V): a definite error is a violation of a site (§3.4). One reported
  // without its site, or at a subexpression that is no site of
  // its own (the argument of a call whose callee requires more than it has),
  // belongs to the innermost site around it, on the facet its id governs.
  if (!id && certainty == core::Certainty::Definite &&
      diagnostic.severity == core::Severity::Error) {
    if (!facet)
      facet = facetOfDiagnostic(diagnostic.id);
    const clang::SourceLocation at =
        site != nullptr ? site->getBeginLoc()
                        : clang::SourceLocation::getFromRawEncoding(
                              static_cast<clang::SourceLocation::UIntTy>(
                                  diagnostic.location.opaque));
    if (facet && at.isValid())
      id = sites.innermostAt(at, std::nullopt, context.getSourceManager());
  }
  if (known) {
    link(*known, diagnostic, certainty, id, facet);
    return;
  }
  publish(std::move(diagnostic), certainty, id, facet);
}

void LedgerAdapter::unconfirm(const clang::Stmt &site, core::Facet facet,
                              std::string_view id) {
  if (isDiscarding())
    return;
  std::optional<core::SiteId> found = sites.find(site);
  if (!found)
    found = sites.findExit(site);
  const auto unresolved =
      core::FacetDecision::unresolvedFor(core::UnresolvedReason::Unconfirmed);
  if (found)
    if (core::FacetRecord *facetRecord = record(*found, facet);
        facetRecord != nullptr &&
        facetRecord->outcome() == core::SiteOutcome::Violation)
      // (Assigned: a violation outranks every other decision.)
      facetRecord->decision = unresolved;
  const clang::SourceManager &sm = context.getSourceManager();
  const auto inSite = [&](const core::SourceLocation &location) {
    const clang::SourceLocation at = toClangLocation(location);
    return at.isValid() &&
           contains(sm, site.getSourceRange(), sm.getExpansionLoc(at));
  };
  for (core::Diagnostic &diagnostic : emitted) {
    if (diagnostic.id != id || diagnostic.severity != core::Severity::Error ||
        !inSite(diagnostic.location))
      continue;
    diagnostic.severity = core::Severity::Warning;
    diagnostic.certainty = core::Certainty::Possible;
    diagnostic.addNote("not confirmed on a feasible path", diagnostic.location);
    for (core::LedgerDiagnostic &entry : ledgerDiagnostics)
      if (entry.id == id && entry.location == diagnostic.location &&
          entry.message == diagnostic.message) {
        entry.severity = core::Severity::Warning;
        entry.certainty = core::Certainty::Possible;
        entry.notes.push_back(
            core::LedgerNote{.message = "not confirmed on a feasible path",
                             .location = diagnostic.location});
      }
  }
}

//===----------------------------------------------------------------------===//
// finish
//===----------------------------------------------------------------------===//

/// §2.6: the outcome of a facet no record decided.
static core::FacetDecision defaultFor(const SiteInfo &site, core::Facet facet,
                                      bool overBudget) {
  // §5.2: a facet that rests only on system-header attributes.
  if ((facet == core::Facet::Null && site.nullSystemApi) ||
      (facet == core::Facet::Spatial && site.spatialSystemApi))
    return core::FacetDecision::trustedFor(core::TrustReason::SystemApi);
  // §7.4: an IntToPtr site's own facets (inside an unsafe region they are
  // trusted, §6.1).
  if (site.kind == core::SiteKind::IntToPtr &&
      (facet == core::Facet::Spatial || facet == core::Facet::Temporal))
    return core::FacetDecision::unresolvedFor(core::UnresolvedReason::RawCast,
                                              "made from a non-pointer value");
  core::FacetDecision decision = core::defaultDecision(overBudget);
  if (decision.unresolved == core::UnresolvedReason::Unanalysed)
    decision.detail = "no decision was published";
  return decision;
}

void LedgerAdapter::fillDefaults() {
  for (const SiteIndex::FunctionSites &function : sites.functions()) {
    core::FunctionLedger &row = unit.functions[function.index];
    for (const SiteInfo &info : function.sites) {
      core::Site *site = row.site(info.id.ordinal);
      if (site == nullptr)
        continue;
      for (const core::Facet facet : core::AllFacets) {
        core::FacetRecord *facetRecord = site->facet(facet);
        if (facetRecord == nullptr)
          continue;
        if (!facetRecord->decided)
          facetRecord->decide(defaultFor(info, facet, row.overBudget));
      }
    }
  }
}

/// Replaces a decision unless it is a violation: definite violations stand
/// in every region (§6.1).
static void overrideRecord(core::FacetRecord &facetRecord,
                           const core::FacetDecision &decision) {
  if (facetRecord.outcome() != core::SiteOutcome::Violation)
    facetRecord.decision = decision;
}

void LedgerAdapter::applyOverrides() {
  for (const SiteIndex::FunctionSites &function : sites.functions()) {
    core::FunctionLedger &row = unit.functions[function.index];
    for (const SiteInfo &info : function.sites) {
      core::Site *site = row.site(info.id.ordinal);
      if (site == nullptr)
        continue;
      // §6.1: spatial and null facets inside an unsafe region are trusted,
      // and every facet of a Raw or IntToPtr site. An Assume site keeps its
      // assertion.
      if (info.inUnsafe && info.kind != core::SiteKind::Assume) {
        const bool everyFacet = info.kind == core::SiteKind::Raw ||
                                info.kind == core::SiteKind::IntToPtr;
        for (const core::Facet facet : core::AllFacets) {
          core::FacetRecord *facetRecord = site->facet(facet);
          if (facetRecord == nullptr ||
              (facet == core::Facet::Temporal && !everyFacet) ||
              facet == core::Facet::Assertion)
            continue;
          overrideRecord(*facetRecord, core::FacetDecision::trustedFor(
                                           core::TrustReason::Unsafe));
        }
      }
      // §5.3: a pointer shared with a thread or signal handler. Its
      // temporal facet, and a null or spatial facet flow facts proved, are
      // trusted(concurrency); what the types prove stays proven.
      if (options.concurrent && options.concurrent(info)) {
        if (core::FacetRecord *temporal = site->facet(core::Facet::Temporal);
            temporal != nullptr &&
            temporal->outcome() == core::SiteOutcome::Proven)
          overrideRecord(*temporal, core::FacetDecision::trustedFor(
                                        core::TrustReason::Concurrency));
        for (const core::Facet facet :
             {core::Facet::Spatial, core::Facet::Null}) {
          core::FacetRecord *facetRecord = site->facet(facet);
          if (facetRecord == nullptr ||
              facetRecord->outcome() != core::SiteOutcome::Proven ||
              info.provenByType)
            continue;
          facetRecord->decision =
              core::FacetDecision::trustedFor(core::TrustReason::Concurrency);
        }
      }
      if (!function.callsSetjmp)
        continue;
      // §5.4: values may be stale after a `longjmp`.
      if (core::FacetRecord *temporal = site->facet(core::Facet::Temporal))
        overrideRecord(*temporal, core::FacetDecision::unresolvedFor(
                                      core::UnresolvedReason::Setjmp));
      for (const core::Facet facet :
           {core::Facet::Spatial, core::Facet::Null}) {
        core::FacetRecord *facetRecord = site->facet(facet);
        if (facetRecord == nullptr ||
            facetRecord->outcome() != core::SiteOutcome::Proven ||
            info.provenByType)
          continue;
        // A proof from flow facts no longer stands (§5.4).
        facetRecord->decision =
            core::FacetDecision::unresolvedFor(core::UnresolvedReason::Setjmp);
      }
    }
  }
}

/// The site kind of a statement no site was enumerated for.
static core::SiteKind orphanKind(const clang::Stmt &stmt) {
  if (llvm::isa<clang::ArraySubscriptExpr>(stmt))
    return core::SiteKind::Index;
  if (llvm::isa<clang::CallExpr, clang::ReturnStmt, clang::CompoundStmt>(stmt))
    return core::SiteKind::Call;
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(&stmt))
    return cast->getCastKind() == clang::CK_IntegralToPointer
               ? core::SiteKind::IntToPtr
               : core::SiteKind::Cast;
  return core::SiteKind::Deref;
}

void LedgerAdapter::appendOrphanRows() {
  const clang::SourceManager &sm = context.getSourceManager();
  // One row per statement, with every facet decided about it.
  llvm::DenseMap<const clang::Stmt *, std::uint32_t> rowOf;
  for (const Orphan &orphan : orphans) {
    core::FunctionLedger &row = unit.functions[orphan.function];
    const core::FacetDecision unanalysed = core::FacetDecision::unresolvedFor(
        core::UnresolvedReason::Unanalysed,
        "an operation the site collector did not enumerate");
    if (const auto found = rowOf.find(orphan.stmt); found != rowOf.end()) {
      core::FacetRecord &facetRecord =
          row.sites[found->second].addFacet(orphan.facet);
      if (!facetRecord.decided)
        facetRecord.decide(unanalysed);
      continue;
    }
    rowOf[orphan.stmt] = static_cast<std::uint32_t>(row.sites.size());
    core::Site site;
    site.ordinal = static_cast<std::uint32_t>(row.sites.size());
    site.kind = orphanKind(*orphan.stmt);
    site.location = toCoreLocation(sm, orphan.stmt->getBeginLoc());
    const llvm::StringRef text = clang::Lexer::getSourceText(
        sm.getExpansionRange(orphan.stmt->getSourceRange()), sm,
        context.getLangOpts());
    site.text = core::siteText(std::string_view(text.data(), text.size()));
    if (site.kind == core::SiteKind::Call)
      site.boundary = llvm::isa<clang::CallExpr>(orphan.stmt)
                          ? core::Boundary::Call
                          : core::Boundary::Exit;
    site.addFacet(orphan.facet).decide(unanalysed);
    row.sites.push_back(std::move(site));
  }
}

core::Ledger LedgerAdapter::finish() {
  core::Ledger result;
  if (isDiscarding() || finished)
    return result;
  finished = true;
  for (const SiteIndex::FunctionSites &function : sites.functions())
    unit.functions[function.index].overBudget =
        overBudgetFunctions.contains(function.index);
  fillDefaults();
  applyOverrides();
  applyBoundaries();
  appendOrphanRows();

  result.units.push_back(std::move(unit));
  result.diagnostics = ledgerDiagnostics;
  core::sortDiagnostics(result);
  assert(core::completenessProblems(result).empty() &&
         "RFC 0030 §2.6: the ledger must be complete by construction");
  return result;
}

} // namespace weavec::analysis
