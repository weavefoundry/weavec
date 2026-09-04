//===- Summary.h - Function summaries for signature inference --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A `FunctionSummary` records what a function does to the pointers it can
// see from the outside (RFC 0003): its parameters and the globals it touches,
// each addressed by a *summary path* spelled with the RFC 0002 place steps.
//
//   root ::= param(i) | global(g) | result
//   path ::= root ('*' | '.' field | '[*]')*
//
// Summaries are frontend-neutral: roots are integers, paths are step lists.
// The Analysis layer resolves them against a call's arguments to obtain
// caller places. Every component joins by set union, so the summary lattice
// is finite and the recursive fixpoint over call-graph cycles terminates.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_SUMMARY_H
#define WEAVEC_CORE_SUMMARY_H

#include "weavec/Core/Borrow.h"
#include "weavec/Core/Ownership.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Scalar.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

/// What a summary path is rooted at.
enum class SummaryRoot : std::uint8_t {
  /// The `index`-th parameter of the function.
  Param,
  /// A global variable, identified by an id interned per translation unit.
  Global,
  /// The object a function returning a record by value hands back (RFC
  /// 0008, *Struct-by-value results*). Only stores are rooted here
  /// (`result.data = fresh`); `index` is always 0.
  Result,
};

/// One step below a summary root; mirrors `PathStep` with the field name.
struct PathElem {
  PathStep step = PathStep::Field;
  std::string field;

  friend bool operator==(const PathElem &, const PathElem &) = default;
  friend std::strong_ordering operator<=>(const PathElem &,
                                          const PathElem &) = default;
};

/// A place relative to a function's interface: `param(0)`, `param(0)*`,
/// `param(0)*.data`, `global(3)`.
struct SummaryPath {
  SummaryRoot root = SummaryRoot::Param;
  std::uint32_t index = 0;
  std::vector<PathElem> steps;

  [[nodiscard]] static SummaryPath param(std::uint32_t index) {
    return SummaryPath{.root = SummaryRoot::Param, .index = index, .steps = {}};
  }
  [[nodiscard]] static SummaryPath global(std::uint32_t id) {
    return SummaryPath{.root = SummaryRoot::Global, .index = id, .steps = {}};
  }
  [[nodiscard]] static SummaryPath result() {
    return SummaryPath{.root = SummaryRoot::Result, .index = 0, .steps = {}};
  }

  [[nodiscard]] SummaryPath deref() const;
  [[nodiscard]] SummaryPath field(std::string_view name) const;
  [[nodiscard]] SummaryPath indexed() const;

  [[nodiscard]] bool isRoot() const noexcept { return steps.empty(); }
  [[nodiscard]] bool isParam() const noexcept {
    return root == SummaryRoot::Param;
  }
  [[nodiscard]] bool isGlobal() const noexcept {
    return root == SummaryRoot::Global;
  }
  [[nodiscard]] bool isResult() const noexcept {
    return root == SummaryRoot::Result;
  }
  /// True if `this` is a proper prefix of `other` (same root, fewer steps).
  [[nodiscard]] bool isProperPrefixOf(const SummaryPath &other) const;
  /// True if any step is a dereference: the path names caller memory rather
  /// than the callee's private copy of an argument.
  [[nodiscard]] bool hasDeref() const noexcept;
  /// The root path (`param(i)` / `global(g)`).
  [[nodiscard]] SummaryPath rootPath() const {
    return SummaryPath{.root = root, .index = index, .steps = {}};
  }

  /// Spells the path the way `PlaceTable` spells places, given the display
  /// name of the root: `p`, `*p`, `p->data`, `a[*]`.
  [[nodiscard]] std::string toString(std::string_view rootName) const;

  friend bool operator==(const SummaryPath &, const SummaryPath &) = default;
  friend std::strong_ordering operator<=>(const SummaryPath &,
                                          const SummaryPath &) = default;
};

/// A guard over summary paths (RFC 0009, *Guards*): the conjunction of facts
/// about the callee's interface under which alone an effect, a store or a
/// return alternative holds. The caller translates it to its own places at
/// the call and prunes it against what it knows about the arguments.
using PathGuard = GuardOn<SummaryPath>;

