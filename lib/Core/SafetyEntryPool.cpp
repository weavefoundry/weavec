//===- SafetyEntryPool.cpp - Shared explanation entries (RFC 0020) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/SafetyEntryPool.h"

#include "weavec/Core/AnalysisStats.h"

#include <unordered_map>

namespace weavec::core {

struct SafetyEntryPool::Storage {
  struct CallHash {
    std::size_t operator()(const CallKey &key) const {
      return std::hash<const SafetyPropagation *>{}(key.source) ^
             (std::hash<std::string>{}(key.callee) << 1U) ^
             (std::hash<std::string>{}(key.location.file) << 3U) ^
             (std::hash<std::string>{}(key.caller) << 4U) ^
             (static_cast<std::size_t>(key.location.line) << 5U) ^
             (static_cast<std::size_t>(key.location.column) << 6U) ^
             (static_cast<std::size_t>(key.trusted) << 2U) ^
             static_cast<std::size_t>(key.unsafe);
    }
  };
  struct CallPreparation {
    std::weak_ptr<const SafetyPropagation> source;
    std::shared_ptr<const PreparedSafetyOrigins> prepared;
    std::size_t bytes;
  };
  struct Snapshot {
    std::weak_ptr<SafetyLedger::Storage> owner;
    // Dereference only while owner.lock() keeps the containing Storage alive.
    const SafetyEntries *entries;
  };
  std::unordered_map<std::size_t,
                     std::vector<std::weak_ptr<const SafetyEntries::Row>>>
      rows;
  std::unordered_map<std::size_t, std::vector<Snapshot>> snapshots;
  std::unordered_map<CallKey, CallPreparation, CallHash> calls;
  std::uint64_t *recordedHits = nullptr;
  std::uint64_t *recordedMisses = nullptr;
  std::uint64_t *recordedResets = nullptr;
  std::uint64_t *recordedSnapshotHits = nullptr;
  std::uint64_t *recordedSnapshotMisses = nullptr;
  std::uint64_t *recordedSnapshotResets = nullptr;
  std::uint64_t *recordedCallHits = nullptr;
  std::uint64_t *recordedCallMisses = nullptr;
  std::uint64_t *recordedCallResets = nullptr;
  std::uint64_t *recordedCallRejections = nullptr;
  std::size_t capacity = 0;
  std::size_t snapshotCapacity = 0;
  std::size_t entries = 0;
  std::size_t snapshotEntries = 0;
  std::size_t callCapacity = 0;
  std::size_t callBytesLimit = 0;
  std::size_t callBytesUsed = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t resets = 0;
  std::uint64_t snapshotHits = 0;
  std::uint64_t snapshotMisses = 0;
  std::uint64_t snapshotResets = 0;
  std::uint64_t callHits = 0;
  std::uint64_t callMisses = 0;
  std::uint64_t callResets = 0;
  std::uint64_t callRejections = 0;
};

thread_local SafetyEntryPool::Storage *SafetyEntryPool::active = nullptr;

static std::size_t combineHash(std::size_t seed, std::size_t value) {
  return seed ^ (value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
}

static std::size_t entryHash(std::string_view key,
                             const SafetyObligation &entry) {
  const auto hashString = std::hash<std::string_view>{};
  auto hash = combineHash(hashString(key), hashString(entry.reason));
  hash = combineHash(hash, static_cast<std::size_t>(entry.outcome));
  // Vary the bucket with the route without hashing long file names again.
  // Full equality below still distinguishes routes in different files.
  for (const auto &call : entry.calls) {
    hash = combineHash(hash, call.line);
    hash = combineHash(hash, call.column);
  }
  return hash;
}

SafetyEntryPool::SafetyEntryPool(AnalysisStats *stats, std::size_t capacity,
                                 std::size_t snapshotCapacity,
                                 std::size_t callCapacity,
                                 std::size_t callBytes) {
  if (active)
    return;
  owned = std::make_unique<Storage>();
  if (stats) {
    owned->recordedHits = &stats->counters["explanation_entry_hits"];
    owned->recordedMisses = &stats->counters["explanation_entry_misses"];
    owned->recordedResets = &stats->counters["explanation_pool_resets"];
    owned->recordedSnapshotHits = &stats->counters["explanation_snapshot_hits"];
    owned->recordedSnapshotMisses =
        &stats->counters["explanation_snapshot_misses"];
    owned->recordedSnapshotResets =
        &stats->counters["explanation_snapshot_resets"];
    owned->recordedCallHits = &stats->counters["explanation_call_hits"];
    owned->recordedCallMisses = &stats->counters["explanation_call_misses"];
    owned->recordedCallResets = &stats->counters["explanation_call_resets"];
    owned->recordedCallRejections =
        &stats->counters["explanation_call_rejections"];
  }
  owned->capacity = capacity;
  owned->snapshotCapacity = snapshotCapacity;
  owned->callCapacity = callCapacity;
  owned->callBytesLimit = callBytes;
  active = owned.get();
}

SafetyEntryPool::~SafetyEntryPool() {
  if (!owned)
    return;
  if (owned->recordedHits) {
    // Aggregate locally: per-entry statistics must not add another map lookup
    // and string allocation to the hot path.
    *owned->recordedHits += owned->hits;
    *owned->recordedMisses += owned->misses;
    *owned->recordedResets += owned->resets;
    *owned->recordedSnapshotHits += owned->snapshotHits;
    *owned->recordedSnapshotMisses += owned->snapshotMisses;
    *owned->recordedSnapshotResets += owned->snapshotResets;
    *owned->recordedCallHits += owned->callHits;
    *owned->recordedCallMisses += owned->callMisses;
    *owned->recordedCallResets += owned->callResets;
    *owned->recordedCallRejections += owned->callRejections;
  }
  active = nullptr;
}

bool SafetyEntryPool::cachesCalls() {
  return active != nullptr && active->callCapacity != 0 &&
         active->callBytesLimit != 0;
}

std::shared_ptr<const PreparedSafetyOrigins> SafetyEntryPool::findCalls(
    const CallKey &key,
    const std::shared_ptr<const SafetyPropagation> &source) {
  auto &pool = *active;
  if (const auto found = pool.calls.find(key); found != pool.calls.end()) {
    if (found->second.source.lock() == source) {
      ++pool.callHits;
      return found->second.prepared;
    }
    // Raw addresses are only indexes. An expired projection can never
    // validate another projection subsequently allocated at that address.
    pool.callBytesUsed -= found->second.bytes;
    pool.calls.erase(found);
  }
  ++pool.callMisses;
  return {};
}

void SafetyEntryPool::saveCalls(
    CallKey key, const std::shared_ptr<const SafetyPropagation> &source,
    std::shared_ptr<const PreparedSafetyOrigins> prepared) {
  auto &pool = *active;
  // Include the control block, hash node and bucket storage conservatively
  // as well as the owned vectors, strings and key. Shared paths are not owned
  // by these records; each application reads the retained source projection.
  const auto bytes = prepared->bytes + sizeof(CallKey) +
                     sizeof(Storage::CallPreparation) + key.callee.capacity() +
                     key.location.file.capacity() + key.caller.capacity() + 128;
  if (bytes > pool.callBytesLimit) {
    ++pool.callRejections;
    return;
  }
  if (const auto previous = pool.calls.find(key);
      previous != pool.calls.end()) {
    pool.callBytesUsed -= previous->second.bytes;
    pool.calls.erase(previous);
  }
  if (pool.calls.size() == pool.callCapacity ||
      bytes > pool.callBytesLimit - pool.callBytesUsed) {
    pool.calls.clear();
    pool.callBytesUsed = 0;
    ++pool.callResets;
  }
  pool.calls.emplace(std::move(key),
                     Storage::CallPreparation{.source = source,
                                              .prepared = std::move(prepared),
                                              .bytes = bytes});
  pool.callBytesUsed += bytes;
}

std::shared_ptr<const SafetyEntries::Row>
SafetyEntryPool::intern(std::string key, SafetyObligation obligation) {
  if (!active || !active->capacity) {
    if (active)
      ++active->misses;
    return std::make_shared<const SafetyEntries::Row>(std::move(key),
                                                      std::move(obligation));
  }
  auto &pool = *active;
  const auto hash = entryHash(key, obligation);
  auto &bucket = pool.rows[hash];
  for (auto it = bucket.begin(); it != bucket.end();) {
    if (auto row = it->lock()) {
      if (row->first == key && row->second == obligation) {
        ++pool.hits;
        return row;
      }
      ++it;
    } else {
      it = bucket.erase(it);
      --pool.entries;
    }
  }
  if (pool.entries == pool.capacity) {
    // This releases weak references only. Every live ledger remains an owner
    // of all its rows, including origins no longer indexed by this pool.
    pool.rows.clear();
    pool.entries = 0;
    ++pool.resets;
  }
  auto row = std::make_shared<const SafetyEntries::Row>(std::move(key),
                                                        std::move(obligation));
  // Look up again: clearing a full index invalidated the earlier bucket.
  pool.rows[hash].push_back(row);
  ++pool.entries;
  ++pool.misses;
  return row;
}

std::shared_ptr<SafetyLedger::Storage>
SafetyEntryPool::internSnapshot(std::shared_ptr<SafetyLedger::Storage> snapshot,
                                const SafetyEntries &entries) {
  if (!active || !active->snapshotCapacity)
    return snapshot;
  auto &pool = *active;
  std::size_t hash = entries.size();
  for (const auto &[key, entry] : entries)
    hash = combineHash(hash, entryHash(key, entry));
  auto &bucket = pool.snapshots[hash];
  for (auto it = bucket.begin(); it != bucket.end();) {
    if (auto owner = it->owner.lock()) {
      if (entries == *it->entries) {
        ++pool.snapshotHits;
        return owner;
      }
      ++it;
    } else {
      it = bucket.erase(it);
      --pool.snapshotEntries;
    }
  }
  if (pool.snapshotEntries == pool.snapshotCapacity) {
    pool.snapshots.clear();
    pool.snapshotEntries = 0;
    ++pool.snapshotResets;
  }
  pool.snapshots[hash].push_back({.owner = snapshot, .entries = &entries});
  ++pool.snapshotEntries;
  ++pool.snapshotMisses;
  return snapshot;
}

} // namespace weavec::core
