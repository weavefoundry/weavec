//===- Container.cpp - Linked storage evidence (RFC 0023) ---===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Container.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace weavec::core {

static bool nameValid(std::string_view name) {
  return !name.empty() && name.size() <= 8192 &&
         std::ranges::all_of(
             name, [](unsigned char c) { return c >= 32 && c < 127; });
}

bool ContainerShape::valid() const {
  if (!object.valid() || static_cast<unsigned>(access) > 2 ||
      initialized.size() > MaxContainerFields ||
      payloads.size() > MaxContainerFields)
    return false;
  const auto fieldValid = [&](const ContainerField &field) {
    return nameValid(field.name) && field.bytes > 0 &&
           field.offset <= object.bytes &&
           field.bytes <= object.bytes - field.offset;
  };
  if (!fieldValid(link) || !std::ranges::is_sorted(initialized) ||
      std::ranges::adjacent_find(initialized) != initialized.end() ||
      !std::ranges::all_of(initialized, fieldValid) ||
      std::ranges::find(initialized, link) == initialized.end())
    return false;
  std::set<std::string> names;
  for (const auto &field : initialized)
    if (!names.insert(field.name).second)
      return false;
  if (!std::ranges::is_sorted(payloads))
    return false;
  names.clear();
  for (const auto &payload : payloads)
    if (!fieldValid(payload.field) || payload.field.name == link.name ||
        !names.insert(payload.field.name).second ||
        !nameValid(payload.family) ||
        std::ranges::find(initialized, payload.field) == initialized.end())
      return false;
  return access == ContainerAccess::Release
             ? nameValid(family)
             : family.empty() && payloads.empty();
}

bool ContainerShape::entails(const ContainerShape &required) const {
  return valid() && required.valid() && object == required.object &&
         link == required.link &&
         static_cast<unsigned>(access) >=
             static_cast<unsigned>(required.access) &&
         (!required.terminal || terminal) &&
         std::ranges::includes(initialized, required.initialized) &&
         (required.access != ContainerAccess::Release ||
          (family == required.family && payloads == required.payloads));
}

static void appendNumber(std::string &text, std::uint64_t value) {
  text += std::to_string(value) + ':';
}
static void appendString(std::string &text, std::string_view value) {
  appendNumber(text, value.size());
  text += value;
}
static void appendField(std::string &text, const ContainerField &field) {
  appendString(text, field.name);
  appendNumber(text, field.offset);
  appendNumber(text, field.bytes);
}
std::string ContainerShape::encode() const {
  if (!valid())
    return {};
  std::string text = "chain1:";
  appendNumber(text, static_cast<unsigned>(access));
  appendNumber(text, static_cast<std::uint64_t>(terminal));
  appendString(text, object.toString());
  appendField(text, link);
  appendString(text, family);
  appendNumber(text, initialized.size());
  for (const auto &field : initialized)
    appendField(text, field);
  appendNumber(text, payloads.size());
  for (const auto &payload : payloads) {
    appendField(text, payload.field);
    appendString(text, payload.family);
  }
  return text.size() <= MaxContainerDescriptorBytes ? text : std::string{};
}
std::optional<ContainerShape> ContainerShape::decode(std::string_view text) {
  if (text.size() > MaxContainerDescriptorBytes || !text.starts_with("chain1:"))
    return std::nullopt;
  const auto original = text;
  text.remove_prefix(7);
  const auto number = [&](std::uint64_t &out) {
    const auto end = text.find(':');
    if (end == std::string_view::npos || end == 0 || end > 20)
      return false;
    const auto part = text.substr(0, end);
    const auto result =
        std::from_chars(part.data(), part.data() + part.size(), out);
    if (result.ec != std::errc{} || result.ptr != part.data() + part.size() ||
        part != std::to_string(out))
      return false;
    text.remove_prefix(end + 1);
    return true;
  };
  const auto string = [&](std::string &out) {
    std::uint64_t size = 0;
    if (!number(size) || size > text.size() ||
        size > MaxContainerDescriptorBytes)
      return false;
    out = text.substr(0, static_cast<std::size_t>(size));
    text.remove_prefix(static_cast<std::size_t>(size));
    return true;
  };
  const auto field = [&](ContainerField &out) {
    return string(out.name) && number(out.offset) && number(out.bytes);
  };
  ContainerShape result;
  std::uint64_t access = 0;
  std::uint64_t terminal = 0;
  std::uint64_t count = 0;
  std::string object;
  if (!number(access) || access > 2 || !number(terminal) || terminal > 1 ||
      !string(object) || !field(result.link) || !string(result.family) ||
      !number(count) || count > MaxContainerFields)
    return std::nullopt;
  const auto parsed = ObjectType::parse(object);
  if (!parsed)
    return std::nullopt;
  result.object = *parsed;
  result.access = static_cast<ContainerAccess>(access);
  result.terminal = terminal != 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    ContainerField value;
    if (!field(value))
      return std::nullopt;
    result.initialized.push_back(std::move(value));
  }
  if (!number(count) || count > MaxContainerFields)
    return std::nullopt;
  for (std::uint64_t i = 0; i < count; ++i) {
    ContainerPayload value;
    if (!field(value.field) || !string(value.family))
      return std::nullopt;
    result.payloads.push_back(std::move(value));
  }
  return text.empty() && result.valid() && result.encode() == original
             ? std::optional(result)
             : std::nullopt;
}

