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
#include "weavec/Core/CallTargets.h"
#include "weavec/Core/IntegerExpression.h"
#include "weavec/Core/Offset.h"
#include "weavec/Core/Ownership.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Safety.h"
#include "weavec/Core/Scalar.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
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
  /// The returned value (RFCs 0008 and 0013). Record fields use `.field`;
  /// pointer-result heap fields use `*.field`. `index` is always zero.
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
  [[nodiscard]] SummaryPath indexed(std::string_view selector = {}) const;

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

using CallbackBindings = std::map<SummaryPath, CallTargets>;

/// A guard over summary paths (RFC 0009, *Guards*): the conjunction of facts
/// about the callee's interface under which alone an effect, a store or a
/// return alternative holds. The caller translates it to its own places at
/// the call and prunes it against what it knows about the arguments.
using PathGuard = GuardOn<SummaryPath>;

/// RFC 0011: an extent expressed against the callee's interface: `scale *
/// path + constant` bytes, or `constant` alone. `xmalloc(n)` returns an
/// object of `param 0 * 1 + 0` bytes; `make_node()` one of `sizeof(struct
/// node)`.
enum class AffineQuantity : std::uint8_t { Integer, Terminator };

struct PathAffine {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<SummaryPath> path = {};
  std::int64_t scale = 1;
  std::int64_t constant = 0;

  // RFC 0017: an actual C value, followed by mathematical byte scaling.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<IntegerExpression<SummaryPath>> expression = {};
  AffineQuantity quantity = AffineQuantity::Integer;

  [[nodiscard]] static PathAffine
  ofExpression(IntegerExpression<SummaryPath> value, std::int64_t scale = 1,
               std::int64_t constant = 0) {
    return {
        .scale = scale, .constant = constant, .expression = std::move(value)};
  }
  [[nodiscard]] static PathAffine ofConstant(std::int64_t constant) {
    return PathAffine{.path = std::nullopt, .scale = 1, .constant = constant};
  }
  [[nodiscard]] static PathAffine
  ofPath(SummaryPath path, std::int64_t scale = 1, std::int64_t constant = 0) {
    return PathAffine{
        .path = std::move(path), .scale = scale, .constant = constant};
  }
  [[nodiscard]] static PathAffine ofTerminator(SummaryPath path,
                                               std::int64_t scale = 1,
                                               std::int64_t constant = 0) {
    return {.path = std::move(path),
            .scale = scale,
            .constant = constant,
            .quantity = AffineQuantity::Terminator};
  }
  [[nodiscard]] bool isConstant() const noexcept {
    return !path && !expression;
  }

  friend bool operator==(const PathAffine &, const PathAffine &) = default;
  friend std::strong_ordering operator<=>(const PathAffine &,
                                          const PathAffine &) = default;
};

/// RFC 0017: a possible numeric output under an interface guard. An absent
/// expression explicitly denotes unknown; it is never dropped from a union.
struct NumericOutput {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<IntegerExpression<SummaryPath>> value = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PathGuard when = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<Outcome> on = {};
  friend auto operator<=>(const NumericOutput &,
                          const NumericOutput &) = default;
};
inline constexpr std::size_t MaxNumericOutputAlternatives = 8;

/// RFC 0011, *Extents in summaries*: what a callee needs of the object
/// behind a pointer parameter, in bytes, on the paths where `when` holds.
struct ExtentRequirement {
  PathAffine need;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PathGuard when = {};
  /// RFC 0017: first accessed byte; absent means a possibly empty range
  /// starting at zero. A negative element access is never an empty call.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PathAffine> start = {};

  friend bool operator==(const ExtentRequirement &,
                         const ExtentRequirement &) = default;
  friend std::strong_ordering operator<=>(const ExtentRequirement &,
                                          const ExtentRequirement &) = default;
};