/// What the callee may do to the object at a summary path.
struct PlaceEffect {
  /// The object is loaded from (through a dereference of the root).
  bool read = false;
  /// The object is stored to.
  bool written = false;
  /// The owned resource at the path is released.
  bool freed = false;
  /// The owned resource at the path is moved to another owner.
  bool moved = false;
  /// RFC 0008, *Replaced values*: `freed`/`moved` describe the value the
  /// caller's memory held at the path on entry, and on every path that
  /// consumed it the callee stored something else there before returning
  /// (`free(b->data); b->data = NULL;`). Only holders of the *old* value
  /// are dead at the call; the place itself is not. A must-fact: joins by
  /// conjunction. Meaningful only when `freed` or `moved` is set; never set
  /// on a parameter root.
  bool replaced = false;
  /// RFC 0008, *Element consumes*: every consume of the path went through an
  /// element access (`free(a[i])`), so which element of the caller's array
  /// is gone is not known to the caller; it applies the consume with an
  /// *unknown* witness (RFC 0006, *Element witnesses*) instead of *whole*.
  /// A must-fact: joins by conjunction. Meaningful only when `freed` or
  /// `moved` is set.
  bool element = false;
  /// The release family of the consume (RFC 0007): the canonical releaser
  /// the resource ends up with (`free`, `fclose`, ...); empty when unknown.
  /// Meaningful only when `freed` or `moved` is set.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string family = {};
  /// RFC 0009: the consume (`freed`/`moved`) happens only when the guard
  /// holds; trivial when it happens on some path whatever the arguments.
  /// Meaningful only when `freed` or `moved` is set. Joins like a guard:
  /// the conjuncts every consuming side agrees on.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PathGuard when = {};

  [[nodiscard]] bool empty() const noexcept {
    return !read && !written && !freed && !moved;
  }
  [[nodiscard]] bool consumed() const noexcept { return freed || moved; }
  /// Anything that changes the object: a caller must hold no loan on it.
  [[nodiscard]] bool mutates() const noexcept {
    return written || freed || moved;
  }

  /// May-join: `or` of every flag but `replaced` and `element`, which hold
  /// only if every consuming side says so. The family survives only when
  /// both sides agree (or only one consumes); a disagreement is "unknown",
  /// so joining can only make the mismatch check report less. The guard is
  /// joined the same way: what both consuming sides require.
  void join(const PlaceEffect &other);

  friend bool operator==(const PlaceEffect &, const PlaceEffect &) = default;
};

/// Where a pointer value the callee stores or returns comes from.
struct ValueSource {
  enum class Kind : std::uint8_t {
    /// A fresh allocation the receiver now owns.
    Fresh,
    /// A copy of the pointer stored at `path` (an argument or a global).
    Copy,
    /// The address of the object at `path`.
    Borrow,
    /// A null pointer.
    Null,
    /// Nothing is known about the value.
    Unknown,
    /// A raw pointer (RFC 0004): the receiver may dereference or release it
    /// only inside an unsafe region.
    Raw,
  };

  Kind kind = Kind::Unknown;
  /// Set for `Copy` and `Borrow`.
  std::optional<SummaryPath> path;
  /// `Copy` only: the value points into the object at `path` but not
  /// necessarily at the same address (`strchr` returns into its argument;
  /// `return p + 1`). A pointer comparison cannot refute an interior copy
  /// (RFC 0006, *Alias exactness*).
  bool interior = false;
  /// `Fresh` only: the release family the receiver must use (RFC 0007);
  /// empty when unknown.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string family = {};
  /// RFC 0009: the value is stored or returned only when the guard holds.
  /// Two sources that differ only in their guard are one alternative whose
  /// guard is the join (`addReturn`, `addStore`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PathGuard when = {};

