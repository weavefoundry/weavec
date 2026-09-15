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
      payloads.size() > MaxContainerFields ||
      children.size() >= MaxContainerFields ||
      ownership.size() > MaxContainerFields ||
      headValues.size() > MaxContainerFields ||
      emptyPayloads.size() > payloads.size() ||
      !std::ranges::all_of(emptyPayloads,
                           [&](const auto &name) {
                             return std::ranges::any_of(
                                 payloads, [&](const auto &payload) {
                                   return payload.field.name == name;
                                 });
                           }) ||
      emptyLinks.size() > children.size() ||
      (terminal && !emptyLinks.empty()) ||
      !std::ranges::all_of(
          emptyLinks, [&](const auto &name) { return recursiveLink(name); }))
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
  if (!std::ranges::is_sorted(children))
    return false;
  names.clear();
  for (const auto &child : children)
    if (!fieldValid(child) || child.name == link.name ||
        !names.insert(child.name).second ||
        std::ranges::find(initialized, child) == initialized.end())
      return false;
  if (!std::ranges::is_sorted(payloads))
    return false;
  names.clear();
  for (const auto &payload : payloads)
    if (!fieldValid(payload.field) || recursiveLink(payload.field.name) ||
        !names.insert(payload.field.name).second ||
        !nameValid(payload.family) ||
        std::ranges::find(initialized, payload.field) == initialized.end())
      return false;
  for (const auto &[name, condition] : ownership) {
    if ((!recursiveLink(name) &&
         std::ranges::none_of(payloads,
                              [&](const auto &payload) {
                                return payload.field.name == name;
                              })) ||
        condition.field.bytes > 8 || condition.field.bytes == 0 ||
        condition.mask == 0 || (condition.value & ~condition.mask) != 0 ||
        (condition.field.bytes < 8 &&
         condition.mask >> (condition.field.bytes * 8) != 0) ||
        std::ranges::find(initialized, condition.field) == initialized.end())
      return false;
  }
  for (const auto &[name, value] : headValues) {
    const auto condition =
        std::ranges::find_if(ownership, [&](const auto &entry) {
          return entry.second.field.name == name;
        });
    if (condition == ownership.end() ||
        (condition->second.field.bytes < 8 &&
         value >> (condition->second.field.bytes * 8) != 0))
      return false;
  }
  return access == ContainerAccess::Release ? nameValid(family)
                                            : family.empty();
}

bool ContainerShape::recursiveLink(std::string_view name) const {
  return link.name == name ||
         std::ranges::any_of(
             children, [&](const auto &child) { return child.name == name; });
}

