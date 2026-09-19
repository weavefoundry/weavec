//===- DiagnosticControlTest.cpp - Tests for -W flag handling -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/DiagnosticControl.h"

#include "llvm/ADT/StringRef.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::frontend {

using core::Certainty;
using core::Severity;
using core::diag::AllocationFailure;
using core::diag::AnnotationRequired;
using core::diag::UseAfterFree;

static core::Diagnostic make(std::string_view id, Severity severity,
                             std::string message = "m", std::uint32_t line = 1,
                             Certainty certainty = Certainty::Definite) {
  return core::Diagnostic{
      .severity = severity,
      .certainty = certainty,
      .id = id,
      .message = std::move(message),
      .location =
          core::SourceLocation{.file = "a.c", .line = line, .column = 3},
      .notes = {},
      .fixits = {},
  };
}

/// A possible finding, with its default severity (RFC 0030 §3).
static core::Diagnostic possible(std::string_view id) {
  return make(id, core::diag::defaultSeverity(id, Certainty::Possible), "m", 1,
              Certainty::Possible);
}

namespace {
class Recorder final : public core::DiagnosticSink {
public:
  void report(const core::Diagnostic &d) override { seen.push_back(d); }
  std::vector<core::Diagnostic> seen;
};
} // namespace

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
  // RFC 0030, *Diagnostics*: ids that are errors whatever their certainty.
  for (const std::string_view id :
       {core::diag::NullDereference, core::diag::UseOfUninitialized,
        core::diag::OutOfBounds, core::diag::UnsafeOperation,
        core::diag::AnnotationMismatch, core::diag::InvalidIntegerOperation,
        core::diag::ContradictedAssumption, core::diag::UnresolvedOperation,
        core::diag::UncheckedOperation}) {
    DiagnosticControl control;
    std::string error;
    const std::string flag = "-Wno-weavec-" + std::string(id);
    ASSERT_TRUE(control.parse(flag, error));
    EXPECT_EQ(error, "'" + flag + "': '" + std::string(id) +
                         "' is an error and cannot be disabled; use "
                         "-Wno-error=weavec-" +
                         std::string(id) + " to make it a warning");
    EXPECT_EQ(control.levelFor(id), Level::Default);

    error.clear();
    ASSERT_TRUE(control.parse("-Wno-error=weavec-" + std::string(id), error));
    EXPECT_TRUE(error.empty());
    const auto lowered = control.apply(make(id, Severity::Error));
    ASSERT_TRUE(lowered);
    EXPECT_EQ(lowered->severity, Severity::Warning);
  }
}

TEST(DiagnosticControl, DisablingAMixedIdKeepsItsDefiniteErrors) {
  // RFC 0030 §3: a possible use-after-free is a warning, which the flag
  // drops; the definite one is an error, which it cannot.
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-weavec-use-after-free", error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(control.apply(possible(UseAfterFree)));
  const auto definite = control.apply(make(UseAfterFree, Severity::Error));
  ASSERT_TRUE(definite);
  EXPECT_EQ(definite->severity, Severity::Error);
  EXPECT_TRUE(control.isEnabled(UseAfterFree));

  // The group flag does the same for every id.
  DiagnosticControl group;
  ASSERT_TRUE(group.parse("-Wno-weavec", error));
  for (const std::string_view id :
       {UseAfterFree, core::diag::DoubleFree, core::diag::UseAfterMove,
        core::diag::ConflictingBorrow, core::diag::LifetimeTooShort,
        core::diag::MismatchedRelease, core::diag::InvalidRelease}) {
    EXPECT_FALSE(group.apply(possible(id))) << id;
    EXPECT_TRUE(group.apply(make(id, Severity::Error))) << id;
  }
}

TEST(DiagnosticControl, AllocationFailureIsOffByDefault) {
  const auto shown =
      [](std::initializer_list<const char *> flags) -> std::optional<Severity> {
    DiagnosticControl control;
    std::string error;
    for (const char *flag : flags) {
      EXPECT_TRUE(control.parse(flag, error));
      EXPECT_TRUE(error.empty()) << flag << ": " << error;
    }
    EXPECT_EQ(
        control.isEnabled(AllocationFailure),
        control.apply(make(AllocationFailure, Severity::Warning)).has_value());
    if (const auto adjusted =
            control.apply(make(AllocationFailure, Severity::Warning)))
      return adjusted->severity;
    return std::nullopt;
  };
  EXPECT_EQ(shown({}), std::nullopt);
  EXPECT_EQ(shown({"-Wweavec-allocation-failure"}), Severity::Warning);
  EXPECT_EQ(shown({"-Wweavec"}), Severity::Warning);
  EXPECT_EQ(shown({"-Werror=weavec-allocation-failure"}), Severity::Error);
  // The severity group flags change severities, not what is enabled.
  EXPECT_EQ(shown({"-Werror=weavec"}), std::nullopt);
  EXPECT_EQ(shown({"-Wno-error=weavec"}), std::nullopt);
  EXPECT_EQ(shown({"-Wweavec", "-Werror=weavec"}), Severity::Error);
  EXPECT_EQ(shown({"-Wweavec-allocation-failure", "-Werror=weavec"}),
            Severity::Error);
  // Later flags win.
  EXPECT_EQ(shown({"-Wweavec", "-Wno-weavec-allocation-failure"}),
            std::nullopt);
  EXPECT_EQ(shown({"-Wweavec-allocation-failure", "-Wno-weavec"}),
            std::nullopt);
  EXPECT_EQ(shown({"-Wno-weavec", "-Wweavec-allocation-failure"}),
            Severity::Warning);
  // Every other id is on by default.
  const DiagnosticControl defaults;
  for (const std::string_view id : core::diag::All)
    EXPECT_EQ(defaults.isEnabled(id), id != AllocationFailure) << id;
}

