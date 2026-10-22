//===- LibrarySpec.h - The C library table of the guards --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0036 §6: one declarative table, `lib/Core/LibrarySpec.txt`, states what
// the guards need of each C library, POSIX and platform function: which
// pointer arguments the call reads or writes and how many bytes, whether
// null is allowed, which arguments must not overlap, how a format reads its
// arguments, the runtime's checked version that replaces the call, and the
// fortified aliases with their object-size argument.
//
// CMake embeds the text in the library; `LibrarySpec::shipped()` parses it
// once on first use. `LibrarySpec::parse` parses any text in the same syntax
// (tests). The syntax and its meaning are documented at the top of
// `LibrarySpec.txt`.
//
// Which calls a row governs: a direct call whose callee's name is the row's
// name, `__builtin_<name>`, or one of the row's `chk` aliases (with the
// alias's arguments remapped onto the row's), provided the unit does not
// define a function of that name (the guard pass's check).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_LIBRARYSPEC_H
#define WEAVEC_CORE_LIBRARYSPEC_H

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

/// A small expression over a call's arguments:
///
///   term ::= INT | 'a' N | 'strlen(a' N ')' | term '*' term
///          | term '+' term | term '-' INT | term '/' INT
///          | 'min(' term ',' term ')' | '(' term ')'
///
/// `*` and `/` bind tighter than `+` and `-`; all associate to the left.
/// `strlen(aN)` counts the elements of argument N's string before its
/// terminator; inside `min(t, …)` it is the bounded length `strnlen(aN, t)`
/// and requires no terminator within `t` elements. Terms are mathematical
/// integers.
struct LibTerm {
  enum class Kind : std::uint8_t {
    Constant,     ///< `value`
    Argument,     ///< `a<arg>`
    StringLength, ///< `strlen(a<arg>)`
    Product,      ///< `operands[0] * operands[1]`
    Sum,          ///< `operands[0] + operands[1]`
    Difference,   ///< `operands[0] - value`
    Min,          ///< `min(operands[0], operands[1])`
    Quotient,     ///< `operands[0] / value`, rounded down
  };
  Kind kind = Kind::Constant;
  std::int64_t value = 0;
  std::uint8_t arg = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibTerm> operands = {};

  [[nodiscard]] static LibTerm constant(std::int64_t value);
  [[nodiscard]] static LibTerm argument(unsigned index);
  [[nodiscard]] static LibTerm stringLength(unsigned index);
  [[nodiscard]] static LibTerm product(LibTerm left, LibTerm right);
  [[nodiscard]] static LibTerm sum(LibTerm left, LibTerm right);
  [[nodiscard]] static LibTerm difference(LibTerm left, std::int64_t right);
  [[nodiscard]] static LibTerm min(LibTerm left, LibTerm right);
  [[nodiscard]] static LibTerm quotient(LibTerm left, std::int64_t divisor);

  /// The canonical spelling, which `parse` reads back to an equal term.
  [[nodiscard]] std::string str() const;
  /// Parses a whole term; `std::nullopt` with `error` set otherwise.
  [[nodiscard]] static std::optional<LibTerm> parse(std::string_view text,
                                                    std::string &error);

  /// Known values for the leaves of a term; an empty function or a
  /// `std::nullopt` answer makes the leaf unknown.
  struct Values {
    std::function<std::optional<std::int64_t>(unsigned)> argument;
    std::function<std::optional<std::int64_t>(unsigned)> stringLength;
  };
  /// Evaluates the term, or `std::nullopt` when a leaf is unknown or the
  /// value leaves the range of `std::int64_t`.
  [[nodiscard]] std::optional<std::int64_t>
  evaluate(const Values &values) const;

  friend bool operator==(const LibTerm &, const LibTerm &) = default;
};

/// `disjoint(first, second, length)`: the first `length` bytes behind the
/// two arguments must not overlap (`memcpy`, `strcpy`).
struct LibDisjoint {
  std::uint8_t first = 0;
  std::uint8_t second = 0;
  LibTerm length;

  friend bool operator==(const LibDisjoint &, const LibDisjoint &) = default;
};

/// `wrapper(name)`: the runtime's checked `__weavec_rt_<name>` replaces the
/// call. It takes the call's arguments and the site (first when the call is
/// variadic, last otherwise) and checks what the call reads and writes as
/// it runs.
struct LibWrapper {
  std::string name;

  friend bool operator==(const LibWrapper &, const LibWrapper &) = default;
};

/// The `printf(f, v)` / `scanf(f, v)` clause.
struct LibFormat {
  enum class Kind : std::uint8_t { Printf, Scanf };
  Kind kind = Kind::Printf;
  /// The format argument.
  std::uint8_t format = 0;
  /// The first variadic argument; for a `v…` function (`vprintf`), whose
  /// row is not variadic, the `va_list` argument.
  std::uint8_t first = 0;
  /// True when `first` is a `va_list` (the row is not variadic): the
  /// conversions cannot be matched against the arguments.
  bool vaList = false;

  friend bool operator==(const LibFormat &, const LibFormat &) = default;
};

