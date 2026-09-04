//===- DiagnosticControlTest.cpp - Tests for -W flag handling -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/DiagnosticControl.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace weavec::frontend {
namespace {

using core::Severity;
using core::diag::AnnotationRequired;
using core::diag::UseAfterFree;

core::Diagnostic make(std::string_view id, Severity severity,
                      std::string message = "m", std::uint32_t line = 1) {
  return core::Diagnostic{
      .severity = severity,
      .id = id,
      .message = std::move(message),
      .location = core::SourceLocation{"a.c", line, 3},
      .notes = {},
      .fixits = {},
  };
}

class Recorder final : public core::DiagnosticSink {
public:
  void report(const core::Diagnostic &d) override { seen.push_back(d); }
  std::vector<core::Diagnostic> seen;
};

using Level = DiagnosticControl::Level;

TEST(DiagnosticControl, DefaultsLeaveDiagnosticsAlone) {
  const DiagnosticControl control;
  const auto error = control.apply(make(UseAfterFree, Severity::Error));
  ASSERT_TRUE(error);
  EXPECT_EQ(error->severity, Severity::Error);
  const auto warning =
      control.apply(make(AnnotationRequired, Severity::Warning));
  ASSERT_TRUE(warning);
  EXPECT_EQ(warning->severity, Severity::Warning);
}

TEST(DiagnosticControl, RecognisesOnlyWeaveCSpellings) {
  EXPECT_TRUE(
      DiagnosticControl::isWeaveCFlag("-Wno-weavec-annotation-required"));
  EXPECT_TRUE(DiagnosticControl::isWeaveCFlag("-Werror=weavec"));
  EXPECT_TRUE(DiagnosticControl::isWeaveCFlag("-Wno-error=weavec-double-free"));
  EXPECT_TRUE(DiagnosticControl::isWeaveCFlag("-Wweavec-invalid-annotation"));
  EXPECT_FALSE(DiagnosticControl::isWeaveCFlag("-Wall"));
  EXPECT_FALSE(DiagnosticControl::isWeaveCFlag("-Wno-unused"));
  EXPECT_FALSE(DiagnosticControl::isWeaveCFlag("-Werror"));
  EXPECT_FALSE(DiagnosticControl::isWeaveCFlag("-Wweavecish"));
  EXPECT_FALSE(DiagnosticControl::isWeaveCFlag("-fweavec"));
}

TEST(DiagnosticControl, DisablesAndReenablesWarnings) {
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-weavec-annotation-required", error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(control.apply(make(AnnotationRequired, Severity::Warning)));
  // Other ids are untouched.
  EXPECT_TRUE(control.apply(make(UseAfterFree, Severity::Error)));

  ASSERT_TRUE(control.parse("-Wweavec-annotation-required", error));
  EXPECT_TRUE(control.apply(make(AnnotationRequired, Severity::Warning)));
}

TEST(DiagnosticControl, ErrorsCannotBeDisabledOnlyLowered) {
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-weavec-use-after-free", error));
  EXPECT_EQ(error, "'-Wno-weavec-use-after-free': 'use-after-free' is an error "
                   "and cannot be disabled; use "
                   "-Wno-error=weavec-use-after-free to make it a warning");
  EXPECT_EQ(control.levelFor(UseAfterFree), Level::Default);

  error.clear();
  ASSERT_TRUE(control.parse("-Wno-error=weavec-use-after-free", error));
  EXPECT_TRUE(error.empty());
  const auto lowered = control.apply(make(UseAfterFree, Severity::Error));
  ASSERT_TRUE(lowered);
  EXPECT_EQ(lowered->severity, Severity::Warning);
}

TEST(DiagnosticControl, RaisesWarningsToErrors) {
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Werror=weavec-annotation-required", error));
  const auto raised =
      control.apply(make(AnnotationRequired, Severity::Warning));
  ASSERT_TRUE(raised);
  EXPECT_EQ(raised->severity, Severity::Error);
}

TEST(DiagnosticControl, GroupFlagsApplyToEveryId) {
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-error=weavec", error));
  EXPECT_EQ(control.apply(make(UseAfterFree, Severity::Error))->severity,
            Severity::Warning);
  EXPECT_EQ(
      control.apply(make(core::diag::DoubleFree, Severity::Error))->severity,
      Severity::Warning);
  EXPECT_EQ(
      control.apply(make(AnnotationRequired, Severity::Warning))->severity,
      Severity::Warning);

  ASSERT_TRUE(control.parse("-Werror=weavec", error));
  EXPECT_EQ(
      control.apply(make(AnnotationRequired, Severity::Warning))->severity,
      Severity::Error);

  // `-Wno-weavec` disables the warnings and leaves the errors alone.
  ASSERT_TRUE(control.parse("-Wno-weavec", error));
  EXPECT_FALSE(control.apply(make(AnnotationRequired, Severity::Warning)));
  EXPECT_TRUE(control.apply(make(UseAfterFree, Severity::Error)));
}

TEST(DiagnosticControl, LaterFlagsWinButDisabledStaysDisabled) {
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Werror=weavec-annotation-required", error));
  ASSERT_TRUE(control.parse("-Wno-error=weavec", error));
  EXPECT_EQ(
      control.apply(make(AnnotationRequired, Severity::Warning))->severity,
      Severity::Warning);

  ASSERT_TRUE(control.parse("-Wno-weavec-annotation-required", error));
  ASSERT_TRUE(control.parse("-Werror=weavec", error));
  EXPECT_FALSE(control.apply(make(AnnotationRequired, Severity::Warning)));
  EXPECT_EQ(control.apply(make(UseAfterFree, Severity::Error))->severity,
            Severity::Error);
}

TEST(DiagnosticControl, RejectsUnknownIds) {
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wweavec-nonsense", error));
  EXPECT_EQ(error,
            "unknown WeaveC diagnostic 'nonsense' in '-Wweavec-nonsense'");
  EXPECT_FALSE(control.parse("-Wunused-variable", error));
}

TEST(FilteringSink, AppliesControlAndRemembersWhatItForwarded) {
  Recorder recorder;
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-error=weavec-use-after-free", error));
  FilteringSink sink(recorder, control);

  sink.report(make(UseAfterFree, Severity::Error, "uaf", 7));
  sink.report(make(core::diag::DoubleFree, Severity::Error, "df", 9));
  ASSERT_EQ(recorder.seen.size(), 2U);
  EXPECT_EQ(recorder.seen[0].severity, Severity::Warning);
  EXPECT_EQ(recorder.seen[1].severity, Severity::Error);
  EXPECT_EQ(sink.warnings(), 1U);
  EXPECT_EQ(sink.errors(), 1U);

  const std::set<ReportedDiagnostic> expected{
      ReportedDiagnostic{
          .id = "use-after-free", .file = "a.c", .line = 7, .column = 3},
      ReportedDiagnostic{
          .id = "double-free", .file = "a.c", .line = 9, .column = 3},
  };
  EXPECT_EQ(sink.reported(), expected);
}

TEST(FilteringSink, SkipsWhatAnEarlierStepReported) {
  Recorder recorder;
  const std::set<ReportedDiagnostic> earlier{ReportedDiagnostic{
      .id = "use-after-free", .file = "a.c", .line = 7, .column = 3}};
  FilteringSink sink(recorder, DiagnosticControl{}, &earlier);
  sink.report(make(UseAfterFree, Severity::Error, "uaf", 7));
  sink.report(make(UseAfterFree, Severity::Error, "uaf", 8));
  ASSERT_EQ(recorder.seen.size(), 1U);
  EXPECT_EQ(recorder.seen[0].location.line, 8U);
  EXPECT_EQ(sink.errors(), 1U);
}

TEST(FilteringSink, ReportsEachBoundaryOncePerProgram) {
  Recorder recorder;
  std::set<std::string> once;
  FilteringSink first(recorder, DiagnosticControl{}, nullptr, &once);
  first.report(make(AnnotationRequired, Severity::Warning, "call to 'f'", 1));
  first.report(make(AnnotationRequired, Severity::Warning, "call to 'f'", 5));
  FilteringSink second(recorder, DiagnosticControl{}, nullptr, &once);
  second.report(make(AnnotationRequired, Severity::Warning, "call to 'f'", 2));
  second.report(make(AnnotationRequired, Severity::Warning, "call to 'g'", 3));
  // Other ids are never deduplicated by message.
  second.report(make(UseAfterFree, Severity::Error, "same", 4));
  second.report(make(UseAfterFree, Severity::Error, "same", 6));
  ASSERT_EQ(recorder.seen.size(), 4U);
  EXPECT_EQ(recorder.seen[0].message, "call to 'f'");
  EXPECT_EQ(recorder.seen[1].message, "call to 'g'");
  EXPECT_EQ(recorder.seen[2].location.line, 4U);
  EXPECT_EQ(recorder.seen[3].location.line, 6U);
}

TEST(DiagnosticIds, DefaultSeverities) {
  EXPECT_TRUE(core::diag::isWarningByDefault(core::diag::AnnotationRequired));
  EXPECT_TRUE(core::diag::isWarningByDefault(core::diag::InvalidAnnotation));
  EXPECT_FALSE(core::diag::isWarningByDefault(core::diag::UseAfterFree));
  EXPECT_FALSE(core::diag::isWarningByDefault(core::diag::UnsafeOperation));
  EXPECT_TRUE(core::diag::isKnown("lifetime-too-short"));
  EXPECT_FALSE(core::diag::isKnown("lifetime"));
  // RFC 0007: a leak is a warning, a mismatched release an error.
  EXPECT_TRUE(core::diag::isWarningByDefault(core::diag::Leak));
  EXPECT_FALSE(core::diag::isWarningByDefault(core::diag::MismatchedRelease));
  EXPECT_TRUE(core::diag::isKnown("leak"));
  EXPECT_TRUE(core::diag::isKnown("mismatched-release"));
  // RFC 0008: all three pointer-validity ids are errors.
  EXPECT_FALSE(core::diag::isWarningByDefault(core::diag::NullDereference));
  EXPECT_FALSE(core::diag::isWarningByDefault(core::diag::UseOfUninitialized));
  EXPECT_FALSE(core::diag::isWarningByDefault(core::diag::InvalidRelease));
  EXPECT_TRUE(core::diag::isKnown("null-dereference"));
  EXPECT_TRUE(core::diag::isKnown("use-of-uninitialized"));
  EXPECT_TRUE(core::diag::isKnown("invalid-release"));
  EXPECT_EQ(core::diag::All.size(), 14U);
}

} // namespace
} // namespace weavec::frontend
