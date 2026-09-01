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
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_ALIASRELATION_H
#define WEAVEC_CORE_ALIASRELATION_H

#include "weavec/Core/Place.h"

#include <cstddef>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace weavec::core {

class AliasRelation {
public:
  /// Records that `a` and `b` hold the same value: relates each of them to
  /// the other and to the other's current aliases.
  void unite(PlaceId a, PlaceId b);

  /// Forgets everything `place` may alias, e.g. because it was reassigned.
  void separate(PlaceId place);

  /// True if `a` and `b` may hold the same value (every place aliases
  /// itself).
  [[nodiscard]] bool mayAlias(PlaceId a, PlaceId b) const noexcept;

  /// `place` and every place it may alias, ascending.
  [[nodiscard]] std::vector<PlaceId> members(PlaceId place) const;

  /// Union of the two relations: "may alias on either incoming path".
  void join(const AliasRelation &other);

  /// Number of places related to at least one other place.
  [[nodiscard]] std::size_t size() const noexcept { return adjacent.size(); }

  /// Every related pair `(a, b)` with `a < b`, ascending.
  [[nodiscard]] std::vector<std::pair<PlaceId, PlaceId>> pairs() const;

  friend bool operator==(const AliasRelation &,
                         const AliasRelation &) = default;

private:
  // place -> the places it may alias (never itself; never empty).
  std::map<PlaceId, std::set<PlaceId>> adjacent;

  void relate(PlaceId a, PlaceId b);
};

} // namespace weavec::core

#endif // WEAVEC_CORE_ALIASRELATION_H
