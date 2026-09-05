//===- AliasRelation.cpp - May-alias relation over places -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AliasRelation.h"

#include <algorithm>

namespace weavec::core {

/// Two claims about one edge: the offset only if they agree, the witness
/// only if they agree, same-share if either is (a release may then reach the
/// other end; RFC 0010).
static AliasEdge merge(const AliasEdge &lhs, const AliasEdge &rhs) {
  PointerOffset offset = lhs.offset;
  offset.join(rhs.offset);
  return AliasEdge{.offset = std::move(offset),
                   .element = lhs.element == rhs.element
                                  ? lhs.element
                                  : ElementWitness::unknown(),
                   .sameShare = lhs.sameShare || rhs.sameShare};
}

/// The element of a target place that an alias `x` of a source place holds,
/// given that the target's element `ofTarget` is the source's element
/// `ofSource` and `x` holds the source's element `xOfSource` (which matches
/// `ofSource`). When the target covers all of the source, `x` names the same
/// element of the target as of the source; otherwise `x` is exactly the
/// element of the target that was copied.
static ElementWitness through(ElementWitness ofTarget, ElementWitness ofSource,
                              ElementWitness xOfSource) {
  if (!ofSource.isWhole())
    return ofTarget;
  if (ofTarget.isWhole())
    return xOfSource;
  if (xOfSource.isWhole())
    return ofTarget;
  return ElementWitness::unknown();
}

void AliasRelation::relate(PlaceId a, PlaceId b, AliasEdge toB, AliasEdge toA) {
  if (a == b)
    return;
  auto [ab, insertedAb] = adjacent[a].try_emplace(b, toB);
  if (!insertedAb)
    ab->second = merge(ab->second, toB);
  auto [ba, insertedBa] = adjacent[b].try_emplace(a, toA);
  if (!insertedBa)
    ba->second = merge(ba->second, toA);
}

void AliasRelation::unite(PlaceId a, PlaceId b, PointerOffset offset,
                          ElementWitness elementA, ElementWitness elementB,
                          bool sameShare, bool alternative) {
  if (a == b)
    return;
  // Snapshot first: relating mutates the maps being iterated. Each entry is
  // `(x, edge from the snapshot's place to x, edge from x back)`.
  struct Neighbour {
    PlaceId place;
    AliasEdge out;
    AliasEdge back;
  };
  const auto neighboursOf = [this](PlaceId place) {
    std::vector<Neighbour> result;
    if (const auto it = adjacent.find(place); it != adjacent.end()) {
      for (const auto &[other, out] : it->second)
        result.push_back(Neighbour{
            .place = other,
            .out = out,
            .back = adjacent.find(other)->second.at(place),
        });
    }
    return result;
  };
  const auto aliasesOfA = neighboursOf(a);
  const auto aliasesOfB = neighboursOf(b);

  // `a = b + offset`: from `a`, `b` is at `-offset`; from `b`, `a` is at
  // `+offset`.
  relate(
      a, b,
      AliasEdge{.offset = offset.negated(),
                .element = elementB,
                .sameShare = sameShare},
      AliasEdge{.offset = offset, .element = elementA, .sameShare = sameShare});
  // `x` aliases element `x.back.element` of `b`; `a` aliases `elementB` of
  // it. They are the same element or `x` is not related to `a`.
  for (const Neighbour &x : aliasesOfB) {
    if (x.place == a || !x.back.element.matches(elementB))
      continue;
    // `x = b + x.out.offset` and `a = b + offset`: `x = a + (x.out - offset)`.
    const PointerOffset toX = offset.negated().plus(x.out.offset);
    const bool shareAx = sameShare && x.out.sameShare;
    relate(a, x.place,
           AliasEdge{
               .offset = toX, .element = x.out.element, .sameShare = shareAx},
           AliasEdge{.offset = toX.negated(),
                     .element = through(elementA, elementB, x.back.element),
                     .sameShare = shareAx});
  }
  // One arm of several: what `a` holds on the other arms is not `b`.
  if (alternative)
    return;
  for (const Neighbour &x : aliasesOfA) {
    if (x.place == b || !x.back.element.matches(elementA))
      continue;
    // `x = a + x.out.offset` and `b = a - offset`: `x = b + (offset + x.out)`.
    const PointerOffset toX = offset.plus(x.out.offset);
    const bool shareBx = sameShare && x.out.sameShare;
    relate(b, x.place,
           AliasEdge{
               .offset = toX, .element = x.out.element, .sameShare = shareBx},
           AliasEdge{.offset = toX.negated(),
                     .element = through(elementB, elementA, x.back.element),
                     .sameShare = shareBx});
  }
}

void AliasRelation::shift(PlaceId place, const PointerOffset &step) {
  if (step.isZero())
    return;
  const auto it = adjacent.find(place);
  if (it == adjacent.end())
    return;
  // `x = place_old + o`, `place_new = place_old + step`: `x = place_new + (o
  // - step)`, and from `x`, `place_new = x + (step - o)`.
  for (auto &[other, edge] : it->second) {
    edge.offset = step.negated().plus(edge.offset);
    adjacent.find(other)->second.at(place).offset = edge.offset.negated();
  }
}

void AliasRelation::separate(PlaceId place) {
  const auto it = adjacent.find(place);
  if (it == adjacent.end())
    return;
  for (const auto &[other, edge] : it->second) {
    const auto otherIt = adjacent.find(other);
    otherIt->second.erase(place);
    if (otherIt->second.empty())
      adjacent.erase(otherIt);
  }
  adjacent.erase(it);
}

void AliasRelation::separateIf(const std::function<bool(PlaceId)> &dead) {
  std::vector<PlaceId> victims;
  for (const auto &[place, aliases] : adjacent) {
    if (dead(place))
      victims.push_back(place);
  }
  for (const PlaceId place : victims)
    separate(place);
}

void AliasRelation::separateExact(PlaceId a, PlaceId b) {
  if (a == b)
    return;
  const auto ab = adjacent.find(a);
  if (ab == adjacent.end())
    return;
  const auto edge = ab->second.find(b);
  if (edge == ab->second.end() || !edge->second.exact())
    return;
  ab->second.erase(edge);
  if (ab->second.empty())
    adjacent.erase(ab);
  const auto ba = adjacent.find(b);
  ba->second.erase(a);
  if (ba->second.empty())
    adjacent.erase(ba);
}

bool AliasRelation::mayAlias(PlaceId a, PlaceId b) const noexcept {
  if (a == b)
    return true;
  const auto it = adjacent.find(a);
  return it != adjacent.end() && it->second.contains(b);
}

bool AliasRelation::isExact(PlaceId a, PlaceId b) const noexcept {
  if (a == b)
    return true;
  const auto found = edge(a, b);
  return found && found->exact();
}

std::optional<PointerOffset> AliasRelation::offsetOf(PlaceId a,
                                                     PlaceId b) const {
  if (a == b)
    return PointerOffset::zero();
  const auto found = edge(a, b);
  if (!found)
    return std::nullopt;
  return found->offset;
}

bool AliasRelation::sameShare(PlaceId a, PlaceId b) const noexcept {
  if (a == b)
    return true;
  const auto found = edge(a, b);
  return !found || found->sameShare;
}

std::optional<AliasEdge> AliasRelation::edge(PlaceId a,
                                             PlaceId b) const noexcept {
  const auto it = adjacent.find(a);
  if (it == adjacent.end())
    return std::nullopt;
  const auto found = it->second.find(b);
  if (found == it->second.end())
    return std::nullopt;
  return found->second;
}

std::vector<PlaceId> AliasRelation::members(PlaceId place) const {
  std::vector<PlaceId> result{place};
  if (const auto it = adjacent.find(place); it != adjacent.end()) {
    for (const auto &[other, edge] : it->second)
      result.push_back(other);
  }
  std::ranges::sort(result);
  return result;
}

std::vector<std::pair<PlaceId, AliasEdge>>
AliasRelation::edgesFrom(PlaceId place) const {
  std::vector<std::pair<PlaceId, AliasEdge>> result;
  if (const auto it = adjacent.find(place); it != adjacent.end())
    result.assign(it->second.begin(), it->second.end());
  return result;
}

bool AliasRelation::join(const AliasRelation &other) {
  bool changed = false;
  for (const auto &[place, aliases] : other.adjacent) {
    auto &mine = adjacent[place];
    for (const auto &[alias, edge] : aliases) {
      // Both directions are visited by the outer loop; merge each on its
      // own side.
      auto [it, inserted] = mine.try_emplace(alias, edge);
      if (inserted) {
        changed = true;
        continue;
      }
      const AliasEdge merged = merge(it->second, edge);
      if (merged != it->second) {
        it->second = merged;
        changed = true;
      }
    }
  }
  return changed;
}

std::vector<std::pair<PlaceId, PlaceId>> AliasRelation::pairs() const {
  std::vector<std::pair<PlaceId, PlaceId>> result;
  for (const auto &[place, aliases] : adjacent) {
    for (const auto &[alias, edge] : aliases) {
      if (place < alias)
        result.emplace_back(place, alias);
    }
  }
  return result;
}

} // namespace weavec::core
