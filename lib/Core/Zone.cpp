//===- Zone.cpp - Difference-bound constraints over symbols ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Zone.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace weavec::core {

/// `a + b`, or none when it does not fit (a dropped bound only forgets).
static std::optional<std::int64_t> addBound(std::int64_t a, std::int64_t b) {
  __int128 sum = static_cast<__int128>(a) + static_cast<__int128>(b);
  if (sum > INT64_MAX || sum < INT64_MIN)
    return std::nullopt;
  return static_cast<std::int64_t>(sum);
}

std::optional<std::int64_t> Zone::stored(Sym x, Sym y) const {
  const auto *row = rows.find(x);
  if (row == nullptr)
    return std::nullopt;
  const std::int64_t *c = row->find(y);
  if (c == nullptr)
    return std::nullopt;
  return *c;
}

std::optional<std::int64_t> Zone::implied(Sym x, Sym y) const {
  if (x == ZeroSym || y == ZeroSym)
    return std::nullopt;
  auto ux = stored(x, ZeroSym);
  auto ly = stored(ZeroSym, y);
  if (!ux || !ly)
    return std::nullopt;
  auto sum = addBound(*ux, *ly);
  if (!sum || *sum >= LooseRelation)
    return std::nullopt;
  return sum;
}

std::optional<std::int64_t> Zone::bound(Sym x, Sym y) const {
  if (bottom)
    return INT64_MIN;
  if (x == y)
    return 0;
  auto own = stored(x, y);
  auto zero = implied(x, y);
  if (own && zero)
    return std::min(*own, *zero);
  return own ? own : zero;
}

void Zone::noteAdded(Sym x, Sym y) {
  if (x == ZeroSym || y == ZeroSym)
    return;
  ++degrees.at(x);
  ++degrees.at(y);
}

void Zone::noteRemoved(Sym x, Sym y) {
  if (x == ZeroSym || y == ZeroSym)
    return;
  for (Sym sym : {x, y})
    if (const std::uint32_t *count = degrees.find(sym)) {
      if (*count <= 1)
        degrees.erase(sym);
      else
        degrees.set(sym, *count - 1);
    }
}

void Zone::setRaw(Sym x, Sym y, std::int64_t c) {
  if (x == y)
    return;
  const auto *row = rows.find(x);
  const bool existed = row != nullptr && row->contains(y);
  rows.at(x).set(y, c);
  if (!existed)
    noteAdded(x, y);
}

void Zone::tighten(Sym x, Sym y, std::int64_t c) {
  if (x == y) {
    if (c < 0)
      setBottom();
    return;
  }
  // A relation between two symbols this loose (what their C types' ranges
  // give) proves nothing and would relate every symbol to every other:
  // forgotten, which is sound.
  if (x != ZeroSym && y != ZeroSym && c >= LooseRelation)
    return;
  // (Nor one their bounds against zero already imply.)
  auto current = bound(x, y);
  if (!current || c < *current)
    setRaw(x, y, c);
}

bool Zone::addLE(Sym x, Sym y, std::int64_t c) {
  return addLimited(x, y, c, true);
}

