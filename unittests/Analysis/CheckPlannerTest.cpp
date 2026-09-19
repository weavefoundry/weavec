//===- CheckPlannerTest.cpp - Tests for CheckPlanner ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §10.1, §10.3, §10.4: expressibility rules, templates, forms and
// placements, from small ASTs and synthetic witnesses.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/CheckPlanner.h"

#include "SiteTestUtils.h"

#include "clang/AST/Expr.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace weavec::analysis {

using test::collectUnit;
using Entry = core::CheckPlanEntry;

/// The site of `function` whose text is `text`.
static const SiteInfo *siteNamed(const test::CollectedUnit &unit,
                                 llvm::StringRef function, llvm::StringRef text,
                                 core::SiteKind kind) {
  const SiteIndex::FunctionSites *sites =
      unit.sites.function(*unit.function(function));
  if (sites == nullptr)
    return nullptr;
  for (const SiteInfo &site : sites->sites)
    if (site.kind == kind &&
        unit.sites.ledgers()[sites->index].sites[site.id.ordinal].text == text)
      return &site;
  return nullptr;
}

static const clang::ValueDecl *paramNamed(const test::CollectedUnit &unit,
                                          llvm::StringRef function,
                                          llvm::StringRef name) {
  for (const clang::ParmVarDecl *param : unit.function(function)->parameters())
    if (param->getName() == name)
      return param;
  return nullptr;
}

static CheckWitness safeWitness() {
  return CheckWitness{.extentClass = core::ExtentClass::Declared,
                      .unmodified = true,
                      .accessesSafe = true};
}

/// Decides every applicable facet of every site of `row` as `decision`.
static void decideAll(core::FunctionLedger &row,
                      const core::FacetDecision &decision) {
  for (core::Site &site : row.sites)
    for (const core::Facet facet : core::AllFacets)
      if (core::FacetRecord *record = site.facet(facet))
        record->decide(decision);
}

static std::vector<std::string> spell(const core::CheckPlan &plan,
                                      const core::UnitLedger &unit) {
  std::vector<std::string> out;
  for (const Entry &entry : plan.entries) {
    std::string text = unit.site(entry.site)->text + " " +
                       std::string(core::toString(entry.facet)) + " " +
                       std::string(core::toString(entry.kind)) + "/" +
                       std::string(core::toString(entry.form)) + "/" +
                       std::string(core::toString(entry.placement));
    if (entry.placement == Entry::Placement::WrapArgument)
      text += "#" + std::to_string(entry.argument);
    for (const core::CheckTerm &term : entry.operands)
      text += " " + term.toString();
    if (entry.proven)
      text += " proven";
    out.push_back(text);
  }
  return out;
}

