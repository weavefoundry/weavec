//===- Persistent.h - Shared, copy-on-write sorted maps ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §4.8: the object engine's states are copied along every CFG edge,
// so every map in a state is a `PMap`: a sorted vector shared by reference
// count and copied on the first write through a shared handle. A copy costs
// one reference; a write to an unshared map costs what a write to a sorted
// vector costs; a write to a shared one copies it once.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_PERSISTENT_H
#define WEAVEC_CORE_PERSISTENT_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace weavec::core {

/// A sorted map with value semantics whose copies share storage until one of
/// them is written. A large value (a symbol's or an object's description)
/// is held in its own shared box, so copying the map to write one entry
/// shares every other entry's value rather than copying it, and an insert
/// moves pointers rather than values.
template <typename Key, typename Value, typename Less = std::less<Key>>
class PMap {
  static constexpr bool Boxed = sizeof(Value) > 4 * sizeof(void *);
  using Slot = std::conditional_t<Boxed, std::shared_ptr<Value>, Value>;
  using Stored = std::pair<Key, Slot>;
  using Storage = std::vector<Stored>;

  static const Value &valueOf(const Slot &slot) {
    if constexpr (Boxed)
      return *slot;
    else
      // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter): a stored slot
      return slot;
  }
  static Slot slotOf(Value value) {
    if constexpr (Boxed)
      return std::make_shared<Value>(std::move(value));
    else
      return value;
  }

public:
  /// Whether a value stays where it is while other entries are written
  /// (a reference to it outlives inserts; not an erase of its own entry).
  static constexpr bool StableValues = Boxed;
  using Reference = std::pair<const Key &, const Value &>;

  /// Iterates the entries in key order as (key, value) reference pairs.
  class ConstIterator {
  public:
    // NOLINTBEGIN(readability-identifier-naming): the standard library's names
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<Key, Value>;
    using difference_type = std::ptrdiff_t;
    using reference = Reference;
    // NOLINTEND(readability-identifier-naming)
    struct Arrow {
      Reference entry;
      const Reference *operator->() const { return &entry; }
    };
    // NOLINTNEXTLINE(readability-identifier-naming): a standard library name
    using pointer = Arrow;

    ConstIterator() = default;
    explicit ConstIterator(Storage::const_iterator at) : at(at) {}
    Reference operator*() const { return {at->first, valueOf(at->second)}; }
    Arrow operator->() const { return Arrow{**this}; }
    ConstIterator &operator++() {
      ++at;
      return *this;
    }
    ConstIterator operator++(int) {
      ConstIterator old = *this;
      ++at;
      return old;
    }
    friend bool operator==(const ConstIterator &a, const ConstIterator &b) {
      return a.at == b.at;
    }

  private:
    Storage::const_iterator at{};
  };

  PMap() = default;

  /// The map of `entries`, which are sorted by key with no key twice.
  static PMap fromSorted(std::vector<std::pair<Key, Value>> entries) {
    PMap map;
    if (entries.empty())
      return map;
    map.data = std::make_shared<Storage>();
    map.data->reserve(entries.size());
    for (auto &[key, value] : entries)
      map.data->emplace_back(key, slotOf(std::move(value)));
    return map;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return data ? data->size() : 0;
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] ConstIterator begin() const {
    return ConstIterator(data ? data->cbegin() : emptyStorage().cbegin());
  }
  [[nodiscard]] ConstIterator end() const {
    return ConstIterator(data ? data->cend() : emptyStorage().cend());
  }

  /// The value at `key`, or null.
  [[nodiscard]] const Value *find(const Key &key) const {
    if (!data)
      return nullptr;
    auto it = lowerBound(*data, key);
    if (it == data->end() || Less{}(key, it->first))
      return nullptr;
    return &valueOf(it->second);
  }
  [[nodiscard]] bool contains(const Key &key) const {
    return find(key) != nullptr;
  }
  /// The value at `key`, or `fallback`.
  [[nodiscard]] Value get(const Key &key, Value fallback = Value{}) const {
    const Value *value = find(key);
    return value != nullptr ? *value : std::move(fallback);
  }