  [[nodiscard]] static ValueSource fresh(std::string family = {}) {
    return ValueSource{.kind = Kind::Fresh,
                       .path = std::nullopt,
                       .interior = false,
                       .family = std::move(family),
                       .when = {}};
  }
  [[nodiscard]] static ValueSource raw() {
    return ValueSource{.kind = Kind::Raw,
                       .path = std::nullopt,
                       .interior = false,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource copy(SummaryPath of) {
    return ValueSource{.kind = Kind::Copy,
                       .path = std::move(of),
                       .interior = false,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource interiorCopy(SummaryPath of) {
    return ValueSource{.kind = Kind::Copy,
                       .path = std::move(of),
                       .interior = true,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource borrow(SummaryPath of) {
    return ValueSource{.kind = Kind::Borrow,
                       .path = std::move(of),
                       .interior = false,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource null() {
    return ValueSource{.kind = Kind::Null,
                       .path = std::nullopt,
                       .interior = false,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource unknown() { return ValueSource{}; }

  [[nodiscard]] bool isFresh() const noexcept { return kind == Kind::Fresh; }
  [[nodiscard]] bool isNull() const noexcept { return kind == Kind::Null; }

  /// The same source with a trivial guard.
  [[nodiscard]] ValueSource unguarded() const {
    ValueSource result = *this;
    result.when.conditions.clear();
    return result;
  }
  /// True if `other` is this alternative up to its guard.
  [[nodiscard]] bool sameValueAs(const ValueSource &other) const {
    return kind == other.kind && path == other.path &&
           interior == other.interior && family == other.family;
  }

  friend bool operator==(const ValueSource &, const ValueSource &) = default;
  friend std::strong_ordering operator<=>(const ValueSource &,
                                          const ValueSource &) = default;
};

/// A pointer value the callee writes into caller-visible memory.
struct Store {
  SummaryPath dest;
  ValueSource value;

  friend bool operator==(const Store &, const Store &) = default;
  friend std::strong_ordering operator<=>(const Store &,
                                          const Store &) = default;
};

/// The consumption that holds on the paths returning one outcome class.
using OutcomeEffects = std::map<SummaryPath, PlaceEffect>;

/// The interface behaviour of one function (RFC 0003, *Summaries*).
class FunctionSummary {
public:
  /// Effects per path; paths with an empty effect are not stored. These are
  /// the *may* effects over every path through the callee.
  std::map<SummaryPath, PlaceEffect> effects;
  /// Pointer values written to caller-visible places.
  std::set<Store> stores;
  /// Alternatives for the pointer result; empty when nothing is known.
  std::set<ValueSource> returns;
  /// Per outcome class the callee may return, the consumption (`freed` /
  /// `moved`) that holds on the paths returning it (RFC 0006). A class with
  /// no entry is one the callee never returns as far as is known; an empty
  /// map means nothing is known about outcomes. `effects` is always a
  /// superset of every class.
  std::map<Outcome, OutcomeEffects> outcomes;
  /// Per outcome class, the caller places that on *every* path returning it
  /// hold null or nothing this function stored there (RFC 0007,
  /// *Per-outcome null stores*): `int make(char **out) { *out = malloc(n);
  /// return *out != NULL; }` has `param 0 *` in class `zero`, and so does an
  /// `init` whose `strm->state = fresh` store lies past its argument checks.
  /// A class present here is also a key of `outcomes`.
  std::map<Outcome, std::set<SummaryPath>> nullOn;
  /// Per outcome class, the caller places that on *every* path returning it
  /// hold a non-null pointer (RFC 0008, *Per-outcome non-null facts*): `int
  /// make(char **out) { *out = malloc(n); return *out != NULL; }` has `param
  /// 0 *` in class `positive`. A class present here is also a key of
  /// `outcomes`. A must-fact: joins by intersection.
  std::map<Outcome, std::set<SummaryPath>> nonNullOn;
  /// Parameters the callee dereferences while nothing is known about their
  /// nullness (RFC 0008, *Requirements*): a caller must not pass a pointer
  /// that may be null. A may-fact: joins by union.
  std::set<std::uint32_t> requiresNonNull;
  /// RFC 0009, *Inferred `noreturn`*: no path through the callee reaches
  /// its exit; a call to it ends the caller's path. A must-fact: joins by
  /// conjunction (an empty summary, the identity of the join, contributes
  /// nothing).
  bool neverReturns = false;

  /// The effect recorded for `path`, or an empty one.
  [[nodiscard]] PlaceEffect effectOf(const SummaryPath &path) const;

  /// Merges `effect` into the record for `path`.
  void addEffect(SummaryPath path, const PlaceEffect &effect);
  /// Adds a store; one to the same destination of the same value under
  /// another guard is merged, the guards joined.
  void addStore(Store store);
  /// Adds a return alternative; the same value under another guard is
  /// merged, the guards joined.
  void addReturn(ValueSource source);
  /// True if some alternative of the result has `kind`, under any guard.
  [[nodiscard]] bool returnsKind(ValueSource::Kind kind) const noexcept;
  /// Drops every alternative of `kind`, whatever its guard.
  void eraseReturns(ValueSource::Kind kind);
  /// Records that `outcome` is possible, with `effect` on `path` (an empty
  /// effect only records the class).
  void addOutcome(Outcome outcome, const SummaryPath &path,
                  const PlaceEffect &effect);
  void addOutcome(Outcome outcome) { outcomes.try_emplace(outcome); }

  /// True if `path` is consumed on every path returning an outcome in
  /// `outcomes`, i.e. its consumption cannot be retracted by a test of the
  /// result. Trivially true when nothing is known about outcomes.
  [[nodiscard]] bool consumesUnconditionally(const SummaryPath &path) const;

  /// True if the callee releases or moves argument `param`.
  [[nodiscard]] bool consumes(std::uint32_t param) const;
  /// The reason argument `param` is dead after the call: freed wins over
  /// moved when both are possible so the note says "freed here".
  [[nodiscard]] bool frees(std::uint32_t param) const;

  /// How the callee borrows what argument `param` points to for the
  /// duration of the call: `Mutable` if anything under `param(i)*` is
  /// mutated or stored to, `Shared` if anything is read, none otherwise.
  [[nodiscard]] std::optional<BorrowKind> borrowKind(std::uint32_t param) const;

  /// The ownership kind the callee's behaviour implies for argument `param`:
  /// `Owned` if consumed, else the borrow kind, else `Unknown`.
  [[nodiscard]] OwnershipKind inferredKind(std::uint32_t param) const;

  /// The kind implied for the return value: `Raw` if any alternative is
  /// raw, else `Owned` if every alternative is fresh (ignoring null),
  /// `Shared`/`Mutable` if every alternative is a borrow or copy, else
  /// `Unknown`.
  [[nodiscard]] OwnershipKind inferredReturnKind() const;

  /// True if some alternative of the result is a fresh allocation, of any
  /// family (RFC 0007).
  [[nodiscard]] bool returnsFresh() const noexcept;
  /// True if every alternative of the result is fresh or null, and at least
  /// one is fresh: the caller owns whatever non-null value it gets.
  [[nodiscard]] bool returnsOnlyFresh() const noexcept;
  /// The family every fresh alternative agrees on; empty when there is none
  /// or they disagree.
  [[nodiscard]] std::string freshReturnFamily() const;
  /// Drops every fresh alternative, whatever its family.
  void eraseFreshReturns();

  /// True if the callee may return a null pointer (`null` is among the
  /// alternatives of the result).
  [[nodiscard]] bool mayReturnNull() const noexcept;
  /// True if argument `param` must not be null.
  [[nodiscard]] bool requiresParam(std::uint32_t param) const {
    return requiresNonNull.contains(param);
  }
  /// True if some store has `path` as its destination.
  [[nodiscard]] bool storesTo(const SummaryPath &path) const {
    return std::ranges::any_of(
        stores, [&path](const Store &store) { return store.dest == path; });
  }

  [[nodiscard]] bool empty() const noexcept {
    return effects.empty() && stores.empty() && returns.empty() &&
           outcomes.empty() && nullOn.empty() && nonNullOn.empty() &&
           requiresNonNull.empty() && !neverReturns;
  }

  /// Component-wise set union (conjunction for the must-facts).
  void join(const FunctionSummary &other);

  friend bool operator==(const FunctionSummary &,
                         const FunctionSummary &) = default;
};

[[nodiscard]] std::string_view toString(ValueSource::Kind kind) noexcept;

/// Maps a global root id to another id, or to `nullopt` to drop the root.
using GlobalIdMap = std::function<std::optional<std::uint32_t>(std::uint32_t)>;

/// Rewrites every global root of `summary` through `map` (RFC 0005, *The
/// program database*): effects on and stores into a dropped root vanish
/// (from the outcome classes too); a `copy` or `borrow` of one becomes
/// `unknown`. Parameter and result roots are kept.
[[nodiscard]] FunctionSummary remapGlobals(const FunctionSummary &summary,
                                           const GlobalIdMap &map);

} // namespace weavec::core

#endif // WEAVEC_CORE_SUMMARY_H
