//===- Heap.h - The object engine's abstract heap ---------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §4: the abstract domain of the object engine.
//
// - A *symbol* (`Sym`) names one runtime value. Copying a value copies its
//   symbol, so a fact on a symbol, or on the objects it points to, is seen
//   through every variable, cell and expression that holds it.
// - An *abstract object* (`ObjectId`) stands for one runtime object when it
//   is singular, or for several. Objects are interned per function in an
//   `ObjectTable` by their origin, so two states name the same object the
//   same way and joins match objects by identity.
// - A *cell* is `(object, key)`: a byte offset, or the summary cell of an
//   element position. Memory maps cells to symbols.
// - Numbers live in a `Zone` over symbols.
//
// Everything here is independent of Clang: program entities are opaque
// `Handle`s the Analysis layer assigns, and questions only the frontend can
// answer (type compatibility, owning slots) go through `HeapOracle`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_HEAP_H
#define WEAVEC_CORE_HEAP_H

#include "weavec/Core/Integer.h"
#include "weavec/Core/Path.h"
#include "weavec/Core/Persistent.h"
#include "weavec/Core/PointerKind.h"
#include "weavec/Core/SourceLocation.h"
#include "weavec/Core/Zone.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace weavec::core {

/// An opaque frontend handle: a declaration, an expression, a type or a
/// site. Zero is none.
using Handle = std::uint64_t;
/// An abstract object, interned per function analysis. Zero is none.
using ObjectId = std::uint32_t;

//===----------------------------------------------------------------------===//
// Objects
//===----------------------------------------------------------------------===//

enum class ObjectKind : std::uint8_t {
  /// A local variable, parameter's storage or compound literal.
  Local,
  Global,
  /// A string literal (read-only).
  Literal,
  /// A function (the target of a function pointer).
  Function,
  /// The most recent allocation of a site (recency abstraction).
  HeapRecent,
  /// Every earlier allocation of a site.
  HeapOld,
  /// An object of the entry heap, named by its path (§4.6).
  Entry,
  /// The k-limited summary of the entry objects below a path.
  EntrySummary,
  /// A singular object focused out of a non-singular one (§4.6).
  Materialized,
  /// The object a variable points to at a join where its incoming paths
  /// point to different objects (§4.6, *Amendment (S1)*): singular, and
  /// possibly equal to each of its candidates.
  Focus,
  /// The result of a call the engine cannot see into, per call site.
  CallResult,
  /// What an unknown or raw pointer points to.
  Unknown,
};

[[nodiscard]] std::string_view spell(ObjectKind kind) noexcept;

/// What names an object deterministically.
struct ObjectKey {
  ObjectKind kind = ObjectKind::Unknown;
  /// The declaration, literal, site or function.
  Handle handle = 0;
  /// `Entry`, `EntrySummary`: the entry path.
  SummaryPath path = {};
  /// `Materialized`: the object it was focused out of. Dead copies (§4.6
  /// garbage collection) of an object name it here with `dead` set.
  ObjectId parent = 0;
  /// `Focus`: the variable's object and cell offset.
  std::int64_t cell = 0;
  /// A copy kept only for the summary and for aliasing questions after the
  /// object became unreachable (§4.6).
  bool dead = false;
  /// `Local`: `handle` is the expression that makes it (a compound literal,
  /// a call's record result), not a variable's declaration.
  bool expression = false;

  friend bool operator==(const ObjectKey &, const ObjectKey &) = default;
  friend std::strong_ordering operator<=>(const ObjectKey &,
                                          const ObjectKey &) = default;
};

/// What is known about an object independently of the program point.
struct ObjectInfo {
  ObjectKey key = {};
  /// The object's type, as a frontend handle (zero when unknown or bytes).
  Handle type = 0;
  /// Whether the object stands for at most one runtime object.
  bool singular = true;
  /// `Entry`: the concrete cell whose entry value points to it (holder,
  /// byte offset): it exists where that value is not null.
  std::optional<std::pair<ObjectId, std::int64_t>> heldIn = std::nullopt;
  /// §4.5 D3: the object from whose owning slot this one was reached (for
  /// `Entry`, `EntrySummary`, `Materialized` and `CallResult` objects).
  ObjectId ownedFrom = 0;
  /// §4.5 D2: loaded from an owning place.
  bool fromOwningSlot = false;
  /// For messages: the object as the program spells it (`p->next`, `buf`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string name = {};
  /// Where it was created (allocation site, declaration).
  SourceLocation created = {};
};

struct HeapState;
struct SymInfo;
struct CellKey;

/// Answers the questions about the program the domain cannot.
class HeapOracle {
public:
  virtual ~HeapOracle();
  HeapOracle() = default;
  HeapOracle(const HeapOracle &) = delete;
  HeapOracle &operator=(const HeapOracle &) = delete;
  HeapOracle(HeapOracle &&) = delete;
  HeapOracle &operator=(HeapOracle &&) = delete;

  /// §4.5 D1: whether objects of these types may be the same object under
  /// the unit's aliasing rules. Zero handles (unknown types) may alias
  /// everything.
  [[nodiscard]] virtual bool typesMayAlias(Handle first,
                                           Handle second) const = 0;
  /// Materialises the value of a cell of `object` this state never wrote,
  /// as a load would (§4.6), writing it into the cell (except a summary
  /// cell: its value is then that of an element no store reached). `hint` is
  /// the value the cell holds on the other side of a join, for its type.
  virtual Sym unwritten(HeapState &state, ObjectId object, CellKey key,
                        const SymInfo &hint) const = 0;
};