/// RFC 0018: sufficient conditions, separate from reachable bug witnesses.
enum class CheckedRequirementKind : std::uint8_t {
  Valid,
  Extent,
  Initialized,
  Release,
  Separated,
  Writable,
  Terminated,
  Copied,
  SumFits,
  Zeroed,
  /// RFC 0021: inclusive byte displacement from the entry pointer in other.
  Position,
  /// RFC 0021: advance(path) <= advance(other) + end.constant.
  Progress,
  /// RFC 0022: family encodes the required target object view.
  ObjectType,
  /// RFC 0023: family encodes a sufficient linked-container predicate.
  Container,
  /// RFC 0023: disjoint node/payload footprints, not just distinct heads.
  ContainerSeparated,
  /// RFC 0023: output capability derives from the call-entry chain in other.
  ContainerDerived,
  /// RFC 0023: output has a fresh, separated owned allocation footprint.
  ContainerFresh,
  /// RFC 0023: the output is a saved successor, separate from the input head.
  ContainerTail,
  /// RFC 0024: format plus trailing pack (begin >= 0) or list (begin=-1).
  FormatArguments,
  /// RFC 0024: sufficient initialized, unconsumed va_list input.
  ArgumentList,
  /// RFC 0024: input list cannot be traversed again after this call.
  ArgumentListConsumed,
  TerminatedWithin,
  /// RFC 0024: named environmental stream; paths and bounds are unused.
  StandardStream,
  /// RFC 0025: independent initialized member view of overlapping storage.
  UnionMember
};
struct CheckedRequirement {
  CheckedRequirementKind kind = CheckedRequirementKind::Valid;
  SummaryPath path;
  SummaryPath other;
  PathAffine begin = PathAffine::ofConstant(0);
  PathAffine end = PathAffine::ofConstant(0);
  std::string family;
  /// RFC 0019: sufficient input implication or established output condition.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PathGuard when = {};
  /// Only output facts may be restricted to a returning outcome.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<Outcome> on = {};
  /// RFC 0022: memory output holds when its final destination pointer is
  /// non-null.
  bool ifNonNull = false;
  friend auto operator<=>(const CheckedRequirement &,
                          const CheckedRequirement &) = default;
};

/// RFC 0021: copied contracts retain immutable requirement/output storage.
/// Mutations detach; no iterator permits changes to a shared entry.
class CheckedRequirements {
public:
  using Set = std::set<CheckedRequirement>;
  using ConstIterator = Set::const_iterator;
  CheckedRequirements() = default;
  CheckedRequirements(std::initializer_list<CheckedRequirement> entries);
  [[nodiscard]] ConstIterator begin() const { return entries().begin(); }
  [[nodiscard]] ConstIterator end() const { return entries().end(); }
  [[nodiscard]] bool empty() const { return entries().empty(); }
  [[nodiscard]] std::size_t size() const { return entries().size(); }
  [[nodiscard]] bool contains(const CheckedRequirement &entry) const {
    return entries().contains(entry);
  }
  std::pair<ConstIterator, bool> insert(CheckedRequirement entry);
  void clear() { values.reset(); }
  void assign(Set entries);
  void intersect(const CheckedRequirements &other);
  friend bool operator==(const CheckedRequirements &left,
                         const CheckedRequirements &right) {
    return left.values == right.values || left.entries() == right.entries();
  }

private:
  [[nodiscard]] const Set &entries() const;
  Set &writable();
  std::shared_ptr<Set> values;
};

struct CheckedContract {
  /// Frontend canonical C function type. Empty only for synthetic Core values.
  std::string signature;
  bool computed = false;
  bool selected = false;
  bool deferred = false;
  bool limited = false;
  CheckedRequirements requirements;
  CheckedRequirements establishes;
  SafetyLedger obligations;
  /// RFC 0025: optional selector discovery, never proof or an entry premise.
  std::set<SummaryPath> caseInputs;

  void noteCaseInput(const SummaryPath &path);

