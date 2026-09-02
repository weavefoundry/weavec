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
//   -Wweavec-<id>          re-enable a warning
//   -Wno-weavec-<id>       disable a diagnostic that is a warning by default
//   -Werror=weavec-<id>    make it an error
//   -Wno-error=weavec-<id> make an error a warning
//   -Werror=weavec / -Wno-error=weavec / -Wweavec / -Wno-weavec
//                          the same for every WeaveC id
//
// An error cannot be disabled outright; it can be lowered to a warning.
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
  /// unknown or the request is not allowed (`-Wno-weavec-use-after-free`),
  /// the flag is still consumed and `error` explains the problem.
  bool parse(llvm::StringRef flag, std::string &error);

  /// True if `flag` looks like one of WeaveC's `-W` spellings.
  [[nodiscard]] static bool isWeaveCFlag(llvm::StringRef flag);

  /// The diagnostic as it should be emitted, or `nullopt` to drop it.
  [[nodiscard]] std::optional<core::Diagnostic>
  apply(const core::Diagnostic &diagnostic) const;

  [[nodiscard]] Level levelFor(std::string_view id) const;

  friend bool operator==(const DiagnosticControl &,
                         const DiagnosticControl &) = default;

private:
  Level all = Level::Default;
  std::map<std::string, Level, std::less<>> perId;
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
/// With `boundaryOnce`, an `annotation-required` whose message is already in
/// the set is dropped too: a boundary is reported once per program (RFC
/// 0005), not once per unit that calls it.
class FilteringSink final : public core::DiagnosticSink {
public:
  FilteringSink(core::DiagnosticSink &next, DiagnosticControl control,
                const std::set<ReportedDiagnostic> *alreadyReported = nullptr,
                std::set<std::string> *boundaryOnce = nullptr)
      : downstream(next), table(std::move(control)), skip(alreadyReported),
        once(boundaryOnce) {}

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
  std::set<std::string> *once;
  std::set<ReportedDiagnostic> forwarded;
  std::size_t errorCount = 0;
  std::size_t warningCount = 0;
};

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DIAGNOSTICCONTROL_H