/// The objects of one function analysis, shared by all of its states.
class ObjectTable {
public:
  /// The object named `key`, creating it with `info` on first use.
  ObjectId intern(const ObjectKey &key, ObjectInfo info);
  [[nodiscard]] std::optional<ObjectId> lookup(const ObjectKey &key) const;
  [[nodiscard]] const ObjectInfo &info(ObjectId id) const {
    return objects.at(id - 1);
  }
  [[nodiscard]] std::size_t size() const noexcept { return objects.size(); }
  /// Gives an untyped object (a `void *` allocation) its type on first
  /// typed use.
  void setTypeIfUnknown(ObjectId id, Handle type) {
    ObjectInfo &object = objects.at(id - 1);
    if (object.type == 0)
      object.type = type;
  }
  /// The dead copy of `id` (§4.6).
  ObjectId deadCopy(ObjectId id);
  /// The object `id` is a dead copy of, or `id` itself.
  [[nodiscard]] ObjectId liveVersion(ObjectId id) const;

private:
  std::vector<ObjectInfo> objects;
  std::map<ObjectKey, ObjectId> byKey;
};

//===----------------------------------------------------------------------===//
// Terms, targets and records
//===----------------------------------------------------------------------===//

/// `scale * var + constant` bytes, or unknown (§4.3).
struct Term {
  Sym var = ZeroSym;
  std::int64_t scale = 0;
  std::int64_t constant = 0;
  bool known = true;

  [[nodiscard]] static Term of(std::int64_t constant) {
    return Term{
        .var = ZeroSym, .scale = 0, .constant = constant, .known = true};
  }
  [[nodiscard]] static Term ofSym(Sym var, std::int64_t scale = 1,
                                  std::int64_t constant = 0) {
    return Term{
        .var = var, .scale = scale, .constant = constant, .known = true};
  }
  [[nodiscard]] static Term unknown() {
    return Term{.var = ZeroSym, .scale = 0, .constant = 0, .known = false};
  }
  [[nodiscard]] bool isConstant() const noexcept {
    return known && (var == ZeroSym || scale == 0);
  }
  /// This term plus `other` when the sum is still one term.
  [[nodiscard]] std::optional<Term> plus(const Term &other) const;
  [[nodiscard]] Term plusConstant(std::int64_t delta) const;

  friend bool operator==(const Term &, const Term &) = default;
  friend auto operator<=>(const Term &, const Term &) = default;
};

/// One object a pointer may point into, with its byte offset.
struct Target {
  ObjectId object = 0;
  Term offset = Term::of(0);

  friend bool operator==(const Target &, const Target &) = default;
  friend auto operator<=>(const Target &, const Target &) = default;
};

enum class PointerNull : std::uint8_t { Null, NonNull, Maybe };

/// A byte offset (a *concrete* cell), the summary cell of an element
/// position (`offset` is then the offset within an element of `stride`
/// bytes), or a *selected* element cell: the cell at byte
/// `stride * index + offset` for the integer symbol `index` (§4.2
/// *Amendment (arrays)*, RFC 0015 §1). Symbols are immutable, so a selected
/// key names one runtime cell for as long as the key exists.
struct CellKey {
  std::int64_t offset = 0;
  std::uint32_t stride = 0;
  Sym index = ZeroSym;

  [[nodiscard]] bool isSummary() const noexcept {
    return stride != 0 && index == ZeroSym;
  }
  [[nodiscard]] bool isSelected() const noexcept { return index != ZeroSym; }
  [[nodiscard]] bool isConcrete() const noexcept { return stride == 0; }
  /// A concrete or selected cell: one runtime cell.
  [[nodiscard]] bool isElement() const noexcept { return !isSummary(); }
  /// The cell's byte offset as a term (unknown for a summary cell).
  [[nodiscard]] Term byteTerm() const;
  /// The summary key of the element position a selected cell is at.
  [[nodiscard]] CellKey position() const;
  /// The key of the cell at byte offset `offset`: concrete for a constant,
  /// selected for an affine term, none otherwise.
  [[nodiscard]] static std::optional<CellKey> at(const Term &offset);

  friend bool operator==(const CellKey &, const CellKey &) = default;
  friend auto operator<=>(const CellKey &, const CellKey &) = default;
};

/// A zero test the path made of the value a cell held at entry (`if (!g)`,
/// `if (b->buf == NULL)`): what a summary case keys a lazy initialisation
/// by.
struct EntryTest {
  ObjectId object = 0;
  CellKey key = {};
  bool zero = true;

  friend bool operator==(const EntryTest &, const EntryTest &) = default;
  friend auto operator<=>(const EntryTest &, const EntryTest &) = default;
};

/// RFC 0030 §3.1's `MoveRecord`, moved onto values and objects.
struct ReleaseRecord {
  enum class Reason : std::uint8_t {
    Freed,
    Moved,
    /// RFC 0010: the value's own reference was released.
    ShareReleased,
    /// The unknown-callee default (RFC 0030 §5.1).
    UnknownCallee,
    /// An open slot without targets (RFC 0030 §9.3).
    Callback,
  };
  // NOLINTBEGIN(readability-redundant-member-init): designated-init defaults
  Reason reason = Reason::Freed;
  SourceLocation where = {};
  /// The release family (RFC 0007), empty when unknown.
  std::string family = {};
  /// The name the value was released through, for `freed here (through
  /// 'm')`.
  std::string via = {};
  bool allPaths = true;
  bool conditional = false;
  bool lossy = false;
  /// Made by a weak cell's merge of values (§4.1): the value may be the
  /// released one only because elements or aliases are not told apart. Such
  /// a record never makes a diagnostic.
  bool aliasOnly = false;
  /// RFC 0030 §9.1: facts about the releasing function's unmodified integer
  /// parameters on the path of the release: `(index, zero)`.
  std::vector<std::pair<std::uint32_t, bool>> paramGuard = {};
  /// RFC 0014: whether pairs of its unmodified pointer parameters compared
  /// equal on the path of the release.
  std::vector<ParamPairTest> pairGuard = {};
  /// The zero tests of entry values on the path of the release (sorted).
  std::vector<EntryTest> entryGuard = {};
  /// RFC 0031 *Pending cases and exit splitting*: the locals, assigned only
  /// where they are declared, that held a non-null pointer on the path of
  /// the release (sorted handles): an exit returning one of them as null
  /// did not release.
  std::vector<Handle> nonNullLocals = {};
  // NOLINTEND(readability-redundant-member-init)

