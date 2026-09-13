//===- Union.cpp - Overlapping member evidence (RFC 0025) ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Union.h"

#include <algorithm>
#include <charconv>

namespace weavec::core {

bool UnionMember::valid() const {
  return object.valid() && value.valid() && value.bytes <= object.bytes &&
         value.alignment <= object.alignment && !name.empty() &&
         name.size() <= 1024 && std::ranges::all_of(name, [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
         });
}

std::string UnionMember::encode() const {
  if (!valid())
    return {};
  std::string result = pointer ? "union1:p:" : "union1:s:";
  for (const auto &text : {object.toString(), value.toString(), name})
    result += std::to_string(text.size()) + ':' + text;
  return result;
}

std::optional<UnionMember> UnionMember::decode(std::string_view text) {
  if (text.size() > 18000 ||
      (!text.starts_with("union1:p:") && !text.starts_with("union1:s:")))
    return std::nullopt;
  const auto original = text;
  UnionMember result;
  result.pointer = text[7] == 'p';
  text.remove_prefix(9);
  const auto field = [&]() -> std::optional<std::string> {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 5)
      return std::nullopt;
    std::size_t size = 0;
    const auto number = text.substr(0, colon);
    const auto parsed =
        std::from_chars(number.data(), number.data() + number.size(), size);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + colon ||
        number != std::to_string(size) || size > text.size() - colon - 1)
      return std::nullopt;
    text.remove_prefix(colon + 1);
    std::string value(text.substr(0, size));
    text.remove_prefix(size);
    return value;
  };
  const auto object = field();
  const auto value = field();
  const auto name = field();
  if (!object || !value || !name || !text.empty())
    return std::nullopt;
  const auto owner = ObjectType::parse(*object);
  const auto member = ObjectType::parse(*value);
  if (!owner || !member)
    return std::nullopt;
  result.object = *owner;
  result.value = *member;
  result.name = *name;
  return result.valid() && result.encode() == original ? std::optional(result)
                                                       : std::nullopt;
}

void UnionState::set(PlaceId storage, std::string member, PlaceGuard when) {
  if (!UnionMember::decode(member) ||
      (!members.contains(storage) && members.size() >= MaxUnionObjects)) {
    invalidate(storage);
    return;
  }
  members[storage] = {{.member = std::move(member), .when = std::move(when)}};
}

void UnionState::invalidate(PlaceId storage) {
  members.erase(storage);
  if (written.size() < MaxUnionObjects)
    written.insert(storage);
  else
    havoc = true;
}

void UnionState::invalidateAll() {
  members.clear();
  havoc = true;
}

void UnionState::forgetDependency(PlaceId place) {
  for (auto &[storage, facts] : members) {
    (void)storage;
    std::erase_if(facts,
                  [&](const auto &fact) { return fact.when.dependsOn(place); });
    for (auto &fact : facts)
      if (fact.pointer &&
          (fact.pointer->offset.place == place ||
           (fact.pointer->extent && fact.pointer->extent->place == place)))
        fact.pointer.reset();
  }
  std::erase_if(members,
                [](const auto &entry) { return entry.second.empty(); });
}

void UnionState::forgetPointer(PlaceId holder) {
  for (auto &[storage, facts] : members) {
    (void)storage;
    for (auto &fact : facts)
      if (fact.pointer && fact.pointer->holder == holder)
        fact.pointer.reset();
  }
}

void UnionState::copy(PlaceId source, PlaceId destination) {
  if (source == destination)
    return;
  const auto found = members.find(source);
  const auto facts =
      found == members.end() ? std::vector<UnionWitness>{} : found->second;
  invalidate(destination);
  if (!facts.empty() && members.size() < MaxUnionObjects) {
    members[destination] = facts;
    // Analysis projects holder identities when copying a record subtree.
    // Without that mapping this standalone member copy carries no pointer.
    for (auto &fact : members[destination])
      fact.pointer.reset();
  }
}

bool UnionState::mayRequire(PlaceId storage) const {
  return !havoc && !written.contains(storage);
}

static bool disjoint(const PlaceGuard &a, const PlaceGuard &b) {
  for (const auto &[path, fact] : a.conditions)
    if (const auto found = b.conditions.find(path);
        found != b.conditions.end() && fact.disjointFrom(found->second))
      return true;
  for (const auto &[pair, equal] : a.pointers)
    if (const auto other = b.pointerFact(pair.first, pair.second);
        other && *other != equal)
      return true;
  for (const auto &predicate : a.integers) {
    const auto value = predicate.evaluate([&](PlaceId place, IntegerType type) {
      const auto found = b.conditions.find(place);
      return found != b.conditions.end() && !found->second.isPointer()
                 ? found->second.inType(type)
                 : IntegerRange::full(type);
    });
    if (value && !*value)
      return true;
  }
  return false;
}

bool UnionState::join(const UnionState &other,
                      const std::vector<PlaceGuard> &left,
                      const std::vector<PlaceGuard> &right) {
  const auto before = *this;
  havoc |= other.havoc;
  for (const auto storage : other.written)
    if (written.size() < MaxUnionObjects)
      written.insert(storage);
    else
      havoc = true;
  std::set<PlaceId> storage;
  for (const auto &[place, facts] : members) {
    (void)facts;
    storage.insert(place);
  }
  for (const auto &[place, facts] : other.members) {
    (void)facts;
    storage.insert(place);
  }
  for (const auto place : storage) {
    const auto a = members.contains(place) ? members.at(place)
                                           : std::vector<UnionWitness>{};
    const auto b = other.members.contains(place) ? other.members.at(place)
                                                 : std::vector<UnionWitness>{};
    std::vector<UnionWitness> common;
    for (const auto &fact : a)
      if (std::ranges::find(b, fact) != b.end())
        common.push_back(fact);
    const auto conditional = [&](const auto &facts, const auto &incoming,
                                 const auto &opposite) {
      if (opposite.empty())
        return;
      const auto excluded = [&](const PlaceGuard &guard) {
        return std::ranges::all_of(opposite, [&](const auto &path) {
          return disjoint(guard, path) || disjoint(path, guard);
        });
      };
      for (const auto &fact : facts) {
        if (excluded(fact.when)) {
          common.push_back(fact);
          continue;
        }
        for (const auto &path : incoming) {
          if (fact.when.size() + path.size() > MaxGuardConjuncts)
            continue;
          auto guarded = fact;
          guarded.when.conjoin(path);
          if (excluded(guarded.when))
            common.push_back(std::move(guarded));
        }
      }
    };
    conditional(a, left, right);
    conditional(b, right, left);
    std::ranges::sort(common);
    common.erase(std::ranges::unique(common).begin(), common.end());
    if (common.empty() || common.size() > MaxUnionAlternatives)
      members.erase(place);
    else if (members.contains(place) || members.size() < MaxUnionObjects)
      members[place] = std::move(common);
  }
  return before != *this;
}

} // namespace weavec::core
