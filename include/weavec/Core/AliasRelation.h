//===- AliasRelation.h - May-alias relation over places --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `AliasRelation` records which pointer places may currently hold the same
// pointer value (RFC 0002, *Alias relation*). Copying a pointer relates the
// destination to the source and to everything the source is related to;
// reassigning a place drops all of its relations; any release or move
// through one place applies to every place related to it.
//
// The relation is symmetric and closed under *copies* but deliberately not
// transitively closed at *joins*. Two places that were each copied from `p`
// on different paths (or different loop iterations) are both related to `p`
// afterwards, but not to each other: nothing on any single path ever made
// them alias, and treating them as if it had is what turns the list-deletion
// idiom (`victim = cur; cur = cur->next; free(victim)`) into a stream of
// false positives.
//
// Each edge carries two attributes (RFC 0006, RFC 0011). It records the
// *offset* of the far end from this end: zero when the two places hold the
// same pointer value (an *exact* alias), `+1` after `q = p + 1`, a field
// after `q = &p->in`, unknown when they point into the same object at an
// offset the checker cannot name. A pointer comparison refutes only exact
// aliases; derivations compose along edges, so `container_of(&p->in)` is
// exact with `p` again. And each end carries the *element witness* of the
// access that created it: after `q = a[i]`, `q` aliases element `i` of
// `a[*]`, so a release through `q` frees that element and not the whole
// summary, and `r = a[j]` does not make `q` and `r` aliases of each other.
//
// A third attribute (RFC 0010, *Shares*) says whether the two ends hold the
// *same share* of a reference-counted object. A copy that carries a surplus
// share away (`q = obj_ref(p)`) relates `q` and `p` as distinct shares: the
// two name one object, so facts below them are mirrored, but a release of
// `q`'s share leaves `p` valid. Every other edge is same-share.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_ALIASRELATION_H
#define WEAVEC_CORE_ALIASRELATION_H

#include "weavec/Core/Moves.h"
#include "weavec/Core/Offset.h"
#include "weavec/Core/Place.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace weavec::core {

/// The attributes of one alias edge as seen from one of its ends.
struct AliasEdge {
  /// RFC 0011: where the *other* end points relative to this one. Zero for
  /// an exact alias.
  PointerOffset offset;
  /// The element of the place at the *other* end that this end aliases.
  ElementWitness element;
  /// RFC 0010: the two ends hold the same share of the object. False only
  /// for the edge a share-splitting copy made and for the edges derived
  /// from it. Joins by disjunction (a release *may* reach the other end).
  bool sameShare = true;

  /// The two ends hold the same pointer value.
  [[nodiscard]] bool exact() const noexcept { return offset.isZero(); }

  friend bool operator==(const AliasEdge &, const AliasEdge &) = default;
};

class AliasRelation {
public:
  /// Records that `a` (its element `elementA`) holds `b` (its element
  /// `elementB`) plus `offset`: the same value when the offset is zero, a
  /// pointer into the same object otherwise. Relates each of them to the
  /// other and to the other's current aliases, composing offsets along the
  /// way (RFC 0011); an alias of `b` that names a different element of `b`
  /// than `elementB` is not related to `a` at all. With `!sameShare` the new
  /// edge, and every edge derived from it, relates distinct shares (RFC
  /// 0010); an alias reached through a distinct-share edge holds a distinct
  /// share.
  ///
  /// With `alternative`, `a` holds `b` on only *one* of several paths that
  /// meet at this assignment (`a = c ? b : d`, a callee that returns one of
  /// several values): `a` is related to `b` and to `b`'s aliases, but `a`'s
  /// other aliases (the other arms) are not related to `b`. The result is
  /// the join of the per-arm relations, as the header says a join must be,
  /// not their composition: `b` and `d` never held the same value.
  void unite(PlaceId a, PlaceId b, PointerOffset offset = PointerOffset::zero(),
             ElementWitness elementA = ElementWitness::whole(),
             ElementWitness elementB = ElementWitness::whole(),
             bool sameShare = true, bool alternative = false);

  /// Forgets everything `place` may alias, e.g. because it was reassigned.
  void separate(PlaceId place);

  /// Separates every related place that satisfies `dead`: it will not be
  /// read again (RFC 0006, *Loans end at the last use of their holder*).
  /// Facts are propagated to every alias when they are made, so no other
  /// place loses anything.
  void separateIf(const std::function<bool(PlaceId)> &dead);

  /// RFC 0011: `place`'s own value moved by `step` (`p++`, `p += k`): every
  /// alias now lies `step` further back from it.
  void shift(PlaceId place, const PointerOffset &step);

  /// `a != b` was established: drops the edge between `a` and `b` if it is
  /// exact. An interior edge stays (the values differ, the object may not),
  /// and so do both places' other aliases.
  void separateExact(PlaceId a, PlaceId b);

  /// True if `a` and `b` may hold the same value (every place aliases
  /// itself).
  [[nodiscard]] bool mayAlias(PlaceId a, PlaceId b) const noexcept;

  /// True if `a` and `b` are related by an exact edge (or are the same
  /// place).
  [[nodiscard]] bool isExact(PlaceId a, PlaceId b) const noexcept;

  /// Where `b` points relative to `a`, if they are related (zero for the
  /// same place).
  [[nodiscard]] std::optional<PointerOffset> offsetOf(PlaceId a,
                                                      PlaceId b) const;

  /// True if `a` and `b` may hold the same share (or are the same place, or
  /// are unrelated: only an explicit distinct-share edge says otherwise).
  [[nodiscard]] bool sameShare(PlaceId a, PlaceId b) const noexcept;

  /// The edge from `a` to `b` (its `element` is `b`'s), if they are related.
  [[nodiscard]] std::optional<AliasEdge> edge(PlaceId a,
                                              PlaceId b) const noexcept;

  /// `place` and every place it may alias, ascending.
  [[nodiscard]] std::vector<PlaceId> members(PlaceId place) const;

  /// The places `place` may alias, each with the edge from `place` to it,
  /// ascending by place.
  [[nodiscard]] std::vector<std::pair<PlaceId, AliasEdge>>
  edgesFrom(PlaceId place) const;

  /// Union of the two relations: "may alias on either incoming path". An
  /// edge's offset is unknown if the sides disagree; its witness is unknown
  /// if the sides disagree; it is same-share if either side says so. Returns
  /// whether this relation changed.
  bool join(const AliasRelation &other);

  /// Number of places related to at least one other place.
  [[nodiscard]] std::size_t size() const noexcept { return adjacent.size(); }

  /// Every related pair `(a, b)` with `a < b`, ascending.
  [[nodiscard]] std::vector<std::pair<PlaceId, PlaceId>> pairs() const;

  friend bool operator==(const AliasRelation &,
                         const AliasRelation &) = default;

private:
  // place -> the places it may alias (never itself; never empty), with the
  // edge as seen from `place`. Stored on both ends.
  std::map<PlaceId, std::map<PlaceId, AliasEdge>> adjacent;

  /// Stores `toB` on `a`'s side and `toA` on `b`'s side, merging with any
  /// existing edge.
  void relate(PlaceId a, PlaceId b, AliasEdge toB, AliasEdge toA);
};

} // namespace weavec::core

#endif // WEAVEC_CORE_ALIASRELATION_H