  [[nodiscard]] bool unknownOrigin() const noexcept {
    return reason == Reason::UnknownCallee || reason == Reason::Callback;
  }
  [[nodiscard]] bool definite() const noexcept {
    return allPaths && !conditional && !unknownOrigin() && !aliasOnly;
  }
  friend bool operator==(const ReleaseRecord &,
                         const ReleaseRecord &) = default;
};

/// Joins two records of the same value or object (RFC 0030 §3.1 rules).
[[nodiscard]] ReleaseRecord joinRecords(const ReleaseRecord &left,
                                        const ReleaseRecord &right);

/// A result class of a call whose effects wait for a test (RFC 0030 §9.1).
struct PendingCase {
  enum class Kind : std::uint8_t {
    /// The subject was released (conditionally) at the call.
    Release,
    /// RFC 0030 §9.2: on these classes the subject is non-null.
    NonNull,
    /// RFC 0031 §6.3: on these classes the call did not store the new
    /// objects the subject points to: they were never made.
    Absent,
    /// RFC 0017: on these classes the integer subject lies in `bound` (a
    /// checked operation's result when it did not overflow).
    Bound,
    /// RFC 0031 §6.3: the call stored `stored` into the cells that now hold
    /// `subject` (the join of the old and new values) on these classes, and
    /// left `previous` there on the others.
    Stored,
  };
  Kind kind = Kind::Release;
  /// The classes of the result under which the effect holds (`null`,
  /// `nonnull`, `zero`, `positive`, `negative`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> classes = {};
  /// The symbol the effect applies to, and what happens to it.
  Sym subject = ZeroSym;
  ReleaseRecord record = {};
  /// `Release`: the effect's parameter test the call left open (the
  /// argument, and whether it must be zero), tested again when the class
  /// is selected.
  std::optional<std::pair<Sym, bool>> argumentZero = std::nullopt;
  std::optional<IntegerRange> bound = std::nullopt;
  /// `Stored`: the value stored, and the value the cells held before.
  Sym stored = ZeroSym;
  Sym previous = ZeroSym;

  friend bool operator==(const PendingCase &, const PendingCase &) = default;
};

/// A comparison a boolean symbol stands for, so a branch on it refines both
/// operands (§5.1).
struct Condition {
  /// `And` and `Or`: `left` and `right` are truth values with conditions of
  /// their own (a logical operator used as a value, `!(p && n)`).
  enum class Op : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge, NonZero, And, Or };
  Op op = Op::NonZero;
  Sym left = ZeroSym;
  Sym right = ZeroSym;
  /// `right` is the constant `constant` instead of a symbol.
  bool rightIsConstant = false;
  std::int64_t constant = 0;
  /// The comparison is between the operands as unsigned values.
  bool isUnsigned = false;

  friend bool operator==(const Condition &, const Condition &) = default;
};

//===----------------------------------------------------------------------===//
// Symbols
//===----------------------------------------------------------------------===//

/// The C operation an integer symbol was computed by (§5.3): what a witness
/// spells when no C place holds the value, and, since symbols are immutable,
/// the key under which the same operation on the same operands is the same
/// value (§4.1).
struct SymDefinition {
  IntegerOp op = IntegerOp::Add;
  Sym left = ZeroSym;
  /// The right operand, or none when it is the constant `constant`.
  Sym right = ZeroSym;
  std::optional<std::int64_t> constant = std::nullopt;
  /// The C result equals the mathematical one for every value (no
  /// wrap-around, RFC 0017), so a 64-bit term may spell it (RFC 0030 §7.4
  /// *Arithmetic*).
  bool exact = false;
  /// The left operand's value when it is a constant (the dividend of
  /// `INT_MAX / m`), which a later state may no longer hold (RFC 0017 §3's
  /// range guards).
  std::optional<IntegerValue> leftValue = std::nullopt;

  [[nodiscard]] bool sameOperation(const SymDefinition &other) const noexcept {
    return op == other.op && left == other.left && right == other.right &&
           constant == other.constant;
  }
  friend bool operator==(const SymDefinition &,
                         const SymDefinition &) = default;
};

/// Where a pointer value became null or possibly null (RFC 0008's notes).
struct NullOrigin {
  enum class Reason : std::uint8_t {
    /// `p = NULL`: "'p' is assigned NULL here".
    Assigned,
    /// The null edge of a test: "'p' may be null: it is compared with NULL
    /// here".
    Tested,
    /// An allocation's result (`allocatorSource`): "allocated here";
    /// `detail` is the allocating function's name.
    Allocated,
  };
  Reason reason = Reason::Assigned;
  SourceLocation where = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};

  friend bool operator==(const NullOrigin &, const NullOrigin &) = default;
};

/// The most entry places a value records it was computed from.
inline constexpr std::size_t MaxEntryOrigins = 8;
/// The most constants a value records it is known not to equal (RFC 0034
/// §6.2).
inline constexpr std::size_t MaxExcluded = 8;

