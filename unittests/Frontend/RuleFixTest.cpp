//===- RuleFixTest.cpp - Rule errors and fixes in the ledger --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0049 §7: an error with its subjects and fixes is written to the
// ledger and read back unchanged, as the fixer and the audit read it.
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/RuleFix.h"

#include "weavec/Frontend/EnforcementLedger.h"

#include "llvm/Support/JSON.h"

#include "gtest/gtest.h"

namespace weavec::frontend {
namespace {

TEST(RuleFixTest, ErrorsRoundTrip) {
  RuleError error;
  error.rule = "index-single";
  error.file = "/src/slice.h";
  error.line = 13;
  error.column = 49;
  error.message = "'data_' points to one object and cannot be indexed";
  error.owner = "Slice::at";
  error.ownerFile = "slice.h";
  error.ownerLine = 12;
  error.surveyed = true;
  error.subjects = {"/src/slice.h|Slice::data_"};
  RuleFix fix;
  fix.level = "likely";
  fix.declaration = "/src/slice.h|Slice::data_";
  fix.name = "Slice::data_";
  fix.aspect = "bound";
  fix.fact = "counted_by(size_)";
  fix.text = "const char *WEAVEC_COUNTED_BY(size_) data_";
  fix.evidence = "'size_' is its count in the call to 'memcmp' at line 19";
  fix.edits.push_back({.file = "/src/slice.h",
                       .offset = 412,
                       .text = "WEAVEC_COUNTED_BY(size_) ",
                       .needsInclude = true});
  error.fixes.push_back(fix);
  error.fixes.push_back({.level = "choice",
                         .declaration = "/src/slice.h|Slice",
                         .name = "Slice",
                         .aspect = "aliased",
                         .fact = "aliased",
                         .text = "declare 'Slice' WEAVEC_ALIASED",
                         .evidence = "",
                         .edits = {}});

  // Through text, as the ledger is.
  std::string text;
  llvm::raw_string_ostream out(text);
  out << errorJson(error);
  llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(text);
  ASSERT_TRUE(static_cast<bool>(parsed));
  RuleError back;
  ASSERT_TRUE(readError(*parsed, back));
  EXPECT_EQ(back.rule, error.rule);
  EXPECT_EQ(back.file, error.file);
  EXPECT_EQ(back.line, 13U);
  EXPECT_EQ(back.column, 49U);
  EXPECT_EQ(back.owner, error.owner);
  EXPECT_EQ(back.ownerLine, 12U);
  EXPECT_TRUE(back.surveyed);
  EXPECT_FALSE(back.global);
  EXPECT_EQ(back.subjects, error.subjects);
  ASSERT_EQ(back.fixes.size(), 2U);
  EXPECT_EQ(back.fixes[0].fact, "counted_by(size_)");
  EXPECT_EQ(back.fixes[0].name, "Slice::data_");
  ASSERT_EQ(back.fixes[0].edits.size(), 1U);
  EXPECT_EQ(back.fixes[0].edits[0].offset, 412U);
  EXPECT_EQ(back.fixes[0].edits[0].text, "WEAVEC_COUNTED_BY(size_) ");
  EXPECT_TRUE(back.fixes[0].edits[0].needsInclude);
  EXPECT_EQ(back.fixes[1].level, "choice");
  EXPECT_TRUE(back.fixes[1].edits.empty());
}

TEST(RuleFixTest, NotAnError) {
  RuleError error;
  EXPECT_FALSE(readError(llvm::json::Value(3), error));
  RuleFix fix;
  EXPECT_FALSE(readFix(llvm::json::Value("x"), fix));
}

} // namespace
} // namespace weavec::frontend
