//===- LibrarySpec.h - The declarative C library table ----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §8: one declarative table, `lib/Core/LibrarySpec.txt`, states what
// every modelled C library, POSIX, platform and compiler-builtin function
// does to its arguments and result: accesses and their lengths, nullability,
// ownership effects and release families, hidden state slots, callbacks,
// format arguments and fortified aliases. It also carries the §5.2 header
// list that decides which system headers are the platform's own.
//
// CMake embeds the text in the library; `LibrarySpec::shipped()` parses it
// once on first use. `LibrarySpec::parse` parses any text in the same syntax
// (tests, experiments). The syntax, its semantics and the extensions to the
// RFC's §8.2 grammar are documented at the top of `LibrarySpec.txt`.
//
// Which calls a row governs (§8): a direct call whose callee's name is the
// row's name, `__builtin_<name>`, or one of the row's `chk` aliases (with the
// alias's arguments remapped onto the row's), provided the program does not
// define a function of that name. That last condition is the engine's to
// check; this class only resolves names.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_LIBRARYSPEC_H
#define WEAVEC_CORE_LIBRARYSPEC_H

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

/// A small expression over a call's arguments (§8.1):
///
///   term ::= INT | 'a' N | 'strlen(a' N ')' | 'fmtlen(a' N ')' | MACRO
///          | term '*' term | term '+' term | term '-' INT
///          | 'min(' term ',' term ')' | '(' term ')'
///
/// `*` binds tighter than `+` and `-`; both associate to the left.
/// `strlen(aN)` counts the elements of argument N's pointee type before the
/// terminating zero element (`wcslen` for `wchar_t`); inside `min(t, …)` it
/// is the bounded length `strnlen(aN, t)` and requires no terminator within
/// `t` elements. `fmtlen(aN)` is the length the `printf` family would
/// produce for the format at argument N; it states a requirement and is
/// never evaluated as a check term. `MACRO` (an identifier that starts with
/// an upper-case letter or `_`, such as `BUFSIZ` or `L_tmpnam`) is the value
/// of that macro in the calling unit; where it is undefined the term is
/// unknown. Terms are mathematical integers: they never wrap.
struct LibTerm {
  enum class Kind : std::uint8_t {
    Constant,     ///< `value`
    Argument,     ///< `a<arg>`
    StringLength, ///< `strlen(a<arg>)`
    FormatLength, ///< `fmtlen(a<arg>)`
    Macro,        ///< `macro`
    Product,      ///< `operands[0] * operands[1]`
    Sum,          ///< `operands[0] + operands[1]`
    Difference,   ///< `operands[0] - value`
    Min,          ///< `min(operands[0], operands[1])`
  };
  Kind kind = Kind::Constant;
  std::int64_t value = 0;
  std::uint8_t arg = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string macro = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibTerm> operands = {};

  [[nodiscard]] static LibTerm constant(std::int64_t value);
  [[nodiscard]] static LibTerm argument(unsigned index);
  [[nodiscard]] static LibTerm stringLength(unsigned index);
  [[nodiscard]] static LibTerm formatLength(unsigned index);
  [[nodiscard]] static LibTerm product(LibTerm left, LibTerm right);
  [[nodiscard]] static LibTerm sum(LibTerm left, LibTerm right);
  [[nodiscard]] static LibTerm difference(LibTerm left, std::int64_t right);
  [[nodiscard]] static LibTerm min(LibTerm left, LibTerm right);

  /// The canonical spelling, which `parse` reads back to an equal term.
  [[nodiscard]] std::string str() const;
  /// Parses a whole term; `std::nullopt` with `error` set otherwise.
  [[nodiscard]] static std::optional<LibTerm> parse(std::string_view text,
                                                    std::string &error);
  /// True if the term mentions `wanted` anywhere.
  [[nodiscard]] bool mentions(Kind wanted) const;
  /// Every argument index the term reads, in first-mention order.
  [[nodiscard]] std::vector<unsigned> arguments() const;