// (The fields stay in the groups they are documented in.)
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): documented order
struct SymInfo {
  enum class Type : std::uint8_t { Unknown, Int, Pointer, Function };
  // NOLINTBEGIN(readability-redundant-member-init): designated-init defaults
  Type type = Type::Unknown;

  // Integers.
  /// The C type of an integer value (for ranges and wrap-around).
  std::optional<IntegerType> intType = std::nullopt;
  /// For a boolean result of a comparison or test.
  std::optional<Condition> condition = std::nullopt;
  /// A pointer converted to an integer: the pointer symbol behind it.
  Sym pointerBehind = ZeroSym;
  /// Known to be non-zero (a disequality the zone cannot hold).
  bool nonZero = false;
  /// RFC 0034 §6.2: the constants it is known not to equal besides what
  /// `nonZero` says (sorted, at most `MaxExcluded`): the false edge of
  /// `x == c` and the true edge of `x != c` add `c`, and an edge that
  /// requires `x == c` for one of them is infeasible.
  std::vector<std::int64_t> excluded = {};
  /// §4.4: the value's interval in its type (RFC 0017), where the zone's
  /// 64-bit bounds cannot hold it (an unsigned 64-bit value above
  /// `INT64_MAX`); none means the type's range, refined by the zone.
  std::optional<IntegerRange> values = std::nullopt;
  /// RFC 0017's range guard (`n <= INT_MAX / m`): this non-negative value
  /// times the symbol is at most the bound, so their product cannot wrap.
  std::optional<std::pair<Sym, std::uint64_t>> productAtMost = std::nullopt;

  // Pointers.
  std::vector<Target> targets = {};
  /// "Any object": no points-to information.
  bool top = false;
  PointerNull null = PointerNull::Maybe;
  bool allocatorSource = false;
  /// RFC 0008, RFC 0031 §5.11: why the value may be null, for the notes of
  /// null findings (never for a decision).
  std::optional<NullOrigin> nullOrigin = std::nullopt;
  std::optional<ReleaseRecord> release = std::nullopt;
  /// RFC 0004, RFC 0033 §2: a raw pointer (declared `WEAVEC_RAW`, or loaded
  /// through or handed out as one) and where it became one. Must: a merge
  /// of a raw value and another is `rawSome`.
  bool raw = false;
  /// Raw only through some of the functions a call may reach (a hook whose
  /// functions return raw and tracked pointers, RFC 0031 *Implementation
  /// amendments*): no definite `unsafe-operation`, and nothing proven.
  /// Joins keep it; only such a call makes it.
  bool rawSome = false;
  SourceLocation rawAt = {};
  /// How the raw origin arose, for the note at `rawAt` (RFC 0004).
  enum class RawOrigin : std::uint8_t { Declared, Loaded, Returned };
  RawOrigin rawOrigin = RawOrigin::Declared;
  /// `Loaded`: the raw pointer it was loaded through; `Returned`: the
  /// callee that handed it out.
  std::string rawFrom = {};
  /// The first place the raw value was read from, for `(through 'p')`.
  std::string rawVia = {};
  /// RFC 0030 §2.3 `raw-cast`: the pointer was made by reinterpretation.
  bool rawCast = false;
  /// RFC 0010: references this value holds on a counted object.
  std::optional<std::int32_t> shares = std::nullopt;
  /// §4.5 D6: the symbols this value was derived from through owning slots,
  /// nearest first (at most 8).
  std::vector<Sym> ancestors = {};
  /// Never assigned (RFC 0008): a local's value before any store.
  bool uninit = false;
  /// Never assigned on some path (RFC 0030 §11): garbage there unless
  /// zero-initialisation ran.
  bool mayUninit = false;
  /// Null on some paths because a join took in a null pointer (not an
  /// unassigned one) beside other values: the value a summary describes as
  /// an entry path is then that path's value or null, where a value only
  /// maybe-null at entry is the path's value alone.
  bool nullJoined = false;
  /// RFC 0011: made from another pointer by `&` on a place below it or by
  /// arithmetic, a borrow of the object rather than a copy of its owner
  /// (§5.5 *Conflicting borrows*).
  bool derived = false;
  /// Pending outcome cases keyed on this value's class (RFC 0030 §9.1).
  std::vector<PendingCase> pending = {};

  // Functions.
  std::vector<Handle> functions = {};
  bool functionsKnown = false;
  /// Functions of other units, by portable name (a callback a caller in
  /// another unit passes, RFC 0031 §7 *cross-unit contexts*).
  std::vector<std::string> foreignFunctions = {};

  /// The spelling the value was created under, for messages.
  std::string name = {};
  /// The value's C type, as a frontend handle (zero when unknown).
  Handle ctype = 0;
  /// An integer equal to this term over another symbol (§4.4): `n * 4`.
  std::optional<Term> linear = std::nullopt;
  /// An unsigned integer equal to this term reduced modulo its type (a
  /// product that may wrap, `n * sizeof *p`): never more than it.
  std::optional<Term> unwrapped = std::nullopt;
  /// The value a cell held at entry, materialised for it (§4.6): the
  /// object and cell, so a summary tells a store from an unchanged cell.
  std::optional<std::pair<ObjectId, CellKey>> entryOf = std::nullopt;
  /// The cells whose entry values this value was computed from (itself,
  /// by pointer arithmetic, a cast, a merge), sorted: a proof about it
  /// rests on those places' entry assumptions (RFC 0030 §9.4 as RFC 0031
  /// amends it). At most `MaxEntryOrigins`; a value from more rests on none
  /// it could name, and says so by `entryOriginsLost`.
  std::vector<std::pair<ObjectId, CellKey>> entryOrigins = {};
  bool entryOriginsLost = false;
  /// The operation that computed this integer, when one did (§5.3).
  std::optional<SymDefinition> defined = std::nullopt;
  // NOLINTEND(readability-redundant-member-init)

