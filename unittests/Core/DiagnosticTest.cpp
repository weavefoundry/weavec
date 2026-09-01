//===- DiagnosticTest.cpp - Tests for core diagnostics --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Diagnostic.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

TEST(SourceLocation, FormatsAsFileLineColumn) {
  const SourceLocation loc{.file = "a.c", .line = 3, .column = 7, .opaque = 0};
  EXPECT_TRUE(loc.isValid());
  EXPECT_EQ(loc.toString(), "a.c:3:7");

  const SourceLocation unknown{};
  EXPECT_FALSE(unknown.isValid());
  EXPECT_EQ(unknown.toString(), "<unknown>");

  const SourceLocation noFile{.file = "", .line = 1, .column = 1, .opaque = 0};
  EXPECT_EQ(noFile.toString(), "<input>:1:1");
}

TEST(Diagnostic, AddNoteInheritsId) {
  Diagnostic diagnostic{.severity = Severity::Error,
                        .id = diag::UseAfterFree,
                        .message = "use after free",
                        .location = {},
                        .notes = {}};
  diagnostic.addNote("freed here", {}).addNote("allocated here", {});
  ASSERT_EQ(diagnostic.notes.size(), 2U);
  EXPECT_EQ(diagnostic.notes[0].severity, Severity::Note);
  EXPECT_EQ(diagnostic.notes[0].id, diag::UseAfterFree);
  EXPECT_EQ(diagnostic.notes[1].message, "allocated here");
}

TEST(DiagnosticCollector, CountsBySeverity) {
  DiagnosticCollector collector;
  EXPECT_TRUE(collector.empty());
  EXPECT_FALSE(collector.hasErrors());

  collector.report(Diagnostic{.severity = Severity::Warning,
                              .id = diag::AnnotationRequired,
                              .message = "w",
                              .location = {},
                              .notes = {}});
  collector.report(Diagnostic{.severity = Severity::Error,
                              .id = diag::DoubleFree,
                              .message = "e",
                              .location = {},
                              .notes = {}});

  EXPECT_EQ(collector.size(), 2U);
  EXPECT_EQ(collector.count(Severity::Warning), 1U);
  EXPECT_EQ(collector.count(Severity::Error), 1U);
  EXPECT_EQ(collector.count(Severity::Note), 0U);
  EXPECT_TRUE(collector.hasErrors());

  collector.clear();
  EXPECT_TRUE(collector.empty());
}

TEST(Severity, ToString) {
  EXPECT_EQ(toString(Severity::Note), "note");
  EXPECT_EQ(toString(Severity::Warning), "warning");
  EXPECT_EQ(toString(Severity::Error), "error");
}

} // namespace
} // namespace weavec::core