  /// Known values for the leaves of a term; an empty function or a
  /// `std::nullopt` answer makes the leaf unknown.
  struct Values {
    std::function<std::optional<std::int64_t>(unsigned)> argument;
    std::function<std::optional<std::int64_t>(unsigned)> stringLength;
    std::function<std::optional<std::int64_t>(unsigned)> formatLength;
    std::function<std::optional<std::int64_t>(std::string_view)> macro;
  };
  /// Evaluates the term, or `std::nullopt` when a leaf is unknown or the
  /// value leaves the range of `std::int64_t`.
  [[nodiscard]] std::optional<std::int64_t>
  evaluate(const Values &values) const;

  friend bool operator==(const LibTerm &, const LibTerm &) = default;
};

/// How a function-pointer argument is invoked (§5.3).
struct LibCallback {
  enum class Kind : std::uint8_t {
    /// Zero or more times before the call returns (`qsort`, `bsearch`).
    Sync,
    /// Later, as an entry point: a thread or a signal handler
    /// (`pthread_create`, `signal`, `sigaction`).
    Entry,
    /// After the main flow ends (`atexit`, `at_quick_exit`, `on_exit`).
    AtExit,
  };
  Kind kind = Kind::Sync;
  /// Sync: for each parameter of the target, the call argument its pointer
  /// points into (`qsort`: `{0, 0}`); empty when the target receives only
  /// pointers to the library's own storage (`nftw`). Entry: at most one
  /// index, the argument passed to the target (`pthread_create`: `{3}`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::uint8_t> arguments = {};

  friend bool operator==(const LibCallback &, const LibCallback &) = default;
};

/// `disjoint(first, second, length)`: the first `length` bytes behind the
/// two arguments must not overlap (`memcpy`, `strcpy`).
struct LibDisjoint {
  std::uint8_t first = 0;
  std::uint8_t second = 0;
  LibTerm length;

  friend bool operator==(const LibDisjoint &, const LibDisjoint &) = default;
};

/// `copies(dst, src, length)`: the call copies the first `length` bytes of
/// argument `src` to argument `dst` (`memcpy`, `strncpy`).
struct LibCopy {
  std::uint8_t dst = 0;
  std::uint8_t src = 0;
  LibTerm length;

  friend bool operator==(const LibCopy &, const LibCopy &) = default;
};

/// `fills(dst, value, length)`: the call sets the first `length` bytes of
/// argument `dst` to `value` (`memset`, `bzero`).
struct LibFill {
  std::uint8_t dst = 0;
  LibTerm value;
  LibTerm length;

  friend bool operator==(const LibFill &, const LibFill &) = default;
};

/// `writes-str(dst[, length])`: when the call succeeds, argument `dst`
/// holds a string of `length` characters, or of unknown length when absent
/// (`strcpy`, `fgets`). Terms read the arguments' values before the call; a
/// negative length means nothing is written (`snprintf(d, 0, …)`).
struct LibStringWrite {
  std::uint8_t dst = 0;
  std::optional<LibTerm> length;

  friend bool operator==(const LibStringWrite &,
                         const LibStringWrite &) = default;
};

/// The `printf(f, v)` / `scanf(f, v)` clause.
struct LibFormat {
  enum class Kind : std::uint8_t { Printf, Scanf };
  Kind kind = Kind::Printf;
  /// The format argument.
  std::uint8_t format = 0;
  /// The first variadic argument; for a `v…` function (`vprintf`), whose
  /// entry is not variadic, the `va_list` argument.
  std::uint8_t first = 0;
  /// True when `first` is a `va_list` (the entry is not variadic): the
  /// conversions cannot be matched against the arguments.
  bool vaList = false;

  friend bool operator==(const LibFormat &, const LibFormat &) = default;
};