  friend bool operator==(const SymInfo &, const SymInfo &) = default;
};

//===----------------------------------------------------------------------===//
// Object states
//===----------------------------------------------------------------------===//

/// RFC 0015 §5, §4.2 *Amendment (arrays)*: the elements whose indices lie in
/// `[from, to)` at the element position `position` (a summary key) each hold
/// a value described by `value`. Each element's value is its own runtime
/// value: a read copies `value`'s attributes into a fresh symbol, so a
/// release record on `value` that is definite says that every element in
/// the range was released.
struct Segment {
  CellKey position = {};
  Term from = Term::of(0);
  Term to = Term::of(0);
  Sym value = ZeroSym;
  /// RFC 0015 §4, §4.9 *Copies*: the range was copied from `source`, an
  /// entry object no store had reached, `shift` bytes further on, so each
  /// element holds the entry value of its own source element (the value a
  /// load of that element reads). Holds while `value` is still `copied`,
  /// the value the copy wrote: any later change of the range makes it a
  /// plain range described by `value`.
  ObjectId source = 0;
  std::int64_t shift = 0;
  Sym copied = ZeroSym;

  [[nodiscard]] bool isCopy() const noexcept {
    return source != 0 && value == copied;
  }
  friend bool operator==(const Segment &, const Segment &) = default;
};

enum class Life : std::uint8_t {
  Live,
  /// Released on every path, with the record.
  Released,
  /// Released on some path, or weakly (a release through a pointer that may
  /// point elsewhere).
  MayReleased,
  /// Reached by an unknown callee (RFC 0030 §5.1).
  UnknownReleased,
  /// Storage whose lifetime ended (a local out of scope).
  Ended,
  MayEnded,
};

/// An object's extent in bytes and how much it may be trusted (RFC 0030
/// §7.1).
struct Extent {
  Term bytes = Term::unknown();
  ExtentClass cls = ExtentClass::LowerBound;
  /// `bytes` is this term reduced modulo the size type (an allocation of a
  /// product that may wrap): never more than it, so an access past it is
  /// past the object.
  std::optional<Term> unwrapped = std::nullopt;

  friend bool operator==(const Extent &, const Extent &) = default;
};

struct ObjectState {
  // NOLINTBEGIN(readability-redundant-member-init): designated-init defaults
  PMap<CellKey, Sym> cells;
  /// Element ranges, newest first: an element is described by its own cell
  /// if it has one, else by the first segment that must contain it (§4.2
  /// *Amendment (arrays)*).
  std::vector<Segment> segments = {};
  /// The element size a variable index was used with on this object (RFC
  /// 0015 array storage), zero when none: which cells are elements.
  std::uint32_t stride = 0;
  /// A store reached this object's cells in this activation (so a cell
  /// holding an entry element's value may hold another element's, §4.9).
  bool stored = false;
  Life life = Life::Live;
  std::optional<ReleaseRecord> record = std::nullopt;
  /// The byte offset into the object of the pointer that released it, when
  /// a constant (a release of `p + 1`, RFC 0008's invalid release).
  std::optional<std::int64_t> releaseOffset = std::nullopt;
  std::optional<Extent> extent = std::nullopt;
  /// RFC 0007: the allocation family, and whether this activation owns it.
  std::string family = {};
  bool owned = false;
  /// On this path the object was never made (its allocation failed, or the
  /// store that would have made it did not happen), so it owns nothing
  /// here; a join takes ownership from the paths on which it exists.
  bool absent = false;
  /// Cells that differ from their entry value on exactly the paths where
  /// an entry test holds (`if (!g) g = malloc(…)` leaves `g` so), by key.
  std::vector<std::pair<CellKey, EntryTest>> storedIff = {};
  /// A join kept the object from the paths that made it only: it exists
  /// where the symbol's zero test is `second` (`if (c) p = malloc(n);`
  /// makes it where `c != 0`), which a later test of the symbol decides.
  std::optional<std::pair<Sym, bool>> existsIf = std::nullopt;
  /// The same over an entry test: made on exactly the paths where the test
  /// holds (`if (!g) g = malloc(…)` makes it where `g` was null at entry).
  std::optional<EntryTest> existsIfEntry = std::nullopt;
  /// Its address was stored where a callee or another unit can reach it.
  bool escaped = false;
  bool readonly = false;
  /// `Focus` objects: the objects it may be (never focus objects).
  std::vector<ObjectId> candidates = {};
  /// The symbol whose release released this object, for §4.5 D6.
  Sym releasedBy = ZeroSym;
  /// Every cell never written reads as zero (a zeroing allocation).
  bool zeroed = false;
  /// Cells never written read as uninitialised (a fresh local or a
  /// non-zeroing allocation with zero-initialisation off).
  bool uninitialised = false;
  /// Cells were forgotten by an unknown call: an unwritten cell reads as a
  /// fresh unknown value.
  bool havocked = false;
  /// The byte ranges `[first, second)` whose cells were forgotten (a member
  /// rewritten by code the analysis does not see, a copy over part of the
  /// object), sorted and disjoint: an unwritten cell there reads as a fresh
  /// unknown value, as everywhere in a `havocked` object.
  std::vector<std::pair<std::int64_t, std::int64_t>> forgotten = {};
  /// The byte ranges forgotten on some paths only (a callee that may have
  /// rewritten them, a join with a path that forgot them): an unwritten
  /// cell there reads as its value otherwise, merged with an unknown one.
  std::vector<std::pair<std::int64_t, std::int64_t>> mayForgotten = {};
  /// RFC 0012 *String facts*: a NUL lies at this offset, so the string at
  /// any offset up to it ends there at the latest ...
  std::optional<Term> nulWithin = std::nullopt;
  /// ... and no NUL lies from `nulFrom` up to it: the string at any offset
  /// in between has exactly the length to it.
  std::optional<Term> nulFrom = std::nullopt;
  /// For the summary: some runtime object this object stood for was
  /// released during the activation (kept when members are focused out and
  /// collected, §4.6).
  bool effectReleased = false;
  bool effectMayReleased = false;
  /// For messages: the last place the program stored a pointer to this
  /// object in (`p`, `b->data`).
  std::string holder = {};
  /// The statement that last used a pointer to this object (a frontend
  /// handle), where a leak is reported.
  Handle lastUse = 0;
  // NOLINTEND(readability-redundant-member-init)

