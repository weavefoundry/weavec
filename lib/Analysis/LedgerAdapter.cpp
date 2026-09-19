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
  if (mode == Mode::Authoritative)
    unit.functions = sites.ledgers();
  unit.source = options.source;
  unit.target = options.target;
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
  witnesses.clear(analysed->index);
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
  if (isDiscarding())
    return;
  decideAt(sites.find(site, kind, boundary), site, facet, decision);
}

void LedgerAdapter::requirement(const clang::Stmt &site, core::Facet facet,
                                core::Requirement record,
                                std::optional<CheckWitness> witness) {
  if (isDiscarding())
    return;
  const std::optional<core::SiteId> id = sites.find(site);
  assert(id.has_value() &&
         "a requirement of a statement SiteCollector did not enumerate");
  if (!id) {
    noteOrphan(site, facet);
    return;
  }
  core::FacetRecord *facetRecord = this->record(*id, facet);
  if (facetRecord == nullptr)
    return;
  const auto index =
      static_cast<std::uint16_t>(facetRecord->requirements.size());
  facetRecord->addRequirement(std::move(record));
  if (witness) {
    witness->requirement = index;
    witnesses.add(*id, facet, std::move(*witness));
  }
}

void LedgerAdapter::witness(const clang::Stmt &site, core::Facet facet,
                            CheckWitness checkWitness) {
  if (isDiscarding())
    return;
  const std::optional<core::SiteId> id = sites.find(site);
  if (id && record(*id, facet) != nullptr)
    witnesses.add(*id, facet, std::move(checkWitness));
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

void LedgerAdapter::overBudget(const clang::FunctionDecl &function) {
  if (isDiscarding())
    return;
  if (const SiteIndex::FunctionSites *analysed = sites.function(function))
    overBudgetFunctions.insert(analysed->index);
}

void LedgerAdapter::storeVerdict(const clang::Stmt &store,
                                 FieldCandidate candidate,
                                 core::Verdict verdict) {
  if (isDiscarding())
    return;
  verdictList.push_back(PublishedVerdict{
      .store = &store, .candidate = std::move(candidate), .verdict = verdict});
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
    core::FacetRecord *linked = facet ? record(*id, *facet) : nullptr;
    if (linked != nullptr) {
      entry.site = id->ordinal;
      entry.facet = facet;
      if (!linked->diagnostic)
        linked->diagnostic = index;
    }
  } else {
    entry.function = functionNameAt(diagnostic.location);
  }
  ledgerDiagnostics.push_back(std::move(entry));
  emitted.push_back(std::move(diagnostic));
}

void LedgerAdapter::report(core::Diagnostic diagnostic,
                           core::Certainty certainty, const clang::Stmt *site,
                           std::optional<core::Facet> facet) {
  if (mode == Mode::Discarding)
    return;
  diagnostic.certainty = certainty;
  if (!emittedKeys
           .emplace(std::string(diagnostic.id), diagnostic.location.file,
                    diagnostic.location.line, diagnostic.location.column,
                    diagnostic.message)
           .second)
    return;
  if (mode == Mode::Collecting) {
    emitted.push_back(std::move(diagnostic));
    return;
  }
  std::optional<core::SiteId> id;
  if (site != nullptr) {
    id = sites.find(*site);
    if (!id)
      id = sites.findExit(*site);
  }
  publish(std::move(diagnostic), certainty, id, facet);
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
  core::FacetDecision decision =
      core::defaultDecision(facet, overBudget, site.spatialCheckable());
  if (decision.unresolved == core::UnresolvedReason::Unanalysed)
    decision.detail = "no decision was published";
  return decision;
}

void LedgerAdapter::fillDefaults(PlannerOptions & /*planner*/) {
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
        // A `checked` spatial facet the engine gave no witness (and a
        // default one) is checked against the extent the declarations give
        // (§2.6); without one the planner finds it inexpressible.
        if (facet == core::Facet::Spatial &&
            facetRecord->outcome() == core::SiteOutcome::Checked &&
            witnesses.of(info.id, facet).empty())
          for (const CheckWitness &witness : info.spatialDefaults)
            witnesses.add(info.id, facet, witness);
      }
    }
  }
}

/// Replaces a decision, and those of the requirement records, unless it is
/// a violation: definite violations stand in every region (§6.1).
static void overrideRecord(core::FacetRecord &facetRecord,
                           const core::FacetDecision &decision) {
  if (facetRecord.outcome() != core::SiteOutcome::Violation)
    facetRecord.decision = decision;
  for (core::Requirement &requirement : facetRecord.requirements)
    if (requirement.decision.outcome != core::SiteOutcome::Violation)
      requirement.decision = decision;
}

