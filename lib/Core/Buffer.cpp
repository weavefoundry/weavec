//===- Buffer.cpp - Contiguous container evidence (RFC 0026) -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/Buffer.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace weavec::core {
bool BufferShape::valid() const {
  if (!object.valid() || elementBytes == 0 ||
      elementBytes > static_cast<std::uint64_t>(INT64_MAX) ||
      (terminated && (elementBytes != 1 || pointerElements)) ||
      (ownsElements && !pointerElements))
    return false;
  const auto validField = [&](const ContainerField &field) {
    return !field.name.empty() && field.name.size() <= 1024 &&
           std::ranges::all_of(
               field.name,
               [](unsigned char c) { return c >= 32 && c < 127; }) &&
           field.bytes && field.offset <= object.bytes &&
           field.bytes <= object.bytes - field.offset;
  };
  const auto separate = [](const ContainerField &a, const ContainerField &b) {
    return a.name != b.name &&
           (a.offset + a.bytes <= b.offset || b.offset + b.bytes <= a.offset);
  };
  return validField(data) && validField(length) && validField(capacity) &&
         length.bytes <= 8 && capacity.bytes <= 8 && separate(data, length) &&
         separate(data, capacity) && separate(length, capacity);
}
bool BufferShape::sameLayoutAs(const BufferShape &other) const {
  auto left = *this;
  auto right = other;
  left.terminated = right.terminated = false;
  left.ownsBacking = right.ownsBacking = false;
  left.ownsElements = right.ownsElements = false;
  return left == right;
}
bool BufferShape::entails(const BufferShape &other) const {
  return valid() && other.valid() && sameLayoutAs(other) &&
         (!other.terminated || terminated) &&
         (!other.ownsBacking || ownsBacking) &&
         (!other.ownsElements || ownsElements);
}
std::string BufferShape::encode() const {
  if (!valid())
    return {};
  // Reuse the bounded field/layout codec; this is a distinct predicate tag.
  ContainerShape carrier{.object = object,
                         .link = data,
                         .initialized = {data, length, capacity},
                         .payloads = {},
                         .family = {},
                         .access = ContainerAccess::Read};
  std::ranges::sort(carrier.initialized);
  const auto field = [](const std::string &name) {
    return std::to_string(name.size()) + ':' + name;
  };
  return "buffer1:" + std::to_string(elementBytes) + ':' +
         std::to_string(static_cast<unsigned>(pointerElements)) + ':' +
         std::to_string(static_cast<unsigned>(terminated)) + ':' +
         std::to_string(static_cast<unsigned>(ownsBacking)) + ':' +
         std::to_string(static_cast<unsigned>(ownsElements)) + ':' +
         field(length.name) + field(capacity.name) + carrier.encode();
}
std::optional<BufferShape> BufferShape::decode(std::string_view text) {
  if (!text.starts_with("buffer1:") || text.size() > 16384)
    return std::nullopt;
  const auto original = text;
  text.remove_prefix(8);
  const auto number = [&](std::uint64_t &value) {
    const auto end = text.find(':');
    if (end == std::string_view::npos || end == 0 || end > 20)
      return false;
    const auto part = text.substr(0, end);
    const auto parsed =
        std::from_chars(part.data(), part.data() + part.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
        part != std::to_string(value))
      return false;
    text.remove_prefix(end + 1);
    return true;
  };
  const auto name = [&](std::string &value) {
    std::uint64_t size = 0;
    if (!number(size) || size > text.size() || size > 1024)
      return false;
    value = text.substr(0, size);
    text.remove_prefix(size);
    return true;
  };
  BufferShape result;
  std::uint64_t pointers = 0;
  std::uint64_t terminated = 0;
  std::uint64_t backing = 0;
  std::uint64_t elements = 0;
  std::string length;
  std::string capacity;
  if (!number(result.elementBytes) || !number(pointers) || pointers > 1 ||
      !number(terminated) || terminated > 1 || !number(backing) ||
      backing > 1 || !number(elements) || elements > 1 || !name(length) ||
      !name(capacity))
    return std::nullopt;
  const auto carrier = ContainerShape::decode(text);
  if (!carrier || carrier->access != ContainerAccess::Read ||
      carrier->terminal || carrier->initialized.size() != 3)
    return std::nullopt;
  result.object = carrier->object;
  result.data = carrier->link;
  for (const auto &field : carrier->initialized) {
    if (field.name == length)
      result.length = field;
    if (field.name == capacity)
      result.capacity = field;
  }
  result.pointerElements = pointers != 0;
  result.terminated = terminated != 0;
  result.ownsBacking = backing != 0;
  result.ownsElements = elements != 0;
  return result.valid() && result.encode() == original ? std::optional(result)
                                                       : std::nullopt;
}
void BufferFacts::set(PlaceId data, BufferFact fact) {
  if (!fact.shape.valid() ||
      (!values.contains(data) && values.size() >= MaxBufferFacts) ||
      (!storage.contains(data) && storage.size() >= MaxBufferFacts)) {
    values.erase(data);
    storage.erase(data);
    pending.erase(data);
    bounds.erase(data);
    sequences.erase(data);
    pendingSequences.erase(data);
    limited = true;
    return;
  }
  storage[data] = fact;
  storage[data].initialized = false;
  storage[data].shape.ownsElements = false;
  storage[data].shape.terminated = false;
  values[data] = std::move(fact);
}
void BufferFacts::forget(PlaceId dependency) {
  for (auto &[data, posts] : pending)
    std::erase_if(posts, [&](const auto &post) {
      return data == dependency || post.fact.object == dependency ||
             post.fact.length == dependency ||
             post.fact.capacity == dependency ||
             post.when.dependsOn(dependency);
    });
  std::erase_if(pending,
                [](const auto &entry) { return entry.second.empty(); });
  for (const auto &[data, fact] : storage)
    if (data == dependency || fact.object == dependency ||
        fact.capacity == dependency || fact.length == dependency)
      pendingSequences.erase(data);
  for (auto &[data, posts] : pendingSequences) {
    (void)data;
    std::erase_if(posts, [dependency](const auto &post) {
      return post.value == dependency || post.borrowed == dependency ||
             post.index.place == dependency || post.when.dependsOn(dependency);
    });
  }
  std::erase_if(sequences, [dependency](const auto &entry) {
    return entry.first == dependency || entry.second.length == dependency ||
           entry.second.appended == dependency;
  });
  std::erase_if(storage, [dependency](const auto &entry) {
    return entry.first == dependency || entry.second.object == dependency ||
           entry.second.capacity == dependency;
  });
  bounds.erase(dependency);
  for (auto &[data, entries] : bounds) {
    (void)data;
    std::erase_if(entries, [dependency](const auto &bound) {
      return bound.capacity == dependency ||
             bound.minimum.place == dependency ||
             bound.when.dependsOn(dependency);
    });
  }
  std::erase_if(bounds, [](const auto &entry) { return entry.second.empty(); });
  std::erase_if(values, [&](const auto &entry) {
    return entry.first == dependency || entry.second.object == dependency ||
           entry.second.length == dependency ||
           entry.second.capacity == dependency;
  });
}
bool BufferFacts::join(const BufferFacts &other) {
  bool changed = !limited && other.limited;
  limited |= other.limited;
  for (auto it = pending.begin(); it != pending.end();) {
    const auto found = other.pending.find(it->first);
    const auto count = it->second.size();
    std::erase_if(it->second, [&](const auto &post) {
      return found == other.pending.end() ||
             std::ranges::find(found->second, post) == found->second.end();
    });
    changed |= count != it->second.size();
    if (it->second.empty())
      it = pending.erase(it);
    else
      ++it;
  }
  for (auto it = pendingSequences.begin(); it != pendingSequences.end();) {
    const auto found = other.pendingSequences.find(it->first);
    const auto count = it->second.size();
    std::erase_if(it->second, [&](const auto &post) {
      return found == other.pendingSequences.end() ||
             std::ranges::find(found->second, post) == found->second.end();
    });
    changed |= count != it->second.size();
    if (it->second.empty())
      it = pendingSequences.erase(it);
    else
      ++it;
  }
  const auto sequenceCount = sequences.size();
  std::erase_if(sequences, [&](const auto &entry) {
    const auto found = other.sequences.find(entry.first);
    return found == other.sequences.end() || found->second != entry.second;
  });
  changed |= sequenceCount != sequences.size();
  for (auto it = storage.begin(); it != storage.end();) {
    const auto found = other.storage.find(it->first);
    if (found == other.storage.end() ||
        !it->second.shape.sameLayoutAs(found->second.shape) ||
        it->second.object != found->second.object ||
        it->second.capacity != found->second.capacity) {
      it = storage.erase(it);
      changed = true;
      continue;
    }
    const auto old = it->second;
    it->second.shape.ownsBacking &= found->second.shape.ownsBacking;
    it->second.nonNull &= found->second.nonNull;
    if (it->second.entryBacking != found->second.entryBacking)
      it->second.entryBacking.reset();
    changed |= old != it->second;
    ++it;
  }
  for (auto it = bounds.begin(); it != bounds.end();) {
    const auto found = other.bounds.find(it->first);
    const auto size = it->second.size();
    std::erase_if(it->second, [&](const auto &bound) {
      return found == other.bounds.end() ||
             std::ranges::find(found->second, bound) == found->second.end();
    });
    changed |= size != it->second.size();
    if (it->second.empty())
      it = bounds.erase(it);
    else
      ++it;
  }
  for (auto it = values.begin(); it != values.end();) {
    const auto found = other.values.find(it->first);
    auto &left = it->second;
    if (found == other.values.end() ||
        !left.shape.sameLayoutAs(found->second.shape) ||
        left.object != found->second.object ||
        left.length != found->second.length ||
        left.capacity != found->second.capacity) {
      it = values.erase(it);
      changed = true;
      continue;
    }
    const auto &right = found->second;
    if (left.entryBacking != right.entryBacking) {
      changed |= left.entryBacking.has_value();
      left.entryBacking.reset();
    }
    const auto shape = left.shape;
    left.shape.ownsBacking &= right.shape.ownsBacking;
    left.shape.ownsElements &= right.shape.ownsElements;
    left.shape.terminated &= right.shape.terminated;
    changed |= shape != left.shape;
    const bool initialized = left.initialized && right.initialized;
    const bool nonNull = left.nonNull && right.nonNull;
    changed |= initialized != left.initialized || nonNull != left.nonNull;
    left.initialized = initialized;
    left.nonNull = nonNull;
    ++it;
  }
  return changed;
}
} // namespace weavec::core