  /// Whether an unwritten cell at `key` reads as a fresh unknown value.
  [[nodiscard]] bool forgets(const CellKey &key) const;
  /// Whether an unwritten cell at `key` may read as an unknown value (and
  /// otherwise as it would).
  [[nodiscard]] bool mayForget(const CellKey &key) const;
  /// Whether any of the object's bytes were, or may have been, forgotten.
  [[nodiscard]] bool forgetsAny() const {
    return havocked || !forgotten.empty() || !mayForgotten.empty();
  }

  friend bool operator==(const ObjectState &, const ObjectState &) = default;
};

//===----------------------------------------------------------------------===//
// The state
//===----------------------------------------------------------------------===//

/// One program point's abstract state.
/// RFC 0014: two pointer values known to compare equal (or not); `first <
/// second`.
struct PointerFact {
  Sym first = ZeroSym;
  Sym second = ZeroSym;
  bool equal = true;

  friend bool operator==(const PointerFact &, const PointerFact &) = default;
  friend auto operator<=>(const PointerFact &, const PointerFact &) = default;
};

/// A load through a pointer to two objects of which each path has exactly
/// one (complementary existence): the value it read, while both cells
/// still hold what they held, so that a test of it holds for the next load.
struct MergedLoad {
  ObjectId first = 0;
  CellKey firstKey = {};
  Sym firstValue = ZeroSym;
  ObjectId second = 0;
  CellKey secondKey = {};
  Sym secondValue = ZeroSym;
  Sym merged = ZeroSym;

  friend bool operator==(const MergedLoad &, const MergedLoad &) = default;
};

struct HeapState {
  PMap<ObjectId, ObjectState> objects;
  PMap<Sym, SymInfo> syms;
  Zone zone;
  /// Values of expressions evaluated in one block and used in another
  /// (`?:`, `&&`, `||` operands and branch conditions).
  PMap<Handle, Sym> exprs;
  /// The returned value, at exits.
  Sym result = ZeroSym;
  /// Pointer comparisons the path decided (sorted).
  std::vector<PointerFact> pointerFacts;
  /// Zero tests of entry values the path decided (sorted, one per cell).
  std::vector<EntryTest> entryTests;
  /// Values of loads through complementary objects (not kept by joins).
  std::vector<MergedLoad> mergedLoads;
  /// Pointers made by arithmetic from a maybe-null pointer, each with the
  /// pointer the chain started from (not kept by joins): arithmetic on null
  /// is undefined, so each is null exactly when that one is (`markNonNull`).
  std::vector<std::pair<Sym, Sym>> nullFollows;
  Sym nextSym = 1;
  bool unreachable = false;

  friend bool operator==(const HeapState &, const HeapState &) = default;
};

/// The verdict on the temporal facet of an access through a value.
struct TemporalVerdict {
  enum class Kind : std::uint8_t {
    Proven,
    /// Definitely released (or moved, or out of scope).
    Violation,
    /// Released on some path: a warning (`may-released`, `may-moved`).
    MayReleased,
    /// An object it may point to was released (`may-alias-released`).
    MayAliasReleased,
    /// An unknown callee or open slot may have released it.
    UnknownCallee,
    Callback,
    /// Points to storage whose lifetime may have ended.
    MayDangle,
  };
  Kind kind = Kind::Proven;
  /// The record the verdict rests on, for diagnostics and notes.
  std::optional<ReleaseRecord> record = std::nullopt;
  /// The object, for `ended` storage messages.
  ObjectId object = 0;
};

/// The verdict on a spatial access `[offset, offset + width)`.
struct SpatialVerdict {
  enum class Kind : std::uint8_t {
    Proven,
    Violation,
    /// Undecided against an extent that may be compared against.
    Checkable,
    UnknownExtent,
    UnknownIndex,
  };
  Kind kind = Kind::Proven;
  /// The extent the verdict was made against.
  std::optional<Extent> extent = std::nullopt;
  /// For a violation: how far the access reaches, in bytes, when constant.
  std::optional<std::int64_t> reach = std::nullopt;
  /// For a violation: the end of the access as a term (for messages).
  std::optional<Term> end = std::nullopt;
  /// Accesses before the start.
  bool beforeStart = false;
};

/// The domain operations of RFC 0031 §4 over one function's objects.
class Heap {
public:
  Heap(ObjectTable &objects, const HeapOracle &oracle)
      : table(objects), oracle(oracle) {}

  [[nodiscard]] ObjectTable &objects() noexcept { return table; }
  [[nodiscard]] const ObjectTable &objects() const noexcept { return table; }

  // Symbols.
  Sym fresh(HeapState &state, SymInfo info) const;
  [[nodiscard]] const SymInfo &info(const HeapState &state, Sym sym) const;
  SymInfo &infoMut(HeapState &state, Sym sym) const;
  /// Refines a maybe-null pointer to non-null, with the pointers made from
  /// it or from what it was made from by arithmetic (`nullFollows`).
  void markNonNull(HeapState &state, Sym pointer) const;
  /// An integer symbol with the constant `value`.
  Sym constant(HeapState &state, std::int64_t value,
               std::optional<IntegerType> type = std::nullopt) const;
  /// A pointer symbol to `targets`.
  Sym pointer(HeapState &state, std::vector<Target> targets, PointerNull null,
              std::string name = {}) const;