TEST(DiagnosticControl, NewIdsFollowTheirSeverities) {
  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-weavec-unanalyzed-input", error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(control.apply(
      make(core::diag::UnanalyzedInput, Severity::Warning, "link input")));
  EXPECT_FALSE(control.isEnabled(core::diag::UnanalyzedInput));
  ASSERT_TRUE(control.parse("-Werror=weavec-unanalyzed-input", error));
  EXPECT_EQ(control
                .apply(make(core::diag::UnanalyzedInput, Severity::Warning,
                            "link input"))
                ->severity,
            Severity::Error);
}

TEST(DiagnosticControl, RejectsRemovedIds) {
  // RFC 0030, *Diagnostics*: no compatibility alias is kept.
  for (const char *flag :
       {"-Wno-weavec-checking-incomplete", "-Wweavec-checking-failed",
        "-Werror=weavec-checking-failed",
        "-Wno-error=weavec-checking-incomplete"}) {
    DiagnosticControl control;
    std::string error;
    ASSERT_TRUE(control.parse(flag, error));
    const llvm::StringRef id =
        llvm::StringRef(flag).substr(llvm::StringRef(flag).find("checking-"));
    EXPECT_EQ(error, "unknown WeaveC diagnostic '" + id.str() +
                         "' (removed by RFC 0030)");
    EXPECT_EQ(control, DiagnosticControl{});
  }
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
  using core::diag::defaultSeverity;
  const auto always = [](std::string_view id, Severity severity) {
    EXPECT_EQ(defaultSeverity(id, Certainty::Definite), severity) << id;
    EXPECT_EQ(defaultSeverity(id, Certainty::Possible), severity) << id;
  };
  // RFC 0030, *Diagnostics*: definite errors and possible warnings.
  for (const std::string_view id :
       {UseAfterFree, core::diag::DoubleFree, core::diag::UseAfterMove,
        core::diag::ConflictingBorrow, core::diag::LifetimeTooShort,
        core::diag::MismatchedRelease, core::diag::InvalidRelease}) {
    EXPECT_EQ(defaultSeverity(id, Certainty::Definite), Severity::Error) << id;
    EXPECT_EQ(defaultSeverity(id, Certainty::Possible), Severity::Warning)
        << id;
  }
  // Errors whatever the certainty; the first three are only ever definite.
  for (const std::string_view id :
       {core::diag::NullDereference, core::diag::UseOfUninitialized,
        core::diag::OutOfBounds, core::diag::UnsafeOperation,
        core::diag::AnnotationMismatch, core::diag::InvalidIntegerOperation,
        core::diag::ContradictedAssumption, core::diag::UnresolvedOperation,
        core::diag::UncheckedOperation})
    always(id, Severity::Error);
  // Warnings, never errors (RFC 0007: a leak; RFC 0030 §13.2: a link input).
  for (const std::string_view id :
       {core::diag::Leak, core::diag::InvalidAnnotation, AllocationFailure,
        core::diag::UnanalyzedInput, AnnotationRequired,
        core::diag::AnalysisIncomplete})
    always(id, Severity::Warning);
  always("no-such-id", Severity::Error);

  EXPECT_FALSE(core::diag::isEnabledByDefault(AllocationFailure));
  for (const std::string_view id : core::diag::All)
    EXPECT_EQ(core::diag::isEnabledByDefault(id), id != AllocationFailure)
        << id;

  EXPECT_TRUE(core::diag::isKnown("lifetime-too-short"));
  EXPECT_FALSE(core::diag::isKnown("lifetime"));
  for (const char *id :
       {"leak", "mismatched-release", "null-dereference",
        "use-of-uninitialized", "invalid-release", "out-of-bounds",
        "contradicted-assumption", "allocation-failure", "unresolved-operation",
        "unchecked-operation", "unanalyzed-input"})
    EXPECT_TRUE(core::diag::isKnown(id)) << id;
  // The 20 ids of RFC 0030, and the two S3-B retires with the engine paths
  // that emit them.
  EXPECT_EQ(core::diag::All.size(), 22U);
}

TEST(DiagnosticIds, RemovedIdsAreRefused) {
  for (const std::string_view id : core::diag::Removed) {
    EXPECT_TRUE(core::diag::isRemoved(id)) << id;
    DiagnosticControl control;
    std::string error;
    ASSERT_TRUE(control.parse("-Wno-weavec-" + std::string(id), error));
    EXPECT_EQ(error, "unknown WeaveC diagnostic '" + std::string(id) +
                         "' (removed by RFC 0030)");
  }
  EXPECT_TRUE(core::diag::isRemoved("checking-incomplete"));
  EXPECT_TRUE(core::diag::isRemoved("checking-failed"));
  EXPECT_FALSE(core::diag::isRemoved("use-after-free"));
}

} // namespace weavec::frontend