bool ContainerFact::valid() const {
  return shape.valid() && members.size() <= MaxContainerNodes &&
         inputs.size() <= MaxContainerFacts &&
         ancestors.size() <= MaxContainerFacts &&
         releasedPayloads.size() <= shape.payloads.size() &&
         (!empty || members.empty());
}
bool ContainerFact::entails(const ContainerShape &required) const {
  return valid() && required.valid() &&
         (empty ||
          (shape.entails(required) &&
           std::ranges::none_of(required.payloads, [&](const auto &payload) {
             return releasedPayloads.contains(payload.field.name);
           })));
}
const ContainerFact *ContainerFacts::find(PlaceId holder) const {
  const auto it = facts.find(holder);
  return it == facts.end() ? nullptr : &it->second;
}
bool ContainerFacts::set(PlaceId holder, ContainerFact fact) {
  if (!fact.valid() ||
      (facts.size() >= MaxContainerFacts && !facts.contains(holder))) {
    facts.erase(holder);
    exhausted = true;
    return false;
  }
  const bool empty = fact.empty;
  facts.insert_or_assign(holder, std::move(fact));
  if (empty)
    for (const auto &[other, value] : facts) {
      (void)value;
      separate(holder, other);
    }
  return true;
}
void ContainerFacts::erase(PlaceId holder) {
  facts.erase(holder);
  fresh.erase(holder);
  std::erase_if(separation, [&](const auto &pair) {
    return pair.first == holder || pair.second == holder;
  });
}
void ContainerFacts::block(PlaceId holder) {
  erase(holder);
  if (killed.size() < MaxContainerFacts) {
    killed.insert(holder);
  } else {
    exhausted = true;
    clear();
  }
}
void ContainerFacts::invalidate(PlaceId member) {
  std::vector<PlaceId> affected;
  for (const auto &[holder, fact] : facts)
    if (holder == member || fact.members.contains(member) ||
        fact.inputs.contains(member))
      affected.push_back(holder);
  for (const auto holder : affected)
    block(holder);
}
void ContainerFacts::clear() {
  invalidated = true;
  facts.clear();
  separation.clear();
  fresh.clear();
}
void ContainerFacts::separate(PlaceId first, PlaceId second) {
  if (first != second &&
      separation.size() < MaxContainerFacts * MaxContainerFacts)
    separation.emplace(std::min(first, second), std::max(first, second));
}
bool ContainerFacts::separated(PlaceId first, PlaceId second) const {
  const auto *a = find(first);
  const auto *b = find(second);
  return (a != nullptr && a->empty) || (b != nullptr && b->empty) ||
         (first != second && separation.contains({std::min(first, second),
                                                  std::max(first, second)}));
}
std::set<PlaceId> ContainerFacts::separatedFrom(PlaceId holder) const {
  std::set<PlaceId> result;
  for (const auto &[a, b] : separation) {
    if (a == holder)
      result.insert(b);
    if (b == holder)
      result.insert(a);
  }
  return result;
}
void ContainerFacts::replace(PlaceId holder) {
  facts.erase(holder);
  fresh.erase(holder);
  std::erase_if(separation, [&](const auto &pair) {
    return pair.first == holder || pair.second == holder;
  });
  for (auto &[place, fact] : facts) {
    (void)place;
    if (fact.tailOf == holder)
      fact.tailOf.reset();
    fact.ancestors.erase(holder);
  }
}
void ContainerFacts::markFresh(PlaceId holder) {
  if (fresh.size() < MaxContainerFacts)
    fresh.insert(holder);
  for (const auto &[other, fact] : facts) {
    (void)fact;
    separate(holder, other);
  }
}
bool ContainerFacts::join(const ContainerFacts &other) {
  const auto before = *this;
  exhausted |= other.exhausted;
  invalidated |= other.invalidated;
  killed.insert(other.killed.begin(), other.killed.end());
  if (killed.size() > MaxContainerFacts) {
    exhausted = true;
    clear();
    return before != *this;
  }
  std::erase_if(separation, [&](const auto &pair) {
    return !other.separation.contains(pair);
  });
  std::erase_if(fresh,
                [&](PlaceId place) { return !other.fresh.contains(place); });
  for (auto it = facts.begin(); it != facts.end();) {
    const auto *right = other.find(it->first);
    auto comparable = right ? right->shape : ContainerShape{};
    comparable.terminal = it->second.shape.terminal;
    if (!right || (!it->second.empty && !right->empty &&
                   it->second.shape != comparable)) {
      it = facts.erase(it);
      continue;
    }
    auto &left = it->second;
    const bool compatible = (left.empty || left.allocationCompatible) &&
                            (right->empty || right->allocationCompatible);
    const bool allocated = (left.empty || left.localAllocation) &&
                           (right->empty || right->localAllocation);
    if (left.empty && !right->empty)
      left.shape = right->shape;
    else if (!right->empty)
      left.shape.terminal &= right->shape.terminal;
    left.empty &= right->empty;
    left.allocationCompatible = compatible;
    left.localAllocation = allocated;
    left.suffix |= right->suffix;
    if (left.tailOf != right->tailOf)
      left.tailOf.reset();
    std::erase_if(left.ancestors, [&](PlaceId ancestor) {
      return !right->ancestors.contains(ancestor);
    });
    left.members.insert(right->members.begin(), right->members.end());
    left.inputs.insert(right->inputs.begin(), right->inputs.end());
    left.releasedPayloads.insert(right->releasedPayloads.begin(),
                                 right->releasedPayloads.end());
    if (!left.valid()) {
      exhausted = true;
      it = facts.erase(it);
    } else {
      ++it;
    }
  }
  return before != *this;
}