  // Objects.
  ObjectState &object(HeapState &state, ObjectId id) const;
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
  [[nodiscard]] const ObjectState *findObject(const HeapState &state,
                                              ObjectId id) const {
    return state.objects.find(id);
  }
  /// Adds `id` to the state if absent (fresh: live, no cells).
  ObjectState &ensure(HeapState &state, ObjectId id) const;

  // Memory.
  /// The symbol stored in `cell`, or none when never written (the caller
  /// materialises the entry value, §4.6).
  [[nodiscard]] std::optional<Sym> read(const HeapState &state, ObjectId object,
                                        CellKey key) const;
  /// Stores `value` into `cell`: a strong update, or with `weak` a merge
  /// with the old value into an alias-join symbol (§4.1).
  void write(HeapState &state, ObjectId object, CellKey key, Sym value,
             bool weak) const;
  /// Forgets every cell of `object` in `[from, from + size)` (bytes), or all
  /// cells when `size` is none: they read as unknown values afterwards, and
  /// the object's string facts are dropped.
  void forgetCells(HeapState &state, ObjectId object, std::int64_t from,
                   std::optional<std::int64_t> size) const;
  /// `forgetCells` on some paths only: the cells in the bytes keep their
  /// values, each merged by `unknownLike` with an unknown value of its kind,
  /// and an unwritten cell there reads as unknown.
  void weakenCells(HeapState &state, ObjectId object, std::int64_t from,
                   std::optional<std::int64_t> size,
                   const std::function<Sym(Sym)> &unknownLike) const;
  /// A weak merge of two values into one alias-join symbol.
  Sym mergeWeak(HeapState &state, Sym left, Sym right) const;

  // Elements (§4.2 *Amendment (arrays)*, RFC 0015).
  /// The limits on selected cells and segments per object.
  static constexpr std::size_t MaxSelectedCells = 32;
  static constexpr std::size_t MaxSegmentsPerPosition = 4;
  /// The value a load of `key` reads: the stored value, or the value the
  /// element facts give an element cell (stored into the cell, so a second
  /// load of the key reads the same symbol). A summary key reads "some
  /// element" and stores nothing.
  Sym load(HeapState &state, ObjectId object, CellKey key,
           const SymInfo &hint) const;
  /// The value of the element cell `key` (concrete or selected) that is not
  /// in memory, without storing it: a cell that must be the same, else the
  /// newest segment that must contain it, else the unwritten value and the
  /// summary cell; cells and segments that may be it contribute their values
  /// as possible ones.
  Sym readElement(HeapState &state, ObjectId object, CellKey key,
                  const SymInfo &hint) const;
  /// A fresh symbol with `value`'s attributes: another element's value.
  Sym copyValue(HeapState &state, Sym value) const;
  /// `left`, or a value an element that may be this one holds: like
  /// `mergeWeak`, but a release record of `right` keeps its evidence (a
  /// possible release, not `aliasOnly`).
  Sym mergePossible(HeapState &state, Sym left, Sym right) const;
  /// The join of two values that each describe every element of a range: a
  /// release record definite on both stays definite.
  Sym joinUniform(HeapState &state, Sym left, Sym right) const;
  /// Removes the selected cell `key`; what it held becomes a weak write to
  /// every element it may have been.
  void evictCell(HeapState &state, ObjectId object, CellKey key) const;
  /// Removes segment `index`; its value becomes a weak write to the
  /// elements it described.
  void evictSegment(HeapState &state, ObjectId object, std::size_t index) const;
  /// Removes the segments `evict` marks (by index), as `evictSegment` would
  /// one at a time, newest first, in one pass.
  void evictSegments(HeapState &state, ObjectId object,
                     const std::vector<bool> &evict) const;
  /// Keeps each position's newest `MaxSegmentsPerPosition` segments of the
  /// object, evicting the rest.
  void trimSegments(HeapState &state, ObjectId object) const;
  /// Moves the element cell `key` into the segments: it extends a segment
  /// it is adjacent to, or becomes a one-element segment. False when the
  /// cell is not an element of the object's stride.
  bool foldCell(HeapState &state, ObjectId object, CellKey key) const;
  /// RFC 0015 §5: a callee's range effect on the elements `[from, to)` at
  /// `position`. Each element's value is released with `record`.
  void releaseElements(HeapState &state, ObjectId object, CellKey position,
                       const Term &from, const Term &to,
                       const ReleaseRecord &record, const SymInfo &hint) const;
  /// Each element in `[from, to)` at `position` is written a value
  /// described by `value` (weakly with `weak`).
  void writeElements(HeapState &state, ObjectId object, CellKey position,
                     const Term &from, const Term &to, Sym value,
                     bool weak) const;
  /// RFC 0015 §4: the elements `[from, to)` at `position` are copied from
  /// the entry object `source`, `shift` bytes further on, which no store
  /// has reached: each holds its source element's entry value (a
  /// `Segment::isCopy` range). `value` describes every element.
  void copyElements(HeapState &state, ObjectId object, CellKey position,
                    const Term &from, const Term &to, Sym value,
                    ObjectId source, std::int64_t shift) const;
  /// Whether the elements `[from, to)` at `position` must contain the cell
  /// at byte `offset` (true), cannot (false), or neither.
  [[nodiscard]] std::optional<bool> contains(const HeapState &state,
                                             const CellKey &position,
                                             const Term &from, const Term &to,
                                             const Term &offset) const;
  /// Whether two cells' byte offsets are equal (true), different (false),
  /// or neither.
  [[nodiscard]] std::optional<bool>
  sameCell(const HeapState &state, const Term &first, const Term &second) const;

