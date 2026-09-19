//===- DiagnosticControl.h - -W flags for WeaveC diagnostics ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compiler-style control over WeaveC's diagnostics (RFC 0005, *Flags*):
//
//   -Wweavec-<id>          re-enable a warning, or enable an id that is off
//                          by default (RFC 0030: `allocation-failure`)
//   -Wno-weavec-<id>       disable the diagnostics of `id` whose default
//                          severity is a warning
//   -Werror=weavec-<id>    make it an error (enabling an off-by-default id)
//   -Wno-error=weavec-<id> make an error a warning
//   -Werror=weavec / -Wno-error=weavec / -Wweavec / -Wno-weavec
//                          the same for every WeaveC id; of these only
//                          -Wweavec enables the off-by-default ids
//
// An error cannot be disabled outright; it can be lowered to a warning.
// Under RFC 0030 the default severity depends on the diagnostic's certainty
// (`core::diag::defaultSeverity`): `-Wno-weavec-use-after-free` drops the
// possible (warning) findings and leaves the definite errors, and is
// refused only for an id that is always an error. A flag naming an id RFC
// 0030 removed is refused with `(removed by RFC 0030)`.
//
// Also here: the key under which an emitted diagnostic is remembered so the
// driver's link step does not print what the compile step already did.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DIAGNOSTICCONTROL_H
#define WEAVEC_FRONTEND_DIAGNOSTICCONTROL_H

#include "weavec/Core/Diagnostic.h"

#include "llvm/ADT/StringRef.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace weavec::frontend {

/// Severity overrides for WeaveC diagnostics.
class DiagnosticControl {
public:
  enum class Level : std::uint8_t {
    Default,
    Off,
    Warning,
    Error,
  };

  /// True if `flag` is one of the spellings above. When it is but the id is
  /// unknown or removed, or the request is not allowed
  /// (`-Wno-weavec-null-dereference`), the flag is still consumed and
  /// `error` explains the problem.
  bool parse(llvm::StringRef flag, std::string &error);

  /// True if `flag` looks like one of WeaveC's `-W` spellings.
  [[nodiscard]] static bool isWeaveCFlag(llvm::StringRef flag);

  /// The diagnostic as it should be emitted, or `nullopt` to drop it.
  [[nodiscard]] std::optional<core::Diagnostic>
  apply(const core::Diagnostic &diagnostic) const;

  [[nodiscard]] Level levelFor(std::string_view id) const;

  /// Whether a diagnostic with `id` can be shown at all: false for an id
  /// that is off by default and no flag enabled, and for an id whose every
  /// form is a warning that `-Wno-weavec[-<id>]` disabled. An analysis may
  /// skip the work behind such an id.
  [[nodiscard]] bool isEnabled(std::string_view id) const;

  friend bool operator==(const DiagnosticControl &,
                         const DiagnosticControl &) = default;

private:
  /// Whether the flags turned `id` on, for an id that is off by default.
  [[nodiscard]] bool enabledByFlags(std::string_view id) const;

  Level all = Level::Default;
  std::map<std::string, Level, std::less<>> perId;
  /// The off-by-default ids: `-Wweavec` turns them all on and `-Wno-weavec`
  /// all off, forgetting earlier per-id flags as they do for the levels;
  /// `-Wweavec-<id>`, `-Werror=weavec-<id>` and `-Wno-weavec-<id>` decide
  /// the one they name.
  bool enableAll = false;
  std::map<std::string, bool, std::less<>> enabledIds;
};

/// Where a diagnostic was emitted, for deduplication between the compile
/// step and the link step of `weavec-cc` (RFC 0005).
struct ReportedDiagnostic {
  std::string id;
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;

  [[nodiscard]] static ReportedDiagnostic of(const core::Diagnostic &d) {
    return ReportedDiagnostic{.id = std::string(d.id),
                              .file = d.location.file,
                              .line = d.location.line,
                              .column = d.location.column};
  }

  friend bool operator==(const ReportedDiagnostic &,
                         const ReportedDiagnostic &) = default;
  friend std::strong_ordering operator<=>(const ReportedDiagnostic &,
                                          const ReportedDiagnostic &) = default;
};

/// A sink that applies a `DiagnosticControl`, drops diagnostics already in
/// `alreadyReported`, remembers what it forwarded, and forwards the rest.
/// With `onlyIds`, every diagnostic
/// whose id is not in the set is dropped (RFC 0012: a unit analysed once
/// more for its sized fields shows only what they can change).
class FilteringSink final : public core::DiagnosticSink {
public:
  FilteringSink(core::DiagnosticSink &next, DiagnosticControl control,
                const std::set<ReportedDiagnostic> *alreadyReported = nullptr,
                const std::set<std::string_view> *onlyIds = nullptr)
      : downstream(next), table(std::move(control)), skip(alreadyReported),
        only(onlyIds) {}

  void report(const core::Diagnostic &diagnostic) override;

  /// Keys of every diagnostic forwarded so far.
  [[nodiscard]] const std::set<ReportedDiagnostic> &reported() const noexcept {
    return forwarded;
  }
  [[nodiscard]] std::size_t errors() const noexcept { return errorCount; }
  [[nodiscard]] std::size_t warnings() const noexcept { return warningCount; }

private:
  core::DiagnosticSink &downstream;
  DiagnosticControl table;
  const std::set<ReportedDiagnostic> *skip;
  const std::set<std::string_view> *only;
  std::set<ReportedDiagnostic> forwarded;
  std::size_t errorCount = 0;
  std::size_t warningCount = 0;
};

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DIAGNOSTICCONTROL_H