bool Zone::addLimited(Sym x, Sym y, std::int64_t c, bool limit) {
  if (bottom)
    return false;
  if (x == y) {
    if (c < 0)
      setBottom();
    return !bottom;
  }
  if (auto current = bound(x, y); current && *current <= c)
    return true;
  // Incremental closure: for every u reaching x and every v reached from y,
  // u - v <= (u - x) + c + (y - v). A path through zero on either side is
  // what the bounds against zero imply (the zero row and column, updated
  // here), so u ranges over zero and the symbols with a stored bound on
  // `u - x`, and v over zero and those with one on `y - v`.
  std::vector<std::pair<Sym, std::int64_t>> intoX{{x, 0}};
  std::vector<std::pair<Sym, std::int64_t>> fromY{{y, 0}};
  if (x != ZeroSym) {
    if (auto zx = stored(ZeroSym, x))
      intoX.emplace_back(ZeroSym, *zx);
    for (const auto &[u, row] : rows) {
      if (u == x || u == ZeroSym)
        continue;
      if (row.contains(x))
        if (auto b = bound(u, x))
          intoX.emplace_back(u, *b);
    }
  }
  if (y != ZeroSym)
    if (const auto *row = rows.find(y))
      for (const auto &[v, b] : *row)
        if (v != y) {
          auto effective = v == ZeroSym ? std::optional(b) : bound(y, v);
          if (effective)
            fromY.emplace_back(v, *effective);
        }
  for (const auto &[u, ux] : intoX)
    for (const auto &[v, yv] : fromY) {
      auto left = addBound(ux, c);
      if (!left)
        continue;
      auto total = addBound(*left, yv);
      if (!total)
        continue;
      tighten(u, v, *total);
      if (bottom)
        return false;
    }
  if (limit)
    enforceLimit();
  return true;
}

bool Zone::addRange(Sym x, std::optional<std::int64_t> lo,
                    std::optional<std::int64_t> hi) {
  if (hi && !addLE(x, ZeroSym, *hi))
    return false;
  if (lo && *lo != INT64_MIN && !addLE(ZeroSym, x, -*lo))
    return false;
  return !bottom;
}

bool Zone::entails(Sym x, Sym y, std::int64_t c) const {
  if (bottom)
    return true;
  auto b = bound(x, y);
  return b && *b <= c;
}

void Zone::forget(Sym x) {
  if (x == ZeroSym)
    return;
  restrictTo([x](Sym sym) { return sym != x; });
}

void Zone::restrictTo(const std::function<bool(Sym)> &keep) {
  if (bottom)
    return;
  bool dropped = rows.size() != 0 && [&] {
    for (const auto &[u, row] : rows) {
      if (u != ZeroSym && !keep(u))
        return true;
      for (const auto &[y, c] : row)
        if (y != ZeroSym && !keep(y))
          return true;
    }
    return false;
  }();
  if (!dropped)
    return;
  rows.eraseIf([&](Sym u, const PMap<Sym, std::int64_t> &) {
    return u != ZeroSym && !keep(u);
  });
  std::vector<std::pair<Sym, PMap<Sym, std::int64_t>>> changed;
  for (const auto &[u, row] : rows) {
    PMap<Sym, std::int64_t> kept = row;
    kept.eraseIf([&](Sym y, std::int64_t) { return y != ZeroSym && !keep(y); });
    if (kept.size() != row.size())
      changed.emplace_back(u, std::move(kept));
  }
  for (auto &[u, row] : changed)
    rows.set(u, std::move(row));
  // The size limit's measure, from the relational bounds left.
  std::map<Sym, std::uint32_t> counts;
  for (const auto &[u, row] : rows)
    if (u != ZeroSym)
      for (const auto &[y, c] : row)
        if (y != ZeroSym) {
          ++counts[u];
          ++counts[y];
        }
  degrees.clear();
  for (const auto &[sym, count] : counts)
    degrees.set(sym, count);
}

