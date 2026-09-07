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
// RFC 0012, *Offset relations and lower bounds*: a relation carries a
// constant offset (`i < n - 1` is `i < n + (-1)`), and a place bounded
// below by a constant (`i >= 8`) is kept beside RFC 0011's upper bounds.
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
#include <vector>

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

/// RFC 0012: `lhs REL rhs + offset`. `j = i + 1` is `{Equal, 1}` on `(j,
/// i)`. Edges are kept normalised: a strict relation only ever has offset
/// zero, so `i < n - 1` reads back as `{LessEqual, -2}` and `i <= n - 1` as
/// `{Less, 0}` (the two spellings of one fact compare equal).
struct RelationEdge {
  Relation relation = Relation::Equal;
  std::int64_t offset = 0;

  /// The edge stated from the other side: `rhs FLIP lhs - offset`.
  [[nodiscard]] std::optional<RelationEdge> flipped() const noexcept {
    if (offset == INT64_MIN)
      return std::nullopt;
    return RelationEdge{.relation = core::flipped(relation), .offset = -offset};
  }

  friend bool operator==(const RelationEdge &, const RelationEdge &) = default;
};

class RelationTracker {
public:
  /// RFC 0017: modular adjustments can establish difference without an
  /// ordering or a mathematical affine equality.
  void requireDifferent(PlaceId a, PlaceId b);
  [[nodiscard]] bool different(PlaceId a, PlaceId b) const;
  [[nodiscard]] const std::set<std::pair<PlaceId, PlaceId>> &
  allDifferent() const {
    return distinct;
  }

  /// `lhs REL rhs + offset` holds from here on; narrows any relation already
  /// known about the pair. When the two contradict (`i < n` then `i > n`)
  /// the pair is forgotten rather than made infeasible: the second fact
  /// wins. Two facts with different offsets that together bound the
  /// difference on both sides (`i < n + 3` and `i > n - 3`) cannot be
  /// spelled by one edge: the second wins there too.
  void learn(PlaceId lhs, Relation relation, PlaceId rhs,
             std::int64_t offset = 0);

  /// The relation `lhs REL rhs` known about the pair with no offset, if any:
  /// learnt for the pair itself, or for a place known equal to one side (`j
  /// = i; if (j < n)` relates `i` and `n`; one hop only). A relation with an
  /// offset is not one this returns; see `edgeBetween`.
  [[nodiscard]] std::optional<Relation> between(PlaceId lhs, PlaceId rhs) const;
  /// RFC 0012: the edge `lhs REL rhs + k` known about the pair, offsets
  /// composed through the one equality hop `between` follows (`j = i + 1;
  /// if (j < n)` gives `i < n - 1`).
  [[nodiscard]] std::optional<RelationEdge> edgeBetween(PlaceId lhs,
                                                        PlaceId rhs) const;

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
  /// place itself, or for one known equal to it (one hop, the equality's
  /// offset applied).
  [[nodiscard]] std::optional<std::int64_t> atMost(PlaceId place) const;
  /// RFC 0012: `place >= bound` holds from here on (`i >= 8`, `i > 7`);
  /// narrows a bound already known and notes the place bounded. What
  /// `atMost` is for the boundary an access may reach, this is for an
  /// access that is past the end on every value allowed (`if (i >= 8)
  /// buf[i]` on eight bytes).
  void learnAtLeast(PlaceId place, std::int64_t bound);
  [[nodiscard]] std::optional<std::int64_t> atLeast(PlaceId place) const;
  /// True if the path's facts condition anything on `place`: a relation
  /// with another place, or a bound.
  [[nodiscard]] bool conditions(PlaceId place) const;

  /// `place` was written: nothing is known about it against anything.
  void forget(PlaceId place);

  /// Keeps a pair only when both sides know it, as the weakest relation
  /// either side implies (with the offsets: the hull of the two, when one
  /// edge spells it), an upper bound only when both sides know one, as the
  /// larger, a lower bound as the smaller; a place bounded on either side
  /// stays bounded. Returns whether this changed.
  bool join(const RelationTracker &other);

  [[nodiscard]] bool empty() const noexcept {
    return pairs.empty() && distinct.empty() && bounded.empty() &&
           upper.empty() && lower.empty();
  }
  /// Every pair known, as `(min, max) -> min REL max + offset`.
  [[nodiscard]] const std::map<std::pair<PlaceId, PlaceId>, RelationEdge> &
  all() const noexcept {
    return pairs;
  }
  /// Every upper bound known, as `place -> place <= bound`.
  [[nodiscard]] const std::map<PlaceId, std::int64_t> &
  allAtMost() const noexcept {
    return upper;
  }
  /// Every lower bound known, as `place -> place >= bound`.
  [[nodiscard]] const std::map<PlaceId, std::int64_t> &
  allAtLeast() const noexcept {
    return lower;
  }

  friend bool operator==(const RelationTracker &,
                         const RelationTracker &) = default;

private:
  /// The edge learnt for the pair itself.
  [[nodiscard]] std::optional<RelationEdge> directly(PlaceId lhs,
                                                     PlaceId rhs) const;
  /// The places known equal to `place`, each with the offset `place ==
  /// other + offset`.
  [[nodiscard]] std::vector<std::pair<PlaceId, std::int64_t>>
  equalsOf(PlaceId place) const;

  // Keyed on `(min, max)`; the edge is stated `min REL max + offset`.
  std::map<std::pair<PlaceId, PlaceId>, RelationEdge> pairs;
  std::set<std::pair<PlaceId, PlaceId>> distinct;
  std::set<PlaceId> bounded;
  /// `place <= upper[place]`.
  std::map<PlaceId, std::int64_t> upper;
  /// `place >= lower[place]`.
  std::map<PlaceId, std::int64_t> lower;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_RELATION_H