/// The result of a call, or (`LibraryParam::out`) the value the call stores
/// through a pointer-to-pointer argument.
struct LibraryResult {
  enum class Kind : std::uint8_t {
    Void,          ///< `void`, `noreturn`
    Int,           ///< `int`: any non-pointer result
    Fresh,         ///< `fresh(F)`: a new resource of family `family`
    Static,        ///< `static(S)`: a borrow of hidden state slot `state`
    Arg,           ///< `arg(N)`: argument `arg` itself
    Interior,      ///< `interior(N)`: a pointer into argument `arg`'s object
    InteriorState, ///< `interior-state(S)`: into the value `S` retains
    Unknown,       ///< `ptr`: a pointer of unknown provenance
  };
  Kind kind = Kind::Void;
  /// Fresh: the release family. Arg: when non-empty, the result is instead
  /// `fresh(family)` when argument `arg` is null (`realpath`, `getcwd`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string family = {};
  /// Static, InteriorState: the hidden state slot. Arg: when non-empty, the
  /// result is instead `static(state)` when argument `arg` is null
  /// (`tmpnam`, `ctermid`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string state = {};
  /// Arg, Interior: the argument.
  std::uint8_t arg = 0;
  /// Fresh, Static: accessible bytes.
  std::optional<LibTerm> extent;
  enum class Null : std::uint8_t {
    /// Never null. For `arg(N)`: exactly the argument, null only when the
    /// argument is.
    Never,
    /// Null exactly when the call fails (allocation or lookup failure).
    OnFailure,
    /// May be null for any other reason (`strchr` found nothing).
    May,
  };
  Null null = Null::Never;
  /// §11: a `fresh` result eligible for zero-initialisation lowering.
  bool zeroInit = false;
  /// `str`: the pointer is to a string terminated by a zero element.
  bool string = false;
  /// `replaces` (out values only): the value previously in the slot is
  /// consumed as by `realloc` of the same family before the new one is
  /// stored (`getline`).
  bool replaces = false;
  /// `value(t)` (Int): the result equals `t` (`strlen` is `strlen(a0)`).
  std::optional<LibTerm> value;
  /// `offset(t)` (Interior): the result is `t` elements past the start of
  /// argument `arg` (`stpcpy` is `strlen(a1)`).
  std::optional<LibTerm> offset;
  /// `zero-filled` (Fresh): the library sets the whole extent to zero bytes
  /// (`calloc`), whatever §11 lowering does.
  bool zeroFilled = false;

  /// True for the pointer kinds (everything but Void and Int).
  [[nodiscard]] bool isPointer() const noexcept;
  friend bool operator==(const LibraryResult &,
                         const LibraryResult &) = default;
};

/// One parameter of a row (§8.1).
struct LibraryParam {
  enum class Type : std::uint8_t { Int, Pointer, Function, Other };
  Type type = Type::Int;
  /// What the call does to the bytes behind a pointer.
  enum class Access : std::uint8_t { None, Read, Write, ReadWrite };
  Access access = Access::None;
  /// Required accessible bytes behind the pointer. Without `bytes`, `count`
  /// or `string`, a pointer needs one element of its pointee type.
  std::optional<LibTerm> bytes;
  /// Required accessible elements of the pointee type (`wmemcpy`).
  std::optional<LibTerm> count;
  /// The pointee must hold a terminated string within its object.
  bool string = false;
  enum class Null : std::uint8_t { Forbidden, Allowed, AllowedIfZero };
  Null null = Null::Forbidden;
  /// AllowedIfZero: null is allowed when this term is zero.
  std::optional<LibTerm> zeroTerm;
  enum class Effect : std::uint8_t {
    Borrow,  ///< Used for the call only.
    Release, ///< Released (`free`); `family` names the releaser.
    Realloc, ///< Released on success, or kept, as `realloc` (§8.2).
    Retain,  ///< Stored in hidden state slot `state` for later `reads`.
    Escape,  ///< Kept by the library where no row can follow it.
    Init,    ///< The pointee acquires a resource of `family` (`regcomp`).
    Fini,    ///< The pointee's `family` resource ends; its storage stays
             ///< valid and may be initialised again (`regfree`).
  };
  Effect effect = Effect::Borrow;
  /// Release, Realloc, Init, Fini: the family.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string family = {};
  /// Retain: the hidden state slot.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string state = {};
  /// Function parameters, and pointer parameters whose pointee holds the
  /// function pointers (`sigaction`'s `act`).
  std::optional<LibCallback> callback;
  /// `out(…)`: the call stores this value through the pointer, which points
  /// to a pointer slot (`getline`, `asprintf`, `strtol`'s end pointer).
  std::optional<LibraryResult> out;

