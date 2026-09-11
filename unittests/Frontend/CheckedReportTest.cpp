//===- CheckedReportTest.cpp - Portable checked results (RFC 0018) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/CheckedReport.h"

#include "weavec/Frontend/CheckedArtifacts.h"
#include "weavec/Frontend/Sidecar.h"

#include "llvm/Support/JSON.h"

#include <gtest/gtest.h>

namespace weavec::frontend {
static analysis::UnitExports checkedUnit() {
  analysis::UnitExports unit;
  unit.source = "space and é/\"source.c";
  unit.checkedTarget = "aarch64-apple-darwin";
  auto &contract = unit.checkedDefinitions["main"];
  contract.computed = contract.selected = true;
  contract.obligations.add(
      {.property = core::SafetyProperty::Bounds,
       .outcome = core::SafetyOutcome::Proven,
       .location = {.file = unit.source, .line = 3, .column = 4, .opaque = 0},
       .function = "main",
       .subject = "index",
       .reason = "guard\nproved",
       .calls = {}});
  return unit;
}
TEST(CheckedReport, EscapingAndScopeRoundTripThroughJson) {
  const auto unit = checkedUnit();
  CheckedReport report;
  report.record(unit);
  auto parsed = llvm::json::parse(report.json());
  ASSERT_TRUE(parsed);
  const auto *object = parsed->getAsObject();
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(object->getInteger("version"), 2);
  EXPECT_EQ(object->getInteger("model_version"), 17);
  ASSERT_NE(object->getObject("totals"), nullptr);
  EXPECT_EQ(object->getObject("totals")->getInteger("complete"), 1);
  const auto *units = object->getArray("units");
  ASSERT_NE(units, nullptr);
  ASSERT_EQ(units->size(), 1U);
  EXPECT_EQ((*units)[0].getAsObject()->getString("source"), unit.source);
  EXPECT_FALSE(CheckedReport::failed(unit));
  auto deferred = unit;
  deferred.checkedDefinitions.at("main").deferred = true;
  EXPECT_TRUE(CheckedReport::failed(deferred));
  EXPECT_FALSE(CheckedReport::failed(deferred, true));
  deferred.checkedDefinitions.at("main").limited = true;
  EXPECT_TRUE(CheckedReport::failed(deferred, true));
}
TEST(CheckedReport, SettledRecordReplacesAnEarlierApproximation) {
  auto unit = checkedUnit();
  unit.checkedDefinitions.at("main").limited = true;
  CheckedReport report;
  report.record(unit);
  EXPECT_NE(report.json().find("\"complete\":false"), std::string::npos);
  report.record(checkedUnit());
  EXPECT_EQ(report.json().find("\"complete\":false"), std::string::npos);
  EXPECT_EQ(report.json(), report.json());
}
TEST(CheckedReport, UnsettledAnalysisCannotPublishCompleteContracts) {
  CheckedReport report;
  report.record(checkedUnit());
  report.invalidate("did not converge");
  EXPECT_NE(report.json(false).find("\"complete\":false"), std::string::npos);
  EXPECT_NE(report.json(false).find("did not converge"), std::string::npos);
}
TEST(CheckedSidecar, ContractsAndBindingsRoundTrip) {
  UnitRecord record;
  record.exports = checkedUnit();
  record.exports.source = "source.c";
  record.command = {"-x", "c", "source.c"};
  record.commandDigest = checkedCommandDigest(record.command);
  record.preprocessingDigest = checkedDigest("preprocessed source");
  record.objectDigest = checkedDigest("object bytes");
  record.exports.checkedInputs["path with spaces/é.h"] =
      checkedDigest("header bytes");
  const auto parsed = parseUnitRecord(printUnitRecord(record));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->exports.checkedDefinitions,
            record.exports.checkedDefinitions);
  EXPECT_EQ(parsed->exports.checkedInputs, record.exports.checkedInputs);
  EXPECT_EQ(parsed->objectDigest, record.objectDigest);
  EXPECT_EQ(parsed->commandDigest, record.commandDigest);
  EXPECT_EQ(parsed->preprocessingDigest, record.preprocessingDigest);
  EXPECT_EQ(parsed->exports.checkedTarget, record.exports.checkedTarget);
}
TEST(CheckedSidecar, DuplicateOrMalformedBindingsAreRejected) {
  UnitRecord record;
  record.exports = checkedUnit();
  record.objectDigest = checkedDigest("a");
  const auto text = printUnitRecord(record);
  EXPECT_FALSE(
      parseUnitRecord(text + "checked-object " + record.objectDigest + "\n"));
  EXPECT_FALSE(parseUnitRecord(text + "checked-input 00 nope\n"));
  EXPECT_FALSE(parseUnitRecord(text + "checked-preprocessing nope\n"));
  const auto bound =
      text + "checked-preprocessing " + checkedDigest("pp") + "\n";
  EXPECT_TRUE(parseUnitRecord(bound));
  EXPECT_FALSE(parseUnitRecord(bound + "checked-preprocessing " +
                               checkedDigest("pp") + "\n"));
  EXPECT_FALSE(parseUnitRecord(text + "checked-definition nope nope\n"));
  EXPECT_FALSE(parseUnitRecord(text + "checked-target other\n"));
}
TEST(CheckedArtifact, MissingBindingIsNotEvidence) {
  UnitRecord record;
  record.exports = checkedUnit();
  std::string error;
  EXPECT_FALSE(validateCheckedArtifact(record, "missing.o", error));
  EXPECT_NE(error.find("missing build binding"), std::string::npos);
  EXPECT_NE(checkedCommandDigest({"a", "bc"}),
            checkedCommandDigest({"ab", "c"}));
}
TEST(CheckedReport, CompactStringsOwnTheirStorageAfterObligationsAreReplaced) {
  CheckedReport report;
  report.compact = true;
  report.record(checkedUnit());
  const auto first = report.json();
  EXPECT_EQ(first, report.json());
  auto parsed = llvm::json::parse(first);
  ASSERT_TRUE(parsed);
  auto *root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getInteger("version"), 3);
  ASSERT_NE(root->getArray("strings"), nullptr);
  bool found = false;
  for (const auto &value : *root->getArray("strings"))
    found |= value.getAsString() == "guard\nproved";
  EXPECT_TRUE(found);
  ASSERT_NE(root->getArray("obligation_records"), nullptr);
  EXPECT_EQ(root->getArray("obligation_records")->size(), 1U);
}

} // namespace weavec::frontend
