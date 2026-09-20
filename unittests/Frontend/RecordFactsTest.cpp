//===- RecordFactsTest.cpp - Tests for a unit's interface facts -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/RecordFacts.h"

#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Frontend/FrontendAction.h"

#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace weavec::frontend::record {
namespace {

/// The interface facts a compile of `code` records, through the unit
/// pipeline as `weavec-cc -c` runs it.
std::shared_ptr<const InterfaceFacts> factsOf(const std::string &code,
                                              UnitResult *out = nullptr) {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c17"}, "unit.c");
  if (!ast)
    return nullptr;
  FrontendOptions options;
  options.collectInterface = true;
  UnitResult result = analyzeRetainedUnit(*ast, options);
  if (out != nullptr)
    *out = result;
  return result.interface;
}

constexpr const char *Unit = R"c(
typedef unsigned long size_t;
void *malloc(size_t);
void free(void *);
void inspect(char *__attribute__((annotate("weavec.borrowed"))) p);
int first(int *p);
int keep(int *p) { return *p; }
int main(void) {
  int a[4] = {1, 2, 3, 4};
  char *b = malloc(8);
  if (!b)
    return 1;
  inspect(b);
  int r = first(a + 1);
  free(b);
  return r + keep(a);
}
)c";

} // namespace

TEST(RecordFacts, ImportsCarryTheirDeclarationsAndCalls) {
  UnitResult result;
  const auto facts = factsOf(Unit, &result);
  ASSERT_TRUE(facts);
  const auto inspect = facts->imports.find("inspect");
  ASSERT_NE(inspect, facts->imports.end());
  ASSERT_EQ(inspect->second.declared.params.size(), 1U);
  EXPECT_EQ(inspect->second.declared.params[0].name, "p");
  EXPECT_EQ(inspect->second.declared.params[0].ownership,
            std::optional<std::string>("WEAVEC_BORROWED"));
  ASSERT_TRUE(inspect->second.location);
  EXPECT_EQ(inspect->second.location->line, 5U);
  EXPECT_EQ(inspect->second.location->column, 6U);
  ASSERT_EQ(inspect->second.calls.size(), 1U);
  EXPECT_EQ(inspect->second.calls[0].function, "main");
  ASSERT_TRUE(inspect->second.calls[0].site);
  // The call is that Call site of the unit's ledger.
  ASSERT_TRUE(result.ledger);
  const auto &functions = result.ledger->ledger.units.at(0).functions;
  const auto main =
      std::ranges::find_if(functions, [](const core::FunctionLedger &f) {
        return f.name == "main";
      });
  ASSERT_NE(main, functions.end());
  const core::Site *site = main->site(*inspect->second.calls[0].site);
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->kind, core::SiteKind::Call);
  EXPECT_EQ(site->callee, "inspect");

  // A cursor is never Single-valid (§7.3).
  const auto first = facts->imports.find("first");
  ASSERT_NE(first, facts->imports.end());
  ASSERT_EQ(first->second.calls.size(), 1U);
  EXPECT_EQ(first->second.calls[0].args,
            std::vector<std::optional<bool>>{false});
  EXPECT_TRUE(first->second.declared.empty());
}

TEST(RecordFacts, DefinitionsCarryTheirKindsAndReliance) {
  const auto facts = factsOf(Unit);
  ASSERT_TRUE(facts);
  const auto keep = facts->functions.find("keep");
  ASSERT_NE(keep, facts->functions.end());
  ASSERT_EQ(keep->second.params.size(), 1U);
  ASSERT_TRUE(keep->second.params[0]);
  const auto kind = parseKind(*keep->second.params[0]);
  ASSERT_TRUE(kind);
  EXPECT_EQ(kind->shape, core::PointerShape::Single);
  EXPECT_EQ(kind->source, core::KindSource::Default);
  EXPECT_EQ(keep->second.reliesOnSingle, std::vector<std::uint32_t>{0});
  ASSERT_TRUE(keep->second.location);
  EXPECT_EQ(keep->second.location->line, 7U);
  // The function-pointer slots of the unit, with its rules.
  EXPECT_TRUE(facts->slots.defined.contains("keep"));
  EXPECT_TRUE(facts->slots.exported.contains("keep"));
  EXPECT_FALSE(facts->allocator);
}

TEST(RecordFacts, AUnitThatDefinesTheAllocatorSaysSo) {
  const auto facts = factsOf(R"c(
typedef unsigned long size_t;
static char arena[64];
void *malloc(size_t n) { (void)n; return arena; }
)c");
  ASSERT_TRUE(facts);
  EXPECT_EQ(facts->allocator, std::optional<std::string>("malloc"));
  EXPECT_EQ(facts->loweredAllocations, 0U);
}

TEST(RecordFacts, TheCompactRowsAreTheUnitLedgers) {
  UnitResult result;
  ASSERT_TRUE(factsOf(Unit, &result));
  const Payload payload = payloadOf(result);
  ASSERT_TRUE(result.ledger);
  const core::UnitLedger &unit = result.ledger->ledger.units.at(0);
  ASSERT_EQ(payload.sites.size(), unit.functions.size());
  for (std::size_t f = 0; f < unit.functions.size(); ++f) {
    EXPECT_EQ(payload.sites[f].function, unit.functions[f].name);
    ASSERT_EQ(payload.sites[f].rows.size(), unit.functions[f].sites.size());
    for (std::size_t s = 0; s < unit.functions[f].sites.size(); ++s) {
      const core::Site &site = unit.functions[f].sites[s];
      const SiteRow &row = payload.sites[f].rows[s];
      EXPECT_EQ(row.kind, site.kind);
      EXPECT_EQ(row.line, site.location.line);
      for (const core::Facet facet : core::AllFacets) {
        const auto &cell = row.facets.at(static_cast<std::size_t>(facet));
        const core::FacetRecord *record = site.facet(facet);
        ASSERT_EQ(cell.has_value(), record != nullptr);
        if (record != nullptr)
          EXPECT_EQ(cell->compact(), record->decision.compact());
      }
    }
  }
  EXPECT_EQ(payload.a5, unit.a5);
  // Through JSON and back, exactly.
  std::string error;
  const auto read = payloadFromJson(toJson(payload), "unit.c", error);
  ASSERT_TRUE(read) << error;
  EXPECT_EQ(read->sites, payload.sites);
  EXPECT_EQ(read->facts, payload.facts);
}

} // namespace weavec::frontend::record
