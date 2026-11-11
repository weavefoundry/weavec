//===- SafeCheck.h - Safe C's rules and its bounds table --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0041. The checker runs over the AST of a unit before code generation.
// It finds the safe functions (`WEAVEC_SAFE`, or every function the unit
// defines outside system headers under `-fweavec-safe`), checks each against
// Safe C's rules (§3), and reports a broken rule as a Clang error that names
// the rule (`[weavec-safe:<id>]`) with a note giving the accepted form. It
// also fills a table that the guard passes read: the safe functions, and the
// bounds that every function's declaration gives its pointer parameters and
// result (§2), whether a parameter escapes or is `lifetimebound` (§5).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_SAFECHECK_H
#define WEAVEC_FRONTEND_SAFECHECK_H

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace clang {
class ASTConsumer;
class CompilerInstance;
} // namespace clang

namespace weavec::frontend {

/// The annotations of `resources/include/weavec.h` (§1, §2, §5), which spells
/// the same.
inline constexpr llvm::StringLiteral SafeAnnotation = "weavec.safe";
inline constexpr llvm::StringLiteral GuardedAnnotation = "weavec.guarded";
inline constexpr llvm::StringLiteral CountedByAnnotation = "weavec.counted_by";
inline constexpr llvm::StringLiteral CountedByOrNullAnnotation =
    "weavec.counted_by_or_null";
inline constexpr llvm::StringLiteral SizedByAnnotation = "weavec.sized_by";
inline constexpr llvm::StringLiteral SizedByOrNullAnnotation =
    "weavec.sized_by_or_null";
inline constexpr llvm::StringLiteral EndedByAnnotation = "weavec.ended_by";
inline constexpr llvm::StringLiteral NullTerminatedAnnotation =
    "weavec.null_terminated";
inline constexpr llvm::StringLiteral NullTerminatedOrNullAnnotation =
    "weavec.null_terminated_or_null";
inline constexpr llvm::StringLiteral EscapesAnnotation = "weavec.escapes";
inline constexpr llvm::StringLiteral BorrowedAnnotation = "weavec.borrowed";

/// What a declaration says about one pointer (§2).
struct PointerBounds {
  enum class Kind : std::uint8_t {
    /// Not a pointer, or a pointer the table says nothing about.
    None,
    /// One object (no annotation).
    Single,
    /// `count` elements of `elementSize` bytes.
    Counted,
    /// Up to the pointer `end`.
    Ended,
    /// Up to and with its NUL.
    NullTerminated,
  };
  Kind kind = Kind::None;
  /// Counted: the parameter that holds the count, or -1 for `constant`.
  int count = -1;
  /// Counted: the count's type is signed (a negative count is empty).
  bool countSigned = false;
  std::uint64_t constant = 0;
  /// Counted: the bytes of one element (1 for `WEAVEC_SIZED_BY`).
  std::uint64_t elementSize = 1;
  /// Ended: the parameter that is the end.
  int end = -1;
  /// The pointer may be null whatever its count (`_OR_NULL`).
  bool orNull = false;
  /// A parameter: `WEAVEC_ESCAPES`.
  bool escapes = false;
  /// A parameter: `lifetimebound`.
  bool lifetimeBound = false;

  [[nodiscard]] bool bounded() const {
    return kind == Kind::Counted || kind == Kind::Ended ||
           kind == Kind::NullTerminated;
  }
  bool operator==(const PointerBounds &) const = default;
};

/// What a function's declarations say, and whether its definition is safe.
struct FunctionBounds {
  std::vector<PointerBounds> parameters;
  PointerBounds result;
  bool safe = false;

  /// Whether `other` gives the same bounds (whatever it says of safety).
  [[nodiscard]] bool sameBounds(const FunctionBounds &other) const {
    return parameters == other.parameters && result == other.result;
  }
};

/// The unit's table (§8), filled at the end of the unit, before the guard
/// passes run.
struct SafeUnit {
  /// By the function's symbol.
  llvm::StringMap<FunctionBounds> functions;
  /// `-fweavec-safe-checks=full` (§7): safe functions keep their guards.
  bool full = false;

  /// Whether `name` is a safe function defined in the unit.
  [[nodiscard]] bool isSafe(llvm::StringRef name) const {
    const auto found = functions.find(name);
    return found != functions.end() && found->second.safe;
  }
  /// The table's entry for `name`, or null.
  [[nodiscard]] const FunctionBounds *find(llvm::StringRef name) const {
    const auto found = functions.find(name);
    return found == functions.end() ? nullptr : &found->second;
  }
};

/// The checker: a consumer to run before code generation. With `unitSafe`
/// (`-fweavec-safe`) every function the unit defines outside system headers
/// is safe unless marked `WEAVEC_GUARDED` or unsafe.
[[nodiscard]] std::unique_ptr<clang::ASTConsumer>
createSafeChecker(clang::CompilerInstance &compiler,
                  std::shared_ptr<SafeUnit> out, bool unitSafe);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_SAFECHECK_H
