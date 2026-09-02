//===- Diagnostic.h - Frontend-neutral diagnostics -------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The core model reports problems as `Diagnostic` values through a
// `DiagnosticSink`. The frontend layer forwards them to Clang's diagnostics
// engine; tests use `DiagnosticCollector` to inspect them directly.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_DIAGNOSTIC_H
#define WEAVEC_CORE_DIAGNOSTIC_H

#include "weavec/Core/SourceLocation.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

enum class Severity : std::uint8_t {
  Note,
  Warning,
  Error,
};

[[nodiscard]] std::string_view toString(Severity severity) noexcept;

/// Stable identifiers for every diagnostic WeaveC can emit. These are part of
/// the user-facing contract (they appear in output and can be used to filter),
/// so treat renames as breaking changes.
namespace diag {
inline constexpr std::string_view UseAfterFree = "use-after-free";
inline constexpr std::string_view DoubleFree = "double-free";
inline constexpr std::string_view UseAfterMove = "use-after-move";
inline constexpr std::string_view ConflictingBorrow = "conflicting-borrow";
inline constexpr std::string_view LifetimeTooShort = "lifetime-too-short";
inline constexpr std::string_view UnsafeOperation = "unsafe-operation";
inline constexpr std::string_view AnnotationRequired = "annotation-required";
inline constexpr std::string_view AnnotationMismatch = "annotation-mismatch";
inline constexpr std::string_view InvalidAnnotation = "invalid-annotation";
} // namespace diag

/// A suggested source edit: insert `insertion` at `location`. Frontends
/// render it as their native fix-it (Clang's `FixItHint`).
struct FixItHint {
  SourceLocation location;
  std::string insertion;

  friend bool operator==(const FixItHint &, const FixItHint &) = default;
};

/// A single diagnostic, optionally accompanied by explanatory notes.
struct Diagnostic {
  Severity severity = Severity::Error;
  /// One of the identifiers in `weavec::core::diag`.
  std::string_view id;
  std::string message;
  SourceLocation location;
  std::vector<Diagnostic> notes;
  std::vector<FixItHint> fixits;

  /// Fluent helper for attaching a note.
  Diagnostic &addNote(std::string noteMessage, SourceLocation noteLocation);
  /// Fluent helper for attaching an insertion fix-it.
  Diagnostic &addFixIt(SourceLocation at, std::string insertion);
};

/// Receives diagnostics produced by the analyses.
class DiagnosticSink {
public:
  virtual ~DiagnosticSink() = default;
  virtual void report(const Diagnostic &diagnostic) = 0;

protected:
  DiagnosticSink() = default;
  DiagnosticSink(const DiagnosticSink &) = default;
  DiagnosticSink(DiagnosticSink &&) = default;
  DiagnosticSink &operator=(const DiagnosticSink &) = default;
  DiagnosticSink &operator=(DiagnosticSink &&) = default;
};

/// A sink that stores diagnostics in memory; useful for tests and tooling.
class DiagnosticCollector final : public DiagnosticSink {
public:
  void report(const Diagnostic &diagnostic) override;

  [[nodiscard]] const std::vector<Diagnostic> &diagnostics() const noexcept {
    return items;
  }
  [[nodiscard]] bool empty() const noexcept { return items.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return items.size(); }
  [[nodiscard]] std::size_t count(Severity severity) const noexcept;
  [[nodiscard]] bool hasErrors() const noexcept {
    return count(Severity::Error) != 0;
  }
  void clear() noexcept { items.clear(); }

private:
  std::vector<Diagnostic> items;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_DIAGNOSTIC_H