  /// Sets `key` to `value`, copying shared storage first.
  void set(const Key &key, Value value) {
    Storage &storage = writable();
    auto it = lowerBoundMutable(storage, key);
    if (it != storage.end() && !Less{}(key, it->first)) {
      if constexpr (Boxed) {
        if (it->second.use_count() == 1) {
          *it->second = std::move(value);
          return;
        }
      }
      it->second = slotOf(std::move(value));
    } else {
      storage.insert(it, Stored(key, slotOf(std::move(value))));
    }
  }
  /// A writable reference to the value at `key`, default-constructed when
  /// absent.
  Value &at(const Key &key) {
    Storage &storage = writable();
    auto it = lowerBoundMutable(storage, key);
    if (it == storage.end() || Less{}(key, it->first))
      it = storage.insert(it, Stored(key, slotOf(Value{})));
    if constexpr (Boxed) {
      if (it->second.use_count() > 1)
        it->second = std::make_shared<Value>(*it->second);
      return *it->second;
    } else {
      return it->second;
    }
  }
  /// Removes `key`; returns whether it was present.
  bool erase(const Key &key) {
    if (!find(key))
      return false;
    Storage &storage = writable();
    storage.erase(lowerBoundMutable(storage, key));
    return true;
  }
  /// Removes every entry for which `drop(key, value)` holds.
  template <typename Predicate>
  void eraseIf(Predicate drop) {
    if (!data)
      return;
    auto first =
        std::find_if(data->begin(), data->end(), [&](const Stored &entry) {
          return drop(entry.first, valueOf(entry.second));
        });
    if (first == data->end())
      return;
    auto from = static_cast<std::size_t>(first - data->begin());
    Storage &storage = writable();
    storage.erase(
        std::remove_if(storage.begin() + static_cast<std::ptrdiff_t>(from),
                       storage.end(),
                       [&](const Stored &entry) {
                         return drop(entry.first, valueOf(entry.second));
                       }),
        storage.end());
  }
  void clear() { data.reset(); }

  /// Whether the two maps share storage (a cheap equality test).
  [[nodiscard]] bool sharesWith(const PMap &other) const noexcept {
    return data == other.data;
  }

  friend bool operator==(const PMap &a, const PMap &b) {
    if (a.data == b.data)
      return true;
    if (a.size() != b.size())
      return false;
    if (a.empty())
      return true;
    return std::equal(a.data->begin(), a.data->end(), b.data->begin(),
                      [](const Stored &x, const Stored &y) {
                        if (Less{}(x.first, y.first) ||
                            Less{}(y.first, x.first))
                          return false;
                        if constexpr (Boxed)
                          if (x.second == y.second)
                            return true;
                        return valueOf(x.second) == valueOf(y.second);
                      });
  }

private:
  std::shared_ptr<Storage> data;

  static const Storage &emptyStorage() {
    static const Storage Empty;
    return Empty;
  }
  static Storage::const_iterator lowerBound(const Storage &storage,
                                            const Key &key) {
    return std::lower_bound(storage.begin(), storage.end(), key,
                            [](const Stored &entry, const Key &probe) {
                              return Less{}(entry.first, probe);
                            });
  }
  static Storage::iterator lowerBoundMutable(Storage &storage, const Key &key) {
    return std::lower_bound(storage.begin(), storage.end(), key,
                            [](const Stored &entry, const Key &probe) {
                              return Less{}(entry.first, probe);
                            });
  }
  Storage &writable() {
    if (!data)
      data = std::make_shared<Storage>();
    else if (data.use_count() > 1)
      data = std::make_shared<Storage>(*data);
    return *data;
  }
};

} // namespace weavec::core

#endif // WEAVEC_CORE_PERSISTENT_H