bool ContainerShape::entails(const ContainerShape &required) const {
  return valid() && required.valid() && object == required.object &&
         link == required.link && children == required.children &&
         ownership == required.ownership &&
         std::ranges::includes(headValues, required.headValues) &&
         std::ranges::includes(emptyPayloads, required.emptyPayloads) &&
         static_cast<unsigned>(access) >=
             static_cast<unsigned>(required.access) &&
         (!required.terminal || terminal) &&
         (terminal || std::ranges::includes(emptyLinks, required.emptyLinks)) &&
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
  if (!emptyPayloads.empty())
    text = "tree5:";
  else if (!headValues.empty())
    text = "tree4:";
  else if (!ownership.empty())
    text = "tree3:";
  else if (!emptyLinks.empty())
    text = "tree2:";
  else if (!children.empty())
    text = "tree1:";
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
  if (!children.empty() || !emptyLinks.empty() || !ownership.empty() ||
      !emptyPayloads.empty()) {
    appendNumber(text, children.size());
    for (const auto &child : children)
      appendField(text, child);
  }
  if (!emptyLinks.empty() || !ownership.empty() || !emptyPayloads.empty()) {
    appendNumber(text, emptyLinks.size());
    for (const auto &name : emptyLinks)
      appendString(text, name);
  }
  if (!ownership.empty() || !emptyPayloads.empty()) {
    appendNumber(text, ownership.size());
    for (const auto &[name, condition] : ownership) {
      appendString(text, name);
      appendField(text, condition.field);
      appendNumber(text, condition.mask);
      appendNumber(text, condition.value);
    }
  }
  if (!headValues.empty() || !emptyPayloads.empty()) {
    appendNumber(text, headValues.size());
    for (const auto &[name, value] : headValues) {
      appendString(text, name);
      appendNumber(text, value);
    }
  }
  if (!emptyPayloads.empty()) {
    appendNumber(text, emptyPayloads.size());
    for (const auto &name : emptyPayloads)
      appendString(text, name);
  }
  return text.size() <= MaxContainerDescriptorBytes ? text : std::string{};
}
std::optional<ContainerShape> ContainerShape::decode(std::string_view text) {
  const bool nullPayloads = text.starts_with("tree5:");
  const bool values = nullPayloads || text.starts_with("tree4:");
  const bool guarded = values || text.starts_with("tree3:");
  const bool head = guarded || text.starts_with("tree2:");
  const bool tree = head || text.starts_with("tree1:");
  if (text.size() > MaxContainerDescriptorBytes ||
      (!tree && !text.starts_with("chain1:")))
    return std::nullopt;
  const auto original = text;
  text.remove_prefix(tree ? 6 : 7);
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
  if (tree) {
    if (!number(count) || (!head && count == 0) || count >= MaxContainerFields)
      return std::nullopt;
    for (std::uint64_t i = 0; i < count; ++i) {
      ContainerField child;
      if (!field(child))
        return std::nullopt;
      result.children.push_back(std::move(child));
    }
  }
  if (head) {
    if (!number(count) || (!guarded && count == 0) ||
        count > result.children.size())
      return std::nullopt;
    for (std::uint64_t i = 0; i < count; ++i) {
      std::string name;
      if (!string(name) || !result.emptyLinks.insert(name).second)
        return std::nullopt;
    }
  }
  if (guarded) {
    if (!number(count) || (!nullPayloads && count == 0) ||
        count > MaxContainerFields)
      return std::nullopt;
    for (std::uint64_t i = 0; i < count; ++i) {
      std::string name;
      ContainerCondition condition;
      if (!string(name) || !field(condition.field) || !number(condition.mask) ||
          !number(condition.value) ||
          !result.ownership.emplace(name, std::move(condition)).second)
        return std::nullopt;
    }
  }
  if (values) {
    if (!number(count) || (!nullPayloads && count == 0) ||
        count > MaxContainerFields)
      return std::nullopt;
    for (std::uint64_t i = 0; i < count; ++i) {
      std::string name;
      std::uint64_t value = 0;
      if (!string(name) || !number(value) ||
          !result.headValues.emplace(name, value).second)
        return std::nullopt;
    }
  }
  if (nullPayloads) {
    if (!number(count) || count == 0 || count > result.payloads.size())
      return std::nullopt;
    for (std::uint64_t i = 0; i < count; ++i) {
      std::string name;
      if (!string(name) || !result.emptyPayloads.insert(name).second)
        return std::nullopt;
    }
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
         releasedChildren.size() <= shape.children.size() + 1 &&
         (!empty || members.empty());
}
bool ContainerFact::entails(const ContainerShape &required) const {
  return valid() && required.valid() &&
         (empty ||
          (releasedChildren.empty() && shape.entails(required) &&
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
    comparable.emptyLinks = it->second.shape.emptyLinks;
    comparable.headValues = it->second.shape.headValues;
    comparable.emptyPayloads = it->second.shape.emptyPayloads;
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
    if (left.empty && !right->empty) {
      left.shape = right->shape;
    } else if (!right->empty) {
      std::erase_if(left.shape.emptyPayloads, [&](const auto &name) {
        return !right->shape.emptyPayloads.contains(name);
      });
      std::erase_if(left.shape.headValues, [&](const auto &entry) {
        const auto found = right->shape.headValues.find(entry.first);
        return found == right->shape.headValues.end() ||
               found->second != entry.second;
      });
      if (left.shape.terminal)
        left.shape.emptyLinks = right->shape.emptyLinks;
      else if (!right->shape.terminal)
        std::erase_if(left.shape.emptyLinks, [&](const auto &name) {
          return !right->shape.emptyLinks.contains(name);
        });
      left.shape.terminal &= right->shape.terminal;
    }
    left.empty &= right->empty;
    left.allocationCompatible = compatible;
    left.localAllocation = allocated;
    left.suffix |= right->suffix;
    if (left.tailOf != right->tailOf || left.tailField != right->tailField) {
      left.tailOf.reset();
      left.tailField.clear();
    }
    left.releasedChildren.insert(right->releasedChildren.begin(),
                                 right->releasedChildren.end());
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
  std::vector<ContainerEdge> pending{first};
  while (!pending.empty()) {
    const auto cursor = pending.back();
    pending.pop_back();
    if (cursor == endpoint)
      continue;
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
    if (cursor == first &&
        !std::ranges::includes(node.scalars, shape.headValues))
      return proof;
    const auto active = [&](const std::string &name) -> std::optional<bool> {
      const auto condition = shape.ownership.find(name);
      if (condition == shape.ownership.end())
        return true;
      const auto value = node.scalars.find(condition->second.field.name);
      if (value == node.scalars.end())
        return std::nullopt;
      return (value->second & condition->second.mask) ==
             condition->second.value;
    };
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
    if (shape.access == ContainerAccess::Release &&
        (!node.allocationBase || node.family != shape.family)) {
      proof.failure = ContainerFailure::Release;
      return proof;
    }
    for (const auto &payload : shape.payloads) {
      if (cursor == first && shape.emptyPayloads.contains(payload.field.name)) {
        const auto edge = node.payloads.find(payload.field.name);
        if (edge == node.payloads.end() ||
            edge->second != ContainerEdge::null())
          return proof;
      }
      const auto owns = active(payload.field.name);
      if (!owns)
        return proof;
      if (!*owns)
        continue;
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
    if (cursor == first &&
        (shape.terminal || shape.emptyLinks.contains(shape.link.name)) &&
        node.next != ContainerEdge::null())
      return proof;
    const auto ownsNext = active(shape.link.name);
    if (!ownsNext)
      return proof;
    if (*ownsNext)
      pending.push_back(node.next);
    for (const auto &child : shape.children) {
      const auto found = node.children.find(child.name);
      if (cursor == first &&
          (shape.terminal || shape.emptyLinks.contains(child.name)) &&
          (found == node.children.end() ||
           found->second != ContainerEdge::null()))
        return proof;
      const auto owns = active(child.name);
      if (!owns)
        return proof;
      if (!*owns)
        continue;
      if (found == node.children.end())
        return proof;
      pending.push_back(found->second);
    }
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