std::vector<Sym> Zone::symbols() const {
  std::vector<Sym> all;
  for (const auto &[x, row] : rows) {
    all.push_back(x);
    for (const auto &[y, c] : row)
      all.push_back(y);
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  return all;
}

void Zone::assign(std::vector<std::tuple<Sym, Sym, std::int64_t>> entries) {
  std::sort(entries.begin(), entries.end());
  std::vector<std::pair<Sym, PMap<Sym, std::int64_t>>> built;
  std::vector<Sym> related;
  for (std::size_t i = 0; i < entries.size();) {
    const Sym x = std::get<0>(entries[i]);
    std::vector<std::pair<Sym, std::int64_t>> row;
    for (; i < entries.size() && std::get<0>(entries[i]) == x; ++i) {
      const auto &[from, to, c] = entries[i];
      row.emplace_back(to, c);
      if (from != ZeroSym && to != ZeroSym) {
        related.push_back(from);
        related.push_back(to);
      }
    }
    built.emplace_back(x, PMap<Sym, std::int64_t>::fromSorted(std::move(row)));
  }
  rows = PMap<Sym, PMap<Sym, std::int64_t>>::fromSorted(std::move(built));
  std::sort(related.begin(), related.end());
  std::vector<std::pair<Sym, std::uint32_t>> counts;
  for (Sym sym : related)
    if (!counts.empty() && counts.back().first == sym)
      ++counts.back().second;
    else
      counts.emplace_back(sym, 1);
  degrees = PMap<Sym, std::uint32_t>::fromSorted(std::move(counts));
}

bool Zone::equals(const Zone &other) const {
  if (bottom != other.bottom)
    return false;
  if (bottom)
    return true;
  if (rows.sharesWith(other.rows))
    return true;
  // The bounds against zero, stored alike.
  auto unary = [](const Zone &zone) {
    std::vector<std::tuple<Sym, Sym, std::int64_t>> out;
    for (const auto &[x, row] : zone.rows)
      for (const auto &[y, c] : row)
        if (x == ZeroSym || y == ZeroSym)
          out.emplace_back(x, y, c);
    return out;
  };
  if (unary(*this) != unary(other))
    return false;
  // Every relation either stores, as both zones bound it.
  auto relationsAgree = [](const Zone &a, const Zone &b) {
    for (const auto &[x, row] : a.rows) {
      if (x == ZeroSym)
        continue;
      for (const auto &[y, c] : row)
        if (y != ZeroSym && a.bound(x, y) != b.bound(x, y))
          return false;
    }
    return true;
  };
  return relationsAgree(*this, other) && relationsAgree(other, *this);
}

void Zone::enforceLimit() {
  // Keep the symbols in the most relational bounds; the others keep their
  // bounds against zero only.
  if (degrees.size() <= MaxRelational)
    return;
  std::vector<std::pair<std::uint32_t, Sym>> order;
  order.reserve(degrees.size());
  for (const auto &[sym, bounds] : degrees)
    order.emplace_back(bounds, sym);
  std::sort(order.begin(), order.end());
  std::set<Sym> demote;
  const std::size_t keep = MaxRelational - MaxRelational / 4;
  for (std::size_t i = 0; i + keep < order.size(); ++i)
    demote.insert(order[i].second);
  // One pass over the rows: a demoted symbol's row keeps its bound against
  // zero, and every other row drops its bounds on demoted symbols.
  std::vector<std::pair<Sym, PMap<Sym, std::int64_t>>> changed;
  for (const auto &[u, row] : rows) {
    bool demoted = demote.contains(u);
    bool touched = false;
    PMap<Sym, std::int64_t> kept;
    for (const auto &[y, c] : row) {
      // (Bounds against zero stay: `0 - y` is a lower bound of `y`.)
      if (u != ZeroSym && y != ZeroSym && (demoted || demote.contains(y))) {
        touched = true;
        noteRemoved(u, y);
        continue;
      }
      kept.set(y, c);
    }
    if (touched)
      changed.emplace_back(u, std::move(kept));
  }
  for (auto &[u, row] : changed)
    rows.set(u, std::move(row));
}

/// The closure of `zone` (Floyd-Warshall), the size limit kept once at the
/// end. A path through a symbol with no stored relation goes through zero,
/// which its bounds against zero already give: only zero and the symbols
/// in stored relations take part.
Zone Zone::closure(const Zone &zone, const std::vector<Sym> &symbols) {
  (void)symbols;
  Zone out = zone;
  if (zone.bottom)
    return out;
  std::set<Sym> related{ZeroSym};
  for (const auto &[x, row] : zone.rows)
    if (x != ZeroSym)
      for (const auto &[y, c] : row)
        if (y != ZeroSym) {
          related.insert(x);
          related.insert(y);
        }
  if (related.size() <= 1)
    return out;
  const std::vector<Sym> nodes(related.begin(), related.end());
  const std::size_t n = nodes.size();
  constexpr std::int64_t None = INT64_MAX;
  std::vector<std::int64_t> d(n * n, None);
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = 0; j < n; ++j) {
      if (i == j) {
        d[i * n + j] = 0;
      } else if (auto c = zone.bound(nodes[i], nodes[j])) {
        d[i * n + j] = *c;
      }
    }
  for (std::size_t k = 0; k < n; ++k)
    for (std::size_t i = 0; i < n; ++i) {
      const std::int64_t ik = d[i * n + k];
      if (ik == None)
        continue;
      for (std::size_t j = 0; j < n; ++j) {
        const std::int64_t kj = d[k * n + j];
        if (kj == None)
          continue;
        const __int128 sum = static_cast<__int128>(ik) + kj;
        if (sum < INT64_MIN || sum >= None)
          continue;
        std::int64_t &ij = d[i * n + j];
        if (ij == None || sum < ij)
          ij = static_cast<std::int64_t>(sum);
      }
    }
  for (std::size_t i = 0; i < n; ++i)
    if (d[i * n + i] < 0) {
      out.setBottom();
      return out;
    }
  // (Nodes[0] is zero.) The bounds against zero first, then the relations
  // they leave something to say.
  for (std::size_t i = 1; i < n; ++i) {
    if (d[i * n] != None)
      out.setRaw(nodes[i], ZeroSym, d[i * n]);
    if (d[i] != None)
      out.setRaw(ZeroSym, nodes[i], d[i]);
  }
  for (std::size_t i = 1; i < n; ++i)
    for (std::size_t j = 1; j < n; ++j) {
      if (i == j || d[i * n + j] == None)
        continue;
      auto zero = out.implied(nodes[i], nodes[j]);
      if (d[i * n + j] < LooseRelation && (!zero || d[i * n + j] < *zero))
        out.setRaw(nodes[i], nodes[j], d[i * n + j]);
    }
  out.enforceLimit();
  return out;
}