  // Distinctness (§4.5).
  /// Whether the two objects may stand for the same runtime object; `state`
  /// supplies focus objects' candidates.
  [[nodiscard]] bool mayOverlap(const HeapState &state, ObjectId first,
                                ObjectId second) const;
  /// Whether `object` is reached from `ancestor` through owning steps.
  [[nodiscard]] bool ownedBelow(ObjectId object, ObjectId ancestor) const;

  // Queries.
  [[nodiscard]] TemporalVerdict temporal(const HeapState &state,
                                         Sym pointer) const;
  [[nodiscard]] SpatialVerdict spatial(const HeapState &state, Sym pointer,
                                       const Term &extraOffset,
                                       std::int64_t width) const;
  /// The same for an access of `need` bytes (a term) from the pointer.
  [[nodiscard]] SpatialVerdict spatialRange(const HeapState &state, Sym pointer,
                                            const Term &need) const;
  /// Whether `left <= right` holds for every value (true), for none
  /// (false), or neither (none).
  [[nodiscard]] std::optional<bool>
  lessEqual(const HeapState &state, const Term &left, const Term &right) const;
  /// Whether the value may be null / is null on every path.
  [[nodiscard]] PointerNull nullness(const HeapState &state, Sym sym) const;
  /// RFC 0014: whether two pointer values compare equal, when the path
  /// decided it.
  [[nodiscard]] static std::optional<bool> pointersEqual(const HeapState &state,
                                                         Sym first, Sym second);
  /// Records that two pointer values compare equal (or not); false when the
  /// path already decided the opposite.
  static bool assumePointersEqual(HeapState &state, Sym first, Sym second,
                                  bool equal);

  // Releases.
  /// Releases `pointer`'s value and targets (§5.5 *Effect*). `possibly`:
  /// the call may release them (a range that may not cover the object, RFC
  /// 0034 §6.3): no target becomes definitely released.
  void release(HeapState &state, Sym pointer, const ReleaseRecord &record,
               bool possibly = false) const;

  // Lattice.
  /// The join of two states at the entry of block `block` (§4.8). At a
  /// loop head (`loopHead`, `right` the back edge's state), element cells
  /// the iteration changed are first folded into segments (§4.2
  /// *Amendment (arrays)*). A join inside one expression (the functions a
  /// call may reach, each applied to a copy of one state) passes that
  /// state's `nextSym` as `keepBelow`: a symbol both sides hold under it is
  /// that state's value on both, and keeps its number, and every other
  /// result is numbered from it on, so the values of the expression's other
  /// operands, which the caller still holds, name no other value.
  [[nodiscard]] HeapState join(const HeapState &left, const HeapState &right,
                               Handle block, bool loopHead = false,
                               Sym keepBelow = ZeroSym) const;
  /// The widening of `previous` by `next` (both at one loop head).
  [[nodiscard]] HeapState
  widen(const HeapState &previous, const HeapState &next, Handle block,
        const std::vector<std::int64_t> &thresholds) const;
  /// Whether `a` and `b` are the same state up to how their symbols are
  /// numbered (a join numbers its results in pairing order, so a loop head
  /// that has settled can come back renumbered). Conservative: false when
  /// a symbol it does not follow differs.
  [[nodiscard]] bool equivalent(const HeapState &a, const HeapState &b) const;
  /// Drops objects no root reaches (§4.6): released entry and materialised
  /// objects become dead copies; unreachable heap objects are dropped and
  /// reported to `leaked` when owned, unreleased and not escaped.
  void collect(HeapState &state, const std::vector<ObjectId> &roots,
               const std::function<void(ObjectId)> &leaked) const;

  /// The symbols reachable from `roots` through memory and attributes.
  [[nodiscard]] std::vector<ObjectId>
  reachableObjects(const HeapState &state,
                   const std::vector<ObjectId> &roots) const;

  /// `--dump-analysis` text.
  [[nodiscard]] std::string dump(const HeapState &state) const;

private:
  ObjectTable &table;
  const HeapOracle &oracle;

  [[nodiscard]] SpatialVerdict spatialAt(const HeapState &state, Sym pointer,
                                         const Term &extraOffset,
                                         const Term &need) const;
  /// A store through a summary key: every element at its position may now
  /// hold `value`.
  void writeSummary(HeapState &state, ObjectId object, CellKey key,
                    Sym value) const;
  /// `sym` as a value read through a pointer `hint`: an integer or untyped
  /// value becomes a raw pointer to any object (§4.2).
  Sym asPointer(HeapState &state, Sym sym, const SymInfo &hint) const;
  /// The value of "some element" at a summary key's position.
  Sym anyElement(HeapState &state, ObjectId object, CellKey key,
                 const SymInfo &hint) const;
  /// The value of an element in a range no cell describes: the unwritten
  /// value and the summary cell, joined with every segment that may overlap
  /// the range unless one must cover it.
  Sym rangeValue(HeapState &state, ObjectId object, CellKey position,
                 const Term &from, const Term &to, const SymInfo &hint) const;
  /// The value of the element at byte `offset` of a copied range
  /// (`Segment::isCopy`): its source element's entry value.
  Sym copiedElement(HeapState &state, const Segment &segment,
                    const Term &offset, const SymInfo &hint) const;
  /// Keeps an object within `MaxSelectedCells` and
  /// `MaxSegmentsPerPosition`, evicting the oldest.
  void limitElements(HeapState &state, ObjectId object, CellKey keep) const;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_HEAP_H