std::string_view toString(ContainerFailure failure) {
  switch (failure) {
  case ContainerFailure::None:
    return "proved container chain";
  case ContainerFailure::Unknown:
    return "container chain requires live represented nodes and links";
  case ContainerFailure::Cycle:
    return "container chain contains a cycle";
  case ContainerFailure::View:
    return "container chain requires compatible node storage";
  case ContainerFailure::Initialization:
    return "container chain requires initialized node fields";
  case ContainerFailure::Writable:
    return "container chain requires writable nodes";
  case ContainerFailure::Release:
    return "container chain requires live allocation bases of the matching "
           "release family";
  case ContainerFailure::Overlap:
    return "container chain requires separated owned nodes and payloads";
  case ContainerFailure::Limit:
    return "container chain proof limit reached";
  }
  return "unknown container proof failure";
}

ContainerProof ContainerGraph::prove(ContainerEdge first,
                                     const ContainerShape &shape,
                                     ContainerEdge endpoint) const {
  ContainerProof proof;
  if (!shape.valid() || !first.known || !endpoint.known)
    return proof;
  auto cursor = first;
  while (cursor != endpoint) {
    if (!cursor.known || !cursor.node)
      return proof;
    if (proof.members.size() >= MaxContainerNodes) {
      proof.failure = ContainerFailure::Limit;
      return proof;
    }
    if (!proof.members.insert(*cursor.node).second) {
      proof.failure = ContainerFailure::Cycle;
      return proof;
    }
    if (shape.terminal && proof.members.size() > 1)
      return proof;
    const auto it = nodes.find(*cursor.node);
    if (it == nodes.end() || !it->second.live)
      return proof;
    const auto &node = it->second;
    if (node.object != shape.object) {
      proof.failure = ContainerFailure::View;
      return proof;
    }
    if (!std::ranges::includes(node.initialized, shape.initialized)) {
      proof.failure = ContainerFailure::Initialization;
      return proof;
    }
    if (shape.access != ContainerAccess::Read && !node.writable) {
      proof.failure = ContainerFailure::Writable;
      return proof;
    }
    if (shape.access == ContainerAccess::Release) {
      if (!node.allocationBase || node.family != shape.family) {
        proof.failure = ContainerFailure::Release;
        return proof;
      }
      for (const auto &payload : shape.payloads) {
        const auto edge = node.payloads.find(payload.field.name);
        if (edge == node.payloads.end() || !edge->second.known)
          return proof;
        if (!edge->second.node)
          continue;
        const auto found = nodes.find(*edge->second.node);
        if (found == nodes.end() || !found->second.live ||
            !found->second.allocationBase ||
            found->second.family != payload.family) {
          proof.failure = ContainerFailure::Release;
          return proof;
        }
        if (!proof.payloads.insert(*edge->second.node).second) {
          proof.failure = ContainerFailure::Overlap;
          return proof;
        }
        if (edge->second.node == endpoint.node) {
          proof.failure = ContainerFailure::Overlap;
          return proof;
        }
        if (proof.members.size() + proof.payloads.size() > MaxContainerNodes) {
          proof.failure = ContainerFailure::Limit;
          return proof;
        }
      }
    }
    cursor = node.next;
  }
  for (const auto member : proof.members)
    if (proof.payloads.contains(member)) {
      proof.failure = ContainerFailure::Overlap;
      return proof;
    }
  proof.failure = ContainerFailure::None;
  return proof;
}

} // namespace weavec::core