Zone Zone::combine(const Zone &left, const Zone &right,
                   const std::vector<SymPair> &pairs, bool widen,
                   const std::vector<std::int64_t> &thresholds) {
  Zone out;
  if (left.isBottom() && right.isBottom()) {
    out.setBottom();
    return out;
  }
  // A bottom side contributes nothing: the other side's projection stands.
  std::vector<SymPair> all;
  all.reserve(pairs.size() + 1);
  all.push_back(SymPair{.result = ZeroSym,
                        .left = ZeroSym,
                        .right = ZeroSym,
                        .hasLeft = !left.isBottom(),
                        .hasRight = !right.isBottom()});
  // (Only a symbol a side's zone bounds, or one several results share on
  // a side, `x - x = 0`, can give a bound in the result.)
  const std::vector<Sym> leftSyms = left.symbols();
  const std::vector<Sym> rightSyms = right.symbols();
  auto sharedOn = [&](bool isLeft) {
    std::vector<Sym> used;
    for (const SymPair &pair : pairs)
      if (isLeft ? pair.hasLeft : pair.hasRight)
        used.push_back(isLeft ? pair.left : pair.right);
    std::sort(used.begin(), used.end());
    std::vector<Sym> shared;
    for (std::size_t i = 1; i < used.size(); ++i)
      if (used[i] == used[i - 1] &&
          (shared.empty() || shared.back() != used[i]))
        shared.push_back(used[i]);
    return shared;
  };
  const std::vector<Sym> leftShared = sharedOn(true);
  const std::vector<Sym> rightShared = sharedOn(false);
  auto bounded = [](const std::vector<Sym> &syms,
                    const std::vector<Sym> &shared, Sym sym) {
    return std::binary_search(syms.begin(), syms.end(), sym) ||
           std::binary_search(shared.begin(), shared.end(), sym);
  };
  for (const SymPair &pair : pairs) {
    SymPair copy = pair;
    copy.hasLeft = copy.hasLeft && !left.isBottom();
    copy.hasRight = copy.hasRight && !right.isBottom();
    if (!(copy.hasLeft && bounded(leftSyms, leftShared, copy.left)) &&
        !(copy.hasRight && bounded(rightSyms, rightShared, copy.right)))
      continue;
    all.push_back(copy);
  }
  // Each side's stored bounds against zero, per entry of `all`.
  const std::size_t count = all.size();
  std::vector<std::optional<std::int64_t>> upLeft(count), downLeft(count),
      upRight(count), downRight(count);
  for (std::size_t i = 1; i < count; ++i) {
    if (all[i].hasLeft) {
      upLeft[i] = left.stored(all[i].left, ZeroSym);
      downLeft[i] = left.stored(ZeroSym, all[i].left);
    }
    if (all[i].hasRight) {
      upRight[i] = right.stored(all[i].right, ZeroSym);
      downRight[i] = right.stored(ZeroSym, all[i].right);
    }
  }
  auto sideBound = [&](bool isLeft, std::size_t ia,
                       std::size_t ib) -> std::optional<std::int64_t> {
    if (ib == 0)
      return isLeft ? upLeft[ia] : upRight[ia];
    if (ia == 0)
      return isLeft ? downLeft[ib] : downRight[ib];
    return isLeft ? left.bound(all[ia].left, all[ib].left)
                  : right.bound(all[ia].right, all[ib].right);
  };
  auto nextThreshold = [&](std::int64_t value) -> std::optional<std::int64_t> {
    std::optional<std::int64_t> best;
    for (std::int64_t t : thresholds)
      if (t >= value && (!best || t < *best))
        best = t;
    return best;
  };
  // The result bound on `a - b`, from the sides' (what zero implies
  // included).
  auto combined = [&](std::size_t ia,
                      std::size_t ib) -> std::optional<std::int64_t> {
    const SymPair &a = all[ia];
    const SymPair &b = all[ib];
    bool leftApplies = a.hasLeft && b.hasLeft;
    bool rightApplies = a.hasRight && b.hasRight;
    std::optional<std::int64_t> lb;
    std::optional<std::int64_t> rb;
    if (leftApplies)
      lb = sideBound(true, ia, ib);
    if (rightApplies)
      rb = sideBound(false, ia, ib);
    std::optional<std::int64_t> result;
    if (leftApplies && rightApplies) {
      if (!widen) {
        if (lb && rb)
          result = std::max(*lb, *rb);
      } else if (lb && rb) {
        // A bound between two symbols stops only at -1, 0 or 1 (`i - n`
        // around a loop exit); the program's constants are for bounds
        // against zero (`i <= 3`), or the relation climbs one constant a
        // round.
        bool relational = a.result != ZeroSym && b.result != ZeroSym;
        if (*rb <= *lb)
          result = *lb;
        else if (!relational)
          result = nextThreshold(*rb);
        else if (*rb <= 1)
          result = *rb <= -1 ? -1 : *rb <= 0 ? 0 : 1;
      }
    } else if (leftApplies || rightApplies) {
      // Only one side has both values. A bound against zero, or between
      // two symbols that side alone has, constrains nothing the other
      // side has; a relation to a symbol both sides have would, through
      // the closure, tighten that symbol with one side's facts only.
      bool aBoth = a.hasLeft && a.hasRight;
      bool bBoth = b.hasLeft && b.hasRight;
      bool constantBound = a.result == ZeroSym || b.result == ZeroSym;
      if (constantBound || (!aBoth && !bBoth))
        result = leftApplies ? lb : rb;
    }
    return result;
  };
  // Bounds against zero first: whether a relation says more than they do
  // depends on them.
  std::vector<std::tuple<Sym, Sym, std::int64_t>> entries;
  std::vector<std::optional<std::int64_t>> outUp(count), outDown(count);
  for (std::size_t i = 1; i < count; ++i) {
    if ((outUp[i] = combined(i, 0)))
      entries.emplace_back(all[i].result, ZeroSym, *outUp[i]);
    if ((outDown[i] = combined(0, i)))
      entries.emplace_back(ZeroSym, all[i].result, *outDown[i]);
  }
  // Relations: those a side stores, those between results sharing a
  // symbol on a side (`x - x = 0`), and those each side's bounds against
  // zero imply that the result's do not (both values moving the same way
  // between the sides, `i` and `j` counted together); widening, every pair
  // bounded on both sides.
  using Index = std::vector<std::pair<Sym, std::size_t>>;
  Index leftBy;
  Index rightBy;
  for (std::size_t i = 1; i < count; ++i) {
    if (all[i].hasLeft)
      leftBy.emplace_back(all[i].left, i);
    if (all[i].hasRight)
      rightBy.emplace_back(all[i].right, i);
  }
  std::sort(leftBy.begin(), leftBy.end());
  std::sort(rightBy.begin(), rightBy.end());
  auto usersOf = [](const Index &by, Sym sym) {
    return std::equal_range(
        by.begin(), by.end(), std::make_pair(sym, std::size_t{0}),
        [](const auto &x, const auto &y) { return x.first < y.first; });
  };
  std::vector<std::pair<std::uint32_t, std::uint32_t>> candidates;
  auto collect = [&](const Zone &side, const Index &by) {
    for (const auto &[x, row] : side.rows) {
      if (x == ZeroSym)
        continue;
      auto [fromBegin, fromEnd] = usersOf(by, x);
      if (fromBegin == fromEnd)
        continue;
      for (const auto &[y, c] : row) {
        if (y == ZeroSym)
          continue;
        auto [toBegin, toEnd] = usersOf(by, y);
        for (auto i = fromBegin; i != fromEnd; ++i)
          for (auto j = toBegin; j != toEnd; ++j)
            candidates.emplace_back(i->second, j->second);
      }
    }
    for (std::size_t start = 0; start < by.size();) {
      std::size_t end = start;
      while (end < by.size() && by[end].first == by[start].first)
        ++end;
      if (end - start > 1)
        for (std::size_t i = start; i < end; ++i)
          for (std::size_t j = start; j < end; ++j)
            candidates.emplace_back(by[i].second, by[j].second);
      start = end;
    }
  };
  if (!left.isBottom())
    collect(left, leftBy);
  if (!right.isBottom())
    collect(right, rightBy);
  {
    auto negated = [](const std::optional<std::int64_t> &down)
        -> std::optional<std::int64_t> {
      if (!down || *down == INT64_MIN)
        return std::nullopt;
      return -*down;
    };
    // (-1, 0, 1: how the upper bound of a, or the lower bound of b, moves
    // from the left side to the right; 2 when a side has none.)
    std::vector<std::size_t> upUp, upDown, downUp, downDown;
    // (Widening: bounds that fall or rise the other way.)
    std::vector<std::size_t> upperFalls, lowerRises;
    for (std::size_t i = 1; i < count; ++i) {
      const SymPair &p = all[i];
      if (!p.hasLeft || !p.hasRight)
        continue;
      const auto &ul = upLeft[i];
      const auto &ur = upRight[i];
      auto ll = negated(downLeft[i]);
      auto lr = negated(downRight[i]);
      if (widen) {
        // (Widening: a relation neither side stores gives one the result's
        // bounds do not imply only when a bound moved, and then only when
        // the right side's `upper(a) - lower(b)` is at most 1; `upUp` and
        // `downDown` here hold the moving ones, `upDown` and `downUp` every
        // bounded one, paired below within that window.)
        if (ul && ur) {
          upDown.push_back(i);
          if (*ur > *ul)
            upUp.push_back(i);
          if (*ur < *ul)
            upperFalls.push_back(i);
        }
        if (ll && lr) {
          downUp.push_back(i);
          if (*lr < *ll)
            downDown.push_back(i);
          if (*lr > *ll)
            lowerRises.push_back(i);
        }
        continue;
      }
      if (ul && ur && *ur > *ul)
        upUp.push_back(i);
      if (ul && ur && *ur < *ul)
        upDown.push_back(i);
      if (ll && lr && *lr > *ll)
        downUp.push_back(i);
      if (ll && lr && *lr < *ll)
        downDown.push_back(i);
    }
    if (!widen) {
      for (std::size_t i : upUp)
        for (std::size_t j : downUp)
          candidates.emplace_back(i, j);
      for (std::size_t i : upDown)
        for (std::size_t j : downDown)
          candidates.emplace_back(i, j);
    } else {
      auto upperR = [&](std::size_t i) { return *upRight[i]; };
      auto lowerR = [&](std::size_t j) { return *negated(downRight[j]); };
      // Lower bounds (right side) descending: those with `upper(a) -
      // lower(b) <= 1` come first.
      std::vector<std::size_t> byLower = downUp;
      std::sort(
          byLower.begin(), byLower.end(),
          [&](std::size_t x, std::size_t y) { return lowerR(x) > lowerR(y); });
      for (std::size_t i : upUp) {
        const std::int64_t up = upperR(i);
        for (std::size_t j : byLower) {
          auto gap = addBound(up, -lowerR(j));
          if (!gap || *gap > 1)
            break;
          candidates.emplace_back(i, j);
        }
      }
      // A relation the right side keeps as tight as the left's, where both
      // bounds moved by as much (then the widened bounds lose it).
      for (std::size_t i : upUp)
        for (std::size_t j : lowerRises)
          candidates.emplace_back(i, j);
      for (std::size_t i : upperFalls)
        for (std::size_t j : downDown)
          candidates.emplace_back(i, j);
      // Upper bounds (right side) ascending, for the moving lower bounds.
      std::vector<std::size_t> byUpper = upDown;
      std::sort(
          byUpper.begin(), byUpper.end(),
          [&](std::size_t x, std::size_t y) { return upperR(x) < upperR(y); });
      for (std::size_t j : downDown) {
        const std::int64_t low = lowerR(j);
        for (std::size_t i : byUpper) {
          auto gap = addBound(upperR(i), -low);
          if (!gap || *gap > 1)
            break;
          candidates.emplace_back(i, j);
        }
      }
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  for (const auto &[ia, ib] : candidates) {
    if (all[ia].result == all[ib].result)
      continue;
    auto result = combined(ia, ib);
    if (!result || *result >= LooseRelation)
      continue;
    // (What the result's bounds against zero imply: `Zone::implied`.)
    std::optional<std::int64_t> zero;
    if (outUp[ia] && outDown[ib])
      if (auto sum = addBound(*outUp[ia], *outDown[ib]);
          sum && *sum < LooseRelation)
        zero = sum;
    if (!zero || *result < *zero)
      entries.emplace_back(all[ia].result, all[ib].result, *result);
  }
  out.assign(std::move(entries));
  if (!widen)
    out = closure(out, {});
  out.enforceLimit();
  return out;
}

std::string Zone::toString(const std::function<std::string(Sym)> &name) const {
  if (bottom)
    return "bottom";
  std::string out;
  auto spell = [&](Sym sym) {
    return sym == ZeroSym ? std::string("0") : name(sym);
  };
  for (const auto &[x, row] : rows)
    for (const auto &[y, c] : row) {
      if (!out.empty())
        out += ", ";
      if (y == ZeroSym)
        out += spell(x) + " <= " + std::to_string(c);
      else if (x == ZeroSym)
        out += spell(y) + " >= " + std::to_string(-c);
      else
        out += spell(x) + " - " + spell(y) + " <= " + std::to_string(c);
    }
  return out;
}

} // namespace weavec::core
