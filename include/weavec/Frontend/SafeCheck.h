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
// RFC 0049: every error is also recorded as data (`SafeUnit::errors`), with
// the fixes the compiler can propose from the function it is compiling.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_SAFECHECK_H
#define WEAVEC_FRONTEND_SAFECHECK_H

#include "weavec/Frontend/RuleFix.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

#include <cstdint>
#include <map>
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
/// RFC 0046 §8: the function may hand the callable to another thread.
inline constexpr llvm::StringLiteral SendsAnnotation = "weavec.sends";
/// RFC 0047 §5: objects of the class are changed through pointers other
/// objects keep.
inline constexpr llvm::StringLiteral AliasedAnnotation = "weavec.aliased";
/// RFC 0048 §5.1: the checker's implicit mark on a local the passes place
/// in typed memory, followed by a space and the local's type.
inline constexpr llvm::StringLiteral PlacedAnnotation = "weavec.placed";
/// RFC 0047 §6: WEAVEC_SAFE_FILE is `_Pragma("weavec safe")`: the pragma's
/// namespace and its one word.
inline constexpr llvm::StringLiteral SafeFilePragmaNamespace = "weavec";
inline constexpr llvm::StringLiteral SafeFilePragmaWord = "safe";
/// RFC 0043 §2: the one pointer responsible for releasing its block.
inline constexpr llvm::StringLiteral OwnedAnnotation = "weavec.owned";
/// RFC 0043 amendment: the function leaves every owner below the parameter
/// null (a destructor of a struct the caller keeps).
inline constexpr llvm::StringLiteral DropsAnnotation = "weavec.drops";
/// RFC 0054 §3: a union in a struct whose members are used under a
/// comparison of the struct's field the annotation names (and, with a
/// second argument, a mask); the values of that field a member is for.
inline constexpr llvm::StringLiteral TaggedByAnnotation = "weavec.tagged_by";
inline constexpr llvm::StringLiteral WhenAnnotation = "weavec.when";
/// RFC 0054 §1.2: the checker's implicit mark on a local of a safe function
/// whose address leaves it, recorded for its scope; its one argument is the
/// index of the local's type in SafeUnit::recordedLocals.
inline constexpr llvm::StringLiteral RecordedAnnotation = "weavec.recorded";
/// RFC 0054 §2.3: the markers the checker puts in a safe function's code,
/// each `ptr <marker>(ptr, i64 index)` returning its pointer: a checked
/// conversion (SafeUnit::casts), the struct of a tagged union's member
/// access (SafeUnit::taggedAccesses), and the struct whose tag is stored
/// (SafeUnit::tagStores). GuardInsert expands them first.
inline constexpr llvm::StringLiteral CastMarker = "__weavec.cast";
inline constexpr llvm::StringLiteral MemberMarker = "__weavec.member";
inline constexpr llvm::StringLiteral TagMarker = "__weavec.tag";
/// What a checked conversion gives, until the guard passes are done with
/// it: `ptr __weavec.object(ptr returned, i64 size)`, one object of the
/// target's size.
inline constexpr llvm::StringLiteral ObjectMarker = "__weavec.object";

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
  /// RFC 0047 §1.1: a member function's result counted by a data member of
  /// its object: the integer of `memberWidth` bytes at `memberOffset` from
  /// the first argument (`this`); -1 when the count is not a member.
  std::int64_t memberOffset = -1;
  std::uint64_t memberWidth = 0;

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
  /// RFC 0045 §1: a unit compiled as safe (`-fweavec-safe`) in the fast
  /// mode: its allocations are typed and its safe code's releases return
  /// typed blocks to their pools (§3); in C++ its believed accesses (§4)
  /// and its trusted library functions (§5) carry no guard.
  bool fast = false;
  /// A C++ unit (RFC 0044).
  bool cxx = false;
  /// RFC 0045 §4, §5 (C++): the symbols of the unit's safe functions (their
  /// lambdas and the implicit members of its safe classes), of the unit's
  /// other functions, and of the library functions its safe functions call
  /// that are trusted.
  llvm::StringSet<> cxxSafe;
  llvm::StringSet<> cxxOther;
  llvm::StringSet<> cxxTrusted;
  /// RFC 0045 §2 (fast mode): the names of the types the unit declares in
  /// a function, which an allocation's `!alloc_token` prints without the
  /// function (two units' `struct Node` of two functions print alike), and
  /// of those the names that more than one of the unit's types has: an
  /// allocation whose type's name mentions one of the first is keyed by
  /// its unit too, and one that mentions one of the second is not typed.
  llvm::StringSet<> localTypeNames;
  llvm::StringSet<> ambiguousTypeNames;
  /// RFC 0051 §2: the symbols of every safe function the unit emits (C or
  /// C++, either mode: its lambdas, its instantiations and the implicit
  /// members of its safe classes), whose code GuardDefine defines; and
  /// whether the unit was compiled with -fweavec-safe, whose options define
  /// the rest of its code too.
  llvm::StringSet<> definedSafe;
  bool defined = false;
  /// RFC 0051 §5: what the unit's safe code believes of the types,
  /// functions and globals it shares with other units (Definitions.cpp);
  /// empty for a unit with no safe function.
  std::string definitionRecord;
  /// RFC 0050 §2.3: where a checked library view keeps its pointer and its
  /// count (an integer member, or `constant` elements when it has none).
  struct ViewLayout {
    std::uint64_t pointerOffset = 0;
    std::int64_t countOffset = -1;
    std::uint64_t countWidth = 0;
    std::uint64_t constant = 0;
    std::uint64_t elementSize = 1;
  };
  /// RFC 0050 §2.3: the constructors of checked views that safe code calls
  /// with a pointer or an iterator, by symbol: each view they make is
  /// guarded once, as a range, where it is made.
  llvm::StringMap<ViewLayout> viewMakers;
  /// RFC 0050 §2.4: the hidden iterator's calls of the range-based `for`s of
  /// safe functions over an array, a library container or a checked view,
  /// as `<file>:<line>:<column>:<symbol>` (the file's name, the loop's
  /// colon), and the loops whose iterator is a pointer, as `<function's
  /// symbol>:<file>:<line>:<column>` (each instance of a template apart).
  llvm::StringSet<> rangeLoops;
  /// RFC 0050 §2.5: the C++ library's own checks are on (libc++'s
  /// hardening mode is `fast`, `extensive` or `debug`, or libstdc++'s
  /// assertions are defined):
  /// without them no library function is trusted.
  bool hardened = true;
  /// RFC 0042 §5: the guard passes run, which read the fields' annotations.
  bool annotates = false;
  /// RFC 0047 §7: `-fweavec-safe-survey`.
  bool survey = false;
  /// RFC 0047 §6: the files that say WEAVEC_SAFE_FILE (by the hash of
  /// their FileID), and the offset where they say it.
  std::map<unsigned, unsigned> safeFiles;
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
    /// RFC 0046 §12: thread starts, lock or guard objects used, and a
    /// WEAVEC_SENDS parameter.
    unsigned starts = 0;
    bool locks = false;
    bool sends = false;
    /// RFC 0051 §8: the signal handlers the function registers (the
    /// table's `handler` rows), what the audit lists of code that is not
    /// safe.
    unsigned handlers = 0;
    /// RFC 0054 §4: a function that is not safe whose body only makes a
    /// conversion safe code now checks and calls (an adapter of a `void *`
    /// context): the audit lists it as one that can be safe.
    bool adapter = false;
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
    /// RFC 0046 §12: for `kind` `class`.
    bool sendable = false;
    bool shareable = false;
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
  /// RFC 0049 §1: every rule error, printed or (under the survey) not.
  std::vector<RuleError> errors;
  /// RFC 0049 §5: the declarations other units want facts about (by their
  /// keys), and what this unit's evidence says of those it sees.
  std::vector<std::string> wants;
  std::vector<RuleFix> evidence;
  /// RFC 0049 §6: the project's files the unit read.
  std::vector<ReadFile> files;
  /// FieldBounds::pointee's sizes (0: still incomplete), by index - 1.
  std::vector<std::uint64_t> pointeeSizes;

  /// RFC 0054 §1.1: a type's layout, by the name Clang's allocation tokens
  /// give the type (`clang::infer_alloc::getAllocTokenMetadata`).
  struct TypeLayout {
    /// The type's name, size and the offsets of its pointers, hashed.
    std::uint64_t identity = 0;
    std::uint64_t size = 0;
    /// 1: plain (no pointer, no `bool`); 2: a scalar.
    std::uint32_t flags = 0;
    /// As a report names it (`struct circle`).
    std::string display;
    struct Part {
      std::uint64_t identity = 0;
      std::uint32_t offset = 0;
      std::uint32_t stride = 0;
      std::uint32_t elements = 0;
      std::uint32_t flags = 0;
    };
    std::vector<Part> parts;
  };
  llvm::StringMap<TypeLayout> layouts;
  /// RFC 0054 §2: each checked conversion of the unit's safe code, by the
  /// index its marker carries.
  struct CheckedCast {
    /// The target's layout (a key of `layouts`).
    std::string type;
    /// The marker's pointer less `delta` is the target object's start (a
    /// base's offset in it).
    std::uint64_t delta = 0;
    /// A class with virtual functions: its vtable's type information is
    /// compared first (the mangled names of the two classes' type
    /// information, the target's and the source's).
    bool polymorphic = false;
    std::string derivedInfo;
    std::string baseInfo;
  };
  std::vector<CheckedCast> casts;
  /// RFC 0054 §3: each access of a tagged union's member in safe code.
  struct TaggedAccess {
    /// The tag's offset in its struct, its width in bytes, its mask.
    std::uint64_t tagOffset = 0;
    unsigned tagWidth = 0;
    std::uint64_t mask = ~std::uint64_t{0};
    std::vector<std::uint64_t> values;
    std::string member;
    std::string tag;
    bool write = false;
  };
  std::vector<TaggedAccess> taggedAccesses;
  /// RFC 0054 §3.3: each store of a tag in safe code, which clears the
  /// union when the value changes.
  struct TagStore {
    std::uint64_t tagOffset = 0;
    unsigned tagWidth = 0;
    std::uint64_t mask = ~std::uint64_t{0};
    std::uint64_t unionOffset = 0;
    std::uint64_t unionSize = 0;
  };
  std::vector<TagStore> tagStores;
  /// RFC 0054 §1.2: the layouts of the unit's globals, by symbol, and of
  /// its recorded locals, by their mark's index.
  llvm::StringMap<std::string> globalTypes;
  std::vector<std::string> recordedLocals;

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

/// RFC 0047 §6: registers the handler of `#pragma weavec safe`, which
/// WEAVEC_SAFE_FILE expands to, on the compiler's preprocessor.
void addSafeFilePragma(const clang::CompilerInstance &compiler,
                       std::shared_ptr<SafeUnit> unit);

/// RFC 0043 §12: `weavec-cc --weavec-explain [<rule>... | all]`: the
/// rules' explanations (resources/safe/rules.md); alone, their list.
int runExplain(llvm::ArrayRef<const char *> operands);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_SAFECHECK_H
