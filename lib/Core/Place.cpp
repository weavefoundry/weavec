//===- Place.cpp - Abstract memory places ---------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Place.h"

#include <cassert>
#include <utility>

namespace weavec::core {

PlaceId PlaceTable::create(std::string displayName) {
  const auto id = static_cast<std::uint32_t>(entries.size());
  entries.push_back(Entry{.name = std::move(displayName),
                          .parent = std::nullopt,
                          .step = PathStep::Field,
                          .field = {}});
  return PlaceId{id};
}

PlaceId PlaceTable::intern(PlaceId parent, PathStep step, std::string field) {
  ChildKey key{.parent = parent.value, .step = step, .field = field};
  if (const auto it = children.find(key); it != children.end())
    return it->second;

  const Entry &parentEntry = entries[parent.value];
  std::string displayName;
  switch (step) {
  case PathStep::Field:
    // `(*p).f` is spelled `p->f`, as the user wrote it.
    if (parentEntry.parent && parentEntry.step == PathStep::Deref)
      displayName = std::string(name(*parentEntry.parent)) + "->" + field;
    else
      displayName = parentEntry.name + "." + field;
    break;
  case PathStep::Deref:
    displayName = "*" + parentEntry.name;
    break;
  case PathStep::Index:
    displayName = parentEntry.name + "[*]";
    break;
  }

  const auto id = static_cast<std::uint32_t>(entries.size());
  entries.push_back(Entry{.name = std::move(displayName),
                          .parent = parent,
                          .step = step,
                          .field = std::move(field)});
  children.emplace(std::move(key), PlaceId{id});
  return PlaceId{id};
}

PlaceId PlaceTable::field(PlaceId parent, std::string_view fieldName) {
  assert(parent.value < entries.size() && "unknown parent place");
  return intern(parent, PathStep::Field, std::string(fieldName));
}

PlaceId PlaceTable::deref(PlaceId parent) {
  assert(parent.value < entries.size() && "unknown parent place");
  return intern(parent, PathStep::Deref, {});
}

PlaceId PlaceTable::index(PlaceId parent) {
  assert(parent.value < entries.size() && "unknown parent place");
  const Entry &entry = entries[parent.value];
  if (entry.parent &&
      (entry.step == PathStep::Index || entry.step == PathStep::Deref))
    return parent;
  return intern(parent, PathStep::Index, {});
}

std::string_view PlaceTable::name(PlaceId id) const noexcept {
  if (id.value < entries.size())
    return entries[id.value].name;
  return "<unknown place>";
}

std::optional<PlaceId> PlaceTable::parent(PlaceId id) const noexcept {
  if (id.value < entries.size())
    return entries[id.value].parent;
  return std::nullopt;
}

PathStep PlaceTable::step(PlaceId id) const noexcept {
  if (id.value < entries.size())
    return entries[id.value].step;
  return PathStep::Field;
}

std::string_view PlaceTable::fieldName(PlaceId id) const noexcept {
  if (id.value < entries.size())
    return entries[id.value].field;
  return {};
}

PlaceId PlaceTable::root(PlaceId id) const noexcept {
  while (const auto up = parent(id))
    id = *up;
  return id;
}

std::size_t PlaceTable::depth(PlaceId id) const noexcept {
  std::size_t result = 0;
  while (const auto up = parent(id)) {
    ++result;
    id = *up;
  }
  return result;
}

bool PlaceTable::isDescendantOf(PlaceId id, PlaceId ancestor) const noexcept {
  while (const auto up = parent(id)) {
    if (*up == ancestor)
      return true;
    id = *up;
  }
  return false;
}

std::vector<PlaceId> PlaceTable::descendants(PlaceId id) const {
  std::vector<PlaceId> result;
  for (std::uint32_t i = 0; i < entries.size(); ++i) {
    if (isDescendantOf(PlaceId{i}, id))
      result.push_back(PlaceId{i});
  }
  return result;
}

std::vector<PlaceId> PlaceTable::ancestors(PlaceId id) const {
  std::vector<PlaceId> result;
  while (const auto up = parent(id)) {
    result.push_back(*up);
    id = *up;
  }
  return result;
}

PlaceId PlaceTable::translate(PlaceId id, PlaceId from, PlaceId to) {
  if (id == from)
    return to;
  assert(isDescendantOf(id, from) && "translate: id is not below from");
  const Entry entry = entries[id.value];
  const PlaceId newParent = translate(*entry.parent, from, to);
  switch (entry.step) {
  case PathStep::Field:
    return field(newParent, entry.field);
  case PathStep::Deref:
    return deref(newParent);
  case PathStep::Index:
    return index(newParent);
  }
  return to;
}

std::optional<PlaceId> PlaceTable::innermostDeref(PlaceId id) const noexcept {
  std::optional<PlaceId> current = id;
  while (current) {
    const Entry &entry = entries[current->value];
    if (entry.parent && entry.step == PathStep::Deref)
      return current;
    current = entry.parent;
  }
  return std::nullopt;
}

} // namespace weavec::core
