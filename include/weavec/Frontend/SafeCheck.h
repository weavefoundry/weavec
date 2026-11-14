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
// RFC 0042: the checker adds typed memory's rules (§3) to safe functions,
// and gives each pointer field of a C unit's structs an implicit
// `annotate("weavec.field", ...)` attribute, and each field that counts one
// an `annotate("weavec.count", ...)`, which Clang emits at every access to
// the field (`llvm.ptr.annotation`), so that the guard passes know a loaded
// pointer's bound (§5).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_SAFECHECK_H
#define WEAVEC_FRONTEND_SAFECHECK_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>
#include <string>
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
/// RFC 0043 §2: the one pointer responsible for releasing its block.
inline constexpr llvm::StringLiteral OwnedAnnotation = "weavec.owned";
/// RFC 0043 amendment: the function leaves every owner below the parameter
/// null (a destructor of a struct the caller keeps).
inline constexpr llvm::StringLiteral DropsAnnotation = "weavec.drops";
/// RFC 0042 §5: the compiler's own field annotations (not spellings a
/// program writes).
inline constexpr llvm::StringLiteral FieldAnnotation = "weavec.field";
inline constexpr llvm::StringLiteral CountAnnotation = "weavec.count";

/// What a `weavec.field` annotation says of a pointer field, in its six
/// integer arguments, in order.
struct FieldBounds {
  enum class Kind : std::uint8_t {
    /// One object of `elementSize` bytes (0: a size the unit does not know).
    Single = 1,
    /// `count` elements of `elementSize` bytes; the count is the integer of
    /// `countWidth` bytes at `countOffset` from the field, or, when
    /// `countWidth` is 0, `countOffset` itself.
    Counted = 2,
    /// A string.
    NullTerminated = 3,
  };
  static constexpr std::uint64_t OrNull = 1;
  static constexpr std::uint64_t CountSigned = 2;
  /// The field is an array of such pointers.
  static constexpr std::uint64_t Array = 4;
  /// RFC 0043 §7: the field is `WEAVEC_OWNED`; one that is neither this nor
  /// `Borrowed` is a reference, tested when a safe function loads it.
  static constexpr std::uint64_t Owner = 8;
  static constexpr std::uint64_t Borrowed = 16;

  Kind kind = Kind::Single;
  std::int64_t elementSize = 0;
  std::int64_t countOffset = 0;
  std::int64_t countWidth = 0;
  std::uint64_t flags = 0;
  /// A single pointer to a type incomplete where the field was declared: an
  /// index into SafeUnit::pointeeSizes, whose size the end of the unit gives.
  std::int64_t pointee = 0;

  bool operator==(const FieldBounds &) const = default;
};

/// What a `weavec.count` annotation says of a field that counts a pointer
/// field, in its three integer arguments: the pointer field's offset from
/// the count, the pointer's element size, and FieldBounds' flags.
struct CountBounds {
  std::int64_t pointerOffset = 0;
  std::int64_t elementSize = 1;
  std::uint64_t flags = 0;
  bool operator==(const CountBounds &) const = default;
};

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
  /// Single: the pointee's size, 0 when the unit does not
  /// know it (`void`, an incomplete type, a function).
  std::uint64_t pointeeSize = 0;
  /// Ended: the parameter that is the end.
  int end = -1;
  /// The pointer may be null whatever its count (`_OR_NULL`).
  bool orNull = false;
  /// A parameter: `WEAVEC_ESCAPES`.
  bool escapes = false;
  /// A parameter: `lifetimebound`.
  bool lifetimeBound = false;
  /// RFC 0043 §2: `WEAVEC_OWNED` (a parameter that takes its block, a
  /// result that gives one).
  bool owned = false;

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
  /// RFC 0042 §6: a declaration carries `WEAVEC_SAFE`, so the function is
  /// defined safe and its body is external.
  bool declaredTyped = false;

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
  /// RFC 0042 §5: the guard passes run, which read the fields' annotations.
  bool annotates = false;
  /// RFC 0042 §10: each function the unit defines outside system headers,
  /// its tier (`safe`, `guarded`, `unsafe`; RFC 0043 amendment 17:
  /// `unsettled`), file and line.
  struct Definition {
    std::string name;
    std::string tier;
    std::string file;
    unsigned line = 0;
    /// RFC 0043 §11: a parameter or the result is `WEAVEC_OWNED`.
    bool owns = false;
  };
  std::vector<Definition> definitions;
  /// RFC 0043 §11: each pointer field of the structs the unit defines
  /// outside system headers, and its kind: `owner`, `reference` or
  /// `borrowed`.
  struct Field {
    std::string record;
    std::string field;
    std::string kind;
    std::string file;
    unsigned line = 0;
  };
  std::vector<Field> fields;
  /// RFC 0043 §9: a global owner that a safe function reads in place.
  struct InPlace {
    std::string function;
    std::string global;
    std::string file;
    unsigned line = 0;
  };
  std::vector<InPlace> inPlace;
  /// FieldBounds::pointee's sizes (0: still incomplete), by index - 1.
  std::vector<std::uint64_t> pointeeSizes;

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
/// is safe unless marked `WEAVEC_GUARDED` or unsafe (RFC 0041, RFC 0042,
/// RFC 0043).
[[nodiscard]] std::unique_ptr<clang::ASTConsumer>
createSafeChecker(clang::CompilerInstance &compiler,
                  std::shared_ptr<SafeUnit> out, bool unitSafe);

/// RFC 0043 §12: `weavec-cc --weavec-explain [<rule>... | all]`: the
/// rules' explanations (resources/safe/rules.md); alone, their list.
int runExplain(llvm::ArrayRef<const char *> operands);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_SAFECHECK_H