  friend bool operator==(const LibraryParam &, const LibraryParam &) = default;
};

/// A fortified alias (§8, "Which calls a row governs").
struct LibraryChk {
  /// `__builtin___memcpy_chk`, `__memcpy_chk`.
  std::string name;
  /// For each alias argument, the row's argument index, or -1 (dropped).
  /// Alias arguments beyond the list continue from the last mapped index
  /// (variadic rows only).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::int8_t> argumentOf = {};

  friend bool operator==(const LibraryChk &, const LibraryChk &) = default;
};

/// The parameter classes of a declaration, to choose between rows of the
/// same name (`qsort_r` has a glibc and a BSD order) and to refuse a row for
/// a declaration that does not fit it.
struct LibSignature {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibraryParam::Type> params = {};
  bool variadic = false;
  bool pointerResult = false;

  friend bool operator==(const LibSignature &, const LibSignature &) = default;
};

/// One row of the table (§8.1).
struct LibraryEntry {
  std::string name;
  /// The header that declares it, relative to its include directory; empty
  /// for compiler builtins.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string header = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibraryChk> chk = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibraryParam> params = {};
  bool variadic = false;
  LibraryResult result;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibDisjoint> disjoint = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibCopy> copies = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibFill> fills = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<LibStringWrite> writesString = {};
  /// State slots whose borrows end (`setenv` → `environ`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> invalidates = {};
  /// State slots read (`strtok(NULL, …)` → `strtok`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> reads = {};
  bool noreturn = false;
  bool exits = false;
  bool returnsTwice = false;
  std::optional<LibFormat> format;
  /// The line of `LibrarySpec.txt` the row starts on.
  unsigned line = 0;

  /// The parameter for argument `index`, or null for a variadic argument.
  [[nodiscard]] const LibraryParam *param(unsigned index) const noexcept;
  /// The result or an `out` value is a fresh resource.
  [[nodiscard]] bool allocates() const noexcept;
  /// Some argument is released or reallocated: calls are Release sites.
  [[nodiscard]] bool releases() const noexcept;
  /// Some argument carries a callback clause (a §9.4 boundary).
  [[nodiscard]] bool hasCallback() const noexcept;
  /// Neither `noreturn` nor `exits` (§7.5 "known to return").
  [[nodiscard]] bool knownToReturn() const noexcept;
  /// Declared by the compiler rather than a header (`__builtin_expect`).
  [[nodiscard]] bool isCompilerBuiltin() const noexcept;
  /// A fact rests on the row's own statement about hidden behaviour: a
  /// `static`/`interior-state` result, `retain`/`reads`/`invalidates`, or
  /// a callback clause. Such facets are `trusted(library-spec)` (§8.2).
  [[nodiscard]] bool trustsLibrarySpec() const noexcept;
  [[nodiscard]] LibSignature signature() const;
  /// True if a declaration with `declared` classes may use this row: the
  /// same arity and variadic-ness, the same pointer-ness of the result, and
  /// each `int`, pointer or `fn` parameter of the same class (`other`
  /// matches anything, as does a pointer declared where the row says
  /// `other`, for `va_list`).
  [[nodiscard]] bool accepts(const LibSignature &declared) const;
  /// The canonical row text, which `LibrarySpec::parse` reads back to an
  /// equal entry (header and line aside).
  [[nodiscard]] std::string str() const;

  friend bool operator==(const LibraryEntry &, const LibraryEntry &) = default;
};