void LedgerAdapter::applyOverrides(PlannerOptions &planner) {
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
        // A proof from flow facts becomes a check (§5.4).
        facetRecord->decision = core::FacetDecision::checked();
        planner.setjmpDowngraded.insert({info.id, facet});
        if (facet == core::Facet::Spatial &&
            witnesses.of(info.id, facet).empty())
          for (const CheckWitness &witness : info.spatialDefaults)
            witnesses.add(info.id, facet, witness);
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

/// The source text of an expression, with whitespace removed (§12.1).
static std::string textOf(const clang::Expr *expr,
                          const clang::ASTContext &context) {
  if (expr == nullptr)
    return {};
  const clang::SourceManager &sm = context.getSourceManager();
  const llvm::StringRef text = clang::Lexer::getSourceText(
      sm.getExpansionRange(expr->IgnoreParenImpCasts()->getSourceRange()), sm,
      context.getLangOpts());
  return core::siteText(std::string_view(text.data(), text.size()));
}

/// The template a check of `facet` uses when none was planned.
static core::CheckTemplate naturalTemplate(core::Facet facet) {
  switch (facet) {
  case core::Facet::Null:
    return core::CheckTemplate::Nonnull;
  case core::Facet::Spatial:
    return core::CheckTemplate::Index;
  case core::Facet::Assertion:
  case core::Facet::Temporal:
    return core::CheckTemplate::Assert;
  }
  return core::CheckTemplate::Assert;
}

void LedgerAdapter::reportRequireLevel() {
  for (const SiteIndex::FunctionSites &function : sites.functions()) {
    const core::FunctionLedger &row = unit.functions[function.index];
    core::RequireLevel level = options.config.require;
    // `WEAVEC_REQUIRE_SAFE` holds the function to `checked` (§6.3).
    if (row.requireSafe && level == core::RequireLevel::None)
      level = core::RequireLevel::Checked;
    if (level == core::RequireLevel::None)
      continue;
    for (const SiteInfo &info : function.sites) {
      const core::Site *site = row.site(info.id.ordinal);
      if (site == nullptr)
        continue;
      const std::string pointer =
          info.operand != nullptr ? textOf(info.operand, context) : site->text;
      const std::string callee = !site->callee.empty() ? site->callee : pointer;
      std::string type;
      if (const auto *expr = llvm::dyn_cast<clang::Expr>(info.stmt))
        type = expr->getType().getAsString();
      std::string operation;
      switch (info.kind) {
      case core::SiteKind::Deref:
        operation =
            core::operationText(core::OperationForm::Dereference, pointer);
        break;
      case core::SiteKind::Raw:
        operation =
            core::operationText(info.library ? core::OperationForm::Release
                                             : core::OperationForm::Dereference,
                                pointer);
        break;
      case core::SiteKind::Release:
        operation = core::operationText(core::OperationForm::Release, pointer);
        break;
      case core::SiteKind::Cast:
      case core::SiteKind::IntToPtr:
        operation =
            core::operationText(core::OperationForm::Conversion, pointer, type);
        break;
      case core::SiteKind::LibCall:
        operation = core::operationText(core::OperationForm::CallTo, callee);
        break;
      case core::SiteKind::Call:
        operation =
            info.boundary == core::Boundary::Exit
                ? core::operationText(core::OperationForm::BoundaryOf, row.name)
                : core::operationText(core::OperationForm::CallTo, callee);
        break;
      case core::SiteKind::Index:
      case core::SiteKind::PtrArith:
      case core::SiteKind::Assume:
        operation =
            core::operationText(core::OperationForm::Access, site->text);
        break;
      }
      for (const core::Facet facet : core::AllFacets) {
        const core::FacetRecord *facetRecord = site->facet(facet);
        if (facetRecord == nullptr)
          continue;
        const core::SiteOutcome outcome = facetRecord->outcome();
        const bool unresolved = outcome == core::SiteOutcome::Unresolved;
        const bool unchecked = level == core::RequireLevel::Proven &&
                               outcome == core::SiteOutcome::Checked;
        if (!unresolved && !unchecked)
          continue;
        core::Diagnostic diagnostic{
            .severity = core::Severity::Error,
            .id = unresolved ? core::diag::UnresolvedOperation
                             : core::diag::UncheckedOperation,
            .message = {},
            .location = site->location,
            .notes = {},
            .fixits = {},
        };
        if (unresolved) {
          const core::PhraseArguments arguments{
              .pointer = pointer,
              .callee = callee,
              .slot = pointer,
              .function = row.name,
              .detail = facetRecord->decision.detail,
          };
          diagnostic.message = core::unresolvedOperationMessage(
              operation, *facetRecord->decision.unresolved, arguments);
        } else {
          diagnostic.message = core::uncheckedOperationMessage(
              operation, facetRecord->check ? facetRecord->check->kind
                                            : naturalTemplate(facet));
          if (facet == core::Facet::Null)
            diagnostic.addNote("nothing is known about the nullness of '" +
                                   pointer + "'",
                               site->location);
        }
        publish(std::move(diagnostic), core::Certainty::Definite, info.id,
                facet);
      }
    }
  }
}

PlannedLedger LedgerAdapter::finish() {
  PlannedLedger result;
  if (isDiscarding() || finished)
    return result;
  finished = true;
  PlannerOptions planner;
  planner.checks = options.config.checks;
  planner.lowered = options.lowered;
  for (const SiteIndex::FunctionSites &function : sites.functions()) {
    core::FunctionLedger &row = unit.functions[function.index];
    row.overBudget = overBudgetFunctions.contains(function.index);
    row.callsSetjmp = function.callsSetjmp;
  }
  fillDefaults(planner);
  applyOverrides(planner);
  const CheckPlanner checkPlanner(context, sites, std::move(planner));
  result.plan = checkPlanner.plan(unit, witnesses, result.handles);
  appendOrphanRows();
  reportRequireLevel();

  result.ledger.scope = core::LedgerScope::Unit;
  result.ledger.config = options.config;
  result.ledger.units.push_back(std::move(unit));
  result.ledger.diagnostics = ledgerDiagnostics;
  core::sortDiagnostics(result.ledger);
  assert(core::completenessProblems(result.ledger).empty() &&
         "RFC 0030 §2.6: the ledger must be complete by construction");
  return result;
}

} // namespace weavec::analysis
