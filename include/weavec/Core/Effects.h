//===- Effects.h - Format-30 function summaries (RFC 0031) ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §6: what a function does to its entry heap (the objects its
// parameters and the globals reach, named by `SummaryPath`s) and what it
// returns, per result case. The object engine derives it at every exit and
// instantiates it at every call; `EffectsIO` spells it as summary format 30.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_EFFECTS_H
#define WEAVEC_CORE_EFFECTS_H

#include "weavec/Core/Integer.h"
#include "weavec/Core/Path.h"
#include "weavec/Core/SourceLocation.h"

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace weavec::core {

/// A result class (RFC 0006): what a test of the result can select.
enum class ResultClass : std::uint8_t {
  Null,
  NonNull,
  Zero,
  Positive,
  Negative
};

[[nodiscard]] std::string_view toString(ResultClass value) noexcept;
[[nodiscard]] std::optional<ResultClass>
parseResultClass(std::string_view text);

/// RFC 0030 §9.1's case: the result classes under which an effect holds,
/// optionally narrowed by a parameter's zero test. An empty class list is
/// `always`.
struct EffectCase {
  std::vector<ResultClass> classes = {};
  /// `param N =0` (true) or `!=0` (false).
  std::optional<std::pair<std::uint32_t, bool>> paramZero = std::nullopt;
  /// RFC 0014: pointer parameters N and M compare equal (true) or not
  /// (false).
  std::optional<ParamPairTest> paramsEqual = std::nullopt;
  /// The value at an entry path (`global0`, `param0*.buf`) was zero (true)
  /// or not (false) at entry: a lazily initialised global or field.
  std::optional<std::pair<SummaryPath, bool>> entryZero = std::nullopt;

  [[nodiscard]] bool always() const noexcept {
    return classes.empty() && !paramZero && !paramsEqual && !entryZero;
  }
  [[nodiscard]] std::string toString() const;
  friend bool operator==(const EffectCase &, const EffectCase &) = default;
  friend auto operator<=>(const EffectCase &, const EffectCase &) = default;
};

/// `scale * <param i> + constant`, or a constant: an extent or value over
/// the interface.
struct PathTerm {
  std::optional<SummaryPath> path = std::nullopt;
  std::int64_t scale = 1;
  std::int64_t constant = 0;

  [[nodiscard]] std::string toString() const;
  friend bool operator==(const PathTerm &, const PathTerm &) = default;
  friend auto operator<=>(const PathTerm &, const PathTerm &) = default;
};

/// RFC 0015 §5, RFC 0031 §4.2 *Amendment (arrays)*: the elements whose
/// indices lie in `[from, to)`, selected by the last index step of a path.
struct ElementRange {
  PathTerm from = {};
  PathTerm to = {};

  [[nodiscard]] std::string toString() const;
  friend bool operator==(const ElementRange &, const ElementRange &) = default;
  friend auto operator<=>(const ElementRange &, const ElementRange &) = default;
};

/// An effect on an entry object named by `path`.
struct PathEffect {
  enum class Kind : std::uint8_t {
    /// Released (`family`).
    Release,
    /// Ownership moved out (RFC 0002).
    Move,
    /// The unknown-callee default reached it (RFC 0030 §5.1).
    Unknown,
    /// Kept where others can reach it (a store into a global or the
    /// caller's heap).
    Escape,
    /// RFC 0010: a reference released or added.
    ShareDown,
    ShareUp,
  };
  Kind kind = Kind::Release;
  SummaryPath path = {};
  std::string family = {};
  EffectCase when = {};
  /// Holds on some paths only (a possible effect).
  bool may = false;
  /// Made by dropping a conjunct it depended on (RFC 0030 §9.1).
  bool lossy = false;
  /// `Release`: the released pointer was this many bytes into the object
  /// (non-zero for an interior release, RFC 0008).
  std::int64_t offset = 0;
  /// `Release`: the released pointer was some number of bytes into the
  /// object, or before it, that the callee does not know (`s - hdr(s)`):
  /// `offset` is then 0 and means nothing.
  bool anyOffset = false;
  /// The effect holds of every element in the range the path's last index
  /// step selects (the objects those elements point to).
  std::optional<ElementRange> elements = std::nullopt;

  friend bool operator==(const PathEffect &, const PathEffect &) = default;
  friend auto operator<=>(const PathEffect &, const PathEffect &) = default;
};

/// A value the function leaves in a cell of the entry heap, or returns.
struct ValueDesc {
  enum class Kind : std::uint8_t {
    Null,
    /// A new object of `family` with `extent` bytes.
    Fresh,
    /// The value an entry path held at entry (plus `offset` bytes).
    Path,
    /// Static storage or a string literal.
    Static,
    /// An integer in `[lo, hi]`, or related to `path`.
    Int,
    /// RFC 0031 §5.7: a pointer to the callee's frame storage, whose
    /// lifetime ended when the callee returned.
    Dangling,
    Unknown,
    /// §7 *Amendment (cross-unit contexts)*: one of the functions
    /// `functions` names (portable names), a callback handed out.
    Function,
  };
  Kind kind = Kind::Unknown;
  std::string family = {};
  std::optional<PathTerm> extent = std::nullopt;
  bool zeroed = false;
  std::optional<SummaryPath> path = std::nullopt;
  std::optional<std::int64_t> offset = std::nullopt;
  std::optional<std::int64_t> lo = std::nullopt;
  std::optional<std::int64_t> hi = std::nullopt;
  /// `Int`: the values as integers of their C type, where `[lo, hi]` cannot
  /// hold them (an unsigned 64-bit value above `INT64_MAX`).
  std::optional<IntegerRange> range = std::nullopt;
  /// May be null (a pointer) on this case.
  bool maybeNull = false;
  /// `Fresh`: which of the function's new objects, so two cells (or the
  /// result and a cell) that hold the same one say so.
  std::uint32_t object = 0;
  /// `Fresh`: several objects of one family (a range of elements each
  /// holding its own).
  bool many = false;
  /// `Function`: the functions, by portable name (sorted).
  std::vector<std::string> functions = {};
  /// `Fresh`: a pointer into the new object at an offset the summary cannot
  /// spell (`offset` says a constant one); RFC 0017's flexible tails.
  bool interior = false;
  /// `Unknown`: a raw pointer (RFC 0004), which stays raw in the caller;
  /// `rawSome`, raw through some of a call's functions (`SymInfo::rawSome`).
  bool raw = false;
  bool rawSome = false;

  [[nodiscard]] std::string toString() const;
  friend bool operator==(const ValueDesc &, const ValueDesc &) = default;
  friend auto operator<=>(const ValueDesc &, const ValueDesc &) = default;
};

struct StoreEffect {
  SummaryPath dest = {};
  ValueDesc value = {};
  EffectCase when = {};
  bool may = false;
  /// Every element in the range `dest`'s last index step selects holds a
  /// value `value` describes (each its own).
  std::optional<ElementRange> elements = std::nullopt;
  /// RFC 0013 heap outputs: `dest` lies below a new object another store of
  /// the summary leaves in the cell named by `dest`'s first `contents`
  /// steps (the object's contents), so its dereferences from that cell on
  /// read the values the summary stores, not the entry heap's (RFC 0031
  /// §6.3).
  std::optional<std::uint32_t> contents = std::nullopt;
  /// RFC 0031 §6.3: `value` is a new object the callee made on every result
  /// class but these, where it stored null instead: on them the object was
  /// never made (`*out = malloc(n); return *out != NULL;`).
  std::vector<ResultClass> absentOn = {};
  /// An `unknown` value over the bytes `[first, second)` of the object
  /// `dest` names, counted from where its pointer points: the callee
  /// rewrote them (a member copied, a union written by code it could not
  /// see) and left the object's other bytes as they were. Without it an
  /// unknown store at an object's own path rewrites every byte.
  std::optional<std::pair<std::int64_t, std::int64_t>> bytes = std::nullopt;

  friend bool operator==(const StoreEffect &, const StoreEffect &) = default;
  friend auto operator<=>(const StoreEffect &, const StoreEffect &) = default;
};

/// RFC 0012 *String facts*, RFC 0031 §6.1 `string <path> nul-within
/// <term>`: at every exit where the object `path` names exists, a NUL lies
/// `nulWithin` bytes from where its pointer points and, with `nulFrom`,
/// none lies from `nulFrom` bytes up to it (the string there has exactly
/// that length).
struct StringEffect {
  SummaryPath path = {};
  /// As `StoreEffect::contents`: the path lies below a new object stored in
  /// the cell its first `contents` steps name.
  std::optional<std::uint32_t> contents = std::nullopt;
  PathTerm nulWithin = {};
  std::optional<PathTerm> nulFrom = std::nullopt;

  friend bool operator==(const StringEffect &, const StringEffect &) = default;
  friend auto operator<=>(const StringEffect &, const StringEffect &) = default;
};

/// One alternative of the result, on the classes it has.
struct ResultEffect {
  ValueDesc value = {};
  std::vector<ResultClass> classes = {};
  /// Arises only when parameter N is zero (true) or non-zero (false): a
  /// call whose argument is known to fail the test never gets it.
  std::optional<std::pair<std::uint32_t, bool>> paramZero = std::nullopt;

  friend bool operator==(const ResultEffect &, const ResultEffect &) = default;
  friend auto operator<=>(const ResultEffect &, const ResultEffect &) = default;
};

/// A function's summary (RFC 0031 §6.1).
struct FunctionEffects {
  enum class Returns : std::uint8_t { Always, May, Never };
  Returns returns = Returns::Always;
  /// Set when the summary may under-approximate the function (RFC 0030
  /// §5.5): callers add the unknown-callee default.
  std::optional<std::string> incomplete = std::nullopt;
  std::vector<PathEffect> effects = {};
  std::vector<StoreEffect> stores = {};
  std::vector<ResultEffect> results = {};
  std::vector<StringEffect> strings = {};
  /// RFC 0030 §9.2: a result in the class implies the path is non-null.
  std::map<ResultClass, std::vector<SummaryPath>> nonNullOn = {};
  /// Entry paths the function reads or writes through (§6.6).
  std::vector<SummaryPath> reads = {};
  std::vector<SummaryPath> writes = {};
  /// The function may run code the analysis does not see (an unknown
  /// callee, or a summary that is incomplete or says this), which may write
  /// any global, the caller's unit's included: a call forgets what the
  /// caller's globals hold, as a call of unknown code does.
  bool unknownGlobals = false;

  [[nodiscard]] bool empty() const noexcept {
    return effects.empty() && stores.empty() && results.empty() &&
           strings.empty() && !incomplete && !unknownGlobals;
  }
  friend bool operator==(const FunctionEffects &,
                         const FunctionEffects &) = default;
};

/// The summary as format-30 text lines (RFC 0031 §6.1), without the
/// `summary` header line.
[[nodiscard]] std::string toText(const FunctionEffects &effects);

} // namespace weavec::core

#endif // WEAVEC_CORE_EFFECTS_H
