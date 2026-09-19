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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

enum class Severity : std::uint8_t {
  Note,
  Warning,
  Error,
};

[[nodiscard]] std::string_view toString(Severity severity) noexcept;

/// RFC 0030 §3: every diagnostic is definite (a violation on every path the
/// analysis considers) or possible. The default severity of most ids
/// depends on it (`diag::defaultSeverity`). Its JSON spelling and parser are
/// in `Ledger.h`.
enum class Certainty : std::uint8_t { Definite, Possible };

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
inline constexpr std::string_view AnnotationMismatch = "annotation-mismatch";
inline constexpr std::string_view InvalidAnnotation = "invalid-annotation";
/// RFC 0007: an owned resource whose every holder went out of reach without
/// it being released, moved, stored, returned or handed to unknown code.
inline constexpr std::string_view Leak = "leak";
/// RFC 0007: a resource released (or moved into a consuming parameter) by a
/// function of a different release family than the one that produced it.
inline constexpr std::string_view MismatchedRelease = "mismatched-release";
/// RFC 0008: a dereference of a pointer that is null; or such a pointer
/// passed to a callee that dereferences its parameter. RFC 0030 §3.2: only
/// definite; a pointer that may be null gets a checked null facet instead.
inline constexpr std::string_view NullDereference = "null-dereference";
/// RFC 0008: a read of a pointer variable or a pointer field of a local
/// record before any assignment reaches it.
inline constexpr std::string_view UseOfUninitialized = "use-of-uninitialized";
/// RFC 0008: a release (or a move into an owning parameter) of a value that
/// is not the start of a heap allocation: borrowed storage, a string
/// literal, an interior pointer.
inline constexpr std::string_view InvalidRelease = "invalid-release";
/// RFC 0011: an access through a pointer, or a library call over a buffer,
/// that reaches past the extent of the object the pointer points into on
/// every value the facts allow. RFC 0030 §3.3: only definite, against an
/// exact extent; the `may` forms are checked facets.
inline constexpr std::string_view OutOfBounds = "out-of-bounds";
/// RFC 0017: a definitely invalid operation in the target integer type.
inline constexpr std::string_view InvalidIntegerOperation =
    "invalid-integer-operation";
/// RFC 0030 §6.2: a `WEAVEC_ASSUME(e)` the analysis refutes.
inline constexpr std::string_view ContradictedAssumption =
    "contradicted-assumption";
/// RFC 0030 §3.2, §8.4: an allocation's result used without a null test.
/// The only id that is off by default (`-Wweavec-allocation-failure`).
inline constexpr std::string_view AllocationFailure = "allocation-failure";
/// RFC 0030 §6.3: under `-fweavec-require=checked|proven` or
/// `WEAVEC_REQUIRE_SAFE`, a facet that is neither proven nor checkable.
inline constexpr std::string_view UnresolvedOperation = "unresolved-operation";
/// RFC 0030 §6.3: under `-fweavec-require=proven`, a facet that relies on a
/// runtime check.
inline constexpr std::string_view UncheckedOperation = "unchecked-operation";
/// RFC 0030 §13.2: the link inputs without a valid WeaveC record, named in
/// one warning per link.
inline constexpr std::string_view UnanalyzedInput = "unanalyzed-input";

/// Every id, for validating user input (`-Wweavec-<id>`).
inline constexpr std::array All{
    UseAfterFree,           DoubleFree,        UseAfterMove,
    ConflictingBorrow,      LifetimeTooShort,  UnsafeOperation,
    AnnotationMismatch,     InvalidAnnotation, Leak,
    MismatchedRelease,      NullDereference,   UseOfUninitialized,
    InvalidRelease,         OutOfBounds,       InvalidIntegerOperation,
    ContradictedAssumption, AllocationFailure, UnresolvedOperation,
    UncheckedOperation,     UnanalyzedInput,
};

[[nodiscard]] constexpr bool isKnown(std::string_view id) noexcept {
  return std::ranges::any_of(
      All, [id](const std::string_view known) { return known == id; });
}

/// Ids RFC 0030 removed (*Diagnostics*). A `-W` flag naming one is an error,
/// `unknown WeaveC diagnostic '<id>' (removed by RFC 0030)`; no alias is
/// kept. `analysis-incomplete` became `unresolved(unanalysed | budget | ...)`
/// ledger rows, `annotation-required` became `unresolved(unknown-callee)`
/// rows with fix-its (§5.1).
inline constexpr std::array Removed{
    std::string_view("analysis-incomplete"),
    std::string_view("annotation-required"),
    std::string_view("checking-incomplete"),
    std::string_view("checking-failed"),
};

[[nodiscard]] constexpr bool isRemoved(std::string_view id) noexcept {
  return std::ranges::any_of(
      Removed, [id](const std::string_view removed) { return removed == id; });
}

/// The severity `id` has unless the user overrides it (RFC 0030,
/// *Diagnostics*): an error when definite and a warning when possible,
/// except that
///   - `leak`, `invalid-annotation`, `allocation-failure` and
///     `unanalyzed-input` are always warnings, and
///   - `null-dereference`, `use-of-uninitialized` and `out-of-bounds`
///     (reported only when definite), `unsafe-operation`,
///     `annotation-mismatch`, `invalid-integer-operation`,
///     `contradicted-assumption`, `unresolved-operation` and
///     `unchecked-operation` are always errors, as is an unknown id.
[[nodiscard]] constexpr Severity defaultSeverity(std::string_view id,
                                                 Certainty certainty) noexcept {
  constexpr std::array AlwaysWarnings{
      Leak,
      InvalidAnnotation,
      AllocationFailure,
      UnanalyzedInput,
  };
  constexpr std::array ByCertainty{
      UseAfterFree,     DoubleFree,        UseAfterMove,   ConflictingBorrow,
      LifetimeTooShort, MismatchedRelease, InvalidRelease,
  };
  if (std::ranges::find(AlwaysWarnings, id) != AlwaysWarnings.end())
    return Severity::Warning;
  if (certainty == Certainty::Possible &&
      std::ranges::find(ByCertainty, id) != ByCertainty.end())
    return Severity::Warning;
  return Severity::Error;
}

/// False only for `allocation-failure`, which is reported only under
/// `-Wweavec-allocation-failure` (or `-Wweavec`).
[[nodiscard]] constexpr bool isEnabledByDefault(std::string_view id) noexcept {
  return id != AllocationFailure;
}
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
  /// RFC 0030 §3. With `id`, it decides the default severity and whether
  /// `-Wno-weavec[-<id>]` can drop the diagnostic (`diag::defaultSeverity`).
  Certainty certainty = Certainty::Definite;
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

  [[nodiscard]] const std::vector<Diagnostic> &diagnostics() const & noexcept {
    return items;
  }
  [[nodiscard]] std::vector<Diagnostic> diagnostics() && noexcept {
    return std::move(items);
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