/// A resolved callee: the row, and the alias it was reached through.
struct LibraryMatch {
  const LibraryEntry *entry = nullptr;
  /// The fortified alias, or null for the row's own name or `__builtin_X`.
  const LibraryChk *alias = nullptr;

  /// The row argument that call argument `callArgument` becomes, or -1 when
  /// the alias drops it (an object-size or flag argument).
  [[nodiscard]] int rowArgument(unsigned callArgument) const noexcept;
  /// The call argument that carries row argument `rowArg`, or -1.
  [[nodiscard]] int callArgument(unsigned rowArg) const noexcept;
  /// The row parameter for call argument `callArgument`, or null (dropped
  /// or variadic).
  [[nodiscard]] const LibraryParam *param(unsigned callArgument) const noexcept;
};

/// The parsed table.
class LibrarySpec {
public:
  /// The table embedded in the library, parsed on first use. A parse error
  /// is a build-time test failure; should it happen anyway the table is
  /// empty and `shippedError()` says why.
  [[nodiscard]] static const LibrarySpec &shipped();
  [[nodiscard]] static const std::string &shippedError();
  /// The embedded text itself.
  [[nodiscard]] static std::string_view shippedText();

  /// Parses `text`; on failure returns `std::nullopt` and sets `error` to
  /// `LibrarySpec.txt:<line>: <message>`.
  [[nodiscard]] static std::optional<LibrarySpec> parse(std::string_view text,
                                                        std::string &error);

  [[nodiscard]] const std::vector<LibraryEntry> &entries() const noexcept {
    return rows;
  }
  /// The §5.2 header list, in file order (`dir/*` patterns included).
  [[nodiscard]] const std::vector<std::string> &headers() const noexcept {
    return headerList;
  }

  /// The row named exactly `name` (the first, if it has overloads).
  [[nodiscard]] const LibraryEntry *find(std::string_view name) const;
  /// Every row named exactly `name`.
  [[nodiscard]] std::vector<const LibraryEntry *>
  overloads(std::string_view name) const;
  /// Resolves a callee name: the row's own name, a `chk` alias, or either
  /// behind a `__builtin_` prefix. The first overload wins.
  [[nodiscard]] std::optional<LibraryMatch>
  lookup(std::string_view callee) const;
  /// As above, choosing the overload that `accepts(declared)`; no match
  /// when none does.
  [[nodiscard]] std::optional<LibraryMatch>
  lookup(std::string_view callee, const LibSignature &declared) const;

  /// §5.2: whether a system header, named relative to the include directory
  /// that found it (`stdio.h`, `sys/ioctl.h`), is a C library, POSIX or
  /// platform header. On Darwin every header of the SDK is
  /// (`darwinSdk`). Headers under the toolchain's resource directory always
  /// are; that is the caller's test, not this one.
  [[nodiscard]] bool isPlatformHeader(std::string_view relativePath,
                                      bool darwinSdk) const;

private:
  std::vector<LibraryEntry> rows;
  std::vector<std::string> headerList;
  /// The concrete names of `headerList`, and its `dir/*` patterns as
  /// `dir/` prefixes.
  std::set<std::string, std::less<>> headerNames;
  std::vector<std::string> headerPrefixes;
  std::multimap<std::string, std::size_t, std::less<>> byName;
  /// Alias name to (row index, index into the row's `chk`).
  std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>>
      byAlias;

  [[nodiscard]] std::optional<LibraryMatch>
  resolve(std::string_view callee, const LibSignature *declared) const;
  friend class LibrarySpecParser;
};

/// The heap family: `malloc`'s releaser (`free`). Release-family names are
/// the releasing function's name (RFC 0007); compare against this rather
/// than spelling the libc name.
inline constexpr std::string_view HeapFamily = "free";
/// The family of `alloca` storage, released when the frame returns.
inline constexpr std::string_view StackFamily = "stack";
/// The family of `FILE` streams from `fopen` and friends.
inline constexpr std::string_view StreamFamily = "fclose";

} // namespace weavec::core

#endif // WEAVEC_CORE_LIBRARYSPEC_H
