//===- Relation.h - Order relations between integer places -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0011, *Relations*. RFC 0009's scalar facts describe one integer place
// against constants (zero, positive, a known value). A bounds check needs
// two places against each other: after `if (i < n)` the access `a[i]` on an
// object of `n` elements is in bounds, and after `for (i = 0; i <= n; i++)`
// it may not be. `RelationTracker` keeps, per unordered pair of integer
// places, the strongest order relation the path has established.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_RELATION_H
#define WEAVEC_CORE_RELATION_H

#include "weavec/Core/Place.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace weavec::core {

enum class Relation : std::uint8_t {
  Less,
  LessEqual,
  Equal,
  GreaterEqual,
  Greater,
};

/// `rhs REL lhs` given `lhs REL rhs`.
[[nodiscard]] Relation flipped(Relation relation) noexcept;
/// The relation both `a` and `b` imply, if there is one (`Less` and
/// `LessEqual` is `Less`; `Less` and `Greater` is nothing: the path is
/// infeasible or the facts are stale).
[[nodiscard]] std::optional<Relation> narrow(Relation a, Relation b) noexcept;
/// The weakest relation implied by either `a` or `b`, if any (`Less` or
/// `Equal` is `LessEqual`; `Less` or `Greater` is nothing).
[[nodiscard]] std::optional<Relation> widen(Relation a, Relation b) noexcept;
[[nodiscard]] std::string_view spelling(Relation relation) noexcept;

class RelationTracker {
public:
  /// `lhs REL rhs` holds from here on; narrows any relation already known
  /// about the pair. When the two contradict (`i < n` then `i > n`) the pair
  /// is forgotten rather than made infeasible: the second fact wins.
  void learn(PlaceId lhs, Relation relation, PlaceId rhs);

  /// The relation `lhs REL rhs` known about the pair, if any: learnt for
  /// the pair itself, or for a place known equal to one side (`j = i; if (j
  /// < n)` relates `i` and `n`; one hop only).
  [[nodiscard]] std::optional<Relation> between(PlaceId lhs, PlaceId rhs) const;

  /// `place` was compared with a constant by an ordering (`n > 4`), which
  /// the scalar facts record only as a class: the path is conditioned on
  /// its value in a way no summary guard can spell (RFC 0011, *Extents in
  /// summaries*).
  void noteBounded(PlaceId place);
  [[nodiscard]] bool isBounded(PlaceId place) const;

  /// `place <= bound` holds from here on (`i < 8` says `i <= 7`); narrows a
  /// bound already known and notes the place bounded. The class facts keep
  /// the sign of a value, this keeps how large it can be: the boundary of
  /// `for (i = 0; i < 8; i++)` is what an access `a[i]` in the body needs
  /// (RFC 0011, *Relations*).
  void learnAtMost(PlaceId place, std::int64_t bound);
  /// The constant `place` is known to be at most, if any: learnt for the
  /// place itself, or for one known equal to it (one hop).
  [[nodiscard]] std::optional<std::int64_t> atMost(PlaceId place) const;
  /// True if the path's facts condition anything on `place`: a relation
  /// with another place, or a bound.
  [[nodiscard]] bool conditions(PlaceId place) const;

  /// `place` was written: nothing is known about it against anything.
  void forget(PlaceId place);

  /// Keeps a pair only when both sides know it, as the weakest relation
  /// either side implies, and an upper bound only when both sides know one,
  /// as the larger; a place bounded on either side stays bounded. Returns
  /// whether this changed.
  bool join(const RelationTracker &other);

  [[nodiscard]] bool empty() const noexcept {
    return pairs.empty() && bounded.empty() && upper.empty();
  }
  /// Every pair known, as `(min, max) -> min REL max`.
  [[nodiscard]] const std::map<std::pair<PlaceId, PlaceId>, Relation> &
  all() const noexcept {
    return pairs;
  }
  /// Every upper bound known, as `place -> place <= bound`.
  [[nodiscard]] const std::map<PlaceId, std::int64_t> &
  allAtMost() const noexcept {
    return upper;
  }

  friend bool operator==(const RelationTracker &,
                         const RelationTracker &) = default;

private:
  /// The relation learnt for the pair itself.
  [[nodiscard]] std::optional<Relation> directly(PlaceId lhs,
                                                 PlaceId rhs) const;

  // Keyed on `(min, max)`; the relation is stated `min REL max`.
  std::map<std::pair<PlaceId, PlaceId>, Relation> pairs;
  std::set<PlaceId> bounded;
  /// `place <= upper[place]`.
  std::map<PlaceId, std::int64_t> upper;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_RELATION_H