namespace {

TEST(CheckPlanner, ExpressibilityRules) {
  const auto unit = collectUnit(R"c(
struct buf { unsigned long cap; volatile unsigned long vol; };
unsigned long g;
int h(void);
int f(int *p, int n, struct buf *b, __int128 wide) {
  int local = n;
  int taken = n;
  const int k = 3;
  int *tp = &taken;
  const int *kp = &k;
  (void)tp;
  (void)kp;
  return p[n] + p[local] + p[taken] + p[k] + p[g] + p[b->cap] + p[b->vol] +
         p[h()] + p[wide] + p[n++];
}
)c");
  const CheckPlanner planner(unit.context(), unit.sites);
  PlaceHandleTable handles;
  const auto express = [&](llvm::StringRef text, const CheckWitness &witness) {
    const SiteInfo *site = siteNamed(unit, "f", text, core::SiteKind::Index);
    EXPECT_NE(site, nullptr) << text.str();
    if (site == nullptr)
      return CheckPlanner::Expression{};
    return planner.express(WitnessTerm::ofExpr(*site->index), *site, witness,
                           handles);
  };
  const CheckWitness safe = safeWitness();
  CheckWitness unsafeReads = safe;
  unsafeReads.accessesSafe = false;

  // Rule 1: parameters, locals whose address is not taken, const locals.
  const auto param = express("p[n]", unsafeReads);
  ASSERT_TRUE(param.term.has_value()) << param.failure;
  EXPECT_EQ(param.term->kind, core::CheckTerm::Kind::Place);
  EXPECT_EQ(handles.resolvePlace(param.term->handle),
            paramNamed(unit, "f", "n"));
  EXPECT_TRUE(express("p[local]", unsafeReads).term.has_value());
  EXPECT_EQ(express("p[taken]", unsafeReads).failure,
            "it names a local whose address is taken");
  // `k` folds to its constant.
  const auto folded = express("p[k]", unsafeReads);
  ASSERT_TRUE(folded.term.has_value()) << folded.failure;
  EXPECT_EQ(folded.term->kind, core::CheckTerm::Kind::Constant);
  EXPECT_EQ(express("p[g]", unsafeReads).failure, "it names a global");
  // Rule 5: a read through a pointer needs its access proven.
  EXPECT_EQ(express("p[b->cap]", unsafeReads).failure,
            "reading it is not proven safe here");
  const auto field = express("p[b->cap]", safe);
  ASSERT_TRUE(field.term.has_value()) << field.failure;
  EXPECT_EQ(field.term->toString(),
            "$" + std::to_string(field.term->handle) + "->cap");
  // Rule 3: side effects, volatile reads and calls.
  EXPECT_FALSE(express("p[b->vol]", safe).term.has_value());
  EXPECT_EQ(express("p[h()]", safe).failure, "it has side effects");
  EXPECT_EQ(express("p[n++]", safe).failure, "it has side effects");
  // Rule 6.
  EXPECT_EQ(express("p[wide]", safe).failure, "it does not fit in 64 bits");
  // Rule 4: the engine must show the places unmodified.
  CheckWitness stale = safe;
  stale.unmodified = false;
  EXPECT_EQ(express("p[n]", stale).failure,
            "its places may have changed since the extent was derived");
  // Rule 7: the operation writes a place the term names.
  const SiteInfo *writes =
      siteNamed(unit, "f", "p[n++]", core::SiteKind::Index);
  ASSERT_NE(writes, nullptr);
  EXPECT_EQ(planner
                .express(WitnessTerm::ofPlace(*paramNamed(unit, "f", "n")),
                         *writes, safe, handles)
                .failure,
            "the operation may write a place it names");
  // Rule 2: `strlen` is bounded by the check's have.
  const SiteInfo *plain = siteNamed(unit, "f", "p[n]", core::SiteKind::Index);
  const WitnessTerm length =
      WitnessTerm::strLen(WitnessTerm::ofPlace(*paramNamed(unit, "f", "p")));
  EXPECT_EQ(planner.express(length, *plain, safe, handles).failure,
            "the string length has no bound here");
  const auto bounded = planner.express(length, *plain, safe, handles,
                                       core::CheckTerm::ofConstant(16));
  ASSERT_TRUE(bounded.term.has_value()) << bounded.failure;
  EXPECT_EQ(bounded.term->kind, core::CheckTerm::Kind::StrNLen);
}

TEST(CheckPlanner, TemplatesFormsAndPlacements) {
  const auto unit = collectUnit(R"c(
int f(int *p, int i, int (*cb)(int *), char *d, const char *s,
      unsigned long n, int *q) {
  int arr[4] = {0};
  memcpy(d, s, n);
  ASSUME(i < 4);
  int shared = *q ?: 1;
  return *p + arr[i] + cb(p) + shared;
}
)c");
  core::UnitLedger ledger;
  ledger.functions = unit.sites.ledgers();
  const SiteIndex::FunctionSites *function =
      unit.sites.function(*unit.function("f"));
  ASSERT_NE(function, nullptr);
  core::FunctionLedger &row = ledger.functions[function->index];
  decideAll(row, core::FacetDecision::checked());
  WitnessTable witnesses;
  const SiteInfo *index = siteNamed(unit, "f", "arr[i]", core::SiteKind::Index);
  ASSERT_NE(index, nullptr);
  ASSERT_EQ(index->spatialDefaults.size(), 1U);
  witnesses.add(index->id, core::Facet::Spatial,
                index->spatialDefaults.front());

  PlaceHandleTable handles;
  const core::CheckPlan plan =
      CheckPlanner(unit.context(), unit.sites).plan(ledger, witnesses, handles);
  const std::vector<std::string> entries = spell(plan, ledger);
  const auto has = [&](const std::string &line) {
    return std::ranges::find(entries, line) != entries.end();
  };
  const std::string n =
      "$" + std::to_string(handles.place(*paramNamed(unit, "f", "n")));
  EXPECT_TRUE(
      has("memcpy(d,s,n) null nonnull/if-non-zero/wrap-argument#0 " + n));
  EXPECT_TRUE(
      has("memcpy(d,s,n) null nonnull/if-non-zero/wrap-argument#1 " + n));
  EXPECT_TRUE(has("ASSUME(i<4) assertion assert/plain/replace-call"));
  EXPECT_TRUE(has("*p null nonnull/plain/wrap-operand"));
  EXPECT_TRUE(has("arr[i] spatial index/plain/wrap-index 4"));
  EXPECT_TRUE(has("cb(p) null nonnull/function/wrap-operand"));
  for (const Entry &entry : plan.entries)
    EXPECT_TRUE(core::isWellFormed(entry));

  // Without a witness a checked spatial facet has nothing to compare; the
  // shared operand of `?:` cannot be wrapped (§2.1).
  const SiteInfo *deref = siteNamed(unit, "f", "*p", core::SiteKind::Deref);
  ASSERT_NE(deref, nullptr);
  const core::FacetRecord *spatial =
      row.sites[deref->id.ordinal].facet(core::Facet::Spatial);
  EXPECT_EQ(spatial->decision.compact(), "unresolved/inexpressible");
  EXPECT_EQ(spatial->decision.detail, "nothing says what the check compares");
  const SiteInfo *common = siteNamed(unit, "f", "*q", core::SiteKind::Deref);
  ASSERT_NE(common, nullptr);
  EXPECT_TRUE(common->sharedOperand);
  EXPECT_EQ(row.sites[common->id.ordinal]
                .facet(core::Facet::Null)
                ->decision.compact(),
            "unresolved/inexpressible");
  // A planned check is recorded on its facet.
  EXPECT_EQ(
      row.sites[index->id.ordinal].facet(core::Facet::Spatial)->check,
      (core::FacetCheck{.kind = core::CheckTemplate::Index, .proven = false}));
}

TEST(CheckPlanner, LowerBoundsSpansViolationsAndVerify) {
  const auto unit = collectUnit(R"c(
int f(int *p, int *q, int *r, int *t) {
  return *p + *q + *r + *t;
}
)c");
  core::UnitLedger ledger;
  ledger.functions = unit.sites.ledgers();
  core::FunctionLedger &row =
      ledger.functions[unit.sites.function(*unit.function("f"))->index];
  const SiteInfo *p = siteNamed(unit, "f", "*p", core::SiteKind::Deref);
  const SiteInfo *q = siteNamed(unit, "f", "*q", core::SiteKind::Deref);
  const SiteInfo *r = siteNamed(unit, "f", "*r", core::SiteKind::Deref);
  const SiteInfo *t = siteNamed(unit, "f", "*t", core::SiteKind::Deref);
  ASSERT_TRUE(p && q && r && t);
  const auto facet = [&](const SiteInfo *site, core::Facet which) {
    return row.sites[site->id.ordinal].facet(which);
  };
  WitnessTable witnesses;
  // *p: a lower bound never makes a check (§7.1, rule 8).
  facet(p, core::Facet::Spatial)->decide(core::FacetDecision::checked());
  witnesses.add(p->id, core::Facet::Spatial,
                CheckWitness{.shape = CheckWitness::Shape::Index,
                             .extent = WitnessTerm::ofConstant(1),
                             .extentClass = core::ExtentClass::LowerBound,
                             .unmodified = true,
                             .accessesSafe = true});
  // *q: a span check subsumes the nonnull check of the same operand.
  facet(q, core::Facet::Spatial)->decide(core::FacetDecision::checked());
  facet(q, core::Facet::Null)->decide(core::FacetDecision::checked());
  witnesses.add(
      q->id, core::Facet::Spatial,
      CheckWitness{.shape = CheckWitness::Shape::Span,
                   .extent = WitnessTerm::ofConstant(16),
                   .extentClass = core::ExtentClass::Declared,
                   .base = WitnessTerm::ofPlace(*paramNamed(unit, "f", "q")),
                   .width = WitnessTerm::sizeOf(unit.context().IntTy),
                   .unmodified = true,
                   .accessesSafe = true});
  // *r: lowered violations are guarded (§3.4).
  facet(r, core::Facet::Null)->decide(core::FacetDecision::violation());
  facet(r, core::Facet::Temporal)->decide(core::FacetDecision::violation());
  // *t: verify mode checks proven facets that have witnesses (§10.7).
  facet(t, core::Facet::Spatial)->decide(core::FacetDecision::proven());
  witnesses.add(t->id, core::Facet::Spatial,
                CheckWitness{.shape = CheckWitness::Shape::Index,
                             .extent = WitnessTerm::ofConstant(1),
                             .extentClass = core::ExtentClass::Exact,
                             .unmodified = true,
                             .accessesSafe = true});
  PlannerOptions options;
  options.checks = core::ChecksMode::Verify;
  options.lowered = [&](core::SiteId site, core::Facet) {
    return site == r->id;
  };
  PlaceHandleTable handles;
  const core::CheckPlan plan = CheckPlanner(unit.context(), unit.sites, options)
                                   .plan(ledger, witnesses, handles);
  const std::vector<std::string> entries = spell(plan, ledger);
  const std::string base =
      "$" + std::to_string(handles.place(*paramNamed(unit, "f", "q")));
  const std::string width =
      "sizeof(#" + std::to_string(handles.type(unit.context().IntTy)) + ")";
  EXPECT_EQ(entries,
            (std::vector<std::string>{
                "*q spatial span/plain/replace-access " + base + " 16 " + width,
                "*r null nonnull/plain/wrap-operand",
                "*r temporal assert/violation/wrap-operand",
                "*t spatial index/plain/wrap-operand 1 proven"}));
  EXPECT_EQ(facet(p, core::Facet::Spatial)->decision.detail,
            "its extent is only a lower bound");
  EXPECT_EQ(facet(q, core::Facet::Null)->check->kind,
            core::CheckTemplate::Span);
  EXPECT_EQ(facet(r, core::Facet::Temporal)->check->kind,
            core::CheckTemplate::Violation);
  EXPECT_TRUE(facet(t, core::Facet::Spatial)->check->proven);
  EXPECT_EQ(facet(t, core::Facet::Spatial)->outcome(),
            core::SiteOutcome::Proven);
}

TEST(CheckPlanner, SetjmpDowngradesBecomeSetjmpWhenInexpressible) {
  const auto unit = collectUnit("int f(int *p) { return *p; }\n");
  core::UnitLedger ledger;
  ledger.functions = unit.sites.ledgers();
  core::FunctionLedger &row =
      ledger.functions[unit.sites.function(*unit.function("f"))->index];
  const SiteInfo *p = siteNamed(unit, "f", "*p", core::SiteKind::Deref);
  ASSERT_NE(p, nullptr);
  core::FacetRecord *spatial =
      row.sites[p->id.ordinal].facet(core::Facet::Spatial);
  spatial->decide(core::FacetDecision::checked());
  PlannerOptions options;
  options.setjmpDowngraded.insert({p->id, core::Facet::Spatial});
  PlaceHandleTable handles;
  const WitnessTable witnesses;
  (void)CheckPlanner(unit.context(), unit.sites, options)
      .plan(ledger, witnesses, handles);
  EXPECT_EQ(spatial->decision.compact(), "unresolved/setjmp");
}

TEST(CheckPlanner, RequirementRecordsArePlannedOneByOne) {
  const auto unit = collectUnit(R"c(
void f(char *d, const char *s, unsigned long n) { memcpy(d, s, n); }
)c");
  core::UnitLedger ledger;
  ledger.functions = unit.sites.ledgers();
  core::FunctionLedger &row =
      ledger.functions[unit.sites.function(*unit.function("f"))->index];
  const SiteInfo *copy =
      siteNamed(unit, "f", "memcpy(d,s,n)", core::SiteKind::LibCall);
  ASSERT_NE(copy, nullptr);
  core::FacetRecord *spatial =
      row.sites[copy->id.ordinal].facet(core::Facet::Spatial);
  // The destination's requirement is checkable, the source's is not.
  spatial->addRequirement(core::Requirement{
      .argument = 0, .decision = core::FacetDecision::checked()});
  spatial->addRequirement(
      core::Requirement{.argument = 1,
                        .decision = core::FacetDecision::unresolvedFor(
                            core::UnresolvedReason::UnknownExtent)});
  WitnessTable witnesses;
  CheckWitness length{.shape = CheckWitness::Shape::Length,
                      .requirement = 0,
                      .argument = 0,
                      .extent = WitnessTerm::ofConstant(16),
                      .extentClass = core::ExtentClass::Exact,
                      .need = WitnessTerm::ofPlace(*paramNamed(unit, "f", "n")),
                      .unmodified = true,
                      .accessesSafe = true};
  witnesses.add(copy->id, core::Facet::Spatial, length);
  PlaceHandleTable handles;
  const core::CheckPlan plan =
      CheckPlanner(unit.context(), unit.sites).plan(ledger, witnesses, handles);
  ASSERT_EQ(plan.size(), 1U);
  EXPECT_EQ(plan.entries.front().kind, Entry::Template::Len);
  EXPECT_EQ(plan.entries.front().placement, Entry::Placement::BeforeCall);
  EXPECT_EQ(plan.entries.front().requirement, 0U);
  EXPECT_EQ(spatial->outcome(), core::SiteOutcome::Unresolved);
  EXPECT_EQ(
      spatial->requirements[0].check,
      (core::FacetCheck{.kind = core::CheckTemplate::Len, .proven = false}));
}

TEST(PlaceHandleTable, ResolvesHandles) {
  const auto unit = collectUnit("int f(int *p, int n) { return p[n]; }\n");
  PlaceHandleTable handles;
  const clang::ValueDecl *p = paramNamed(unit, "f", "p");
  const clang::ValueDecl *n = paramNamed(unit, "f", "n");
  const std::uint64_t first = handles.place(*p);
  EXPECT_EQ(first, 1U);
  EXPECT_EQ(handles.place(*n), 2U);
  EXPECT_EQ(handles.place(*p), first);
  EXPECT_EQ(handles.resolvePlace(first), p);
  EXPECT_EQ(handles.resolvePlace(0), nullptr);
  EXPECT_EQ(handles.resolvePlace(9), nullptr);
  const std::uint64_t type = handles.type(unit.context().IntTy);
  EXPECT_EQ(handles.resolveType(type), unit.context().IntTy);
  EXPECT_TRUE(handles.resolveType(0).isNull());
}

} // namespace
} // namespace weavec::analysis
