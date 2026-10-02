//===- Heap.cpp - The object engine's abstract heap -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Heap.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace weavec::core {

HeapOracle::~HeapOracle() = default;

std::string_view spell(ObjectKind kind) noexcept {
  switch (kind) {
  case ObjectKind::Local:
    return "local";
  case ObjectKind::Global:
    return "global";
  case ObjectKind::Literal:
    return "literal";
  case ObjectKind::Function:
    return "function";
  case ObjectKind::HeapRecent:
    return "heap";
  case ObjectKind::HeapOld:
    return "heap-old";
  case ObjectKind::Entry:
    return "entry";
  case ObjectKind::EntrySummary:
    return "entry-summary";
  case ObjectKind::Materialized:
    return "materialized";
  case ObjectKind::Focus:
    return "focus";
  case ObjectKind::CallResult:
    return "call-result";
  case ObjectKind::Unknown:
    return "unknown";
  }
  return "?";
}

//===----------------------------------------------------------------------===//
// ObjectTable
//===----------------------------------------------------------------------===//

ObjectId ObjectTable::intern(const ObjectKey &key, ObjectInfo info) {
  if (auto it = byKey.find(key); it != byKey.end())
    return it->second;
  info.key = key;
  objects.push_back(std::move(info));
  auto id = static_cast<ObjectId>(objects.size());
  byKey.emplace(key, id);
  return id;
}

std::optional<ObjectId> ObjectTable::lookup(const ObjectKey &key) const {
  if (auto it = byKey.find(key); it != byKey.end())
    return it->second;
  return std::nullopt;
}

ObjectId ObjectTable::deadCopy(ObjectId id) {
  const ObjectInfo &live = info(id);
  if (live.key.dead)
    return id;
  ObjectKey key = live.key;
  key.dead = true;
  ObjectInfo copy = live;
  return intern(key, std::move(copy));
}

ObjectId ObjectTable::liveVersion(ObjectId id) const {
  const ObjectInfo &object = info(id);
  if (!object.key.dead)
    return id;
  ObjectKey key = object.key;
  key.dead = false;
  if (auto live = lookup(key))
    return *live;
  return id;
}

//===----------------------------------------------------------------------===//
// Terms and records
//===----------------------------------------------------------------------===//

static std::optional<std::int64_t> checkedAdd(std::int64_t a, std::int64_t b) {
  __int128 sum = static_cast<__int128>(a) + b;
  if (sum > INT64_MAX || sum < INT64_MIN)
    return std::nullopt;
  return static_cast<std::int64_t>(sum);
}

std::optional<Term> Term::plus(const Term &other) const {
  if (!known || !other.known)
    return Term::unknown();
  auto constantSum = checkedAdd(constant, other.constant);
  if (!constantSum)
    return std::nullopt;
  bool leftVar = var != ZeroSym && scale != 0;
  bool rightVar = other.var != ZeroSym && other.scale != 0;
  if (!leftVar && !rightVar)
    return Term::of(*constantSum);
  if (leftVar && !rightVar)
    return Term::ofSym(var, scale, *constantSum);
  if (!leftVar && rightVar)
    return Term::ofSym(other.var, other.scale, *constantSum);
  if (var == other.var) {
    auto scaleSum = checkedAdd(scale, other.scale);
    if (!scaleSum)
      return std::nullopt;
    if (*scaleSum == 0)
      return Term::of(*constantSum);
    return Term::ofSym(var, *scaleSum, *constantSum);
  }
  return std::nullopt;
}

Term Term::plusConstant(std::int64_t delta) const {
  if (!known)
    return *this;
  auto sum = checkedAdd(constant, delta);
  if (!sum)
    return Term::unknown();
  Term out = *this;
  out.constant = *sum;
  return out;
}

ReleaseRecord joinRecords(const ReleaseRecord &left,
                          const ReleaseRecord &right) {
  // RFC 0030 §3.1: a known record wins over an unknown one for the
  // reason, location and names; the bits join.
  ReleaseRecord out =
      left.unknownOrigin() && !right.unknownOrigin() ? right : left;
  out.allPaths = left.allPaths && right.allPaths &&
                 left.unknownOrigin() == right.unknownOrigin();
  out.conditional = left.conditional || right.conditional;
  out.lossy = left.lossy || right.lossy;
  out.aliasOnly = left.aliasOnly && right.aliasOnly;
  if (left.unknownOrigin() && right.unknownOrigin())
    out.reason = left.reason;
  // Only the parameter facts both releases had.
  out.paramGuard.clear();
  for (const auto &fact : left.paramGuard)
    if (std::ranges::find(right.paramGuard, fact) != right.paramGuard.end())
      out.paramGuard.push_back(fact);
  out.entryGuard.clear();
  std::ranges::set_intersection(left.entryGuard, right.entryGuard,
                                std::back_inserter(out.entryGuard));
  out.pairGuard.clear();
  for (const ParamPairTest &fact : left.pairGuard)
    if (std::ranges::find(right.pairGuard, fact) != right.pairGuard.end())
      out.pairGuard.push_back(fact);
  out.nonNullLocals.clear();
  std::ranges::set_intersection(left.nonNullLocals, right.nonNullLocals,
                                std::back_inserter(out.nonNullLocals));
  return out;
}