/// One parameter of a row.
struct LibraryParam {
  enum class Type : std::uint8_t { Int, Pointer, Function, Other };
  Type type = Type::Int;
  /// What the call does to the bytes behind a pointer.
  enum class Access : std::uint8_t { None, Read, Write, ReadWrite };
  Access access = Access::None;
  /// Bytes the call accesses behind the pointer. Without `bytes`, `count`
  /// or `string`, its first byte is guarded.
  std::optional<LibTerm> bytes;
  /// `wchar_t` elements the call accesses (wide rows).
  std::optional<LibTerm> count;
  /// The pointee holds a terminated string.
  bool string = false;
  enum class Null : std::uint8_t { Forbidden, Allowed, AllowedIfZero };
  Null null = Null::Forbidden;
  /// AllowedIfZero: null is allowed when this term is zero.
  std::optional<LibTerm> zeroTerm;
  /// `release`, `realloc`: the allocator validates the pointer.
  enum class Effect : std::uint8_t { Borrow, Release, Realloc };
  Effect effect = Effect::Borrow;

  friend bool operator==(const LibraryParam &, const LibraryParam &) = default;
};

/// A fortified alias.
struct LibraryChk {
  /// `__builtin___memcpy_chk`, `__memcpy_chk`.
  std::string name;
  /// For each alias argument, the row's argument index, or -1 (dropped:
  /// a flag, or the object size). Alias arguments beyond the list continue
  /// from the last mapped index (variadic rows only).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::int8_t> argumentOf = {};
  /// The alias argument that carries the size of the object the row
  /// writes (`size`), or -1.
  int sizeArgument = -1;

  friend bool operator==(const LibraryChk &, const LibraryChk &) = default;
};

/// One row of the table.
struct LibraryEntry {
  std::string name;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibraryChk> chk = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibraryParam> params = {};
  bool variadic = false;
  /// The result: none, a non-pointer, or a pointer.
  enum class Result : std::uint8_t { Void, Int, Pointer };
  Result result = Result::Void;
  /// `value(t)` (Int): the result equals `t` (`strlen` is `strlen(a0)`).
  std::optional<LibTerm> value;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibDisjoint> disjoint = {};
  /// `wide`: the row's strings end in a zero `wchar_t`, and `count` and
  /// `strlen(aN)` count `wchar_t` elements (`wcslen`).
  bool wide = false;
  std::optional<LibFormat> format;
  std::optional<LibWrapper> wrapper;
  /// The line of `LibrarySpec.txt` the row starts on.
  unsigned line = 0;

  /// The parameter for argument `index`, or null for a variadic argument.
  [[nodiscard]] const LibraryParam *param(unsigned index) const noexcept;

  friend bool operator==(const LibraryEntry &, const LibraryEntry &) = default;
};

/// A resolved callee: the row, and the alias it was reached through.
struct LibraryMatch {
  const LibraryEntry *entry = nullptr;
  /// The fortified alias, or null for the row's own name or `__builtin_X`.
  const LibraryChk *alias = nullptr;

  /// The row argument that call argument `callArgument` becomes, or -1 when
  /// the alias drops it.
  [[nodiscard]] int rowArgument(unsigned callArgument) const noexcept;
  /// The call argument that carries row argument `rowArg`, or -1.
  [[nodiscard]] int callArgument(unsigned rowArg) const noexcept;
  /// The call argument that carries the object size of a fortified alias,
  /// or -1.
  [[nodiscard]] int sizeArgument() const noexcept {
    return alias != nullptr ? alias->sizeArgument : -1;
  }
};

/// The parsed table.
class LibrarySpec {
public:
  /// The table embedded in the library, parsed on first use. A parse error
  /// is a build-time test failure; should it happen anyway the table is
  /// empty.
  [[nodiscard]] static const LibrarySpec &shipped();
  /// The embedded text itself.
  [[nodiscard]] static std::string_view shippedText();

  /// Parses `text`; on failure returns `std::nullopt` and sets `error` to
  /// `LibrarySpec.txt:<line>: <message>`.
  [[nodiscard]] static std::optional<LibrarySpec> parse(std::string_view text,
                                                        std::string &error);

  [[nodiscard]] const std::vector<LibraryEntry> &entries() const noexcept {
    return rows;
  }

  /// The row named exactly `name` (the first, if it has overloads).
  [[nodiscard]] const LibraryEntry *find(std::string_view name) const;
  /// Resolves a callee name: the row's own name, a `chk` alias, or either
  /// behind a `__builtin_` prefix. With `arguments`, the first overload
  /// whose arity the call matches (a variadic row accepts more), else the
  /// first.
  [[nodiscard]] std::optional<LibraryMatch>
  lookup(std::string_view callee,
         std::optional<unsigned> arguments = std::nullopt) const;

private:
  std::vector<LibraryEntry> rows;
  std::multimap<std::string, std::size_t, std::less<>> byName;
  /// Alias name to (row index, index into the row's `chk`).
  std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>>
      byAlias;
  friend class LibrarySpecParser;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_LIBRARYSPEC_H
