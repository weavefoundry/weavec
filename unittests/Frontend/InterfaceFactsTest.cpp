//===- InterfaceFactsTest.cpp - Tests for a unit's interface facts --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/InterfaceFacts.h"

#include "weavec/Frontend/FrontendAction.h"

#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace weavec::frontend {
namespace {

/// The interface facts a run of `code` collects, through the unit pipeline
/// as `weavec --whole-program` runs it.
std::shared_ptr<const InterfaceFacts> factsOf(const std::string &code) {
  auto ast =
      clang::tooling::buildASTFromCodeWithArgs(code, {"-std=c17"}, "unit.c");
  if (!ast)
    return nullptr;
  FrontendOptions options;
  options.collectInterface = true;
  return analyzeRetainedUnit(*ast, options).interface;
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

TEST(InterfaceFacts, ImportsCarryTheirDeclarations) {
  const auto facts = factsOf(Unit);
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

  const auto first = facts->imports.find("first");
  ASSERT_NE(first, facts->imports.end());
  EXPECT_TRUE(first->second.declared.empty());
}

TEST(InterfaceFacts, DefinitionsCarryTheirKinds) {
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
  ASSERT_TRUE(keep->second.location);
  EXPECT_EQ(keep->second.location->line, 7U);
  // The function-pointer slots of the unit, with its rules.
  EXPECT_TRUE(facts->slots.defined.contains("keep"));
  EXPECT_TRUE(facts->slots.exported.contains("keep"));
}

TEST(InterfaceFacts, KindsSpellTheirSource) {
  const auto kind = parseKind("declared single nonnull");
  ASSERT_TRUE(kind);
  EXPECT_EQ(kind->source, core::KindSource::Declared);
  EXPECT_EQ(spellKind(*kind), "declared single nonnull");
  EXPECT_FALSE(parseKind("single"));
  EXPECT_FALSE(parseKind("nowhere single"));
}

} // namespace weavec::frontend
