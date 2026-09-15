//===- SummarySteps.cpp - Shared interface paths (RFC 0028) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/Summary.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace weavec::core {

// One allocation owns this header followed by a correctly aligned element
// array. The placement array construction establishes every element lifetime;
// hidden suffix elements remain constructed until reuse or final destruction.
struct alignas(PathElem) SummarySteps::Storage {
  std::atomic<std::size_t> references{1};
  std::size_t capacity;

  PathElem *elements = nullptr;

  static constexpr std::size_t maximumCapacity() {
    return (std::numeric_limits<std::size_t>::max() - sizeof(Storage)) /
           sizeof(PathElem);
  }

  PathElem *data() const { return elements; }

  static Storage *create(std::size_t capacity) {
    static_assert(alignof(Storage) <= alignof(std::max_align_t));
    if (capacity > maximumCapacity())
      throw std::length_error("summary path allocation overflow");
    auto *memory =
        ::operator new(sizeof(Storage) + (capacity * sizeof(PathElem)));
    auto *result = ::new (memory) Storage{.capacity = capacity};
    try {
      auto *arrayMemory = static_cast<std::byte *>(memory) + sizeof(Storage);
      // Non-allocating placement array establishes lifetimes in the checked
      // trailing allocation.
      result->elements =
          ::new (static_cast<void *>(arrayMemory)) PathElem[capacity];
    } catch (...) {
      result->~Storage();
      ::operator delete(memory);
      throw;
    }
    return result;
  }

  void retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }
  void release() noexcept {
    if (references.fetch_sub(1, std::memory_order_acq_rel) != 1)
      return;
    std::destroy_n(data(), capacity);
    this->~Storage();
    ::operator delete(this);
  }
};

SummarySteps::SummarySteps(const SummarySteps &other) noexcept
    : storage(other.storage), count(other.count) {
  if (storage)
    storage->retain();
}

SummarySteps &SummarySteps::operator=(const SummarySteps &other) noexcept {
  if (this == &other)
    return *this;
  if (other.storage)
    other.storage->retain();
  if (storage)
    storage->release();
  storage = other.storage;
  count = other.count;
  return *this;
}

SummarySteps::SummarySteps(SummarySteps &&other) noexcept
    : storage(std::exchange(other.storage, nullptr)),
      count(std::exchange(other.count, 0)) {}

SummarySteps &SummarySteps::operator=(SummarySteps &&other) noexcept {
  if (this != &other) {
    if (storage)
      storage->release();
    storage = std::exchange(other.storage, nullptr);
    count = std::exchange(other.count, 0);
  }
  return *this;
}

SummarySteps::~SummarySteps() {
  if (storage)
    storage->release();
}

SummarySteps::SummarySteps(std::initializer_list<PathElem> elements) {
  SummarySteps result;
  for (const auto &element : elements)
    result.pushBack(element);
  *this = std::move(result);
}

std::span<const PathElem> SummarySteps::entries() const {
  return storage ? std::span<const PathElem>(storage->data(), count)
                 : std::span<const PathElem>{};
}

void SummarySteps::makeWritable(std::size_t minimumCapacity) {
  // Acquire the other handles' releases before editing bytes they last read.
  const bool unique = storage != nullptr &&
                      storage->references.load(std::memory_order_acquire) == 1;
  if (unique && minimumCapacity <= storage->capacity) {
    for (std::size_t i = count; i < storage->capacity; ++i)
      storage->data()[i] = {};
    return;
  }
  auto capacity = minimumCapacity;
  if (unique && storage->capacity <= Storage::maximumCapacity() / 2)
    capacity = std::max(capacity, storage->capacity * 2);
  SummarySteps next;
  next.storage = Storage::create(capacity);
  next.count = count;
  if (unique)
    std::ranges::move(std::span<PathElem>(storage->data(), count),
                      next.storage->data());
  else
    std::ranges::copy(entries(), next.storage->data());
  *this = std::move(next);
}

void SummarySteps::pushBack(PathElem element) {
  if (empty() && element.step == PathStep::Deref && element.field.empty()) {
    static const SummarySteps Dereference = [] {
      SummarySteps value;
      value.storage = Storage::create(1);
      value.storage->data()[0].step = PathStep::Deref;
      value.count = 1;
      return value;
    }();
    *this = Dereference;
    return;
  }
  if (count == std::numeric_limits<std::size_t>::max())
    throw std::length_error("summary path allocation overflow");
  makeWritable(count + 1);
  storage->data()[count++] = std::move(element);
}

void SummarySteps::pushFront(PathElem element) {
  if (empty()) {
    pushBack(std::move(element));
    return;
  }
  if (count == std::numeric_limits<std::size_t>::max())
    throw std::length_error("summary path allocation overflow");
  makeWritable(count + 1);
  std::ranges::move_backward(std::span<PathElem>(storage->data(), count),
                             storage->data() + count + 1);
  storage->data()[0] = std::move(element);
  ++count;
}

void SummarySteps::truncate(std::size_t size) {
  assert(size <= count && "cannot grow a prefix by truncating");
  count = size;
  if (empty() && storage) {
    storage->release();
    storage = nullptr;
  }
}

void SummarySteps::popBack() {
  assert(!empty() && "cannot remove a step from an empty path");
  truncate(count - 1);
}

void SummarySteps::append(const SummarySteps &other, std::size_t first) {
  assert(first <= other.size() && "unknown path suffix");
  if (first == other.size())
    return;
  // Self-append and shared prefixes need an independent owner while edits
  // may replace this same backing.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const auto owner = other;
  const auto source = owner.entries().subspan(first);
  if (source.size() > std::numeric_limits<std::size_t>::max() - count)
    throw std::length_error("summary path allocation overflow");
  makeWritable(count + source.size());
  std::ranges::copy(source, storage->data() + count);
  count += source.size();
}

bool operator==(const SummarySteps &left, const SummarySteps &right) {
  return left.count == right.count &&
         (left.storage == right.storage || std::ranges::equal(left, right));
}

std::strong_ordering operator<=>(const SummarySteps &left,
                                 const SummarySteps &right) {
  if (left.storage == right.storage)
    return left.count <=> right.count;
  return std::lexicographical_compare_three_way(left.begin(), left.end(),
                                                right.begin(), right.end());
}

} // namespace weavec::core