  void require(CheckedRequirement requirement);
  void establish(CheckedRequirement requirement);
  void join(const CheckedContract &other);
  [[nodiscard]] bool complete() const {
    return computed && !limited && !deferred && obligations.complete();
  }
  friend bool operator==(const CheckedContract &,
                         const CheckedContract &) = default;
};
[[nodiscard]] std::string_view toString(CheckedRequirementKind value) noexcept;
[[nodiscard]] std::optional<CheckedRequirementKind>
parseCheckedRequirementKind(std::string_view value);
/// RFC 0023: retain every input premise when joining derived chain outputs.
[[nodiscard]] std::optional<CheckedRequirement>
joinContainerOutput(const CheckedRequirement &first,
                    const CheckedRequirement &second);

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
  /// RFC 0010, *Shares*: the consume releases one share of the object at
  /// the path (a reference-count decrement whose zero test guards the free)
  /// rather than the object. The caller's name is dead; other shares live
  /// on. A must-fact: a plain free on one side makes the join a plain free.
  /// Meaningful only when `freed` or `moved` is set.
  bool share = false;
  /// RFC 0010, *Stores out of sight*: the callee stored a copy of the value
  /// at the path into memory its summary cannot name (a field of a node it
  /// allocated and linked into the caller's container: `n->value = v;
  /// t->first = n;`), so the value has a second home the caller cannot see.
  /// The caller marks its argument escaped, as it does for a value a
  /// `store` copies (RFC 0007, *Escape*). A may-fact: joins by disjunction.
  bool escaped = false;
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
  /// RFC 0011, *Derived pointers*: where, relative to the value at the
  /// path, the pointer the callee released points (`free(container_of(i,
  /// T, f))` releases `param 0` at `-T.f`). The caller composes it with the
  /// offset it passed: an argument derived at `+f` handed to such a callee
  /// releases the start of its object. Meaningful only when `freed` or
  /// `moved` is set; a must-fact: consuming sides that disagree make it
  /// unknown.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PointerOffset at = {};

  [[nodiscard]] bool empty() const noexcept {
    return !read && !written && !freed && !moved && !escaped;
  }
  [[nodiscard]] bool consumed() const noexcept { return freed || moved; }
  /// Anything that changes the object: a caller must hold no loan on it.
  [[nodiscard]] bool mutates() const noexcept {
    return written || freed || moved;
  }

  /// May-join: `or` of every flag but `replaced`, `element` and `share`,
  /// which hold only if every consuming side says so. The family survives only
  /// when both sides agree (or only one consumes); a disagreement is "unknown",
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
    /// RFC 0014: function pointer values have no ownership obligation.
    Function,
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
  CallTargets targets = {};
  /// Set for `Copy` and `Borrow`.
  std::optional<SummaryPath> path;
  /// `Copy` and `Fresh` (RFC 0011): where in its object the value points.
  /// A copy at a non-zero offset points into the object at `path` but not
  /// at the same address (`strchr` returns into its argument; `return p +
  /// 1`); a pointer comparison cannot refute it (RFC 0006, *Alias
  /// exactness*). A fresh value at a non-zero offset is an allocation the
  /// receiver gets a derived pointer to (`return &o->in`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PointerOffset offset = {};
  /// `Fresh` only (RFC 0011): the extent of the allocation, when the callee
  /// knows it against its interface.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PathAffine> extent = {};
  /// `Fresh` only: the release family the receiver must use (RFC 0007);
  /// empty when unknown.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string family = {};
  /// RFC 0009: the value is stored or returned only when the guard holds.
  /// Two sources that differ only in their guard are one alternative whose
  /// guard is the join (`addReturn`, `addStore`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PathGuard when = {};

  /// RFC 0013: a graph reference reads the post-state of this heap
  /// description, rather than an incoming argument value.
  bool post = false;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PathAffine> stringLength = {};
  bool unterminated = false;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PointerOffset> boundsOffset = {};

  [[nodiscard]] static ValueSource fresh(std::string family = {}) {
    return ValueSource{.kind = Kind::Fresh,
                       .path = std::nullopt,
                       .offset = {},
                       .extent = std::nullopt,
                       .family = std::move(family),
                       .when = {}};
  }
  /// RFC 0011: a fresh allocation of `extent` bytes the receiver gets at
  /// `offset`.
  [[nodiscard]] static ValueSource
  freshAt(std::string family, PointerOffset offset,
          std::optional<PathAffine> extent,
          std::optional<PointerOffset> boundsOffset = {}) {
    return ValueSource{.kind = Kind::Fresh,
                       .path = std::nullopt,
                       .offset = std::move(offset),
                       .extent = std::move(extent),
                       .family = std::move(family),
                       .when = {},
                       .boundsOffset = std::move(boundsOffset)};
  }
  [[nodiscard]] static ValueSource function(CallTargets targets) {
    ValueSource result;
    result.kind = Kind::Function;
    result.targets = std::move(targets);
    return result;
  }
  [[nodiscard]] static ValueSource raw() {
    return ValueSource{.kind = Kind::Raw,
                       .path = std::nullopt,
                       .offset = {},
                       .extent = std::nullopt,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource copy(SummaryPath of) {
    return ValueSource{.kind = Kind::Copy,
                       .path = std::move(of),
                       .offset = {},
                       .extent = std::nullopt,
                       .family = {},
                       .when = {}};
  }
  /// RFC 0011: a copy of the pointer at `of`, stepped by `offset`.
  [[nodiscard]] static ValueSource copyAt(SummaryPath of,
                                          PointerOffset offset) {
    return ValueSource{.kind = Kind::Copy,
                       .path = std::move(of),
                       .offset = std::move(offset),
                       .extent = std::nullopt,
                       .family = {},
                       .when = {}};
  }
  /// A copy somewhere into the object at `of` (`strchr`): the offset is
  /// unknown.
  [[nodiscard]] static ValueSource interiorCopy(SummaryPath of) {
    return copyAt(std::move(of), PointerOffset::unknown());
  }
  [[nodiscard]] static ValueSource borrow(SummaryPath of) {
    return ValueSource{.kind = Kind::Borrow,
                       .path = std::move(of),
                       .offset = {},
                       .extent = std::nullopt,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource null() {
    return ValueSource{.kind = Kind::Null,
                       .path = std::nullopt,
                       .offset = {},
                       .extent = std::nullopt,
                       .family = {},
                       .when = {}};
  }
  [[nodiscard]] static ValueSource unknown() { return ValueSource{}; }

  [[nodiscard]] bool isFresh() const noexcept { return kind == Kind::Fresh; }
  [[nodiscard]] bool isNull() const noexcept { return kind == Kind::Null; }
  /// A copy that does not necessarily hold the same address as its source.
  [[nodiscard]] bool isInterior() const noexcept {
    return kind == Kind::Copy && !offset.isZero();
  }

  /// The same source with a trivial guard.
  [[nodiscard]] ValueSource unguarded() const {
    ValueSource result = *this;
    result.when.clear();
    return result;
  }
  /// True if `other` is this alternative up to its guard.
  [[nodiscard]] bool sameValueAs(const ValueSource &other) const {
    return kind == other.kind && targets == other.targets &&
           path == other.path && offset == other.offset &&
           extent == other.extent && family == other.family &&
           post == other.post && stringLength == other.stringLength &&
           unterminated == other.unterminated;
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

/// RFC 0013: deterministic projection limits, shared by import validation
/// and the checker. Exceeding either limit marks coverage incomplete.
inline constexpr std::size_t MaxHeapPathDepth = 8;
inline constexpr std::size_t MaxHeapFields = 128;
inline constexpr std::size_t MaxHeapAlternatives = 8;

/// RFC 0013: a finite graph of the final pointer cells reachable from an
/// output. Destinations and post references are relative to `result`, which
/// denotes this description's root. A fresh value introduces an object; a
/// post copy names that same object. Missing fields join with unknown.
struct HeapDescription {
  std::set<Store> fields;
  bool incomplete = false;

  void addField(Store field);
  /// Weakens references lost at a projection limit to unknown.
  void normalize();
  void join(const HeapDescription &other);
  /// Checks graph structure independently of frontend types.
  [[nodiscard]] bool valid() const;

  friend bool operator==(const HeapDescription &,
                         const HeapDescription &) = default;
};

/// The consumption that holds on the paths returning one outcome class.
using OutcomeEffects = std::map<SummaryPath, PlaceEffect>;

/// RFC 0010, *Per-outcome integer facts*: per integer path in caller memory
/// the callee wrote, the fact that holds on every path returning one class.
using OutcomeFacts = std::map<SummaryPath, ValueFact>;

/// RFC 0015: a final contiguous copy from entry contents. Explicit final
/// cell postconditions override this range. A non-definite effect carries
/// possible contents only and never justifies a strong replacement.
struct ArrayCopy {
  SummaryPath dest;
  SummaryPath source;
  PathAffine destBegin;
  PathAffine sourceBegin;
  PathAffine count;
  std::int64_t elementBytes = 0;
  std::string view;
  PathGuard when;
  bool definite = true;
  friend auto operator<=>(const ArrayCopy &, const ArrayCopy &) = default;
};

/// RFC 0015: a proved zero-based fill. Missing bytes means null; otherwise
/// each element receives a distinct malloc result of that constant extent.
struct ArrayFill {
  SummaryPath storage;
  PathAffine count;
  std::optional<std::int64_t> bytes;
  PathGuard when;
  bool definite = true;
  friend auto operator<=>(const ArrayFill &, const ArrayFill &) = default;
};

/// RFC 0015: a proved complete traversal releases every pointer cell in a
/// contiguous interval. Clearing a slot does not release its aliases again.
struct ArrayRelease {
  SummaryPath storage;
  PathAffine begin;
  PathAffine count;
  PathGuard when;
  bool cleared = false;
  bool definite = true;
  friend auto operator<=>(const ArrayRelease &, const ArrayRelease &) = default;
};

/// The interface behaviour of one function (RFC 0003, *Summaries*).
class FunctionSummary {
public:
  CheckedContract checked;
  /// RFC 0014: explicit reasons why this summary is incomplete.
  std::set<std::string> incomplete;
  /// RFC 0014: interface paths whose function values specialize this body.
  std::set<SummaryPath> callbackInputs;
  std::map<SummaryPath, std::string> objectViews;
  /// Effects per path; paths with an empty effect are not stored. These are
  /// the *may* effects over every path through the callee.
  std::map<SummaryPath, PlaceEffect> effects;
  /// Pointer values written to caller-visible places.
  std::set<Store> stores;
  /// RFC 0013: final heap state, keyed by the output root.
  std::map<SummaryPath, HeapDescription> heap;
  std::set<ArrayCopy> arrayCopies;
  std::set<ArrayFill> arrayFills;
  std::set<ArrayRelease> arrayReleases;
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
  /// RFC 0010, *Shares*: integer paths the callee adds one to (`param 0
  /// *.rc` for `o->rc++`), directly or through a callee. The caller's
  /// argument *retains* its object: its place gains a share. A may-fact:
  /// joins by union.
  std::set<SummaryPath> increments;
  /// RFC 0010: integer paths the callee subtracts one from. Informational
  /// (a decrement not followed by a zero-guarded release is not a share
  /// release); joins by union.
  std::set<SummaryPath> decrements;
  /// RFC 0010: the count paths of the callee's share releases (`param 0
  /// *.rc` for an `unref` of `param 0`), for the registry of known counts
  /// that decides whether a retained share leaks. Joins by union.
  std::set<SummaryPath> counts;
  /// RFC 0010, *Per-outcome stores*: per outcome class, the store
  /// destinations written on *some* path returning it (a may-fact per
  /// class, joining by union). Empty when every class stores to every
  /// destination (the common case; see `storesOnClass`). A caller that
  /// narrows the classes retracts a destination stored in none of the
  /// remaining ones. A class present here is also a key of `outcomes`.
  std::map<Outcome, std::set<SummaryPath>> storesOn;
  /// RFC 0010, *Per-outcome integer facts*: per outcome class, the facts
  /// about integer paths in caller memory that hold on every path returning
  /// it (`int dec_and_test(int *r) { return --*r == 0; }` has `param 0 *`
  /// equal to zero in class `positive`). A must-fact: joins by intersection
  /// of the paths, the facts joined. A class present here is also a key of
  /// `outcomes`.
  std::map<Outcome, OutcomeFacts> factOn;
  std::map<SummaryPath, std::set<NumericOutput>> numericOutputs;
  void addNumericOutput(const SummaryPath &path, NumericOutput output);
  /// RFC 0011, *Extents in summaries*: per pointer parameter, what the
  /// callee requires of the extent of the object behind it (from a
  /// `WEAVEC_SIZED_BY` annotation, or inferred from its accesses). A
  /// may-fact: joins by union; a caller whose argument is known to be
  /// smaller is reported at the call.
  std::map<std::uint32_t, std::set<ExtentRequirement>> requiresExtent;

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
  /// RFC 0011: adds a requirement on parameter `param`; the same need under
  /// another guard is merged, the guards joined.
  void addRequirement(std::uint32_t param, ExtentRequirement requirement);

  /// True if `path` is consumed on every path returning an outcome in
  /// `outcomes`, whatever the arguments (RFC 0009: no class consumes it
  /// under a guard), i.e. its consumption cannot be retracted by a test of
  /// the result. Without classes, true unless the effect itself carries a
  /// guard.
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
  /// Every store destination, ascending.
  [[nodiscard]] std::set<SummaryPath> storeDestinations() const;
  /// The destinations stored on some path returning `outcome` (RFC 0010):
  /// its `storesOn` entry, or every destination when the map is empty (the
  /// stores are unconditional) or the class is unknown to `outcomes`.
  [[nodiscard]] std::set<SummaryPath> storesOnClass(Outcome outcome) const;
  /// Drops `storesOn` when it says nothing: every class of `outcomes`
  /// stores to every destination.
  void normalizeStoresOn();
  /// True if the callee retains argument `param` (some increment path lies
  /// under `param(i)*`).
  [[nodiscard]] bool retains(std::uint32_t param) const;

  [[nodiscard]] bool empty() const noexcept {
    return objectViews.empty() && incomplete.empty() &&
           callbackInputs.empty() && effects.empty() && stores.empty() &&
           returns.empty() && outcomes.empty() && nullOn.empty() &&
           nonNullOn.empty() && requiresNonNull.empty() && !neverReturns &&
           increments.empty() && decrements.empty() && counts.empty() &&
           storesOn.empty() && factOn.empty() && numericOutputs.empty() &&
           requiresExtent.empty() && heap.empty() && arrayCopies.empty() &&
           arrayReleases.empty() && arrayFills.empty();
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
/// (from the outcome classes too, and from the RFC 0010 count and per-class
/// sets); a `copy` or `borrow` of one becomes `unknown`. Parameter and
/// result roots are kept.
[[nodiscard]] FunctionSummary remapGlobals(const FunctionSummary &summary,
                                           const GlobalIdMap &map);

} // namespace weavec::core

#endif // WEAVEC_CORE_SUMMARY_H
