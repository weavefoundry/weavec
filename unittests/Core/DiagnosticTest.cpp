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
                        .notes = {},
                        .fixits = {}};
  diagnostic.addNote("freed here", {}).addNote("allocated here", {});
  ASSERT_EQ(diagnostic.notes.size(), 2U);
  EXPECT_EQ(diagnostic.notes[0].severity, Severity::Note);
  EXPECT_EQ(diagnostic.notes[0].id, diag::UseAfterFree);
  EXPECT_EQ(diagnostic.notes[1].message, "allocated here");
}

TEST(DiagnosticCollector, CountsBySeverity) {
  DiagnosticCollector collector;
  EXPECT_TRUE(collector.empty());

  collector.report(Diagnostic{.severity = Severity::Warning,
                              .id = diag::Leak,
                              .message = "w",
                              .location = {},
                              .notes = {},
                              .fixits = {}});
  collector.report(Diagnostic{.severity = Severity::Error,
                              .id = diag::DoubleFree,
                              .message = "e",
                              .location = {},
                              .notes = {},
                              .fixits = {}});

  EXPECT_EQ(collector.size(), 2U);

  collector.clear();
  EXPECT_TRUE(collector.empty());
}

TEST(DiagnosticIds, PointerValidityIdsAreKnown) {
  // RFC 0008, *Diagnostics*.
  EXPECT_EQ(diag::NullDereference, "null-dereference");
  EXPECT_EQ(diag::UseOfUninitialized, "use-of-uninitialized");
  EXPECT_EQ(diag::InvalidRelease, "invalid-release");
  for (const std::string_view id :
       {diag::NullDereference, diag::UseOfUninitialized, diag::InvalidRelease})
    EXPECT_TRUE(diag::isKnown(id)) << id;
  // RFC 0030, *Diagnostics*: 15 kept ids and 5 added ones, less the three
  // RFC 0035 deletes.
  EXPECT_EQ(diag::All.size(), 17U);
}

TEST(DiagnosticIds, SpatialSafetyIdIsKnown) {
  // RFC 0011, *Diagnostics*; RFC 0030 §3.3: an error, and only definite.
  EXPECT_EQ(diag::OutOfBounds, "out-of-bounds");
  EXPECT_TRUE(diag::isKnown(diag::OutOfBounds));
  EXPECT_EQ(diag::defaultSeverity(diag::OutOfBounds, Certainty::Definite),
            Severity::Error);
  EXPECT_EQ(diag::defaultSeverity(diag::OutOfBounds, Certainty::Possible),
            Severity::Error);
}

TEST(DiagnosticIds, ProveOrTrapIdsAreKnown) {
  // RFC 0030, *Diagnostics*: the added ids RFC 0035 keeps, and their
  // severities.
  EXPECT_EQ(diag::ContradictedAssumption, "contradicted-assumption");
  EXPECT_EQ(diag::AllocationFailure, "allocation-failure");
  // RFC 0035 deletes the require levels' ids and the link step's.
  EXPECT_FALSE(diag::isKnown("unresolved-operation"));
  EXPECT_FALSE(diag::isKnown("unchecked-operation"));
  EXPECT_FALSE(diag::isKnown("unanalyzed-input"));
  for (const std::string_view id : {diag::ContradictedAssumption}) {
    EXPECT_TRUE(diag::isKnown(id)) << id;
    EXPECT_EQ(diag::defaultSeverity(id, Certainty::Definite), Severity::Error)
        << id;
    EXPECT_TRUE(diag::isEnabledByDefault(id)) << id;
  }
  EXPECT_TRUE(diag::isKnown(diag::AllocationFailure));
  EXPECT_EQ(diag::defaultSeverity(diag::AllocationFailure, Certainty::Definite),
            Severity::Warning);
  EXPECT_FALSE(diag::isEnabledByDefault(diag::AllocationFailure));
  // A temporal id is an error when definite and a warning when possible.
  EXPECT_EQ(diag::defaultSeverity(diag::UseAfterFree, Certainty::Definite),
            Severity::Error);
  EXPECT_EQ(diag::defaultSeverity(diag::UseAfterFree, Certainty::Possible),
            Severity::Warning);
  // Checked mode's ids are gone, and so are the two RFC 0030 replaces with
  // ledger rows (`unresolved(unanalysed | budget | ...)`, and
  // `unresolved(unknown-callee)` with a fix-it).
  for (const std::string_view id :
       {"checking-incomplete", "checking-failed", "analysis-incomplete",
        "annotation-required"}) {
    EXPECT_TRUE(diag::isRemoved(id)) << id;
    EXPECT_FALSE(diag::isKnown(id)) << id;
  }
}

TEST(Diagnostic, CertaintyDefaultsToDefinite) {
  const Diagnostic diagnostic{};
  EXPECT_EQ(diagnostic.certainty, Certainty::Definite);
}

} // namespace
} // namespace weavec::core
