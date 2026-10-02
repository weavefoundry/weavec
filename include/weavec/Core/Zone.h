//===- Zone.h - Difference-bound constraints over symbols -------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §4.4: the object engine's numeric domain. A `Zone` holds bounds
// `x - y <= c` between integer symbols, with the reserved symbol 0 standing
// for the constant zero, so `x <= c` is `x - 0 <= c` and `x >= c` is
// `0 - x <= -c`. The matrix is kept closed, so a query is a lookup; a
// bound between two symbols is stored only where it is tighter than what
// their bounds against zero imply (`x - y <= upper(x) - lower(y)`), which a
// query adds back (§4.4 *Amendment (sparse zone)*). Bounds are 64-bit; a
// bound that would not fit is dropped, which forgets and is therefore
// sound.
//
// Joins and widenings are *paired* (§4.8): the heap decides which symbol of
// each side a result symbol stands for, and the zone combines the two
// projections. A result symbol present on one side only keeps that side's
// bounds (the other side has no such value to constrain).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_ZONE_H
#define WEAVEC_CORE_ZONE_H

#include "weavec/Core/Persistent.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace weavec::core {

/// A symbolic value (RFC 0031 §4.1). Zero is the constant zero in a `Zone`
/// and "no symbol" elsewhere.
using Sym = std::uint32_t;
inline constexpr Sym ZeroSym = 0;

/// What a paired combination maps each result symbol to.
struct SymPair {
  Sym result = ZeroSym;
  /// The left and right symbols, or `ZeroSym` when the side has none.
  Sym left = ZeroSym;
  Sym right = ZeroSym;
  bool hasLeft = false;
  bool hasRight = false;
};

class Zone {
public:
  /// The largest number of symbols with relational bounds (§4.4); symbols
  /// beyond it keep their bounds against zero only.
  static constexpr std::size_t MaxRelational = 64;
  /// Bounds between two symbols from here up are not kept (§4.4
  /// *Amendment (zone cost)*).
  static constexpr std::int64_t LooseRelation =
      static_cast<std::int64_t>(std::uint64_t{1} << 31U);

  Zone() = default;

  [[nodiscard]] bool isBottom() const noexcept { return bottom; }
  void setBottom() {
    bottom = true;
    rows.clear();
    degrees.clear();
  }

  /// The bound on `x - y`, if any. `bound(x, x)` is 0.
  [[nodiscard]] std::optional<std::int64_t> bound(Sym x, Sym y) const;
  [[nodiscard]] std::optional<std::int64_t> upper(Sym x) const {
    return bound(x, ZeroSym);
  }
  [[nodiscard]] std::optional<std::int64_t> lower(Sym x) const {
    auto b = bound(ZeroSym, x);
    if (!b || *b == INT64_MIN)
      return std::nullopt;
    return -*b;
  }
  [[nodiscard]] std::optional<std::int64_t> constant(Sym x) const {
    auto lo = lower(x);
    auto hi = upper(x);
    if (lo && hi && *lo == *hi)
      return lo;
    return std::nullopt;
  }

  /// Adds `x - y <= c` and closes; the zone becomes bottom when the
  /// constraints are unsatisfiable. Returns false then.
  bool addLE(Sym x, Sym y, std::int64_t c);
  /// `x == y + c`.
  bool addEq(Sym x, Sym y, std::int64_t c) {
    return addLE(x, y, c) && addLE(y, x, c == INT64_MIN ? INT64_MAX : -c);
  }
  /// `lo <= x <= hi`, each optional.
  bool addRange(Sym x, std::optional<std::int64_t> lo,
                std::optional<std::int64_t> hi);
  /// Whether `x - y <= c` holds.
  [[nodiscard]] bool entails(Sym x, Sym y, std::int64_t c) const;
  /// Removes every bound mentioning `x`.
  void forget(Sym x);
  /// Symbols with at least one bound.
  [[nodiscard]] std::vector<Sym> symbols() const;
  /// Keeps only the symbols `keep` accepts.
  template <typename Keep>
  void restrict(Keep keep) {
    restrictTo(std::function<bool(Sym)>(keep));
  }

  /// The join (or, with `widen`, the widening of `left` by `right`) of two
  /// zones over the result symbols of `pairs`. `thresholds` are the
  /// constants a widened bound may stop at before it is dropped.
  [[nodiscard]] static Zone
  combine(const Zone &left, const Zone &right,
          const std::vector<SymPair> &pairs, bool widen,
          const std::vector<std::int64_t> &thresholds);
  /// A copy with every symbol renamed by `rename` (symbols it maps to
  /// `ZeroSym`, other than zero itself, are dropped).
  template <typename Rename>
  [[nodiscard]] Zone renamed(Rename rename) const {
    Zone out;
    if (bottom) {
      out.setBottom();
      return out;
    }
    for (const auto &[x, row] : rows) {
      Sym rx = x == ZeroSym ? ZeroSym : rename(x);
      if (x != ZeroSym && rx == ZeroSym)
        continue;
      for (const auto &[y, c] : row) {
        Sym ry = y == ZeroSym ? ZeroSym : rename(y);
        if (y != ZeroSym && ry == ZeroSym)
          continue;
        out.setRaw(rx, ry, c);
      }
    }
    return out;
  }

  /// `x - y <= c` rows, for dumps: `a - b <= 3, a <= 7`.
  [[nodiscard]] std::string
  toString(const std::function<std::string(Sym)> &name) const;

  /// The same bounds (not the same stored ones: a stored bound its
  /// symbols' bounds against zero now imply is the same as none).
  friend bool operator==(const Zone &a, const Zone &b) { return a.equals(b); }

private:
  bool bottom = false;
  /// rows[x][y] = c: `x - y <= c`, closed.
  PMap<Sym, PMap<Sym, std::int64_t>> rows;
  /// The number of relational bounds (neither side zero) each symbol is in,
  /// kept as bounds come and go: the size limit's measure.
  PMap<Sym, std::uint32_t> degrees;
  void noteAdded(Sym x, Sym y);
  void noteRemoved(Sym x, Sym y);

  void setRaw(Sym x, Sym y, std::int64_t c);
  /// Replaces the rows by `entries` (`x - y <= c`, no pair twice).
  void assign(std::vector<std::tuple<Sym, Sym, std::int64_t>> entries);
  /// The stored bound on `x - y`, without what zero implies.
  [[nodiscard]] std::optional<std::int64_t> stored(Sym x, Sym y) const;
  /// `upper(x) - lower(y)`, when both exist and it is tighter than
  /// `LooseRelation`.
  [[nodiscard]] std::optional<std::int64_t> implied(Sym x, Sym y) const;
  void restrictTo(const std::function<bool(Sym)> &keep);
  [[nodiscard]] bool equals(const Zone &other) const;
  void tighten(Sym x, Sym y, std::int64_t c);
  void enforceLimit();
  /// `addLE`, keeping the size limit only when `limit` (a bulk operation
  /// keeps it once, at its end).
  bool addLimited(Sym x, Sym y, std::int64_t c, bool limit);
  /// The closure of `zone` over `symbols`: every bound re-added.
  static Zone closure(const Zone &zone, const std::vector<Sym> &symbols);
};

} // namespace weavec::core

#endif // WEAVEC_CORE_ZONE_H