//===----------------------------------------------------------------------===//
// Symbols and objects
//===----------------------------------------------------------------------===//

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
Sym Heap::fresh(HeapState &state, SymInfo info) const {
  Sym sym = state.nextSym++;
  state.syms.set(sym, std::move(info));
  return sym;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
const SymInfo &Heap::info(const HeapState &state, Sym sym) const {
  static const SymInfo None;
  if (const SymInfo *found = state.syms.find(sym))
    return *found;
  return None;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
SymInfo &Heap::infoMut(HeapState &state, Sym sym) const {
  return state.syms.at(sym);
}

void Heap::markNonNull(HeapState &state, Sym pointer) const {
  Sym root = pointer;
  for (const auto &[derived, from] : state.nullFollows)
    if (derived == pointer) {
      root = from;
      break;
    }
  auto refine = [&](Sym sym) {
    const SymInfo *value = state.syms.find(sym);
    if (value != nullptr && value->type == SymInfo::Type::Pointer &&
        value->null == PointerNull::Maybe)
      infoMut(state, sym).null = PointerNull::NonNull;
  };
  refine(root);
  for (const auto &[derived, from] : state.nullFollows)
    if (from == root)
      refine(derived);
}

Sym Heap::constant(HeapState &state, std::int64_t value,
                   std::optional<IntegerType> type) const {
  SymInfo info;
  info.type = SymInfo::Type::Int;
  info.intType = type;
  Sym sym = fresh(state, std::move(info));
  state.zone.addRange(sym, value, value);
  return sym;
}

Sym Heap::pointer(HeapState &state, std::vector<Target> targets,
                  PointerNull null, std::string name) const {
  SymInfo info;
  info.type = SymInfo::Type::Pointer;
  std::ranges::sort(targets);
  targets.erase(std::ranges::unique(targets).begin(), targets.end());
  info.targets = std::move(targets);
  info.null = null;
  info.name = std::move(name);
  return fresh(state, std::move(info));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
ObjectState &Heap::object(HeapState &state, ObjectId id) const {
  return state.objects.at(id);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
ObjectState &Heap::ensure(HeapState &state, ObjectId id) const {
  return state.objects.at(id);
}

//===----------------------------------------------------------------------===//
// Term comparisons (§4.4)
//===----------------------------------------------------------------------===//

/// Lower and upper bounds of a term, from the zone.
namespace {
struct TermRange {
  std::optional<__int128> lo;
  std::optional<__int128> hi;
};
} // namespace

static TermRange rangeOf(const Zone &zone, const Term &term) {
  TermRange out;
  if (!term.known)
    return out;
  if (term.var == ZeroSym || term.scale == 0) {
    out.lo = term.constant;
    out.hi = term.constant;
    return out;
  }
  auto lo = zone.lower(term.var);
  auto hi = zone.upper(term.var);
  if (term.scale > 0) {
    if (lo)
      out.lo = (static_cast<__int128>(*lo) * term.scale) + term.constant;
    if (hi)
      out.hi = (static_cast<__int128>(*hi) * term.scale) + term.constant;
  } else {
    if (hi)
      out.lo = (static_cast<__int128>(*hi) * term.scale) + term.constant;
    if (lo)
      out.hi = (static_cast<__int128>(*lo) * term.scale) + term.constant;
  }
  return out;
}

/// Whether `left <= right` holds for every value (`Proven`), for no value
/// (`Refuted`) or neither.
namespace {
enum class Order : std::uint8_t { Proven, Refuted, Unknown };
} // namespace

static Order compareTerms(const Zone &zone, const Term &left,
                          const Term &right) {
  if (!left.known || !right.known)
    return Order::Unknown;
  // Same variable: compare the linear parts.
  bool leftVar = left.var != ZeroSym && left.scale != 0;
  bool rightVar = right.var != ZeroSym && right.scale != 0;
  if (leftVar && rightVar && left.var == right.var &&
      left.scale == right.scale) {
    return left.constant <= right.constant ? Order::Proven : Order::Refuted;
  }
  // Same scale, different variables: a zone query on the difference.
  if (leftVar && rightVar && left.scale == right.scale && left.scale > 0) {
    // scale*(x - y) <= c2 - c1  <=>  x - y <= floor((c2 - c1) / scale)
    __int128 diff = static_cast<__int128>(right.constant) - left.constant;
    __int128 q = diff >= 0 ? diff / left.scale
                           : -((-diff + left.scale - 1) / left.scale);
    if (q >= INT64_MIN && q <= INT64_MAX &&
        zone.entails(left.var, right.var, static_cast<std::int64_t>(q)))
      return Order::Proven;
    // Refuted when y - x <= ceil-bound making left > right always.
    __int128 strict = q + 1; // x - y >= q + 1  <=>  y - x <= -(q + 1)
    if (-strict >= INT64_MIN && -strict <= INT64_MAX &&
        zone.entails(right.var, left.var, static_cast<std::int64_t>(-strict)))
      return Order::Refuted;
  }
  // A constant against a scaled variable.
  if (!leftVar && rightVar && right.scale > 0) {
    // c1 <= s*y + c2  <=>  y >= ceil((c1 - c2) / s)
    __int128 need = static_cast<__int128>(left.constant) - right.constant;
    __int128 q = need >= 0 ? (need + right.scale - 1) / right.scale
                           : -((-need) / right.scale);
    if (auto lo = zone.lower(right.var); lo && *lo >= q)
      return Order::Proven;
    if (auto hi = zone.upper(right.var); hi && *hi < q)
      return Order::Refuted;
  }
  if (leftVar && !rightVar && left.scale > 0) {
    // s*x + c1 <= c2  <=>  x <= floor((c2 - c1) / s)
    __int128 room = static_cast<__int128>(right.constant) - left.constant;
    __int128 q = room >= 0 ? room / left.scale
                           : -((-room + left.scale - 1) / left.scale);
    if (auto hi = zone.upper(left.var); hi && *hi <= q)
      return Order::Proven;
    if (auto lo = zone.lower(left.var); lo && *lo > q)
      return Order::Refuted;
  }
  TermRange l = rangeOf(zone, left);
  TermRange r = rangeOf(zone, right);
  if (l.hi && r.lo && *l.hi <= *r.lo)
    return Order::Proven;
  if (l.lo && r.hi && *l.lo > *r.hi)
    return Order::Refuted;
  return Order::Unknown;
}

//===----------------------------------------------------------------------===//
// Cell keys and element positions (§4.2 *Amendment (arrays)*)
//===----------------------------------------------------------------------===//

Term CellKey::byteTerm() const {
  if (isSummary())
    return Term::unknown();
  if (isSelected())
    return Term::ofSym(index, stride, offset);
  return Term::of(offset);
}

CellKey CellKey::position() const {
  if (stride == 0)
    return *this;
  auto size = static_cast<std::int64_t>(stride);
  std::int64_t base = offset % size;
  if (base < 0)
    base += size;
  return CellKey{.offset = base, .stride = stride, .index = ZeroSym};
}

std::optional<CellKey> CellKey::at(const Term &offset) {
  if (!offset.known)
    return std::nullopt;
  if (offset.isConstant())
    return CellKey{.offset = offset.constant, .stride = 0, .index = ZeroSym};
  if (offset.scale <= 0 || offset.scale > std::int64_t{1U << 20U})
    return std::nullopt;
  return CellKey{.offset = offset.constant,
                 .stride = static_cast<std::uint32_t>(offset.scale),
                 .index = offset.var};
}

static std::int64_t commonDivisor(std::int64_t a, std::int64_t b) {
  a = a < 0 ? -a : a;
  b = b < 0 ? -b : b;
  while (b != 0) {
    std::int64_t rest = a % b;
    a = b;
    b = rest;
  }
  return a;
}

/// Whether the byte offsets `a` and `b` name the same cell (true), different
/// cells (false), or neither. Cells are keyed by their offsets (§4.2), so
/// offsets with different residues modulo the scales' common divisor are
/// different cells: other fields of the same or another element.
static std::optional<bool> sameOffset(const Zone &zone, const Term &a,
                                      const Term &b) {
  if (!a.known || !b.known)
    return std::nullopt;
  if (a == b)
    return true;
  bool aVar = a.var != ZeroSym && a.scale != 0;
  bool bVar = b.var != ZeroSym && b.scale != 0;
  if (aVar || bVar) {
    std::int64_t divisor =
        commonDivisor(aVar ? a.scale : 0, bVar ? b.scale : 0);
    if (divisor > 1) {
      __int128 difference = static_cast<__int128>(a.constant) - b.constant;
      if (difference % divisor != 0)
        return false;
    }
  }
  Order le = compareTerms(zone, a, b);
  Order ge = compareTerms(zone, b, a);
  if (le == Order::Proven && ge == Order::Proven)
    return true;
  if (le == Order::Refuted || ge == Order::Refuted)
    return false;
  if (compareTerms(zone, a, b.plusConstant(-1)) == Order::Proven ||
      compareTerms(zone, b, a.plusConstant(-1)) == Order::Proven)
    return false;
  return std::nullopt;
}

/// `sym` as `root + delta` (mod 2^width) through the additions and
/// subtractions of constants that computed it in one integer type.
static std::pair<Sym, __int128> constantOffsetRoot(const HeapState &state,
                                                   Sym sym) {
  __int128 delta = 0;
  const SymInfo *info = state.syms.find(sym);
  for (int depth = 0;
       depth < 8 && info != nullptr && info->defined && info->intType &&
       info->defined->right == ZeroSym && info->defined->constant;
       ++depth) {
    const SymDefinition &definition = *info->defined;
    if (definition.op != IntegerOp::Add && definition.op != IntegerOp::Subtract)
      break;
    const SymInfo *base = state.syms.find(definition.left);
    if (base == nullptr || base->intType != info->intType)
      break;
    delta += definition.op == IntegerOp::Add ? *definition.constant
                                             : -*definition.constant;
    sym = definition.left;
    info = base;
  }
  return {sym, delta};
}

/// §4.9: whether two index symbols hold different values on every path,
/// though the zone relates neither to the other: one is the other plus a
/// constant that is not a multiple of 2^width (`i + 1` of an `int` that may
/// overflow still differs from `i`, RFC 0017).
static bool differentValues(const HeapState &state, Sym a, Sym b) {
  if (a == b)
    return false;
  const SymInfo *ai = state.syms.find(a);
  const SymInfo *bi = state.syms.find(b);
  if (ai == nullptr || bi == nullptr || !ai->intType ||
      ai->intType != bi->intType || ai->intType->width == 0 ||
      ai->intType->width > 64)
    return false;
  auto [rootA, deltaA] = constantOffsetRoot(state, a);
  auto [rootB, deltaB] = constantOffsetRoot(state, b);
  if (rootA != rootB)
    return false;
  __int128 difference = deltaA - deltaB;
  if (ai->intType->width < 64)
    difference %= static_cast<__int128>(static_cast<unsigned __int128>(1)
                                        << ai->intType->width);
  else
    difference %=
        static_cast<__int128>(static_cast<unsigned __int128>(1) << 64U);
  return difference != 0;
}

/// `sameOffset`, also telling apart two cells whose index symbols hold
/// different values (`differentValues`) at the same scale and constant.
static std::optional<bool> sameOffset(const HeapState &state, const Term &a,
                                      const Term &b) {
  std::optional<bool> same = sameOffset(state.zone, a, b);
  if (!same && a.known && b.known && a.scale == b.scale && a.scale != 0 &&
      a.constant == b.constant && a.var != ZeroSym && b.var != ZeroSym &&
      differentValues(state, a.var, b.var))
    return false;
  return same;
}

/// The element index of the cell at byte `offset` among the elements at
/// `position`. `at` is false when the cell is not at that position, and
/// unset when that is not known.
namespace {
struct ElementIndex {
  std::optional<bool> at;
  Term index = Term::unknown();
};
} // namespace

static ElementIndex elementIndex(const CellKey &position, const Term &offset) {
  ElementIndex out;
  if (!offset.known || position.stride == 0)
    return out;
  auto size = static_cast<std::int64_t>(position.stride);
  bool var = offset.var != ZeroSym && offset.scale != 0;
  if (var && offset.scale % size != 0)
    return out;
  std::int64_t rest = offset.constant - position.offset;
  if (((rest % size) + size) % size != 0) {
    out.at = false;
    return out;
  }
  out.at = true;
  std::int64_t base = rest / size;
  out.index =
      var ? Term::ofSym(offset.var, offset.scale / size, base) : Term::of(base);
  return out;
}

/// Whether the elements `[from, to)` at `position` contain the cell at byte
/// `offset`.
static Order inRange(const Zone &zone, const CellKey &position,
                     const Term &from, const Term &to, const Term &offset) {
  ElementIndex element = elementIndex(position, offset);
  if (element.at && !*element.at)
    return Order::Refuted;
  if (compareTerms(zone, to, from) == Order::Proven)
    return Order::Refuted; // an empty range
  if (!element.at)
    return Order::Unknown;
  Order lower = compareTerms(zone, from, element.index);
  Order upper = compareTerms(zone, element.index.plusConstant(1), to);
  if (lower == Order::Proven && upper == Order::Proven)
    return Order::Proven;
  if (lower == Order::Refuted || upper == Order::Refuted)
    return Order::Refuted;
  return Order::Unknown;
}

/// Whether element positions may share cells: equal positions, or
/// different strides (cells of one may be cells of the other).
static bool positionsOverlap(const CellKey &a, const CellKey &b) {
  if (a.stride == b.stride)
    return a.offset == b.offset;
  return true;
}

/// Whether two ranges at one position must be disjoint.
static bool rangesDisjoint(const Zone &zone, const Segment &a,
                           const Segment &b) {
  if (!positionsOverlap(a.position, b.position))
    return true;
  if (a.position != b.position)
    return false;
  return compareTerms(zone, a.to, b.from) == Order::Proven ||
         compareTerms(zone, b.to, a.from) == Order::Proven;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
std::optional<bool> Heap::contains(const HeapState &state,
                                   const CellKey &position, const Term &from,
                                   const Term &to, const Term &offset) const {
  switch (inRange(state.zone, position, from, to, offset)) {
  case Order::Proven:
    return true;
  case Order::Refuted:
    return false;
  case Order::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
std::optional<bool> Heap::sameCell(const HeapState &state, const Term &first,
                                   const Term &second) const {
  return sameOffset(state, first, second);
}

//===----------------------------------------------------------------------===//
// Memory
//===----------------------------------------------------------------------===//

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
std::optional<Sym> Heap::read(const HeapState &state, ObjectId object,
                              CellKey key) const {
  const ObjectState *found = state.objects.find(object);
  if (found == nullptr)
    return std::nullopt;
  if (const Sym *sym = found->cells.find(key))
    return *sym;
  return std::nullopt;
}

/// The note of whichever side may be null (RFC 0008): a side that is
/// non-null contributes nothing to the joined value's nullness.
static std::optional<NullOrigin> joinNullOrigins(const SymInfo &a,
                                                 const SymInfo &b) {
  if (a.nullOrigin && a.null != PointerNull::NonNull)
    return a.nullOrigin;
  if (b.nullOrigin && b.null != PointerNull::NonNull)
    return b.nullOrigin;
  return a.nullOrigin ? a.nullOrigin : b.nullOrigin;
}

/// `out`'s entry origins: those of `a` and of `b`.
/// Whether a join of the pointers `a` and `b` takes in a null one beside
/// a value that is not (`SymInfo::nullJoined`).
static bool joinsNull(const SymInfo &a, const SymInfo &b) {
  auto isNull = [](const SymInfo &value) {
    return value.null == PointerNull::Null && !value.uninit;
  };
  return a.nullJoined || b.nullJoined || (isNull(a) && !isNull(b)) ||
         (isNull(b) && !isNull(a));
}

static void uniteEntryOrigins(SymInfo &out, const SymInfo &a,
                              const SymInfo &b) {
  // (Both sorted.)
  if (b.entryOrigins.empty() || a.entryOrigins == b.entryOrigins) {
    out.entryOrigins = a.entryOrigins;
  } else if (a.entryOrigins.empty()) {
    out.entryOrigins = b.entryOrigins;
  } else {
    out.entryOrigins.clear();
    out.entryOrigins.reserve(a.entryOrigins.size() + b.entryOrigins.size());
    std::ranges::set_union(a.entryOrigins, b.entryOrigins,
                           std::back_inserter(out.entryOrigins));
    out.entryOrigins.erase(std::ranges::unique(out.entryOrigins).begin(),
                           out.entryOrigins.end());
  }
  out.entryOriginsLost = a.entryOriginsLost || b.entryOriginsLost;
  if (out.entryOrigins.size() > MaxEntryOrigins) {
    out.entryOrigins.resize(MaxEntryOrigins);
    out.entryOriginsLost = true;
  }
}

Sym Heap::mergeWeak(HeapState &state, Sym left, Sym right) const {
  if (left == right)
    return left;
  const SymInfo &a = info(state, left);
  const SymInfo &b = info(state, right);
  SymInfo out;
  out.type = a.type == b.type ? a.type : SymInfo::Type::Unknown;
  out.name = !a.name.empty() ? a.name : b.name;
  uniteEntryOrigins(out, a, b);
  if (out.type == SymInfo::Type::Int) {
    out.intType = a.intType ? a.intType : b.intType;
    if (a.values && b.values && a.values->type == b.values->type)
      out.values = a.values->united(*b.values);
  } else if (out.type == SymInfo::Type::Pointer) {
    out.top = a.top || b.top;
    if (!out.top) {
      out.targets = a.targets;
      out.targets.insert(out.targets.end(), b.targets.begin(), b.targets.end());
      std::ranges::sort(out.targets);
      out.targets.erase(std::ranges::unique(out.targets).begin(),
                        out.targets.end());
      if (out.targets.size() > 8) {
        out.targets.clear();
        out.top = true;
      }
    }
    out.null = a.null == b.null ? a.null : PointerNull::Maybe;
    out.allocatorSource = a.allocatorSource || b.allocatorSource;
    out.nullOrigin = joinNullOrigins(a, b);
    if (a.release || b.release) {
      ReleaseRecord record = a.release ? *a.release : *b.release;
      if (a.release && b.release)
        record = joinRecords(*a.release, *b.release);
      record.allPaths = false;
      record.aliasOnly = true;
      out.release = record;
    }
    out.raw = a.raw || b.raw;
    out.rawSome = a.rawSome || b.rawSome;
    out.rawAt = a.raw ? a.rawAt : b.rawAt;
    out.rawOrigin = a.raw ? a.rawOrigin : b.rawOrigin;
    out.rawFrom = a.raw ? a.rawFrom : b.rawFrom;
    out.rawVia = a.raw ? a.rawVia : b.rawVia;
    out.rawCast = a.rawCast || b.rawCast;
    out.uninit = a.uninit && b.uninit;
    out.mayUninit = a.uninit || b.uninit || a.mayUninit || b.mayUninit;
    out.nullJoined = joinsNull(a, b);
  } else if (out.type == SymInfo::Type::Function) {
    out.functionsKnown = a.functionsKnown && b.functionsKnown;
    if (out.functionsKnown) {
      out.foreignFunctions = a.foreignFunctions;
      out.foreignFunctions.insert(out.foreignFunctions.end(),
                                  b.foreignFunctions.begin(),
                                  b.foreignFunctions.end());
      std::ranges::sort(out.foreignFunctions);
      out.foreignFunctions.erase(
          std::ranges::unique(out.foreignFunctions).begin(),
          out.foreignFunctions.end());
      out.functions = a.functions;
      out.functions.insert(out.functions.end(), b.functions.begin(),
                           b.functions.end());
      std::ranges::sort(out.functions);
      out.functions.erase(std::ranges::unique(out.functions).begin(),
                          out.functions.end());
      if (out.functions.size() > 32) {
        out.functions.clear();
        out.functionsKnown = false;
      }
    }
  }
  Sym merged = fresh(state, std::move(out));
  if (info(state, merged).type == SymInfo::Type::Int) {
    // The hull of the two ranges.
    auto lo1 = state.zone.lower(left);
    auto lo2 = state.zone.lower(right);
    auto hi1 = state.zone.upper(left);
    auto hi2 = state.zone.upper(right);
    std::optional<std::int64_t> lo;
    std::optional<std::int64_t> hi;
    if (lo1 && lo2)
      lo = std::min(*lo1, *lo2);
    if (hi1 && hi2)
      hi = std::max(*hi1, *hi2);
    state.zone.addRange(merged, lo, hi);
  }
  return merged;
}

Sym Heap::copyValue(HeapState &state, Sym value) const {
  SymInfo copy = info(state, value);
  copy.entryOf.reset();
  copy.pending.clear();
  copy.condition.reset();
  copy.linear.reset();
  copy.unwrapped.reset();
  copy.productAtMost.reset();
  bool integer = copy.type == SymInfo::Type::Int;
  Sym sym = fresh(state, std::move(copy));
  if (integer)
    state.zone.addRange(sym, state.zone.lower(value), state.zone.upper(value));
  return sym;
}

/// A release record that is evidence about this very value (not made by a
/// weak merge).
static bool hasEvidence(const SymInfo &value) {
  return value.release && !value.release->aliasOnly;
}

Sym Heap::mergePossible(HeapState &state, Sym left, Sym right) const {
  if (left == right)
    return left;
  bool evidence =
      hasEvidence(info(state, left)) || hasEvidence(info(state, right));
  Sym merged = mergeWeak(state, left, right);
  if (evidence) {
    SymInfo &out = infoMut(state, merged);
    if (out.release) {
      out.release->aliasOnly = false;
      out.release->allPaths = false;
    }
  }
  return merged;
}

Sym Heap::joinUniform(HeapState &state, Sym left, Sym right) const {
  if (left == right)
    return left;
  std::optional<ReleaseRecord> a = info(state, left).release;
  std::optional<ReleaseRecord> b = info(state, right).release;
  Sym merged = mergeWeak(state, left, right);
  // Every element was released on both sides: every element was released.
  if (a && b)
    infoMut(state, merged).release = joinRecords(*a, *b);
  return merged;
}

Sym Heap::load(HeapState &state, ObjectId object, CellKey key,
               const SymInfo &hint) const {
  ensure(state, object);
  // A summary key reads "some element": its cell holds only what stores
  // through unknown indices wrote.
  if (key.isSummary())
    return anyElement(state, object, key, hint);
  if (auto existing = read(state, object, key))
    return *existing;
  Sym value = readElement(state, object, key, hint);
  state.objects.at(object).cells.set(key, value);
  if (key.isSelected())
    limitElements(state, object, key);
  return value;
}

Sym Heap::readElement(HeapState &state, ObjectId object, CellKey key,
                      const SymInfo &hint) const {
  Term offset = key.byteTerm();
  std::vector<std::pair<CellKey, Sym>> cells;
  std::vector<Segment> segments;
  if (const ObjectState *found = state.objects.find(object)) {
    for (const auto &[cell, sym] : found->cells)
      if (cell != key)
        cells.emplace_back(cell, sym);
    segments = found->segments;
  }
  // Values of cells and ranges that may be this cell.
  std::vector<Sym> possible;
  for (const auto &[cell, sym] : cells) {
    if (cell.isSummary())
      continue;
    std::optional<bool> same = sameOffset(state, offset, cell.byteTerm());
    if (same && *same)
      return sym;
    if (!same)
      possible.push_back(sym);
  }
  Sym value = ZeroSym;
  for (const Segment &segment : segments) {
    Order in =
        inRange(state.zone, segment.position, segment.from, segment.to, offset);
    if (in == Order::Proven) {
      value = segment.isCopy() ? copiedElement(state, segment, offset, hint)
                               : copyValue(state, segment.value);
      break;
    }
    if (in == Order::Unknown)
      possible.push_back(segment.isCopy()
                             ? copiedElement(state, segment, offset, hint)
                             : segment.value);
  }
  if (value == ZeroSym) {
    // No range must hold it: the value no store reached, or one a store
    // through an unknown index left.
    value = oracle.unwritten(state, object, key, hint);
    for (const auto &[cell, sym] : cells)
      if (cell.isSummary()) {
        ElementIndex element = elementIndex(cell, offset);
        if (!element.at || *element.at)
          value = mergeWeak(state, value, sym);
      }
  }
  for (Sym sym : possible)
    value = mergePossible(state, value, sym);
  return value;
}

Sym Heap::anyElement(HeapState &state, ObjectId object, CellKey key,
                     const SymInfo &hint) const {
  Sym value = oracle.unwritten(state, object, key, hint);
  std::vector<Sym> members;
  if (const ObjectState *found = state.objects.find(object)) {
    for (const auto &[cell, sym] : found->cells) {
      if (cell.isSummary()) {
        if (positionsOverlap(cell, key))
          members.push_back(sym);
        continue;
      }
      ElementIndex element = elementIndex(key, cell.byteTerm());
      if (!element.at || *element.at)
        members.push_back(sym);
    }
    for (const Segment &segment : found->segments)
      if (positionsOverlap(segment.position, key))
        members.push_back(segment.value);
  }
  for (Sym sym : members)
    value = mergeWeak(state, value, sym);
  return value;
}

Sym Heap::rangeValue(HeapState &state, ObjectId object, CellKey position,
                     const Term &from, const Term &to,
                     const SymInfo &hint) const {
  const ObjectState &found = ensure(state, object);
  std::vector<Segment> segments = found.segments;
  std::optional<Sym> summary;
  if (const Sym *sym = found.cells.find(position))
    summary = *sym;
  Sym value = ZeroSym;
  bool covered = false;
  Segment range{.position = position, .from = from, .to = to, .value = ZeroSym};
  for (const Segment &segment : segments) {
    if (rangesDisjoint(state.zone, segment, range))
      continue;
    if (segment.position == position &&
        compareTerms(state.zone, segment.from, from) == Order::Proven &&
        compareTerms(state.zone, to, segment.to) == Order::Proven) {
      // The newest range that may overlap covers it.
      value = value == ZeroSym ? segment.value
                               : joinUniform(state, value, segment.value);
      covered = true;
      break;
    }
    value = value == ZeroSym ? segment.value
                             : joinUniform(state, value, segment.value);
  }
  if (!covered) {
    Sym base = oracle.unwritten(state, object, position, hint);
    if (summary)
      base = mergeWeak(state, base, *summary);
    value = value == ZeroSym ? base : joinUniform(state, value, base);
  }
  return value;
}

void Heap::write(HeapState &state, ObjectId objectId, CellKey key, Sym value,
                 bool weak) const {
  ObjectState &written = ensure(state, objectId);
  written.stored = true;
  // A store here, on every path: no longer one entry test's.
  std::erase_if(written.storedIff, [&](const auto &guard) {
    return !key.isConcrete() || guard.first == key;
  });
  if (key.isSummary()) {
    writeSummary(state, objectId, key, value);
    return;
  }
  Term offset = key.byteTerm();
  Sym stored = value;
  if (weak) {
    SymInfo hint = info(state, value);
    stored = mergeWeak(state, load(state, objectId, key, hint), value);
  }
  // Other element cells: the same cell gets the value; one that may be it
  // may now hold it (a weak update).
  std::vector<std::pair<CellKey, Sym>> others;
  for (const auto &[cell, sym] : state.objects.at(objectId).cells)
    if (cell != key && !cell.isSummary())
      others.emplace_back(cell, sym);
  for (const auto &[cell, sym] : others) {
    std::optional<bool> same = sameOffset(state, offset, cell.byteTerm());
    if (same && !*same)
      continue;
    Sym next = same ? stored : mergeWeak(state, sym, value);
    state.objects.at(objectId).cells.set(cell, next);
  }
  // Ranges that may hold the cell no longer describe it exactly; one that
  // must hold it is shadowed by the cell.
  std::vector<Segment> segments = state.objects.at(objectId).segments;
  for (Segment &segment : segments)
    if (inRange(state.zone, segment.position, segment.from, segment.to,
                offset) == Order::Unknown)
      segment.value = mergeWeak(state, segment.value, value);
  state.objects.at(objectId).segments = std::move(segments);
  state.objects.at(objectId).cells.set(key, stored);
  if (key.isSelected())
    limitElements(state, objectId, key);
  // A focus object's candidates may be the object written.
  std::vector<ObjectId> candidates = state.objects.at(objectId).candidates;
  for (ObjectId candidate : candidates) {
    if (!state.objects.contains(candidate))
      continue;
    SymInfo hint = info(state, value);
    Sym old = load(state, candidate, key, hint);
    Sym next = mergeWeak(state, old, value);
    state.objects.at(candidate).cells.set(key, next);
  }
}

void Heap::writeSummary(HeapState &state, ObjectId objectId, CellKey key,
                        Sym value) const {
  const ObjectState &target = state.objects.at(objectId);
  Sym old = ZeroSym;
  if (const Sym *existing = target.cells.find(key))
    old = *existing;
  std::vector<std::pair<CellKey, Sym>> covered;
  for (const auto &[cell, sym] : target.cells) {
    if (cell.isSummary())
      continue;
    ElementIndex element = elementIndex(key, cell.byteTerm());
    if (!element.at || *element.at)
      covered.emplace_back(cell, sym);
  }
  std::vector<Segment> segments = target.segments;
  Sym merged = old == ZeroSym ? value : mergeWeak(state, old, value);
  state.objects.at(objectId).cells.set(key, merged);
  for (const auto &[cell, sym] : covered)
    state.objects.at(objectId).cells.set(cell, mergeWeak(state, sym, value));
  for (Segment &segment : segments)
    if (positionsOverlap(segment.position, key))
      segment.value = mergeWeak(state, segment.value, value);
  state.objects.at(objectId).segments = std::move(segments);
}

void Heap::evictCell(HeapState &state, ObjectId objectId, CellKey key) const {
  auto held = read(state, objectId, key);
  if (!held || key.isSummary())
    return;
  // A concrete cell is an element of the object's stride.
  std::uint32_t stride =
      key.stride != 0 ? key.stride : state.objects.at(objectId).stride;
  if (stride == 0)
    return;
  CellKey position =
      CellKey{.offset = key.offset, .stride = stride, .index = ZeroSym}
          .position();
  Sym value = *held;
  state.objects.at(objectId).cells.erase(key);
  Term offset = key.byteTerm();
  // Every range that may hold the element may now hold its value; so may
  // every element a store through an unknown index reaches, unless a range
  // must hold it.
  std::vector<Segment> segments = state.objects.at(objectId).segments;
  bool covered = false;
  for (Segment &segment : segments) {
    Order in =
        inRange(state.zone, segment.position, segment.from, segment.to, offset);
    if (in == Order::Refuted)
      continue;
    segment.value = mergeWeak(state, segment.value, value);
    if (in == Order::Proven) {
      covered = true;
      break;
    }
  }
  state.objects.at(objectId).segments = std::move(segments);
  if (!covered) {
    const Sym *summary = state.objects.at(objectId).cells.find(position);
    Sym merged = summary == nullptr ? value : mergeWeak(state, *summary, value);
    state.objects.at(objectId).cells.set(position, merged);
  }
}

void Heap::evictSegment(HeapState &state, ObjectId objectId,
                        std::size_t index) const {
  std::vector<Segment> segments = state.objects.at(objectId).segments;
  if (index >= segments.size())
    return;
  Segment removed = segments[index];
  segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(index));
  // Older ranges it shadowed may hold its value; so may any element
  // (through the summary cell).
  for (std::size_t i = index; i < segments.size(); ++i)
    if (!rangesDisjoint(state.zone, segments[i], removed))
      segments[i].value = mergeWeak(state, segments[i].value, removed.value);
  state.objects.at(objectId).segments = std::move(segments);
  const Sym *summary = state.objects.at(objectId).cells.find(removed.position);
  Sym merged = summary == nullptr ? removed.value
                                  : mergeWeak(state, *summary, removed.value);
  state.objects.at(objectId).cells.set(removed.position, merged);
}

void Heap::limitElements(HeapState &state, ObjectId objectId,
                         CellKey keep) const {
  std::vector<CellKey> selected;
  for (const auto &[cell, sym] : state.objects.at(objectId).cells)
    if (cell.isSelected() && cell != keep)
      selected.push_back(cell);
  for (std::size_t i = 0; i + MaxSelectedCells <= selected.size(); ++i)
    evictCell(state, objectId, selected[i]);
  trimSegments(state, objectId);
}

void Heap::evictSegments(HeapState &state, ObjectId objectId,
                         const std::vector<bool> &evict) const {
  std::vector<Segment> segments = state.objects.at(objectId).segments;
  std::vector<Segment> kept;
  kept.reserve(segments.size());
  std::vector<Segment> removed;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i < evict.size() && evict[i]) {
      // (What an evicted one shadowed, the older ranges after it, may hold
      // its value.)
      for (std::size_t j = i + 1; j < segments.size(); ++j)
        if ((j >= evict.size() || !evict[j]) &&
            !rangesDisjoint(state.zone, segments[j], segments[i]))
          segments[j].value =
              mergeWeak(state, segments[j].value, segments[i].value);
      removed.push_back(segments[i]);
      continue;
    }
    kept.push_back(segments[i]);
  }
  if (removed.empty())
    return;
  state.objects.at(objectId).segments = std::move(kept);
  for (const Segment &segment : removed) {
    const Sym *summary =
        state.objects.at(objectId).cells.find(segment.position);
    Sym merged = summary == nullptr ? segment.value
                                    : mergeWeak(state, *summary, segment.value);
    state.objects.at(objectId).cells.set(segment.position, merged);
  }
}

void Heap::trimSegments(HeapState &state, ObjectId objectId) const {
  const std::vector<Segment> &segments = state.objects.at(objectId).segments;
  if (segments.size() <= MaxSegmentsPerPosition)
    return;
  std::map<CellKey, std::size_t> counts;
  std::vector<bool> evict(segments.size(), false);
  bool any = false;
  for (std::size_t i = 0; i < segments.size(); ++i)
    if (++counts[segments[i].position] > MaxSegmentsPerPosition) {
      evict[i] = true;
      any = true;
    }
  if (any)
    evictSegments(state, objectId, evict);
}

bool Heap::foldCell(HeapState &state, ObjectId objectId, CellKey key) const {
  const ObjectState &target = state.objects.at(objectId);
  if (target.stride == 0 || key.isSummary())
    return false;
  auto held = read(state, objectId, key);
  if (!held)
    return false;
  if (key.isSelected() && key.stride % target.stride != 0)
    return false;
  CellKey position =
      CellKey{.offset = key.offset, .stride = target.stride, .index = ZeroSym}
          .position();
  Term offset = key.byteTerm();
  ElementIndex element = elementIndex(position, offset);
  if (!element.at || !*element.at)
    return false;
  Sym value = *held;
  std::vector<Segment> segments = target.segments;
  state.objects.at(objectId).cells.erase(key);
  Term next = element.index.plusConstant(1);
  for (Segment &segment : segments) {
    if (segment.position == position) {
      std::optional<bool> after =
          sameOffset(state.zone, segment.to, element.index);
      std::optional<bool> before = sameOffset(state.zone, segment.from, next);
      if ((after && *after) || (before && *before)) {
        if (after && *after)
          segment.to = next;
        else
          segment.from = element.index;
        segment.value = joinUniform(state, segment.value, value);
        state.objects.at(objectId).segments = std::move(segments);
        return true;
      }
    }
    // A newer range that may hold the element: an older one cannot grow
    // over it.
    if (inRange(state.zone, segment.position, segment.from, segment.to,
                offset) != Order::Refuted)
      break;
  }
  segments.insert(segments.begin(), Segment{.position = position,
                                            .from = element.index,
                                            .to = next,
                                            .value = value});
  state.objects.at(objectId).segments = std::move(segments);
  limitElements(state, objectId, CellKey{});
  return true;
}

void Heap::releaseElements(HeapState &state, ObjectId objectId,
                           CellKey position, const Term &from, const Term &to,
                           const ReleaseRecord &record,
                           const SymInfo &hint) const {
  ensure(state, objectId);
  ReleaseRecord possible = record;
  possible.allPaths = false;
  // The cells in the range.
  std::vector<std::pair<CellKey, Sym>> cells;
  for (const auto &[cell, sym] : state.objects.at(objectId).cells)
    if (!cell.isSummary())
      cells.emplace_back(cell, sym);
  for (const auto &[cell, sym] : cells) {
    Order in = inRange(state.zone, position, from, to, cell.byteTerm());
    if (in == Order::Refuted ||
        info(state, sym).type != SymInfo::Type::Pointer ||
        info(state, sym).null == PointerNull::Null)
      continue;
    release(state, sym, in == Order::Proven ? record : possible);
  }
  // The rest: a range whose elements hold the released values.
  Sym value =
      copyValue(state, rangeValue(state, objectId, position, from, to, hint));
  if (info(state, value).type == SymInfo::Type::Pointer &&
      info(state, value).null != PointerNull::Null)
    release(state, value, record);
  std::vector<Segment> segments = state.objects.at(objectId).segments;
  segments.insert(
      segments.begin(),
      Segment{.position = position, .from = from, .to = to, .value = value});
  state.objects.at(objectId).segments = std::move(segments);
  limitElements(state, objectId, CellKey{});
}

void Heap::writeElements(HeapState &state, ObjectId objectId, CellKey position,
                         const Term &from, const Term &to, Sym value,
                         bool weak) const {
  ensure(state, objectId).storedIff.clear();
  ensure(state, objectId).stored = true;
  std::vector<std::pair<CellKey, Sym>> cells;
  for (const auto &[cell, sym] : state.objects.at(objectId).cells)
    if (!cell.isSummary())
      cells.emplace_back(cell, sym);
  for (const auto &[cell, sym] : cells) {
    Order in = inRange(state.zone, position, from, to, cell.byteTerm());
    if (in == Order::Refuted)
      continue;
    Sym next = in == Order::Proven && !weak ? copyValue(state, value)
                                            : mergeWeak(state, sym, value);
    state.objects.at(objectId).cells.set(cell, next);
  }
  Sym element = value;
  if (weak) {
    SymInfo hint = info(state, value);
    element = mergeWeak(
        state, rangeValue(state, objectId, position, from, to, hint), value);
  }
  std::vector<Segment> segments = state.objects.at(objectId).segments;
  segments.insert(
      segments.begin(),
      Segment{.position = position, .from = from, .to = to, .value = element});
  state.objects.at(objectId).segments = std::move(segments);
  limitElements(state, objectId, CellKey{});
}

void Heap::copyElements(HeapState &state, ObjectId objectId, CellKey position,
                        const Term &from, const Term &to, Sym value,
                        ObjectId source, std::int64_t shift) const {
  ensure(state, objectId).stored = true;
  Segment range{.position = position,
                .from = from,
                .to = to,
                .value = value,
                .source = source,
                .shift = shift,
                .copied = value};
  // The element cells the range must hold take their source element's
  // value; one it may hold may now hold it.
  std::vector<std::pair<CellKey, Sym>> cells;
  for (const auto &[cell, sym] : state.objects.at(objectId).cells)
    if (!cell.isSummary())
      cells.emplace_back(cell, sym);
  for (const auto &[cell, sym] : cells) {
    Order in = inRange(state.zone, position, from, to, cell.byteTerm());
    if (in == Order::Refuted)
      continue;
    SymInfo hint = info(state, sym);
    Sym element = copiedElement(state, range, cell.byteTerm(), hint);
    state.objects.at(objectId).cells.set(
        cell,
        in == Order::Proven ? element : mergePossible(state, sym, element));
  }
  std::vector<Segment> segments = state.objects.at(objectId).segments;
  segments.insert(segments.begin(), range);
  state.objects.at(objectId).segments = std::move(segments);
  limitElements(state, objectId, CellKey{});
}

Sym Heap::copiedElement(HeapState &state, const Segment &segment,
                        const Term &offset, const SymInfo &hint) const {
  std::optional<CellKey> key = CellKey::at(offset.plusConstant(segment.shift));
  if (!key || key->isSummary())
    return copyValue(state, segment.value);
  ObjectId source = segment.source;
  ensure(state, source);
  const auto entryOf = std::make_pair(source, *key);
  if (key->isConcrete()) {
    // The entry value itself, where a load of it left it.
    if (const Sym *held = state.objects.at(source).cells.find(*key);
        held != nullptr && info(state, *held).entryOf == entryOf)
      return *held;
    std::vector<Sym> found;
    for (const auto &[sym, symInfo] : state.syms)
      if (symInfo.entryOf == entryOf)
        found.push_back(sym);
    if (!found.empty()) {
      Sym value = found.front();
      for (std::size_t i = 1; i < found.size(); ++i)
        value = mergePossible(state, value, found[i]);
      return value;
    }
  }
  const ObjectState &object = state.objects.at(source);
  // No store has reached the source since: a load reads the entry value
  // (and later loads of the element read the same symbol).
  if (!object.stored && !object.forgetsAny())
    return load(state, source, *key, hint);
  // Otherwise the entry value, which nothing holds any more.
  std::optional<Sym> held = read(state, source, *key);
  Sym value = oracle.unwritten(state, source, *key, hint);
  if (held)
    state.objects.at(source).cells.set(*key, *held);
  else
    state.objects.at(source).cells.erase(*key);
  return value;
}

static void
addForgotten(std::vector<std::pair<std::int64_t, std::int64_t>> &ranges,
             std::int64_t from, std::int64_t to);

void Heap::weakenCells(HeapState &state, ObjectId objectId, std::int64_t from,
                       std::optional<std::int64_t> size,
                       const std::function<Sym(Sym)> &unknownLike) const {
  if (!state.objects.contains(objectId))
    return;
  std::vector<std::pair<CellKey, Sym>> inside;
  for (const auto &[key, sym] : state.objects.at(objectId).cells) {
    bool within = true;
    if (size && !key.isSummary()) {
      Term offset = key.byteTerm();
      within = compareTerms(state.zone, offset.plusConstant(1),
                            Term::of(from)) != Order::Proven &&
               compareTerms(state.zone, Term::of(from + *size), offset) !=
                   Order::Proven;
    }
    if (within)
      inside.emplace_back(key, sym);
  }
  for (const auto &[key, sym] : inside) {
    Sym merged = mergeWeak(state, sym, unknownLike(sym));
    state.objects.at(objectId).cells.set(key, merged);
  }
  ObjectState &target = state.objects.at(objectId);
  target.storedIff.clear();
  // (Every byte: the widest range the offsets can name.)
  if (!target.havocked)
    addForgotten(target.mayForgotten, size ? from : INT64_MIN / 2,
                 size ? from + *size : INT64_MAX / 2);
  target.nulWithin.reset();
  target.nulFrom.reset();
}

/// Adds `[from, to)` to the sorted, disjoint `ranges`, merging what it
/// overlaps or touches.
static void
addForgotten(std::vector<std::pair<std::int64_t, std::int64_t>> &ranges,
             std::int64_t from, std::int64_t to) {
  if (from >= to)
    return;
  std::vector<std::pair<std::int64_t, std::int64_t>> out;
  out.reserve(ranges.size() + 1);
  bool placed = false;
  for (const auto &range : ranges) {
    if (range.second < from) {
      out.push_back(range);
    } else if (to < range.first) {
      if (!placed) {
        out.emplace_back(from, to);
        placed = true;
      }
      out.push_back(range);
    } else {
      from = std::min(from, range.first);
      to = std::max(to, range.second);
    }
  }
  if (!placed)
    out.emplace_back(from, to);
  std::ranges::sort(out);
  ranges = std::move(out);
}

/// Whether `key` may lie in one of `ranges`. (An element cell's byte is
/// not a constant here: any range may hold it.)
static bool
inRanges(const std::vector<std::pair<std::int64_t, std::int64_t>> &ranges,
         const CellKey &key) {
  if (ranges.empty())
    return false;
  if (!key.isConcrete())
    return true;
  return std::ranges::any_of(ranges, [&](const auto &r) {
    return r.first <= key.offset && key.offset < r.second;
  });
}

bool ObjectState::forgets(const CellKey &key) const {
  return havocked || inRanges(forgotten, key);
}

bool ObjectState::mayForget(const CellKey &key) const {
  return inRanges(mayForgotten, key);
}

/// The ranges both `a` and `b` cover.
static std::vector<std::pair<std::int64_t, std::int64_t>>
intersectRanges(const std::vector<std::pair<std::int64_t, std::int64_t>> &a,
                const std::vector<std::pair<std::int64_t, std::int64_t>> &b) {
  std::vector<std::pair<std::int64_t, std::int64_t>> out;
  for (const auto &x : a)
    for (const auto &y : b) {
      std::int64_t from = std::max(x.first, y.first);
      std::int64_t to = std::min(x.second, y.second);
      if (from < to)
        out.emplace_back(from, to);
    }
  std::ranges::sort(out);
  return out;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
void Heap::forgetCells(HeapState &state, ObjectId objectId, std::int64_t from,
                       std::optional<std::int64_t> size) const {
  if (!state.objects.contains(objectId))
    return;
  const Zone &zone = state.zone;
  ObjectState &target = state.objects.at(objectId);
  target.storedIff.clear();
  target.cells.eraseIf([&](const CellKey &key, Sym) {
    if (!size || key.isSummary())
      return true;
    Term offset = key.byteTerm();
    // Kept only when it must lie outside the bytes forgotten.
    return compareTerms(zone, offset.plusConstant(1), Term::of(from)) !=
               Order::Proven &&
           compareTerms(zone, Term::of(from + *size), offset) != Order::Proven;
  });
  // What the forgotten bytes hold is unknown: an unwritten cell there no
  // longer reads as the zero or uninitialised value of the object's
  // creation, nor as its entry value.
  if (!size) {
    target.havocked = true;
    target.forgotten.clear();
    target.mayForgotten.clear();
  } else if (*size > 0 && !target.havocked) {
    addForgotten(target.forgotten, from, from + *size);
  }
  if (!size || *size > 0) {
    target.nulWithin.reset();
    target.nulFrom.reset();
  }
  if (!size) {
    target.segments.clear();
    return;
  }
  std::vector<Segment> kept;
  for (const Segment &segment : target.segments) {
    auto stride = static_cast<std::int64_t>(segment.position.stride);
    Term start = segment.from;
    Term end = segment.to;
    start.scale *= stride;
    start.constant = (start.constant * stride) + segment.position.offset;
    end.scale *= stride;
    end.constant = (end.constant * stride) + segment.position.offset;
    if (compareTerms(zone, end, Term::of(from)) == Order::Proven ||
        compareTerms(zone, Term::of(from + *size), start) == Order::Proven)
      kept.push_back(segment);
  }
  target.segments = std::move(kept);
}

//===----------------------------------------------------------------------===//
// Distinctness (§4.5)
//===----------------------------------------------------------------------===//

bool Heap::ownedBelow(ObjectId object, ObjectId ancestor) const {
  ObjectId live = table.liveVersion(ancestor);
  ObjectId current = table.liveVersion(object);
  for (int depth = 0; depth < 16 && current != 0; ++depth) {
    const ObjectInfo &info = table.info(current);
    ObjectId parent = info.ownedFrom;
    if (parent == 0)
      return false;
    parent = table.liveVersion(parent);
    if (parent == live)
      return true;
    current = parent;
  }
  return false;
}

/// Whether objects of these kinds existed before the activation began.
static bool isEntryLike(ObjectKind kind) {
  return kind == ObjectKind::Entry || kind == ObjectKind::EntrySummary ||
         kind == ObjectKind::CallResult;
}

bool Heap::mayOverlap(const HeapState &state, ObjectId first,
                      ObjectId second) const {
  if (first == second)
    return true;
  ObjectId a = table.liveVersion(first);
  ObjectId b = table.liveVersion(second);
  // An object and its own dead copy: on the paths where the copy exists, no
  // pointer to the live version does (§4.6).
  if (a == b)
    return false;
  // Focus objects (and their dead copies) may be any of their candidates.
  auto candidatesOf = [&](ObjectId id) -> const std::vector<ObjectId> * {
    if (table.info(id).key.kind != ObjectKind::Focus)
      return nullptr;
    if (const ObjectState *found = state.objects.find(id))
      return &found->candidates;
    return nullptr;
  };
  if (table.info(first).key.kind == ObjectKind::Focus) {
    if (const auto *candidates = candidatesOf(first))
      for (ObjectId candidate : *candidates)
        if (mayOverlap(state, candidate, second))
          return true;
    return false;
  }
  if (table.info(second).key.kind == ObjectKind::Focus) {
    if (const auto *candidates = candidatesOf(second))
      for (ObjectId candidate : *candidates)
        if (mayOverlap(state, first, candidate))
          return true;
    return false;
  }
  const ObjectInfo &x = table.info(a);
  const ObjectInfo &y = table.info(b);
  ObjectKind kx = x.key.kind;
  ObjectKind ky = y.key.kind;
  if (kx == ObjectKind::Unknown || ky == ObjectKind::Unknown)
    return true;
  // A materialised object is one member of its parent; it may be what the
  // parent may be, except the rest of the parent.
  if (kx == ObjectKind::Materialized) {
    if (table.liveVersion(x.key.parent) == b)
      return false;
    return mayOverlap(state, x.key.parent, second);
  }
  if (ky == ObjectKind::Materialized) {
    if (table.liveVersion(y.key.parent) == a)
      return false;
    return mayOverlap(state, first, y.key.parent);
  }
  if (kx == ObjectKind::Function || ky == ObjectKind::Function)
    return false;
  bool entryX = isEntryLike(kx);
  bool entryY = isEntryLike(ky);
  if (!entryX && !entryY)
    return false; // D4, D5: distinct objects this activation created
  if (entryX != entryY) {
    // Entry objects existed before the activation: they cannot be a local,
    // a literal or an allocation the activation made (D4), but a caller's
    // pointer may point to a global or a literal.
    ObjectKind other = entryX ? ky : kx;
    const ObjectInfo &otherInfo = entryX ? y : x;
    const ObjectInfo &entryInfo = entryX ? x : y;
    if (entryInfo.key.kind == ObjectKind::CallResult) {
      // An unknown callee may return anything it could reach: not an
      // allocation of this activation that never escaped (every call to
      // unknown code marks what it can reach escaped, and an escape is never
      // undone, so one that has not escaped now had not at the call).
      if (other == ObjectKind::HeapRecent || other == ObjectKind::HeapOld) {
        const ObjectState *object = state.objects.find(entryX ? b : a);
        if (object != nullptr && !object->escaped)
          return false;
      }
      return other != ObjectKind::Literal ||
             oracle.typesMayAlias(entryInfo.type, otherInfo.type);
    }
    if (other == ObjectKind::Global || other == ObjectKind::Literal)
      return oracle.typesMayAlias(entryInfo.type, otherInfo.type);
    return false;
  }
  // Two entry objects: D1, D2, D3.
  if (!oracle.typesMayAlias(x.type, y.type))
    return false;
  if (x.fromOwningSlot && y.fromOwningSlot)
    return false;
  if (ownedBelow(a, b) || ownedBelow(b, a))
    return false;
  return true;
}

//===----------------------------------------------------------------------===//
// Queries
//===----------------------------------------------------------------------===//

PointerNull Heap::nullness(const HeapState &state, Sym sym) const {
  return info(state, sym).null;
}

std::optional<bool> Heap::pointersEqual(const HeapState &state, Sym first,
                                        Sym second) {
  if (first == second)
    return true;
  if (second < first)
    std::swap(first, second);
  for (const PointerFact &fact : state.pointerFacts)
    if (fact.first == first && fact.second == second)
      return fact.equal;
  return std::nullopt;
}

bool Heap::assumePointersEqual(HeapState &state, Sym first, Sym second,
                               bool equal) {
  if (auto known = pointersEqual(state, first, second))
    return *known == equal;
  if (second < first)
    std::swap(first, second);
  PointerFact fact{.first = first, .second = second, .equal = equal};
  state.pointerFacts.insert(std::ranges::lower_bound(state.pointerFacts, fact),
                            fact);
  return true;
}

static bool isReleasedLife(Life life) {
  return life == Life::Released || life == Life::MayReleased ||
         life == Life::UnknownReleased;
}

TemporalVerdict Heap::temporal(const HeapState &state, Sym pointer) const {
  using Kind = TemporalVerdict::Kind;
  const SymInfo &value = info(state, pointer);
  TemporalVerdict verdict;
  auto rank = [](Kind kind) {
    switch (kind) {
    case Kind::Violation:
      return 6;
    case Kind::MayReleased:
      return 5;
    case Kind::MayDangle:
      return 4;
    case Kind::UnknownCallee:
    case Kind::Callback:
      return 3;
    case Kind::MayAliasReleased:
      return 2;
    case Kind::Proven:
      return 0;
    }
    return 0;
  };
  auto raise = [&](Kind kind, std::optional<ReleaseRecord> record,
                   ObjectId object = 0) {
    if (rank(kind) > rank(verdict.kind)) {
      verdict.kind = kind;
      verdict.record = std::move(record);
      verdict.object = object;
    }
  };
  auto fromRecord = [&](const ReleaseRecord &record, bool single) {
    if (record.reason == ReleaseRecord::Reason::UnknownCallee)
      raise(Kind::UnknownCallee, record);
    else if (record.reason == ReleaseRecord::Reason::Callback)
      raise(Kind::Callback, record);
    else if (record.aliasOnly || !single)
      raise(Kind::MayAliasReleased, record);
    else if (record.definite())
      raise(Kind::Violation, record);
    else
      raise(Kind::MayReleased, record);
  };
  if (value.release)
    fromRecord(*value.release, true);
  bool single = value.targets.size() == 1 && !value.top;
  auto derivedFrom = [&](Sym releaser) {
    if (releaser == ZeroSym)
      return false;
    if (releaser == pointer)
      return false;
    return std::ranges::find(value.ancestors, releaser) !=
           value.ancestors.end();
  };
  for (const Target &target : value.targets) {
    const ObjectState *object = state.objects.find(target.object);
    bool singular = table.info(target.object).singular;
    // Memory the analysis knows nothing about (what an unknown callee left
    // in a cell, an integer made a pointer) may be anything, freed or not
    // (RFC 0030 §5.1).
    if (table.info(target.object).key.kind == ObjectKind::Unknown)
      raise(Kind::UnknownCallee, std::nullopt);
    if (object != nullptr) {
      switch (object->life) {
      case Life::Live:
        break;
      case Life::Released:
      case Life::MayReleased:
        if (object->record && !derivedFrom(object->releasedBy)) {
          ReleaseRecord record = *object->record;
          if (object->life == Life::MayReleased)
            record.allPaths = false;
          fromRecord(record, single && singular);
        } else if (!derivedFrom(object->releasedBy)) {
          raise(Kind::MayAliasReleased, std::nullopt);
        }
        break;
      case Life::UnknownReleased:
        if (object->record)
          fromRecord(*object->record, single && singular);
        else
          raise(Kind::UnknownCallee, std::nullopt);
        break;
      case Life::Ended:
        if (single && singular)
          raise(Kind::Violation, std::nullopt, target.object);
        else
          raise(Kind::MayDangle, std::nullopt, target.object);
        break;
      case Life::MayEnded:
        raise(Kind::MayDangle, std::nullopt, target.object);
        break;
      }
    }
    // Objects released elsewhere that this target may be (§4.5).
    for (const auto &[otherId, other] : state.objects) {
      if (otherId == target.object || !isReleasedLife(other.life))
        continue;
      if (derivedFrom(other.releasedBy))
        continue;
      if (!mayOverlap(state, target.object, otherId))
        continue;
      if (other.life == Life::UnknownReleased) {
        if (other.record &&
            other.record->reason == ReleaseRecord::Reason::Callback)
          raise(Kind::Callback, other.record);
        else
          raise(Kind::UnknownCallee, other.record);
      } else {
        raise(Kind::MayAliasReleased, other.record);
      }
    }
  }
  if (value.top) {
    for (const auto &[otherId, other] : state.objects)
      if (isReleasedLife(other.life)) {
        raise(other.life == Life::UnknownReleased ? Kind::UnknownCallee
                                                  : Kind::MayAliasReleased,
              other.record);
        break;
      }
  }
  return verdict;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
std::optional<bool> Heap::lessEqual(const HeapState &state, const Term &left,
                                    const Term &right) const {
  switch (compareTerms(state.zone, left, right)) {
  case Order::Proven:
    return true;
  case Order::Refuted:
    return false;
  case Order::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

SpatialVerdict Heap::spatial(const HeapState &state, Sym pointer,
                             const Term &extraOffset,
                             std::int64_t width) const {
  return spatialAt(state, pointer, extraOffset, Term::of(width));
}

SpatialVerdict Heap::spatialRange(const HeapState &state, Sym pointer,
                                  const Term &need) const {
  return spatialAt(state, pointer, Term::of(0), need);
}

SpatialVerdict Heap::spatialAt(const HeapState &state, Sym pointer,
                               const Term &extraOffset,
                               const Term &need) const {
  using Kind = SpatialVerdict::Kind;
  const SymInfo &value = info(state, pointer);
  SpatialVerdict out;
  if (value.top || value.targets.empty()) {
    out.kind = Kind::UnknownExtent;
    return out;
  }
  bool allProven = true;
  bool allViolate = true;
  bool anyUnknownExtent = false;
  bool anyUnknownIndex = false;
  std::optional<Extent> firstExtent;
  for (const Target &target : value.targets) {
    const ObjectState *object = state.objects.find(target.object);
    std::optional<Extent> extent;
    if (object != nullptr)
      extent = object->extent;
    if (!extent || !extent->bytes.known) {
      anyUnknownExtent = true;
      allProven = false;
      allViolate = false;
      continue;
    }
    if (!firstExtent)
      firstExtent = extent;
    else if (!(firstExtent->bytes == extent->bytes))
      anyUnknownIndex = true; // different check expressions per target
    auto offset = target.offset.plus(extraOffset);
    if (!offset || !offset->known) {
      anyUnknownIndex = true;
      allProven = false;
      allViolate = false;
      continue;
    }
    auto endSum = need.known ? offset->plus(need) : std::nullopt;
    if (!endSum || !endSum->known) {
      anyUnknownIndex = true;
      allProven = false;
      allViolate = false;
      continue;
    }
    Term end = *endSum;
    Order lower = compareTerms(state.zone, Term::of(0), *offset);
    Order upper = compareTerms(state.zone, end, extent->bytes);
    if (upper != Order::Refuted && extent->unwrapped &&
        compareTerms(state.zone, end, *extent->unwrapped) == Order::Refuted)
      upper = Order::Refuted;
    bool proven = lower == Order::Proven && upper == Order::Proven;
    bool violate = lower == Order::Refuted || upper == Order::Refuted;
    if (!proven)
      allProven = false;
    if (!violate || extent->cls != ExtentClass::Exact)
      allViolate = false;
    if (violate && lower == Order::Refuted)
      out.beforeStart = true;
    if (violate && !out.reach) {
      TermRange r = rangeOf(state.zone, end);
      if (r.lo && *r.lo >= INT64_MIN && *r.lo <= INT64_MAX)
        out.reach = static_cast<std::int64_t>(*r.lo);
    }
    if (violate && !out.end)
      out.end = end;
  }
  out.extent = firstExtent;
  if (allProven) {
    out.kind = Kind::Proven;
    return out;
  }
  if (allViolate) {
    out.kind = Kind::Violation;
    return out;
  }
  if (anyUnknownExtent) {
    out.kind = Kind::UnknownExtent;
    return out;
  }
  if (anyUnknownIndex) {
    out.kind = Kind::UnknownIndex;
    return out;
  }
  if (firstExtent && (firstExtent->cls == ExtentClass::Exact ||
                      firstExtent->cls == ExtentClass::Declared)) {
    out.kind = Kind::Checkable;
    return out;
  }
  out.kind = Kind::UnknownExtent;
  return out;
}

//===----------------------------------------------------------------------===//
// Releases
//===----------------------------------------------------------------------===//

void Heap::release(HeapState &state, Sym pointer,
                   const ReleaseRecord &record) const {
  SymInfo &value = infoMut(state, pointer);
  value.release = record;
  std::vector<Target> targets = value.targets;
  bool single = targets.size() == 1 && !value.top;
  for (const Target &target : targets) {
    ObjectState &object = state.objects.at(target.object);
    bool strong = single && table.info(target.object).singular;
    ReleaseRecord objectRecord = record;
    if (!strong)
      objectRecord.allPaths = false;
    if (object.record && isReleasedLife(object.life))
      objectRecord = joinRecords(*object.record, objectRecord);
    object.record = objectRecord;
    object.releasedBy = pointer;
    object.releaseOffset = target.offset.isConstant()
                               ? std::optional(target.offset.constant)
                               : std::nullopt;
    if (record.unknownOrigin())
      object.life = Life::UnknownReleased;
    else if (strong)
      object.life = Life::Released;
    else if (object.life != Life::Released)
      object.life = Life::MayReleased;
    if (strong)
      object.effectReleased = true;
    else
      object.effectMayReleased = true;
  }
}

//===----------------------------------------------------------------------===//
// Reachability and collection
//===----------------------------------------------------------------------===//

/// Every symbol an object or symbol refers to.
static void referencedSyms(const SymInfo &info, std::vector<Sym> &out) {
  for (const Target &target : info.targets)
    if (target.offset.var != ZeroSym)
      out.push_back(target.offset.var);
  if (info.pointerBehind != ZeroSym)
    out.push_back(info.pointerBehind);
  if (info.linear && info.linear->var != ZeroSym)
    out.push_back(info.linear->var);
  if (info.unwrapped && info.unwrapped->var != ZeroSym)
    out.push_back(info.unwrapped->var);
  // The operands of the operation that computed it, which a witness spells
  // when no C place holds the value (RFC 0031 §5.3).
  if (info.defined) {
    if (info.defined->left != ZeroSym)
      out.push_back(info.defined->left);
    if (info.defined->right != ZeroSym)
      out.push_back(info.defined->right);
  }
}

std::vector<ObjectId>
Heap::reachableObjects(const HeapState &state,
                       const std::vector<ObjectId> &roots) const {
  std::set<ObjectId> seen;
  std::deque<ObjectId> work;
  auto visitSym = [&](Sym sym, auto &self) -> void {
    const SymInfo &value = info(state, sym);
    for (const Target &target : value.targets)
      if (seen.insert(target.object).second)
        work.push_back(target.object);
    if (value.pointerBehind != ZeroSym)
      self(value.pointerBehind, self);
  };
  for (ObjectId root : roots)
    if (seen.insert(root).second)
      work.push_back(root);
  for (const auto &[handle, sym] : state.exprs)
    visitSym(sym, visitSym);
  if (state.result != ZeroSym)
    visitSym(state.result, visitSym);
  while (!work.empty()) {
    ObjectId id = work.front();
    work.pop_front();
    const ObjectState *object = state.objects.find(id);
    if (object == nullptr)
      continue;
    // A released object's storage holds nothing any more.
    if (object->life == Life::Released)
      continue;
    for (const auto &[key, sym] : object->cells)
      visitSym(sym, visitSym);
    for (const Segment &segment : object->segments)
      visitSym(segment.value, visitSym);
    for (ObjectId candidate : object->candidates)
      if (seen.insert(candidate).second)
        work.push_back(candidate);
  }
  return {seen.begin(), seen.end()};
}

/// Calls `visit` on every symbol field of `value`, in a fixed order, and
/// leaves each zero, so what remains compares by value.
template <typename Visit>
static void stripSyms(SymInfo &value, Visit visit) {
  auto sym = [&](Sym &field) {
    visit(field);
    field = ZeroSym;
  };
  auto term = [&](Term &field) { sym(field.var); };
  if (value.condition) {
    sym(value.condition->left);
    sym(value.condition->right);
  }
  sym(value.pointerBehind);
  if (value.productAtMost)
    sym(value.productAtMost->first);
  for (Target &target : value.targets)
    term(target.offset);
  for (Sym &ancestor : value.ancestors)
    sym(ancestor);
  for (PendingCase &pending : value.pending) {
    sym(pending.subject);
    sym(pending.stored);
    sym(pending.previous);
    if (pending.argumentZero)
      sym(pending.argumentZero->first);
  }
  if (value.linear)
    term(*value.linear);
  if (value.unwrapped)
    term(*value.unwrapped);
  if (value.defined) {
    sym(value.defined->left);
    sym(value.defined->right);
  }
}

template <typename Visit>
static void stripSyms(ObjectState &object, Visit visit) {
  auto sym = [&](Sym &field) {
    visit(field);
    field = ZeroSym;
  };
  auto term = [&](Term &field) { sym(field.var); };
  PMap<CellKey, Sym> cells;
  for (const auto &[key, value] : object.cells) {
    Sym held = value;
    sym(held);
    cells.set(key, ZeroSym);
  }
  object.cells = std::move(cells);
  for (Segment &segment : object.segments) {
    term(segment.from);
    term(segment.to);
    sym(segment.value);
    sym(segment.copied);
  }
  if (object.extent) {
    term(object.extent->bytes);
    if (object.extent->unwrapped)
      term(*object.extent->unwrapped);
  }
  if (object.nulWithin)
    term(*object.nulWithin);
  if (object.nulFrom)
    term(*object.nulFrom);
  sym(object.releasedBy);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Heap API
bool Heap::equivalent(const HeapState &a, const HeapState &b) const {
  if (a.unreachable != b.unreachable || a.syms.size() != b.syms.size() ||
      a.objects.size() != b.objects.size() ||
      a.exprs.size() != b.exprs.size() ||
      a.pointerFacts.size() != b.pointerFacts.size() ||
      !(a.entryTests == b.entryTests))
    return false;
  std::map<Sym, Sym> forward;
  std::map<Sym, Sym> backward;
  std::vector<std::pair<Sym, Sym>> work;
  auto unify = [&](Sym x, Sym y) {
    if (x == ZeroSym || y == ZeroSym)
      return x == y;
    auto f = forward.find(x);
    auto g = backward.find(y);
    if (f != forward.end() || g != backward.end())
      return f != forward.end() && g != backward.end() && f->second == y &&
             g->second == x;
    forward.emplace(x, y);
    backward.emplace(y, x);
    work.emplace_back(x, y);
    return true;
  };
  auto unifyAll = [&](const std::vector<Sym> &xs, const std::vector<Sym> &ys) {
    if (xs.size() != ys.size())
      return false;
    for (std::size_t i = 0; i < xs.size(); ++i)
      if (!unify(xs[i], ys[i]))
        return false;
    return true;
  };
  // Objects, cell by cell (a selected cell's key names a symbol, and its
  // order depends on the numbering: such an object compares as is).
  auto ia = a.objects.begin();
  auto ib = b.objects.begin();
  for (; ia != a.objects.end(); ++ia, ++ib) {
    if (ia->first != ib->first)
      return false;
    bool selected = false;
    for (const auto &[key, value] : ia->second.cells)
      selected = selected || key.isSelected();
    for (const auto &[key, value] : ib->second.cells)
      selected = selected || key.isSelected();
    if (selected) {
      if (!(ia->second == ib->second))
        return false;
      for (const auto &[key, value] : ia->second.cells)
        if (!unify(value, value))
          return false;
      continue;
    }
    ObjectState x = ia->second;
    ObjectState y = ib->second;
    std::vector<Sym> xs;
    std::vector<Sym> ys;
    stripSyms(x, [&](Sym s) { xs.push_back(s); });
    stripSyms(y, [&](Sym s) { ys.push_back(s); });
    if (!(x == y) || !unifyAll(xs, ys))
      return false;
  }
  auto ea = a.exprs.begin();
  auto eb = b.exprs.begin();
  for (; ea != a.exprs.end(); ++ea, ++eb)
    if (ea->first != eb->first || !unify(ea->second, eb->second))
      return false;
  if (!unify(a.result, b.result))
    return false;
  // What each symbol pair says, and the symbols that names.
  // NOLINTNEXTLINE(modernize-loop-convert): unifying appends to the work list
  for (std::size_t done = 0; done < work.size(); ++done) {
    auto [x, y] = work[done];
    const SymInfo *ix = a.syms.find(x);
    const SymInfo *iy = b.syms.find(y);
    if (ix == nullptr || iy == nullptr) {
      if (ix != iy)
        return false;
      continue;
    }
    SymInfo cx = *ix;
    SymInfo cy = *iy;
    std::vector<Sym> xs;
    std::vector<Sym> ys;
    stripSyms(cx, [&](Sym s) { xs.push_back(s); });
    stripSyms(cy, [&](Sym s) { ys.push_back(s); });
    if (!(cx == cy) || !unifyAll(xs, ys))
      return false;
  }
  // Every symbol accounted for, and the zone and the facts the same over
  // the correspondence.
  if (forward.size() != a.syms.size())
    return false;
  for (Sym s : a.zone.symbols())
    if (s != ZeroSym && !forward.contains(s))
      return false;
  Zone mapped = a.zone.renamed([&](Sym s) {
    auto it = forward.find(s);
    return it != forward.end() ? it->second : ZeroSym;
  });
  if (!(mapped == b.zone))
    return false;
  std::vector<PointerFact> facts;
  for (const PointerFact &fact : a.pointerFacts) {
    auto first = forward.find(fact.first);
    auto second = forward.find(fact.second);
    if (first == forward.end() || second == forward.end())
      return false;
    PointerFact renamedFact{.first = std::min(first->second, second->second),
                            .second = std::max(first->second, second->second),
                            .equal = fact.equal};
    facts.push_back(renamedFact);
  }
  std::ranges::sort(facts);
  if (!(facts == b.pointerFacts))
    return false;
  if (a.mergedLoads.size() != b.mergedLoads.size())
    return false;
  for (std::size_t i = 0; i < a.mergedLoads.size(); ++i) {
    MergedLoad x = a.mergedLoads[i];
    const MergedLoad &y = b.mergedLoads[i];
    auto map = [&](Sym s) {
      if (s == ZeroSym)
        return ZeroSym;
      auto it = forward.find(s);
      return it != forward.end() ? it->second : Sym{~0U};
    };
    x.firstValue = map(x.firstValue);
    x.secondValue = map(x.secondValue);
    x.merged = map(x.merged);
    if (!(x == y))
      return false;
  }
  return true;
}

void Heap::collect(HeapState &state, const std::vector<ObjectId> &roots,
                   const std::function<void(ObjectId)> &leaked) const {
  if (state.unreachable)
    return;
  std::vector<ObjectId> reachable = reachableObjects(state, roots);
  std::set<ObjectId> keep(reachable.begin(), reachable.end());
  std::vector<ObjectId> drop;
  for (const auto &[id, object] : state.objects)
    if (!keep.contains(id))
      drop.push_back(id);
  for (ObjectId id : drop) {
    const ObjectInfo &objectInfo = table.info(id);
    ObjectState object = *state.objects.find(id);
    ObjectKind kind = objectInfo.key.kind;
    bool released = isReleasedLife(object.life) || object.effectReleased ||
                    object.effectMayReleased;
    switch (kind) {
    case ObjectKind::Entry:
    case ObjectKind::EntrySummary:
    case ObjectKind::CallResult:
    case ObjectKind::Focus:
    case ObjectKind::Materialized:
      // Entry objects stay for the summary; once released and
      // unreachable they become dead copies (§4.6).
      if (objectInfo.key.dead)
        continue;
      if (!released && kind != ObjectKind::Focus &&
          kind != ObjectKind::Materialized)
        continue;
      state.objects.erase(id);
      if (released) {
        ObjectId dead = table.deadCopy(id);
        object.cells = {};
        object.segments.clear();
        if (const ObjectState *existing = state.objects.find(dead)) {
          ObjectState merged = *existing;
          merged.effectReleased =
              merged.effectReleased || object.effectReleased;
          merged.effectMayReleased =
              merged.effectMayReleased || object.effectMayReleased;
          merged.life =
              merged.life == object.life ? merged.life : Life::MayReleased;
          if (object.record && merged.record)
            merged.record = joinRecords(*merged.record, *object.record);
          else if (object.record)
            merged.record = object.record;
          for (ObjectId candidate : object.candidates)
            if (std::ranges::find(merged.candidates, candidate) ==
                merged.candidates.end())
              merged.candidates.push_back(candidate);
          merged.releasedBy = ZeroSym;
          state.objects.set(dead, merged);
        } else {
          object.releasedBy = ZeroSym;
          state.objects.set(dead, object);
        }
      }
      break;
    case ObjectKind::HeapRecent:
    case ObjectKind::HeapOld:
      if (object.owned && !isReleasedLife(object.life) && !object.escaped &&
          object.life != Life::Ended && leaked)
        leaked(id);
      state.objects.erase(id);
      break;
    case ObjectKind::Local:
    case ObjectKind::Global:
    case ObjectKind::Literal:
    case ObjectKind::Function:
    case ObjectKind::Unknown:
      if (object.life == Life::Ended || kind == ObjectKind::Literal ||
          kind == ObjectKind::Function)
        state.objects.erase(id);
      break;
    }
  }
  // Symbols nothing refers to are dropped with their zone rows.
  std::set<Sym> live;
  std::vector<Sym> pending;
  auto hold = [&](Sym sym) {
    if (sym != ZeroSym && live.insert(sym).second)
      pending.push_back(sym);
  };
  for (const auto &[id, object] : state.objects) {
    for (const auto &[key, sym] : object.cells) {
      hold(sym);
      hold(key.index);
    }
    for (const Segment &segment : object.segments) {
      hold(segment.value);
      hold(segment.from.var);
      hold(segment.to.var);
    }
    if (object.extent && object.extent->bytes.var != ZeroSym)
      hold(object.extent->bytes.var);
    if (object.extent && object.extent->unwrapped &&
        object.extent->unwrapped->var != ZeroSym)
      hold(object.extent->unwrapped->var);
    if (object.nulWithin && object.nulWithin->var != ZeroSym)
      hold(object.nulWithin->var);
    if (object.nulFrom && object.nulFrom->var != ZeroSym)
      hold(object.nulFrom->var);
    hold(object.releasedBy);
  }
  for (const auto &[handle, sym] : state.exprs)
    hold(sym);
  hold(state.result);
  while (!pending.empty()) {
    Sym sym = pending.back();
    pending.pop_back();
    std::vector<Sym> refs;
    const SymInfo &value = info(state, sym);
    referencedSyms(value, refs);
    for (Sym ancestor : value.ancestors)
      refs.push_back(ancestor);
    if (value.condition) {
      refs.push_back(value.condition->left);
      if (!value.condition->rightIsConstant)
        refs.push_back(value.condition->right);
    }
    for (const PendingCase &pendingCase : value.pending) {
      refs.push_back(pendingCase.subject);
      refs.push_back(pendingCase.stored);
      refs.push_back(pendingCase.previous);
    }
    for (Sym ref : refs)
      hold(ref);
  }
  state.syms.eraseIf(
      [&](Sym sym, const SymInfo &) { return !live.contains(sym); });
  state.zone.restrict([&](Sym sym) { return live.contains(sym); });
  std::erase_if(state.nullFollows, [&](const std::pair<Sym, Sym> &link) {
    return !live.contains(link.first);
  });
}

//===----------------------------------------------------------------------===//
// Join and widening (§4.8)
//===----------------------------------------------------------------------===//

namespace {
/// The pairing of two states' symbols into result symbols.
class Pairing {
public:
  Pairing(const Heap &heap, HeapState &left, HeapState &right, HeapState &out,
          Sym keepBelow = ZeroSym)
      : heap(heap), left(left), right(right), out(out), keepBelow(keepBelow) {
    out.nextSym = std::max(out.nextSym, keepBelow);
  }

  /// The result symbol for `(a, b)`; zero on a side means the side has no
  /// such value.
  Sym pair(Sym a, Sym b) {
    if (a == ZeroSym && b == ZeroSym)
      return ZeroSym;
    const std::uint64_t key = keyOf(a, b);
    if (auto it = results.find(key); it != results.end())
      return it->second;
    Sym result = a == b && a < keepBelow ? a : out.nextSym++;
    results.emplace(key, result);
    if (a != ZeroSym && b != ZeroSym) {
      bothLeft.try_emplace(a, result);
      bothRight.try_emplace(b, result);
    }
    pairs.push_back(SymPair{.result = result,
                            .left = a,
                            .right = b,
                            .hasLeft = a != ZeroSym,
                            .hasRight = b != ZeroSym});
    return result;
  }

  /// `pair` for a cell of an object both sides have that one side never
  /// wrote (it reads the entry value there, or nothing): a release of the
  /// value happened on the other side's paths only.
  Sym pairOneSided(Sym a, Sym b) {
    Sym result = pair(a, b);
    oneSidedCells.insert(result);
    return result;
  }

  Term pairTerm(const Term &a, const Term *b) {
    if (!a.known)
      return a;
    if (b != nullptr) {
      if (!b->known)
        return Term::unknown();
      if (a.scale == b->scale && a.constant == b->constant &&
          (a.var == ZeroSym) == (b->var == ZeroSym)) {
        if (a.var == ZeroSym)
          return a;
        return Term::ofSym(pair(a.var, b->var), a.scale, a.constant);
      }
      // §4.8: terms of different shapes (a cursor at offset 0 on one path
      // and 1 on the other): one symbol per side equal to its term, joined,
      // so the zone keeps the range and the relations of each.
      Sym x = sideSym(left, a);
      Sym y = sideSym(right, *b);
      if (x == ZeroSym || y == ZeroSym)
        return Term::unknown();
      return Term::ofSym(pair(x, y), 1, 0);
    }
    if (a.var == ZeroSym)
      return a;
    return Term::ofSym(onlyLeft(a.var), a.scale, a.constant);
  }
  Term pairTermRight(const Term &b) {
    if (!b.known || b.var == ZeroSym)
      return b;
    return Term::ofSym(onlyRight(b.var), b.scale, b.constant);
  }
  /// A new integer symbol of `state` equal to `term` (bounded by it when
  /// its scale is not 1).
  Sym sideSym(HeapState &state, const Term &term) {
    if (!term.known)
      return ZeroSym;
    SymInfo info;
    info.type = SymInfo::Type::Int;
    Sym sym = heap.fresh(state, info);
    if (term.isConstant()) {
      state.zone.addRange(sym, term.constant, term.constant);
    } else if (term.scale == 1) {
      state.zone.addEq(sym, term.var, term.constant);
    } else {
      TermRange range = rangeOf(state.zone, term);
      auto fit = [](std::optional<__int128> v) -> std::optional<std::int64_t> {
        if (v && *v >= INT64_MIN && *v <= INT64_MAX)
          return static_cast<std::int64_t>(*v);
        return std::nullopt;
      };
      state.zone.addRange(sym, fit(range.lo), fit(range.hi));
    }
    return sym;
  }
  /// The result for a value only one side's term uses (an object or target
  /// only that side has): a result that already pairs it with the other
  /// side equals it on every path of its side, and the term holds on no
  /// other path, so sharing it keeps the term related to the cells that
  /// hold the value (an entry object's extent and the count field it was
  /// read from).
  Sym onlyLeft(Sym a) {
    auto it = bothLeft.find(a);
    return it != bothLeft.end() ? it->second : pair(a, ZeroSym);
  }
  Sym onlyRight(Sym b) {
    auto it = bothRight.find(b);
    return it != bothRight.end() ? it->second : pair(ZeroSym, b);
  }

  /// Computes the attributes of every result symbol.
  void finish(bool widen) {
    // (Joining attributes may pair more symbols: `pairs` grows meanwhile.)
    for (; finished < pairs.size(); ++finished) {
      SymPair p = pairs[finished];
      out.syms.set(p.result, joinInfo(p, widen));
    }
  }

  /// The result symbol of `(a, b)` when the join made one.
  [[nodiscard]] Sym find(Sym a, Sym b) const {
    auto it = results.find(keyOf(a, b));
    return it != results.end() ? it->second : ZeroSym;
  }

  /// The unique result symbol a left (or right) symbol maps to, if unique.
  Sym uniqueLeft(Sym a) const { return unique(a, true); }
  Sym uniqueRight(Sym b) const { return unique(b, false); }

  std::vector<SymPair> pairs;

private:
  const Heap &heap;
  HeapState &left;
  HeapState &right;
  HeapState &out;
  /// `Heap::join`'s `keepBelow`.
  Sym keepBelow = ZeroSym;
  static std::uint64_t keyOf(Sym a, Sym b) {
    return (static_cast<std::uint64_t>(a) << 32U) | b;
  }
  std::unordered_map<std::uint64_t, Sym> results;
  /// The first result pairing each side's symbol with the other side's.
  std::unordered_map<Sym, Sym> bothLeft;
  std::unordered_map<Sym, Sym> bothRight;
  /// Results of `pairOneSided`.
  std::unordered_set<Sym> oneSidedCells;
  /// The pairs whose attributes `finish` has computed.
  std::size_t finished = 0;

  Sym unique(Sym sym, bool isLeft) const {
    Sym found = ZeroSym;
    for (const SymPair &p : pairs) {
      Sym side = isLeft ? p.left : p.right;
      if (side != sym)
        continue;
      if (found != ZeroSym)
        return ZeroSym;
      found = p.result;
    }
    return found;
  }

  SymInfo joinInfo(const SymPair &p, bool widen) {
    // (Pairing terms may add symbols to either side; a map holds each
    // symbol's attributes in a box of its own, which that leaves in place.)
    static_assert(PMap<Sym, SymInfo>::StableValues);
    const SymInfo *a = p.hasLeft ? left.syms.find(p.left) : nullptr;
    const SymInfo *b = p.hasRight ? right.syms.find(p.right) : nullptr;
    static const SymInfo None;
    if (p.hasLeft && a == nullptr)
      a = &None;
    if (p.hasRight && b == nullptr)
      b = &None;
    if (a == nullptr || b == nullptr) {
      // (`pair` makes no pair without a side.)
      // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker): one is set
      SymInfo joined = a == nullptr ? renamed(*b, false) : renamed(*a, true);
      if (joined.release && oneSidedCells.contains(p.result))
        joined.release->allPaths = false;
      return joined;
    }
    SymInfo joined;
    joined.type = a->type == b->type ? a->type : SymInfo::Type::Unknown;
    joined.name = !a->name.empty() ? a->name : b->name;
    // The same cell's entry value on both sides.
    if (a->entryOf == b->entryOf)
      joined.entryOf = a->entryOf;
    uniteEntryOrigins(joined, *a, *b);
    if (joined.type == SymInfo::Type::Int) {
      joined.intType = a->intType ? a->intType : b->intType;
      joined.nonZero = a->nonZero && b->nonZero;
      joined.ctype = a->ctype == b->ctype ? a->ctype : 0;
      // §4.4: intervals beyond the zone join as their hull; a widening
      // forgets one that grew (the type's range).
      if (a->values && b->values && a->values->type == b->values->type &&
          (!widen || *a->values == *b->values))
        joined.values = a->values->united(*b->values);
      // §5.3: a value both sides computed by the same operation is that
      // operation of the paired operands on every path.
      // (Only over operands both sides still hold, so a definition never
      // keeps a chain of earlier values alive.)
      auto held = [](const HeapState &state, Sym sym) {
        return sym == ZeroSym || state.syms.contains(sym);
      };
      if (a->defined && b->defined && a->defined->op == b->defined->op &&
          a->defined->constant == b->defined->constant &&
          (a->defined->right == ZeroSym) == (b->defined->right == ZeroSym) &&
          joined.ctype != 0 && held(left, a->defined->left) &&
          held(left, a->defined->right) && held(right, b->defined->left) &&
          held(right, b->defined->right))
        joined.defined = SymDefinition{
            .op = a->defined->op,
            .left = pair(a->defined->left, b->defined->left),
            .right = pair(a->defined->right, b->defined->right),
            .constant = a->defined->constant,
            .exact = a->defined->exact && b->defined->exact,
            .leftValue = a->defined->leftValue == b->defined->leftValue
                             ? a->defined->leftValue
                             : std::nullopt};
      if (a->linear && b->linear && a->linear->scale == b->linear->scale &&
          a->linear->constant == b->linear->constant) {
        Term linear = pairTerm(*a->linear, &*b->linear);
        if (linear.known && !linear.isConstant())
          joined.linear = linear;
      }
      if (a->unwrapped && b->unwrapped &&
          a->unwrapped->scale == b->unwrapped->scale &&
          a->unwrapped->constant == b->unwrapped->constant) {
        Term unwrapped = pairTerm(*a->unwrapped, &*b->unwrapped);
        if (unwrapped.known && !unwrapped.isConstant())
          joined.unwrapped = unwrapped;
      }
    } else if (joined.type == SymInfo::Type::Pointer) {
      joined.top = a->top || b->top;
      if (!joined.top) {
        std::map<ObjectId, Term> merged;
        std::set<ObjectId> both;
        for (const Target &t : a->targets)
          for (const Target &u : b->targets)
            if (t.object == u.object) {
              both.insert(t.object);
              merged[t.object] = pairTerm(t.offset, &u.offset);
            }
        for (const Target &t : a->targets)
          if (!both.contains(t.object))
            merged[t.object] = pairTerm(t.offset, nullptr);
        for (const Target &u : b->targets)
          if (!both.contains(u.object))
            merged[u.object] = pairTermRight(u.offset);
        for (const auto &[object, offset] : merged)
          joined.targets.push_back(Target{.object = object, .offset = offset});
        if (joined.targets.size() > 8) {
          joined.targets.clear();
          joined.top = true;
        }
      }
      joined.null = a->null == b->null ? a->null : PointerNull::Maybe;
      joined.allocatorSource = a->allocatorSource || b->allocatorSource;
      joined.nullOrigin = joinNullOrigins(*a, *b);
      if (a->release && b->release) {
        joined.release = joinRecords(*a->release, *b->release);
      } else if (a->release || b->release) {
        ReleaseRecord record = a->release ? *a->release : *b->release;
        record.allPaths = false;
        joined.release = record;
      }
      joined.raw = a->raw || b->raw;
      joined.rawSome = a->rawSome || b->rawSome;
      joined.rawAt = a->raw ? a->rawAt : b->rawAt;
      joined.rawOrigin = a->raw ? a->rawOrigin : b->rawOrigin;
      joined.rawFrom = a->raw ? a->rawFrom : b->rawFrom;
      joined.rawVia = a->raw ? a->rawVia : b->rawVia;
      joined.rawCast = a->rawCast || b->rawCast;
      if (a->shares && b->shares && *a->shares == *b->shares)
        joined.shares = a->shares;
      joined.uninit = a->uninit && b->uninit;
      joined.mayUninit = a->uninit || b->uninit || a->mayUninit || b->mayUninit;
      joined.nullJoined = joinsNull(*a, *b);
      joined.derived = a->derived && b->derived;
      // Ancestors both sides agree on.
      for (Sym ancestor : a->ancestors) {
        Sym l = uniqueLeft(ancestor);
        if (l == ZeroSym)
          continue;
        for (Sym other : b->ancestors)
          if (uniqueRight(other) == l)
            joined.ancestors.push_back(l);
      }
    } else if (joined.type == SymInfo::Type::Function) {
      joined.functionsKnown = a->functionsKnown && b->functionsKnown;
      if (joined.functionsKnown) {
        joined.foreignFunctions = a->foreignFunctions;
        joined.foreignFunctions.insert(joined.foreignFunctions.end(),
                                       b->foreignFunctions.begin(),
                                       b->foreignFunctions.end());
        std::ranges::sort(joined.foreignFunctions);
        joined.foreignFunctions.erase(
            std::ranges::unique(joined.foreignFunctions).begin(),
            joined.foreignFunctions.end());
        joined.functions = a->functions;
        joined.functions.insert(joined.functions.end(), b->functions.begin(),
                                b->functions.end());
        std::ranges::sort(joined.functions);
        joined.functions.erase(std::ranges::unique(joined.functions).begin(),
                               joined.functions.end());
        if (joined.functions.size() > 32) {
          joined.functions.clear();
          joined.functionsKnown = false;
        }
      }
    }
    (void)widen;
    return joined;
  }

  SymInfo renamed(const SymInfo &value, bool isLeft) {
    SymInfo joined = value;
    joined.condition.reset();
    joined.linear.reset();
    joined.unwrapped.reset();
    joined.defined.reset();
    joined.productAtMost.reset();
    joined.pending.clear();
    for (Target &target : joined.targets)
      target.offset = isLeft ? pairTerm(target.offset, nullptr)
                             : pairTermRight(target.offset);
    if (value.pointerBehind != ZeroSym)
      joined.pointerBehind = isLeft ? pair(value.pointerBehind, ZeroSym)
                                    : pair(ZeroSym, value.pointerBehind);
    joined.ancestors.clear();
    for (Sym ancestor : value.ancestors) {
      Sym mapped = isLeft ? uniqueLeft(ancestor) : uniqueRight(ancestor);
      if (mapped != ZeroSym)
        joined.ancestors.push_back(mapped);
    }
    return joined;
  }
};
} // namespace

/// The join of two lives.
static Life joinLife(Life a, Life b) {
  if (a == b)
    return a;
  if (a == Life::UnknownReleased || b == Life::UnknownReleased)
    return Life::UnknownReleased;
  if (a == Life::Ended || b == Life::Ended || a == Life::MayEnded ||
      b == Life::MayEnded)
    return Life::MayEnded;
  return Life::MayReleased;
}

static std::optional<ReleaseRecord> joinObjectRecords(const ObjectState &a,
                                                      const ObjectState &b) {
  if (a.record && b.record)
    return joinRecords(*a.record, *b.record);
  if (a.record || b.record) {
    ReleaseRecord record = a.record ? *a.record : *b.record;
    record.allPaths = false;
    return record;
  }
  return std::nullopt;
}

/// The cells of one side the alignment leaves alone (matched across the
/// join).
using CellSet = std::set<std::pair<ObjectId, CellKey>>;

/// Fills the concrete cells one state lacks for objects both states have
/// (§4.8). Summary cells hold what stores through unknown indices wrote,
/// so one missing on a side is nothing written there and needs no value;
/// selected cells were aligned before (`alignSelected`).
static void alignCells(const Heap &heap, HeapState &left, HeapState &right,
                       const CellSet &skipLeft, const CellSet &skipRight) {
  for (int round = 0; round < 4; ++round) {
    bool changed = false;
    std::vector<ObjectId> shared;
    for (const auto &[id, object] : left.objects)
      if (right.objects.contains(id))
        shared.push_back(id);
    for (ObjectId id : shared) {
      std::vector<std::pair<CellKey, Sym>> onlyLeft;
      std::vector<std::pair<CellKey, Sym>> onlyRight;
      const ObjectState &l = *left.objects.find(id);
      const ObjectState &r = *right.objects.find(id);
      for (const auto &[key, sym] : l.cells)
        if (key.isConcrete() && !r.cells.contains(key) &&
            !skipLeft.contains({id, key}))
          onlyLeft.emplace_back(key, sym);
      for (const auto &[key, sym] : r.cells)
        if (key.isConcrete() && !l.cells.contains(key) &&
            !skipRight.contains({id, key}))
          onlyRight.emplace_back(key, sym);
      // An element the other side's ranges cannot tell apart gives way:
      // its value joins its own side's ranges instead (§4.2 *Amendment
      // (arrays)*).
      auto vague = [&](const HeapState &other, const CellKey &key) {
        const ObjectState &object = *other.objects.find(id);
        for (const Segment &segment : object.segments) {
          Order in = inRange(other.zone, segment.position, segment.from,
                             segment.to, key.byteTerm());
          if (in == Order::Proven)
            return false;
          if (in == Order::Unknown)
            return true;
        }
        return false;
      };
      // (A cell an earlier materialisation wrote meanwhile, such as the
      // count field an entry pointer's extent reads, keeps its value.)
      for (const auto &[key, sym] : onlyLeft) {
        if (right.objects.find(id)->cells.contains(key))
          continue;
        if (vague(right, key) && left.objects.find(id)->stride != 0) {
          heap.evictCell(left, id, key);
        } else {
          const SymInfo &hint = heap.info(left, sym);
          heap.load(right, id, key, hint);
        }
        changed = true;
      }
      for (const auto &[key, sym] : onlyRight) {
        if (left.objects.find(id)->cells.contains(key))
          continue;
        if (vague(left, key) && right.objects.find(id)->stride != 0) {
          heap.evictCell(right, id, key);
        } else {
          const SymInfo &hint = heap.info(right, sym);
          heap.load(left, id, key, hint);
        }
        changed = true;
      }
    }
    if (!changed)
      return;
  }
}

/// The objects both states have.
static std::vector<ObjectId> sharedObjects(const HeapState &left,
                                           const HeapState &right) {
  std::vector<ObjectId> shared;
  for (const auto &[id, object] : left.objects)
    if (right.objects.contains(id))
      shared.push_back(id);
  return shared;
}

/// §4.2 *Amendment (arrays)*: at a loop head, the element cells the
/// iteration changed (on the back edge `right`, against the head's `left`)
/// fold into segments on both sides, so a cell written at the induction
/// variable becomes a range that grows with it.
static void foldChangedElements(const Heap &heap, HeapState &left,
                                HeapState &right) {
  for (ObjectId id : sharedObjects(left, right)) {
    // (Reads through `find`: `at` copies an object another state shares.)
    const std::uint32_t leftStride = left.objects.find(id)->stride;
    const std::uint32_t rightStride = right.objects.find(id)->stride;
    std::uint32_t stride = leftStride != 0 ? leftStride : rightStride;
    if (stride == 0)
      continue;
    if (leftStride != stride)
      left.objects.at(id).stride = stride;
    if (rightStride != stride)
      right.objects.at(id).stride = stride;
    // Changed: new, or a value with other facts (another value, a
    // release). Symbols are numbered per state (a join renumbers them), so
    // the numbers are no evidence either way; a cell whose value has the
    // same facts stays a cell, and the join pairs its two values, which is
    // sound whether or not they are one value.
    std::vector<CellKey> changed;
    const PMap<CellKey, Sym> &leftCells = left.objects.find(id)->cells;
    for (const auto &[key, sym] : right.objects.find(id)->cells) {
      if (key.isSummary())
        continue;
      const Sym *before = leftCells.find(key);
      if (before == nullptr ||
          !(heap.info(left, *before) == heap.info(right, sym)))
        changed.push_back(key);
    }
    // The head's own cell for the element gives way to the range the
    // iteration grows: its value joins the ranges that hold it.
    for (const CellKey &key : changed)
      if (heap.foldCell(right, id, key) &&
          left.objects.find(id)->cells.contains(key))
        heap.evictCell(left, id, key);
  }
}

/// Selected cells on one side only: materialised on the other side when a
/// variable holds their index symbol on both sides (the same value there),
/// else evicted (their value becomes a weak write). Symbols are numbered
/// per state, so a number both states have is otherwise no evidence of one
/// value.
static void alignSelected(const Heap &heap, HeapState &left, HeapState &right,
                          const CellSet &skipLeft, const CellSet &skipRight,
                          const std::vector<std::pair<Sym, Sym>> &pairs) {
  std::set<Sym> shared;
  for (const auto &[a, b] : pairs)
    if (a == b)
      shared.insert(a);
  for (ObjectId id : sharedObjects(left, right)) {
    // (Reads through `find`: `at` copies an object another state shares.)
    auto cellsOf = [&](const HeapState &state) -> const PMap<CellKey, Sym> & {
      return state.objects.find(id)->cells;
    };
    std::vector<std::pair<CellKey, Sym>> onlyLeft;
    std::vector<std::pair<CellKey, Sym>> onlyRight;
    for (const auto &[key, sym] : cellsOf(left))
      if (key.isSelected() && !cellsOf(right).contains(key) &&
          !skipLeft.contains({id, key}))
        onlyLeft.emplace_back(key, sym);
    for (const auto &[key, sym] : cellsOf(right))
      if (key.isSelected() && !cellsOf(left).contains(key) &&
          !skipRight.contains({id, key}))
        onlyRight.emplace_back(key, sym);
    auto align = [&](HeapState &from, HeapState &to,
                     const std::vector<std::pair<CellKey, Sym>> &keys) {
      for (const auto &[key, sym] : keys) {
        if (shared.contains(key.index)) {
          const SymInfo &hint = heap.info(from, sym);
          heap.load(to, id, key, hint);
        } else {
          heap.evictCell(from, id, key);
        }
      }
    };
    align(left, right, onlyLeft);
    align(right, left, onlyRight);
    // A cell a materialisation on the other side evicted (the bound on
    // selected cells).
    std::vector<CellKey> stale;
    for (const auto &[key, sym] : cellsOf(left))
      if (key.isSelected() && !cellsOf(right).contains(key) &&
          !skipLeft.contains({id, key}))
        stale.push_back(key);
    for (const CellKey &key : stale)
      heap.evictCell(left, id, key);
    stale.clear();
    for (const auto &[key, sym] : cellsOf(right))
      if (key.isSelected() && !cellsOf(left).contains(key) &&
          !skipRight.contains({id, key}))
        stale.push_back(key);
    for (const CellKey &key : stale)
      heap.evictCell(right, id, key);
  }
}

namespace {
/// How a bound of a joined segment is formed: a constant, or
/// `scale * s + value` for the result symbol `s` pairing `left` and
/// `right`. `leftTerm` and `rightTerm` are what it is on each side.
struct BoundSpec {
  bool constant = true;
  std::int64_t value = 0;
  std::int64_t scale = 1;
  Sym left = ZeroSym;
  Sym right = ZeroSym;
  Term leftTerm = Term::unknown();
  Term rightTerm = Term::unknown();
};

/// A segment of the join: the segments it pairs (either may be absent: the
/// range is empty on that side) and its bounds.
struct SegmentMatch {
  std::optional<std::size_t> left;
  std::optional<std::size_t> right;
  BoundSpec from;
  BoundSpec to;
};

/// Matches the segments of one object across a join (§4.2 *Amendment
/// (arrays)*): bounds correspond when a pair of symbols the join pairs
/// anyway (the variables' values) equals them on each side.
class SegmentMatcher {
public:
  SegmentMatcher(const Heap &heap, const HeapState &left,
                 const HeapState &right,
                 const std::vector<std::pair<Sym, Sym>> &pairs)
      : heap(heap), left(left), right(right), pairs(pairs) {}

  /// The matches of `id`'s segments, or the segments to evict first.
  /// Segments are pushed newest first, so per position the two lists are
  /// aligned from their oldest ends; what one side has in front of the
  /// aligned part is new there, and matches an empty range on the other
  /// side or is evicted.
  bool plan(ObjectId id, std::vector<SegmentMatch> &out,
            std::vector<std::size_t> &evictLeft,
            std::vector<std::size_t> &evictRight) const {
    const std::vector<Segment> &ls = left.objects.find(id)->segments;
    const std::vector<Segment> &rs = right.objects.find(id)->segments;
    std::set<CellKey> positions;
    for (const Segment &segment : ls)
      positions.insert(segment.position);
    for (const Segment &segment : rs)
      positions.insert(segment.position);
    for (const CellKey &position : positions) {
      std::vector<std::size_t> li;
      std::vector<std::size_t> ri;
      for (std::size_t i = 0; i < ls.size(); ++i)
        if (ls[i].position == position)
          li.push_back(i);
      for (std::size_t j = 0; j < rs.size(); ++j)
        if (rs[j].position == position)
          ri.push_back(j);
      // The aligned tails, oldest first.
      std::vector<SegmentMatch> tail;
      std::size_t a = li.size();
      std::size_t b = ri.size();
      while (a > 0 && b > 0) {
        const Segment &l = ls[li[a - 1]];
        const Segment &r = rs[ri[b - 1]];
        auto from = matchBound(l.from, r.from);
        auto to = matchBound(l.to, r.to);
        if (!from || !to)
          break;
        tail.push_back(SegmentMatch{
            .left = li[a - 1], .right = ri[b - 1], .from = *from, .to = *to});
        --a;
        --b;
      }
      // The new ones in front: empty on the other side, or evicted.
      for (std::size_t j = 0; j < b; ++j) {
        if (auto bounds = matchEmpty(rs[ri[j]], false))
          out.push_back(SegmentMatch{.left = std::nullopt,
                                     .right = ri[j],
                                     .from = bounds->first,
                                     .to = bounds->second});
        else
          evictRight.push_back(ri[j]);
      }
      for (std::size_t i = 0; i < a; ++i) {
        if (auto bounds = matchEmpty(ls[li[i]], true))
          out.push_back(SegmentMatch{.left = li[i],
                                     .right = std::nullopt,
                                     .from = bounds->first,
                                     .to = bounds->second});
        else
          evictLeft.push_back(li[i]);
      }
      out.insert(out.end(), tail.rbegin(), tail.rend());
    }
    std::ranges::sort(evictLeft);
    std::ranges::sort(evictRight);
    return evictLeft.empty() && evictRight.empty();
  }

private:
  const Heap &heap;
  const HeapState &left;
  const HeapState &right;
  const std::vector<std::pair<Sym, Sym>> &pairs;

  /// The ways the join can form a bound that is `bound` on one side.
  std::vector<BoundSpec> candidates(const Term &bound, bool onLeft) const {
    std::vector<BoundSpec> out;
    if (!bound.known)
      return out;
    const HeapState &own = onLeft ? left : right;
    if (bound.isConstant()) {
      out.push_back(BoundSpec{.constant = true,
                              .value = bound.constant,
                              .scale = 0,
                              .left = ZeroSym,
                              .right = ZeroSym,
                              .leftTerm = Term::of(bound.constant),
                              .rightTerm = Term::of(bound.constant)});
    }
    for (const auto &[a, b] : pairs) {
      Sym mine = onLeft ? a : b;
      std::optional<std::int64_t> offset = difference(own.zone, bound, mine);
      if (!offset)
        continue;
      out.push_back(BoundSpec{.constant = false,
                              .value = *offset,
                              .scale = 1,
                              .left = a,
                              .right = b,
                              .leftTerm = Term::ofSym(a, 1, *offset),
                              .rightTerm = Term::ofSym(b, 1, *offset)});
    }
    return out;
  }

  /// `bound - sym` when the zone makes it a constant.
  static std::optional<std::int64_t> difference(const Zone &zone,
                                                const Term &bound, Sym sym) {
    if (!bound.known)
      return std::nullopt;
    if (bound.isConstant()) {
      auto value = zone.constant(sym);
      if (!value || *value == INT64_MIN)
        return std::nullopt;
      return checkedAdd(bound.constant, -*value);
    }
    if (bound.scale != 1)
      return std::nullopt;
    if (bound.var == sym)
      return bound.constant;
    auto upper = zone.bound(bound.var, sym);
    auto lower = zone.bound(sym, bound.var);
    if (!upper || !lower || *lower == INT64_MIN || *upper != -*lower)
      return std::nullopt;
    return checkedAdd(*upper, bound.constant);
  }

public:
  /// A bound that is `l` on the left and `r` on the right: one the join
  /// pairs anyway, else the pair of the two bounds' own symbols.
  std::optional<BoundSpec> matchBound(const Term &l, const Term &r) const {
    if (auto spec = matchBoth(l, r))
      return spec;
    if (!l.known || !r.known || l.isConstant() || r.isConstant() ||
        l.scale != r.scale || l.constant != r.constant)
      return std::nullopt;
    return BoundSpec{.constant = false,
                     .value = l.constant,
                     .scale = l.scale,
                     .left = l.var,
                     .right = r.var,
                     .leftTerm = l,
                     .rightTerm = r};
  }

  /// A bound that is `l` on the left and `r` on the right.
  std::optional<BoundSpec> matchBoth(const Term &l, const Term &r) const {
    for (const BoundSpec &spec : candidates(l, true)) {
      std::optional<bool> same = heap.sameCell(right, spec.rightTerm, r);
      if (same && *same)
        return spec;
    }
    return std::nullopt;
  }

private:
  /// Bounds for a segment of one side that are an empty range on the
  /// other.
  std::optional<std::pair<BoundSpec, BoundSpec>>
  matchEmpty(const Segment &segment, bool onLeft) const {
    const HeapState &other = onLeft ? right : left;
    for (const BoundSpec &from : candidates(segment.from, onLeft))
      for (const BoundSpec &to : candidates(segment.to, onLeft)) {
        const Term &start = onLeft ? from.rightTerm : from.leftTerm;
        const Term &end = onLeft ? to.rightTerm : to.leftTerm;
        if (heap.lessEqual(other, end, start).value_or(false))
          return std::make_pair(from, to);
      }
    return std::nullopt;
  }
};
} // namespace

/// The integer values the join pairs anyway: the variables' cells.
static std::vector<std::pair<Sym, Sym>> variablePairs(const Heap &heap,
                                                      const ObjectTable &table,
                                                      const HeapState &left,
                                                      const HeapState &right) {
  std::vector<std::pair<Sym, Sym>> pairs;
  for (ObjectId id : sharedObjects(left, right)) {
    ObjectKind kind = table.info(id).key.kind;
    if (kind != ObjectKind::Local && kind != ObjectKind::Global)
      continue;
    const ObjectState &l = *left.objects.find(id);
    const ObjectState &r = *right.objects.find(id);
    for (const auto &[key, sym] : l.cells) {
      if (!key.isConcrete())
        continue;
      const Sym *other = r.cells.find(key);
      if (other != nullptr && heap.info(left, sym).type == SymInfo::Type::Int &&
          heap.info(right, *other).type == SymInfo::Type::Int)
        pairs.emplace_back(sym, *other);
    }
  }
  return pairs;
}

/// Plans the segments of every shared object, evicting those that match
/// nothing on the other side.
static std::map<ObjectId, std::vector<SegmentMatch>>
planSegments(const Heap &heap, const ObjectTable &table, HeapState &left,
             HeapState &right) {
  std::map<ObjectId, std::vector<SegmentMatch>> plans;
  for (int round = 0; round < 8; ++round) {
    plans.clear();
    bool evicted = false;
    std::vector<std::pair<Sym, Sym>> pairs =
        variablePairs(heap, table, left, right);
    SegmentMatcher matcher(heap, left, right, pairs);
    for (ObjectId id : sharedObjects(left, right)) {
      if (left.objects.find(id)->segments.empty() &&
          right.objects.find(id)->segments.empty())
        continue;
      std::vector<SegmentMatch> matches;
      std::vector<std::size_t> evictLeft;
      std::vector<std::size_t> evictRight;
      if (matcher.plan(id, matches, evictLeft, evictRight)) {
        plans[id] = std::move(matches);
        continue;
      }
      auto marks = [](const std::vector<std::size_t> &indices,
                      std::size_t size) {
        std::vector<bool> out(size, false);
        for (std::size_t index : indices)
          if (index < size)
            out[index] = true;
        return out;
      };
      heap.evictSegments(
          left, id, marks(evictLeft, left.objects.find(id)->segments.size()));
      heap.evictSegments(
          right, id,
          marks(evictRight, right.objects.find(id)->segments.size()));
      evicted = true;
    }
    if (!evicted)
      return plans;
  }
  // Still unmatched: every segment goes.
  plans.clear();
  for (ObjectId id : sharedObjects(left, right))
    for (HeapState *side : {&left, &right})
      heap.evictSegments(
          *side, id,
          std::vector<bool>(side->objects.find(id)->segments.size(), true));
  return plans;
}

namespace {
/// Two element cells, one on each side, at indices the join pairs: on each
/// path the joined cell is that side's cell (§4.2 *Amendment (arrays)*).
/// The first iteration of a loop reads `a[0]` where later ones read
/// `a[i]`; matched, the two are one cell `a[i]` of the join.
struct CellMatch {
  CellKey left;
  CellKey right;
  CellKey position;
  BoundSpec index;
};
} // namespace

/// The element index of a cell at an object's stride, if it has one.
static std::optional<std::pair<CellKey, Term>>
strideIndex(const ObjectState &object, const CellKey &key) {
  if (object.stride == 0 || key.isSummary())
    return std::nullopt;
  if (key.isSelected() && key.stride % object.stride != 0)
    return std::nullopt;
  CellKey position =
      CellKey{.offset = key.offset, .stride = object.stride, .index = ZeroSym}
          .position();
  ElementIndex element = elementIndex(position, key.byteTerm());
  if (!element.at || !*element.at)
    return std::nullopt;
  return std::make_pair(position, element.index);
}

static std::map<ObjectId, std::vector<CellMatch>>
matchCells(const Heap &heap, const ObjectTable &table, const HeapState &left,
           const HeapState &right, CellSet &skipLeft, CellSet &skipRight) {
  std::map<ObjectId, std::vector<CellMatch>> matches;
  std::vector<std::pair<Sym, Sym>> pairs =
      variablePairs(heap, table, left, right);
  SegmentMatcher matcher(heap, left, right, pairs);
  for (ObjectId id : sharedObjects(left, right)) {
    const ObjectState &l = *left.objects.find(id);
    const ObjectState &r = *right.objects.find(id);
    if (l.stride == 0 || l.stride != r.stride)
      continue;
    std::vector<CellKey> onlyRight;
    for (const auto &[key, sym] : r.cells)
      if (!key.isSummary() && !l.cells.contains(key))
        onlyRight.push_back(key);
    std::vector<bool> used(onlyRight.size(), false);
    for (const auto &[key, sym] : l.cells) {
      if (key.isSummary() || r.cells.contains(key))
        continue;
      auto leftIndex = strideIndex(l, key);
      if (!leftIndex)
        continue;
      for (std::size_t j = 0; j < onlyRight.size(); ++j) {
        if (used[j])
          continue;
        auto rightIndex = strideIndex(r, onlyRight[j]);
        if (!rightIndex || rightIndex->first != leftIndex->first)
          continue;
        auto spec = matcher.matchBoth(leftIndex->second, rightIndex->second);
        if (!spec || spec->constant)
          continue;
        used[j] = true;
        matches[id].push_back(CellMatch{.left = key,
                                        .right = onlyRight[j],
                                        .position = leftIndex->first,
                                        .index = *spec});
        skipLeft.emplace(id, key);
        skipRight.emplace(id, onlyRight[j]);
        break;
      }
    }
  }
  return matches;
}

/// The joint of two object states (cells paired separately).
static ObjectState joinObjectAttributes(const ObjectState &a,
                                        const ObjectState &b) {
  ObjectState out;
  out.life = joinLife(a.life, b.life);
  out.record = joinObjectRecords(a, b);
  if (!isReleasedLife(a.life))
    out.releaseOffset = b.releaseOffset;
  else if (!isReleasedLife(b.life) || a.releaseOffset == b.releaseOffset)
    out.releaseOffset = a.releaseOffset;
  out.family = a.family == b.family ? a.family : std::string();
  // An object one side never made is owned where the other side made it.
  out.absent = a.absent && b.absent;
  out.owned = (a.owned || a.absent) && (b.owned || b.absent) && !out.absent;
  out.escaped = a.escaped || b.escaped;
  out.readonly = a.readonly && b.readonly;
  out.zeroed = a.zeroed && b.zeroed;
  out.uninitialised = a.uninitialised || b.uninitialised;
  out.havocked = a.havocked || b.havocked;
  out.forgotten.clear();
  out.mayForgotten.clear();
  if (!out.havocked) {
    // Forgotten on both sides, or on one side only (there the other side's
    // value still holds).
    out.forgotten = intersectRanges(a.forgotten, b.forgotten);
    for (const auto *ranges :
         {&a.forgotten, &b.forgotten, &a.mayForgotten, &b.mayForgotten})
      for (const auto &range : *ranges)
        addForgotten(out.mayForgotten, range.first, range.second);
  }
  out.stored = a.stored || b.stored;
  out.effectReleased = a.effectReleased && b.effectReleased;
  out.effectMayReleased = a.effectMayReleased || b.effectMayReleased ||
                          a.effectReleased != b.effectReleased;
  // For messages only: a name and a last use either side has.
  out.holder = !a.holder.empty() ? a.holder : b.holder;
  out.lastUse = a.lastUse != 0 ? a.lastUse : b.lastUse;
  out.candidates = a.candidates;
  for (ObjectId candidate : b.candidates)
    if (std::ranges::find(out.candidates, candidate) == out.candidates.end())
      out.candidates.push_back(candidate);
  std::ranges::sort(out.candidates);
  return out;
}

static HeapState combineStates(const Heap &heap, ObjectTable &table,
                               const HeapState &leftIn,
                               const HeapState &rightIn, Handle block,
                               bool widen, bool loopHead,
                               const std::vector<std::int64_t> &thresholds,
                               Sym keepBelow = ZeroSym) {
  if (leftIn.unreachable)
    return rightIn;
  if (rightIn.unreachable)
    return leftIn;
  HeapState left = leftIn;
  HeapState right = rightIn;
  // Elements first (§4.2 *Amendment (arrays)*): what the loop body wrote
  // becomes ranges, selected cells are aligned or evicted, segments are
  // matched or evicted; then the concrete cells are aligned.
  if (loopHead)
    foldChangedElements(heap, left, right);
  CellSet skipLeft;
  CellSet skipRight;
  std::map<ObjectId, std::vector<CellMatch>> cellMatches =
      matchCells(heap, table, left, right, skipLeft, skipRight);
  alignSelected(heap, left, right, skipLeft, skipRight,
                variablePairs(heap, table, left, right));
  std::map<ObjectId, std::vector<SegmentMatch>> plans =
      planSegments(heap, table, left, right);
  alignCells(heap, left, right, skipLeft, skipRight);
  HeapState out;
  Pairing pairing(heap, left, right, out, keepBelow);
  // A selected cell's key names its index symbol, which the join renames.
  auto renameKey = [&](CellKey key, bool onLeft, bool onRight) {
    if (key.isSelected())
      key.index = pairing.pair(onLeft ? key.index : ZeroSym,
                               onRight ? key.index : ZeroSym);
    return key;
  };
  auto boundTerm = [&](const BoundSpec &spec) {
    if (spec.constant)
      return Term::of(spec.value);
    return Term::ofSym(pairing.pair(spec.left, spec.right), spec.scale,
                       spec.value);
  };

  // Focus objects (§4.6, *Amendment (S1)*): a variable pointing to one
  // object on each side, different objects.
  struct FocusPlan {
    ObjectId holder;
    CellKey key;
    ObjectId leftObject;
    ObjectId rightObject;
    ObjectId focus;
  };
  std::vector<FocusPlan> focusPlans;
  auto singleTarget = [&](const HeapState &state, Sym sym) -> ObjectId {
    const SymInfo &value = heap.info(state, sym);
    if (value.type != SymInfo::Type::Pointer || value.top ||
        value.targets.size() != 1)
      return 0;
    return value.targets.front().object;
  };
  auto focusable = [&](ObjectId id) {
    ObjectKind kind = table.info(id).key.kind;
    return kind == ObjectKind::Entry || kind == ObjectKind::Materialized ||
           kind == ObjectKind::Focus || kind == ObjectKind::CallResult;
  };

  std::set<ObjectId> ids;
  std::vector<std::pair<ObjectId, bool>> oneSided;
  for (const auto &[id, object] : left.objects)
    ids.insert(id);
  for (const auto &[id, object] : right.objects)
    ids.insert(id);
  for (ObjectId id : ids) {
    const ObjectState *a = left.objects.find(id);
    const ObjectState *b = right.objects.find(id);
    ObjectState result;
    if (a != nullptr && b != nullptr) {
      result = joinObjectAttributes(*a, *b);
      result.stride = a->stride != 0 ? a->stride : b->stride;
      for (const auto &[key, sym] : b->cells)
        if (!a->cells.contains(key) && !skipRight.contains({id, key}))
          result.cells.set(renameKey(key, false, true),
                           pairing.pairOneSided(ZeroSym, sym));
      if (auto matched = cellMatches.find(id); matched != cellMatches.end())
        for (const CellMatch &match : matched->second) {
          // The cell at `stride * (scale * s + value) + offset`.
          Term index = boundTerm(match.index);
          auto stride = static_cast<std::int64_t>(match.position.stride);
          CellKey key{
              .offset = match.position.offset + (stride * index.constant),
              .stride = static_cast<std::uint32_t>(stride * index.scale),
              .index = index.var};
          result.cells.set(key, pairing.pair(*a->cells.find(match.left),
                                             *b->cells.find(match.right)));
        }
      if (auto plan = plans.find(id); plan != plans.end())
        for (const SegmentMatch &match : plan->second) {
          const Segment &shape =
              match.left ? a->segments[*match.left] : b->segments[*match.right];
          result.segments.push_back(Segment{
              .position = shape.position,
              .from = boundTerm(match.from),
              .to = boundTerm(match.to),
              .value = pairing.pair(
                  match.left ? a->segments[*match.left].value : ZeroSym,
                  match.right ? b->segments[*match.right].value : ZeroSym)});
        }
      for (const auto &[key, sym] : a->cells) {
        if (skipLeft.contains({id, key}))
          continue;
        const Sym *other = b->cells.find(key);
        Sym bs = other != nullptr ? *other : ZeroSym;
        result.cells.set(renameKey(key, true, other != nullptr),
                         other != nullptr ? pairing.pair(sym, bs)
                                          : pairing.pairOneSided(sym, bs));
        ObjectKind holderKind = table.info(id).key.kind;
        if (other != nullptr && *other != sym &&
            (holderKind == ObjectKind::Local ||
             holderKind == ObjectKind::Global) &&
            key.isConcrete()) {
          ObjectId lo = singleTarget(left, sym);
          ObjectId ro = singleTarget(right, *other);
          if (lo != 0 && ro != 0 && lo != ro && focusable(lo) && focusable(ro))
            focusPlans.push_back(FocusPlan{.holder = id,
                                           .key = key,
                                           .leftObject = lo,
                                           .rightObject = ro,
                                           .focus = 0});
        }
      }
      auto pairExtent =
          [&](const std::optional<Extent> &x,
              const std::optional<Extent> &y) -> std::optional<Extent> {
        if (!x || !y)
          return std::nullopt;
        Extent e;
        e.bytes = pairing.pairTerm(x->bytes, &y->bytes);
        e.cls = x->cls == y->cls ? x->cls : ExtentClass::LowerBound;
        if (!e.bytes.known)
          return std::nullopt;
        if (x->unwrapped && y->unwrapped &&
            x->unwrapped->scale == y->unwrapped->scale &&
            x->unwrapped->constant == y->unwrapped->constant) {
          Term unwrapped = pairing.pairTerm(*x->unwrapped, &*y->unwrapped);
          if (unwrapped.known)
            e.unwrapped = unwrapped;
        }
        return e;
      };
      result.extent = pairExtent(a->extent, b->extent);
      if (a->nulWithin && b->nulWithin) {
        Term t = pairing.pairTerm(*a->nulWithin, &*b->nulWithin);
        if (t.known) {
          result.nulWithin = t;
          if (a->nulFrom && b->nulFrom) {
            Term from = pairing.pairTerm(*a->nulFrom, &*b->nulFrom);
            if (from.known)
              result.nulFrom = from;
          }
        }
      }
      if (a->releasedBy != ZeroSym && a->releasedBy == b->releasedBy)
        result.releasedBy = ZeroSym;
    } else {
      const ObjectState *only = a != nullptr ? a : b;
      bool isLeft = a != nullptr;
      result = *only;
      result.cells = {};
      // An entry object exists on every path; the side that never
      // materialised it left it live (§4.6), so a release on the other
      // side is a release on some paths only.
      // A dead copy (§4.6) stands for the same object as its live version.
      ObjectKind onlyKind = table.info(id).key.kind;
      ObjectKey otherKey = table.info(id).key;
      otherKey.dead = !otherKey.dead;
      std::optional<ObjectId> counterpart = table.lookup(otherKey);
      const ObjectState *there =
          counterpart ? (isLeft ? right : left).objects.find(*counterpart)
                      : nullptr;
      bool releasedThere = there != nullptr && (there->life == Life::Released ||
                                                there->effectReleased);
      if ((onlyKind == ObjectKind::Entry ||
           onlyKind == ObjectKind::EntrySummary) &&
          !releasedThere &&
          (result.life == Life::Released || result.effectReleased)) {
        result.life = joinLife(result.life, Life::Live);
        if (result.record)
          result.record->allPaths = false;
        result.effectMayReleased = true;
        result.effectReleased = false;
      }
      // (A global or entry object exists on the other side too, unread
      // there: its cells hold the values of that side's paths only.)
      const bool implicit = onlyKind == ObjectKind::Global ||
                            onlyKind == ObjectKind::Entry ||
                            onlyKind == ObjectKind::EntrySummary;
      for (const auto &[key, sym] : only->cells) {
        const Sym onLeft = isLeft ? sym : ZeroSym;
        const Sym onRight = isLeft ? ZeroSym : sym;
        result.cells.set(renameKey(key, isLeft, !isLeft),
                         implicit ? pairing.pairOneSided(onLeft, onRight)
                                  : pairing.pair(onLeft, onRight));
      }
      for (Segment &segment : result.segments) {
        segment.from = isLeft ? pairing.pairTerm(segment.from, nullptr)
                              : pairing.pairTermRight(segment.from);
        segment.to = isLeft ? pairing.pairTerm(segment.to, nullptr)
                            : pairing.pairTermRight(segment.to);
        segment.value = isLeft ? pairing.pair(segment.value, ZeroSym)
                               : pairing.pair(ZeroSym, segment.value);
        // Renumbered: a copied range becomes the plain range its value
        // describes.
        segment.source = 0;
        segment.copied = ZeroSym;
      }
      // The terms after every object's cells are paired (below).
      result.extent.reset();
      result.nulWithin.reset();
      result.nulFrom.reset();
      oneSided.emplace_back(id, isLeft);
      result.releasedBy = ZeroSym;
    }
    out.objects.set(id, std::move(result));
  }
  // An object one side has: its extent and string fact over that side's
  // values, related to the cells both sides hold them in (Pairing::onlyLeft).
  for (const auto &[id, isLeft] : oneSided) {
    const ObjectState &only =
        isLeft ? *left.objects.find(id) : *right.objects.find(id);
    ObjectState &result = out.objects.at(id);
    if (only.extent) {
      Extent e = *only.extent;
      e.bytes = isLeft ? pairing.pairTerm(e.bytes, nullptr)
                       : pairing.pairTermRight(e.bytes);
      if (e.unwrapped) {
        Term unwrapped = isLeft ? pairing.pairTerm(*e.unwrapped, nullptr)
                                : pairing.pairTermRight(*e.unwrapped);
        e.unwrapped =
            unwrapped.known ? std::optional<Term>(unwrapped) : std::nullopt;
      }
      result.extent = e;
    }
    if (only.nulWithin)
      result.nulWithin = isLeft ? pairing.pairTerm(*only.nulWithin, nullptr)
                                : pairing.pairTermRight(*only.nulWithin);
    if (only.nulFrom)
      result.nulFrom = isLeft ? pairing.pairTerm(*only.nulFrom, nullptr)
                              : pairing.pairTermRight(*only.nulFrom);
  }
  // Expression values, after every symbol the objects reach is numbered:
  // a value one side still holds must not renumber the rest, or two
  // states that differ only in it never compare equal at a loop head.
  pairing.finish(widen);
  std::set<Handle> handles;
  for (const auto &[handle, sym] : left.exprs)
    handles.insert(handle);
  for (const auto &[handle, sym] : right.exprs)
    handles.insert(handle);
  for (Handle handle : handles) {
    const Sym *a = left.exprs.find(handle);
    const Sym *b = right.exprs.find(handle);
    out.exprs.set(handle, pairing.pair(a ? *a : ZeroSym, b ? *b : ZeroSym));
  }
  if (left.result != ZeroSym || right.result != ZeroSym)
    out.result = pairing.pair(left.result, right.result);
  pairing.finish(widen);

  // Focus objects.
  for (FocusPlan &plan : focusPlans) {
    ObjectKey key;
    key.kind = ObjectKind::Focus;
    key.handle = block;
    key.parent = plan.holder;
    key.cell = plan.key.offset;
    ObjectInfo info;
    const ObjectInfo &l = table.info(plan.leftObject);
    const ObjectInfo &r = table.info(plan.rightObject);
    info.type = l.type == r.type ? l.type : 0;
    info.singular = true;
    info.name = l.name;
    plan.focus = table.intern(key, info);
    // The focus object's state pairs the two sides' objects.
    const ObjectState *lo = left.objects.find(plan.leftObject);
    const ObjectState *ro = right.objects.find(plan.rightObject);
    if (lo == nullptr || ro == nullptr)
      continue;
    ObjectState focusState = joinObjectAttributes(*lo, *ro);
    focusState.candidates.clear();
    auto addCandidates = [&](ObjectId id, const ObjectState &state) {
      if (table.info(id).key.kind == ObjectKind::Focus) {
        for (ObjectId c : state.candidates)
          focusState.candidates.push_back(c);
      } else {
        focusState.candidates.push_back(id);
      }
    };
    addCandidates(plan.leftObject, *lo);
    addCandidates(plan.rightObject, *ro);
    std::ranges::sort(focusState.candidates);
    focusState.candidates.erase(
        std::ranges::unique(focusState.candidates).begin(),
        focusState.candidates.end());
    focusState.candidates.erase(
        std::ranges::remove(focusState.candidates, plan.focus).begin(),
        focusState.candidates.end());
    std::set<CellKey> keys;
    for (const auto &[k, s] : lo->cells)
      keys.insert(k);
    for (const auto &[k, s] : ro->cells)
      keys.insert(k);
    // The candidates keep their ranges; the focus object reads them
    // through its candidates.
    focusState.segments.clear();
    for (const CellKey &k : keys) {
      const Sym *x = lo->cells.find(k);
      const Sym *y = ro->cells.find(k);
      focusState.cells.set(renameKey(k, x != nullptr, y != nullptr),
                           pairing.pair(x ? *x : ZeroSym, y ? *y : ZeroSym));
    }
    if (lo->extent && ro->extent) {
      Extent e;
      e.bytes = pairing.pairTerm(lo->extent->bytes, &ro->extent->bytes);
      e.cls = lo->extent->cls == ro->extent->cls ? lo->extent->cls
                                                 : ExtentClass::LowerBound;
      if (e.bytes.known)
        focusState.extent = e;
    }
    out.objects.set(plan.focus, focusState);
    // The variable now points to the focus object.
    const Sym *cell = out.objects.find(plan.holder)->cells.find(plan.key);
    if (cell != nullptr) {
      SymInfo &value = out.syms.at(*cell);
      Term offset = Term::unknown();
      const SymInfo &a = heap.info(
          left, *left.objects.find(plan.holder)->cells.find(plan.key));
      const SymInfo &b = heap.info(
          right, *right.objects.find(plan.holder)->cells.find(plan.key));
      if (a.targets.front().offset == b.targets.front().offset &&
          a.targets.front().offset.isConstant())
        offset = a.targets.front().offset;
      value.targets = {Target{.object = plan.focus, .offset = offset}};
      value.top = false;
    }
  }
  pairing.finish(widen);

  // Existence guards: an owned allocation one side made and the other did
  // not exists where a local's value tests as it did on the side that made
  // it, when the two sides' values test differently.
  {
    auto zeroTest = [&](const HeapState &state,
                        Sym sym) -> std::optional<bool> {
      const SymInfo *info = state.syms.find(sym);
      if (info == nullptr)
        return std::nullopt;
      if (info->type == SymInfo::Type::Pointer) {
        if (info->null == PointerNull::Null)
          return true;
        if (info->null == PointerNull::NonNull)
          return false;
        return std::nullopt;
      }
      if (info->type != SymInfo::Type::Int)
        return std::nullopt;
      auto lo = state.zone.lower(sym);
      auto hi = state.zone.upper(sym);
      if (lo && hi && *lo == 0 && *hi == 0)
        return true;
      if (info->nonZero || (lo && *lo > 0) || (hi && *hi < 0))
        return false;
      return std::nullopt;
    };
    std::map<Sym, const SymPair *> byResult;
    for (const SymPair &p : pairing.pairs)
      if (p.hasLeft && p.hasRight)
        byResult[p.result] = &p;
    // The locals' values that the two sides test differently.
    std::vector<std::pair<Sym, bool>> leftTests;
    for (const auto &[id, object] : out.objects) {
      if (table.info(id).key.kind != ObjectKind::Local)
        continue;
      for (const auto &[key, sym] : object.cells) {
        auto p = byResult.find(sym);
        if (p == byResult.end())
          continue;
        auto l = zeroTest(left, p->second->left);
        auto r = zeroTest(right, p->second->right);
        if (l && r && *l != *r)
          leftTests.emplace_back(sym, *l);
      }
    }
    for (const auto &[id, isLeft] : oneSided) {
      ObjectState &object = out.objects.at(id);
      ObjectKind kind = table.info(id).key.kind;
      // (A guard it had names the other state's symbols.)
      object.existsIf.reset();
      object.existsIfEntry.reset();
      if (!object.owned ||
          (kind != ObjectKind::HeapRecent && kind != ObjectKind::HeapOld))
        continue;
      // An entry test the side that made it took and the other did not.
      const HeapState &made = isLeft ? left : right;
      const HeapState &other = isLeft ? right : left;
      for (const EntryTest &test : made.entryTests) {
        EntryTest opposite = test;
        opposite.zero = !opposite.zero;
        if (std::ranges::binary_search(other.entryTests, opposite)) {
          object.existsIfEntry = test;
          break;
        }
      }
      if (!leftTests.empty())
        object.existsIf = std::make_pair(leftTests.front().first,
                                         isLeft ? leftTests.front().second
                                                : !leftTests.front().second);
    }
    // One made on both sides keeps a guard both had over paired symbols.
    std::vector<std::pair<ObjectId, Sym>> kept;
    for (const auto &[id, object] : out.objects) {
      const ObjectState *a = left.objects.find(id);
      const ObjectState *b = right.objects.find(id);
      if (a == nullptr || b == nullptr || (!a->existsIf && !b->existsIf))
        continue;
      if (a->existsIf && b->existsIf &&
          a->existsIf->second == b->existsIf->second)
        if (Sym paired = pairing.find(a->existsIf->first, b->existsIf->first))
          kept.emplace_back(id, paired);
    }
    for (const auto &[id, paired] : kept)
      out.objects.at(id).existsIf =
          std::make_pair(paired, left.objects.find(id)->existsIf->second);
    // An entry guard both sides had.
    for (const auto &[id, object] : out.objects) {
      const ObjectState *a = left.objects.find(id);
      const ObjectState *b = right.objects.find(id);
      if (a != nullptr && b != nullptr && a->existsIfEntry &&
          a->existsIfEntry == b->existsIfEntry)
        kept.emplace_back(id, ZeroSym);
    }
    for (const auto &[id, paired] : kept)
      if (paired == ZeroSym)
        out.objects.at(id).existsIfEntry = left.objects.find(id)->existsIfEntry;
  }

  // Cells stored on exactly the paths of one entry test (a lazy
  // initialisation, §6.2): one side stored the cell under a test whose
  // opposite the other side took, leaving the entry value there.
  {
    // Whether a side's cell holds a value this activation stored (false:
    // its entry value; none: unknown).
    auto stored = [&](const HeapState &state, ObjectId id,
                      const CellKey &key) -> std::optional<bool> {
      const ObjectState *object = state.objects.find(id);
      if (object == nullptr)
        return false;
      const Sym *held = object->cells.find(key);
      if (held == nullptr)
        return object->forgets(key) ? std::nullopt : std::optional<bool>(false);
      const SymInfo *info = state.syms.find(*held);
      if (info == nullptr)
        return std::nullopt;
      return !(info->entryOf && *info->entryOf == std::make_pair(id, key));
    };
    auto opposite = [](const HeapState &made,
                       const HeapState &kept) -> std::optional<EntryTest> {
      for (const EntryTest &test : made.entryTests)
        for (const EntryTest &other : kept.entryTests)
          if (other.object == test.object && other.key == test.key &&
              other.zero != test.zero)
            return test;
      return std::nullopt;
    };
    auto guardOf = [](const ObjectState *object,
                      const CellKey &key) -> std::optional<EntryTest> {
      if (object != nullptr)
        for (const auto &[at, test] : object->storedIff)
          if (at == key)
            return test;
      return std::nullopt;
    };
    std::vector<std::pair<ObjectId, std::vector<std::pair<CellKey, EntryTest>>>>
        guards;
    for (const auto &[id, object] : out.objects) {
      const ObjectKey &objectKey = table.info(id).key;
      if ((objectKey.kind != ObjectKind::Entry || objectKey.dead) &&
          objectKey.kind != ObjectKind::Global)
        continue;
      std::vector<std::pair<CellKey, EntryTest>> cells;
      for (const auto &[key, sym] : object.cells) {
        if (!key.isConcrete())
          continue;
        auto l = stored(left, id, key);
        auto r = stored(right, id, key);
        std::optional<EntryTest> guard;
        if (l == true && r == false) {
          guard = opposite(left, right);
        } else if (l == false && r == true) {
          guard = opposite(right, left);
        } else if (l == true && r == true) {
          auto a = guardOf(left.objects.find(id), key);
          auto b = guardOf(right.objects.find(id), key);
          if (a && b && *a == *b)
            guard = a;
        }
        if (guard)
          cells.emplace_back(key, *guard);
      }
      if (!cells.empty() || !object.storedIff.empty())
        guards.emplace_back(id, std::move(cells));
    }
    for (auto &[id, cells] : guards)
      out.objects.at(id).storedIff = std::move(cells);
  }

  // Entry tests both sides decided alike (they name cells, not symbols).
  std::ranges::set_intersection(left.entryTests, right.entryTests,
                                std::back_inserter(out.entryTests));

  // RFC 0014: a pointer comparison both sides decided alike, over the
  // symbols that pair their operands.
  if (!left.pointerFacts.empty() && !right.pointerFacts.empty()) {
    std::map<Sym, std::vector<const SymPair *>> byLeft;
    for (const SymPair &p : pairing.pairs)
      if (p.hasLeft && p.hasRight)
        byLeft[p.left].push_back(&p);
    for (const PointerFact &fact : left.pointerFacts) {
      auto x = byLeft.find(fact.first);
      auto y = byLeft.find(fact.second);
      if (x == byLeft.end() || y == byLeft.end())
        continue;
      for (const SymPair *a : x->second)
        for (const SymPair *b : y->second)
          if (Heap::pointersEqual(right, a->right, b->right) == fact.equal)
            Heap::assumePointersEqual(out, a->result, b->result, fact.equal);
    }
  }

  // Symbols: the zone over the result symbols.
  std::vector<std::int64_t> noThresholds;
  out.zone = Zone::combine(left.zone, right.zone, pairing.pairs, widen,
                           widen ? thresholds : noThresholds);
  // A join keeps both sides' unmatched ranges: each position's newest
  // `MaxSegmentsPerPosition` stay, or a loop head compounds them.
  std::vector<ObjectId> crowded;
  for (const auto &[id, object] : out.objects)
    if (object.segments.size() > Heap::MaxSegmentsPerPosition)
      crowded.push_back(id);
  for (ObjectId id : crowded)
    heap.trimSegments(out, id);
  return out;
}

HeapState Heap::join(const HeapState &left, const HeapState &right,
                     Handle block, bool loopHead, Sym keepBelow) const {
  return combineStates(*this, table, left, right, block, /*widen=*/false,
                       loopHead, {}, keepBelow);
}

HeapState Heap::widen(const HeapState &previous, const HeapState &next,
                      Handle block,
                      const std::vector<std::int64_t> &thresholds) const {
  return combineStates(*this, table, previous, next, block, /*widen=*/true,
                       /*loopHead=*/false, thresholds);
}

//===----------------------------------------------------------------------===//
// Dumps
//===----------------------------------------------------------------------===//

std::string Heap::dump(const HeapState &state) const {
  std::ostringstream os;
  if (state.unreachable)
    return "unreachable\n";
  auto name = [&](Sym sym) { return "s" + std::to_string(sym); };
  auto spellTerm = [&](const Term &term) {
    if (!term.known)
      return std::string("?");
    if (term.var == ZeroSym || term.scale == 0)
      return std::to_string(term.constant);
    std::string out =
        (term.scale == 1 ? "" : std::to_string(term.scale) + "*") +
        name(term.var);
    if (term.constant != 0)
      out += (term.constant > 0 ? "+" : "") + std::to_string(term.constant);
    return out;
  };
  for (const auto &[id, object] : state.objects) {
    const ObjectInfo &objectInfo = table.info(id);
    os << "  o" << id << " " << spell(objectInfo.key.kind)
       << (objectInfo.key.dead ? " dead" : "") << " '" << objectInfo.name
       << "'";
    switch (object.life) {
    case Life::Live:
      break;
    case Life::Released:
      os << " released";
      break;
    case Life::MayReleased:
      os << " may-released";
      break;
    case Life::UnknownReleased:
      os << " unknown-released";
      break;
    case Life::Ended:
      os << " ended";
      break;
    case Life::MayEnded:
      os << " may-ended";
      break;
    }
    if (object.extent) {
      os << " extent=" << spellTerm(object.extent->bytes) << "/"
         << toString(object.extent->cls);
      if (object.extent->unwrapped)
        os << " at-most=" << spellTerm(*object.extent->unwrapped);
    }
    for (const auto &[from, to] : object.forgotten)
      os << " forgotten=[" << from << "," << to << ")";
    for (const auto &[from, to] : object.mayForgotten)
      os << " may-forgotten=[" << from << "," << to << ")";
    if (!object.candidates.empty()) {
      os << " candidates={";
      for (ObjectId c : object.candidates)
        os << " o" << c;
      os << " }";
    }
    os << "\n";
    for (const Segment &segment : object.segments) {
      os << "    [" << spellTerm(segment.from) << ".." << spellTerm(segment.to)
         << ")*" << segment.position.stride << "+" << segment.position.offset
         << " = " << name(segment.value);
      if (const auto &release = info(state, segment.value).release)
        os << (release->definite() ? " released" : " may-released");
      os << "\n";
    }
    for (const auto &[key, sym] : object.cells) {
      if (key.isSelected())
        os << "    [" << key.stride << "*" << name(key.index) << "+"
           << key.offset << "] = " << name(sym);
      else
        os << "    [" << (key.isSummary() ? "*" : "") << key.offset
           << (key.isSummary() ? "/" + std::to_string(key.stride) : "")
           << "] = " << name(sym);
      const SymInfo &value = info(state, sym);
      if (value.type == SymInfo::Type::Pointer) {
        os << " ->";
        if (value.top)
          os << " top";
        for (const Target &t : value.targets)
          os << " o" << t.object << "+" << spellTerm(t.offset);
        if (value.null == PointerNull::Null)
          os << " null";
        else if (value.null == PointerNull::NonNull)
          os << " nonnull";
        else
          os << " maybe-null";
        if (value.release)
          os << (value.release->definite() ? " released" : " may-released");
      }
      os << "\n";
    }
  }
  if (!state.exprs.empty()) {
    os << "  exprs:";
    for (const auto &[handle, sym] : state.exprs)
      os << " " << std::hex << handle << std::dec << "=" << name(sym);
    os << "\n";
  }
  std::string zone = state.zone.toString(name);
  if (!zone.empty())
    os << "  zone: " << zone << "\n";
  return os.str();
}

} // namespace weavec::core
