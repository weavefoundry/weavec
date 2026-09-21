//===- Moves.cpp - Move / deinitialization tracking -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Moves.h"

#include <algorithm>
#include <utility>

namespace weavec::core {

bool ElementWitness::matches(const ElementWitness &other) const noexcept {
  if (kind == Kind::Whole || other.kind == Kind::Whole)
    return true;
  if (kind != other.kind)
    return false;
  switch (kind) {
  case Kind::Constant:
    return constant == other.constant;
  case Kind::Variable:
    return variable == other.variable;
  case Kind::Whole:
  case Kind::Unknown:
    return false;
  }
  return false;
}

std::optional<MoveRecord>
MoveTracker::markMoved(PlaceId place, MoveReason reason,
                       SourceLocation location, std::optional<PlaceId> via,
                       ElementWitness element, std::string family,
                       bool ownValue, PlaceGuard guard, MoveOrigin origin) {
  MoveRecord record{.reason = reason,
                    .location = std::move(location),
                    .via = via,
                    .element = element,
                    .family = std::move(family),
                    .ownValue = ownValue,
                    .guard = std::move(guard),
                    // RFC 0030 §5.1: the unknown-callee default holds on
                    // some paths only.
                    .allPaths = !origin.unknownOrigin,
                    .conditional = origin.conditional || origin.lossy,
                    .lossy = origin.lossy,
                    .unknownOrigin = origin.unknownOrigin,
                    .callback = origin.callback};
  return copyRecord(place, std::move(record));
}

// MoveRecord's equality (Moves.h) is written out member by member. These
// bindings stop compiling when a member of MoveRecord or of its location is
// added or removed: update the equality with them.
[[maybe_unused]] static void equalityNamesEveryMember(const MoveRecord &r) {
  [[maybe_unused]] const auto &[reason, location, via, element, family,
                                ownValue, guard, allPaths, conditional, lossy,
                                released, local, unknownOrigin, callback,
                                origin] = r;
  [[maybe_unused]] const auto &[file, line, column, opaque] = r.location;
}

const MoveTracker::Store &MoveTracker::tallies() const {
  static const Store Empty;
  return records ? *records : Empty;
}

MoveTracker::Store &MoveTracker::edit() {
  if (!records)
    records = std::make_shared<Store>();
  else if (records.use_count() > 1)
    records = std::make_shared<Store>(*records);
  return *records;
}

/// The position of bucket `index` in `buckets`, or where it would go.
template <typename Buckets>
static auto bucketAt(Buckets &buckets, std::uint32_t index) {
  return std::lower_bound(
      buckets.begin(), buckets.end(), index,
      [](const auto &bucket, std::uint32_t key) { return bucket.first < key; });
}

/// The position of `place` in `entries`, or where it would go.
template <typename Entries>
static auto entryAt(Entries &entries, PlaceId place) {
  return std::lower_bound(
      entries.begin(), entries.end(), place,
      [](const auto &entry, PlaceId key) { return entry.first < key; });
}

const MoveRecord *MoveTracker::lookup(PlaceId place) const {
  if (!records)
    return nullptr;
  const auto &buckets = records->buckets;
  const auto bucket = bucketAt(buckets, bucketOf(place));
  if (bucket == buckets.end() || bucket->first != bucketOf(place))
    return nullptr;
  const auto &entries = bucket->second->entries;
  const auto entry = entryAt(entries, place);
  return entry == entries.end() || entry->first != place ? nullptr
                                                         : &entry->second;
}

MoveRecord *MoveTracker::writable(Store &store, PlaceId place) {
  const auto bucket = bucketAt(store.buckets, bucketOf(place));
  if (bucket == store.buckets.end() || bucket->first != bucketOf(place))
    return nullptr;
  if (const auto &entries = bucket->second->entries;
      entryAt(entries, place) == entries.end() ||
      entryAt(entries, place)->first != place)
    return nullptr;
  auto &entries = unshare(bucket->second).entries;
  return &entryAt(entries, place)->second;
}

std::pair<MoveRecord *, bool> MoveTracker::emplace(Store &store, PlaceId place,
                                                   const MoveRecord &record) {
  const std::uint32_t index = bucketOf(place);
  auto bucket = bucketAt(store.buckets, index);
  if (bucket == store.buckets.end() || bucket->first != index) {
    auto fresh = std::make_shared<Bucket>();
    fresh->entries.emplace_back(place, record);
    bucket = store.buckets.emplace(bucket, index, std::move(fresh));
    MoveRecord &inserted = bucket->second->entries.front().second;
    ++store.size;
    count(store, inserted);
    return {&inserted, true};
  }
  if (const auto &entries = bucket->second->entries;
      entryAt(entries, place) != entries.end() &&
      entryAt(entries, place)->first == place) {
    auto &mine = unshare(bucket->second).entries;
    return {&entryAt(mine, place)->second, false};
  }
  auto &entries = unshare(bucket->second).entries;
  const auto at = entries.emplace(entryAt(entries, place), place, record);
  ++store.size;
  count(store, at->second);
  return {&at->second, true};
}

void MoveTracker::erase(Store &store, PlaceId place) {
  const auto bucket = bucketAt(store.buckets, bucketOf(place));
  const auto &shared = bucket->second->entries;
  const auto at = entryAt(shared, place);
  uncount(store, at->second);
  --store.size;
  if (shared.size() == 1) {
    store.buckets.erase(bucket);
    return;
  }
  auto &entries = unshare(bucket->second).entries;
  entries.erase(entryAt(entries, place));
}

std::optional<MoveRecord> MoveTracker::copyRecord(PlaceId place,
                                                  MoveRecord record) {
  // Already moved with no guard to join: nothing changes, and the records
  // stay shared.
  if (const MoveRecord *found = lookup(place);
      found != nullptr && found->element.matches(record.element) &&
      found->guard.trivial())
    return *found;
  Store &store = edit();
  const auto [mine, inserted] = emplace(store, place, record);
  if (inserted)
    return std::nullopt;
  if (mine->element.matches(record.element)) {
    // Already moved: report the earlier move but keep the original record so
    // later diagnostics point at the first offending site. A second consume
    // under a guard the first did not have is still a second consume; the
    // place is now moved whenever either happened.
    if (!mine->guard.trivial()) {
      uncount(store, *mine);
      mine->guard.join(record.guard);
      count(store, *mine);
    }
    return *mine;
  }
  // Another element of the same summarised place: the most recent one is
  // what later accesses in the same iteration name (RFC 0006).
  uncount(store, *mine);
  *mine = std::move(record);
  count(store, *mine);
  return std::nullopt;
}

void MoveTracker::reinitialize(PlaceId place, ElementWitness element) {
  const MoveRecord *found = lookup(place);
  if (found == nullptr)
    return;
  if (element.isWhole() || found->element.matches(element))
    erase(edit(), place);
}

void MoveTracker::reinitializeAll(std::vector<PlaceId> places) {
  if (!records || records->size == 0 || places.empty())
    return;
  std::ranges::sort(places);
  const auto named = [&places](PlaceId place) {
    return std::ranges::binary_search(places, place);
  };
  // Read first: the records stay shared when none is named.
  if (!std::ranges::any_of(
          places, [this](PlaceId place) { return lookup(place) != nullptr; }))
    return;
  Store &store = edit();
  for (auto bucket = store.buckets.begin(); bucket != store.buckets.end();) {
    const auto &entries = bucket->second->entries;
    if (std::ranges::none_of(
            entries, [&](const Entry &entry) { return named(entry.first); })) {
      ++bucket;
      continue;
    }
    auto &mine = unshare(bucket->second).entries;
    std::erase_if(mine, [&](const Entry &entry) {
      if (!named(entry.first))
        return false;
      uncount(store, entry.second);
      --store.size;
      return true;
    });
    if (mine.empty())
      bucket = store.buckets.erase(bucket);
    else
      ++bucket;
  }
}

std::optional<MoveRecord> MoveTracker::movedAt(PlaceId place,
                                               ElementWitness element) const {
  const MoveRecord *found = lookup(place);
  if (found == nullptr || !found->element.matches(element))
    return std::nullopt;
  return *found;
}

std::optional<MoveRecord> MoveTracker::recordOf(PlaceId place) const {
  const MoveRecord *found = lookup(place);
  if (found == nullptr)
    return std::nullopt;
  return *found;
}

const MoveRecord *MoveTracker::find(PlaceId place) const {
  return lookup(place);
}

void MoveTracker::forgetWitness(PlaceId variable) {
  const auto names = [variable](const MoveRecord &record) {
    return record.element.kind == ElementWitness::Kind::Variable &&
           record.element.variable == variable;
  };
  if (tallies().variables == 0 || !anyRecord(names))
    return;
  Store &store = edit();
  for (auto &[index, bucket] : store.buckets) {
    if (!anyEntry(*bucket, names))
      continue;
    for (auto &[place, record] : unshare(bucket).entries) {
      if (names(record)) {
        uncount(store, record);
        record.element = ElementWitness::unknown();
        count(store, record);
      }
    }
  }
}

void MoveTracker::settleConditional(PlaceId place) {
  const MoveRecord *found = lookup(place);
  if (found != nullptr && !found->lossy && found->conditional)
    writable(edit(), place)->conditional = false;
}

void MoveTracker::setLocal(PlaceId place) {
  const MoveRecord *found = lookup(place);
  if (found != nullptr && !found->local)
    writable(edit(), place)->local = true;
}

void MoveTracker::setReleased(PlaceId place) {
  const MoveRecord *found = lookup(place);
  if (found != nullptr && found->reason == MoveReason::Moved &&
      !found->released)
    writable(edit(), place)->released = true;
}

bool MoveTracker::markUnknown(PlaceId place, const SourceLocation &location,
                              std::string_view origin, bool callback) {
  if (lookup(place) != nullptr)
    return false;
  const MoveRecord record{.reason = MoveReason::Freed,
                          .location = SourceLocation{.file = {},
                                                     .line = location.line,
                                                     .column = location.column,
                                                     .opaque = location.opaque},
                          .via = std::nullopt,
                          .element = ElementWitness::whole(),
                          .family = {},
                          .ownValue = false,
                          .guard = {},
                          .allPaths = false,
                          .conditional = false,
                          .lossy = false,
                          .unknownOrigin = true,
                          .callback = callback,
                          .origin = std::string(origin)};
  return emplace(edit(), place, record).second;
}

bool MoveTracker::eraseUnknown(PlaceId place) {
  const MoveRecord *found = lookup(place);
  if (found == nullptr || !found->unknownOrigin)
    return false;
  erase(edit(), place);
  return true;
}

void MoveTracker::reaffirm(PlaceId place, PlaceGuard guard) {
  const MoveRecord *found = lookup(place);
  if (found == nullptr || found->unknownOrigin)
    return;
  Store &store = edit();
  MoveRecord &record = *writable(store, place);
  uncount(store, record);
  record.guard = std::move(guard);
  record.allPaths = true;
  record.conditional = false;
  record.lossy = false;
  count(store, record);
}

/// The join of one place's two records (see `join`), into `mine`. Returns
/// whether `mine` changed.
static bool joinRecord(MoveRecord &mine, const MoveRecord &record) {
  // RFC 0030 §3.1 (amended in S3): a release of the function's own value
  // (RFC 0008, `ownValue`) and an unknown-origin record of the caller's
  // value describe different values. All the join knows of the caller's
  // value is that unknown code had it, so the unknown record stands, on
  // some paths.
  if (mine.unknownOrigin != record.unknownOrigin) {
    const MoveRecord &known = mine.unknownOrigin ? record : mine;
    const MoveRecord &unknown = mine.unknownOrigin ? mine : record;
    if (known.ownValue && !unknown.ownValue) {
      if (!mine.unknownOrigin) {
        mine = record;
        mine.allPaths = false;
        return true;
      }
      if (mine.allPaths) {
        mine.allPaths = false;
        return true;
      }
      return false;
    }
  }
  // A known record joined into an unknown one brings its own reason,
  // position and names (only a known record is ever diagnosed); the bits,
  // the guard and the element join as below.
  if (mine.unknownOrigin && !record.unknownOrigin) {
    MoveRecord unknown = std::move(mine);
    mine = record;
    mine.allPaths = false;
    mine.conditional = mine.conditional || unknown.conditional;
    mine.lossy = mine.lossy || unknown.lossy;
    mine.released = mine.released || unknown.released;
    // RFC 0030 §9.4: the known record's own locality stands. A record of
    // unknown origin is never exported as a release either (§5.1 turns it
    // into the `unknown` effect), so it cannot make a *local* known record
    // — one taken through an interior alias, which no summary may claim —
    // exportable. `mine` is the known record here, so its flag is kept.
    mine.callback = false;
    // RFC 0030 §9.1: the unknown side is never diagnosed and is exported as
    // the `unknown` effect, not as a release, so joining its guard into the
    // known one claims the release where the function does not perform it
    // (`if (json == value) json_decref(value);` on one path, an unknown
    // callee's default on another). The record stands here, so a use on
    // either path is still reported, but the consume is widened: no caller
    // may make a definite finding from it.
    if (mine.guard.join(unknown.guard))
      mine.lossy = mine.conditional = true;
    if (mine.ownValue && !unknown.ownValue)
      mine.ownValue = false;
    if (unknown.element.isWhole())
      mine.element = ElementWitness::whole();
    else if (!mine.element.isWhole() && mine.element != unknown.element)
      mine.element = ElementWitness::unknown();
    return true;
  }
  bool changed = false;
  // Read before the bits below replace them.
  const bool onlyOneUnknown = mine.unknownOrigin != record.unknownOrigin;
  const bool allPaths = mine.allPaths && record.allPaths &&
                        mine.unknownOrigin == record.unknownOrigin;
  const bool conditional = mine.conditional || record.conditional;
  const bool lossy = mine.lossy || record.lossy;
  const bool released = mine.released || record.released;
  // As above: a side of unknown origin is never exported as a release, so
  // it cannot lift the other side's `local` (RFC 0030 §9.4/§5.1). With both
  // sides known (or both unknown) the flag joins by conjunction: the record
  // may be exported as soon as one path reached it through an owning name.
  const auto joinLocal = [&] {
    if (mine.unknownOrigin == record.unknownOrigin)
      return mine.local && record.local;
    return mine.unknownOrigin ? record.local : mine.local;
  };
  const bool local = joinLocal();
  const bool unknownOrigin = mine.unknownOrigin && record.unknownOrigin;
  const bool callback = mine.callback && record.callback;
  if (mine.allPaths != allPaths || mine.conditional != conditional ||
      mine.lossy != lossy || mine.released != released || mine.local != local ||
      mine.unknownOrigin != unknownOrigin || mine.callback != callback) {
    mine.allPaths = allPaths;
    mine.conditional = conditional;
    mine.lossy = lossy;
    mine.released = released;
    mine.local = local;
    mine.unknownOrigin = unknownOrigin;
    mine.callback = callback;
    changed = true;
  }
  // Both sides moved the place: it is moved when either guard holds. A side
  // of unknown origin claims no release of its own (§5.1), so letting its
  // guard widen the known one's would claim this function's release where
  // it does not happen: the record stands, widened (§9.1, as above).
  const bool widens = mine.guard.join(record.guard);
  changed |= widens;
  if (widens && onlyOneUnknown && (!mine.lossy || !mine.conditional)) {
    mine.lossy = true;
    mine.conditional = true;
    changed = true;
  }
  // A record that may be the caller's value on either path is the
  // caller's after the join (RFC 0008, *Replaced values*).
  if (mine.ownValue && !record.ownValue) {
    mine.ownValue = false;
    changed = true;
  }
  if (mine.element == record.element)
    return changed;
  // Both paths moved the place but not the same element. A whole-place
  // move on either side covers every element; otherwise the element is
  // unknown.
  ElementWitness element = mine.element;
  if (record.element.isWhole())
    element = ElementWitness::whole();
  else if (!element.isWhole())
    element = ElementWitness::unknown();
  if (element != mine.element) {
    mine.element = element;
    changed = true;
  }
  return changed;
}

namespace {
/// What joining one side's records into the other's would do: whether the
/// two are equal, whether the join changes the joined-into side, and
/// whether it would change that side were they not equal (equal records
/// with guards are joined too, then, and only records with trivial guards
/// provably join to themselves).
struct JoinProbe {
  bool equal = true;
  bool changes = false;
  bool changesUnlessEqual = false;
};
} // namespace

using MoveEntries = std::vector<std::pair<PlaceId, MoveRecord>>;

/// Probes the join of two buckets' records; stops at the first change.
static void probeJoin(const MoveEntries &mine, const MoveEntries &theirs,
                      JoinProbe &probe) {
  auto m = mine.begin();
  auto t = theirs.begin();
  while (!probe.changes && (m != mine.end() || t != theirs.end())) {
    if (t == theirs.end() || (m != mine.end() && m->first < t->first)) {
      probe.equal = false;
      probe.changes = m->second.allPaths;
      ++m;
    } else if (m == mine.end() || t->first < m->first) {
      probe.equal = false;
      probe.changes = true;
    } else {
      if (m->second == t->second) {
        if (!m->second.guard.trivial()) {
          MoveRecord joined = m->second;
          probe.changesUnlessEqual |= joinRecord(joined, t->second);
        }
      } else {
        probe.equal = false;
        MoveRecord joined = m->second;
        probe.changes = joinRecord(joined, t->second);
      }
      ++m;
      ++t;
    }
  }
}

/// The guarded records of a bucket both sides share, joined with
/// themselves (see `JoinProbe`).
static bool sharedGuardsChange(const MoveEntries &entries) {
  return std::ranges::any_of(entries, [](const auto &entry) {
    if (entry.second.guard.trivial())
      return false;
    MoveRecord joined = entry.second;
    return joinRecord(joined, entry.second);
  });
}

bool MoveTracker::join(const MoveTracker &other) {
  // The same records on both sides (copies of one state): nothing changes,
  // and equal records are shared from now on.
  if (records == other.records)
    return false;
  const Store &theirs = other.tallies();
  // One read-only pass in place order finds whether the sides are equal
  // (then they are shared and nothing changes) and whether the join changes
  // this side at all; only then is anything unshared. The buckets both
  // sides share hold equal records.
  {
    const Store &mine = tallies();
    JoinProbe probe;
    probe.equal = mine.size == theirs.size;
    auto m = mine.buckets.begin();
    auto t = theirs.buckets.begin();
    while (!probe.changes &&
           (m != mine.buckets.end() || t != theirs.buckets.end())) {
      if (t == theirs.buckets.end() ||
          (m != mine.buckets.end() && m->first < t->first)) {
        probe.equal = false;
        probe.changes = anyEntry(*m->second, [](const MoveRecord &record) {
          return record.allPaths;
        });
        ++m;
      } else if (m == mine.buckets.end() || t->first < m->first) {
        probe.equal = false;
        probe.changes = true;
      } else {
        if (m->second != t->second)
          probeJoin(m->second->entries, t->second->entries, probe);
        else if (mine.guarded != 0 && !probe.changesUnlessEqual)
          probe.changesUnlessEqual = sharedGuardsChange(m->second->entries);
        ++m;
        ++t;
      }
    }
    if (probe.equal) {
      records = other.records;
      return false;
    }
    if (!probe.changes && !probe.changesUnlessEqual)
      return false;
  }
  // One merge of the two sides in place order, unsharing only the buckets
  // that change.
  bool changed = false;
  Store &store = edit();
  // RFC 0030 §3.1: a record this side has and the other lacks reached here
  // on some paths only.
  const auto somePaths = [&changed](Bucket &bucket) {
    for (auto &[place, record] : bucket.entries)
      if (record.allPaths) {
        record.allPaths = false;
        changed = true;
      }
  };
  const auto onAllPaths = [](const MoveRecord &record) {
    return record.allPaths;
  };
  std::vector<std::pair<std::uint32_t, BucketRef>> merged;
  merged.reserve(store.buckets.size() + theirs.buckets.size());
  auto m = store.buckets.begin();
  auto t = theirs.buckets.begin();
  while (m != store.buckets.end() || t != theirs.buckets.end()) {
    if (t == theirs.buckets.end() ||
        (m != store.buckets.end() && m->first < t->first)) {
      if (anyEntry(*m->second, onAllPaths))
        somePaths(unshare(m->second));
      merged.push_back(std::move(*m));
      ++m;
      continue;
    }
    if (m == store.buckets.end() || t->first < m->first) {
      // Records the other side has alone arrive on some paths only.
      BucketRef bucket = t->second;
      if (anyEntry(*bucket, onAllPaths)) {
        bucket = std::make_shared<Bucket>(*bucket);
        for (auto &[place, record] : bucket->entries)
          record.allPaths = false;
      }
      for (const auto &[place, record] : bucket->entries) {
        ++store.size;
        count(store, record);
      }
      changed = true;
      merged.emplace_back(t->first, std::move(bucket));
      ++t;
      continue;
    }
    if (m->second == t->second) {
      if (store.guarded != 0 && sharedGuardsChange(m->second->entries)) {
        for (auto &[place, record] : unshare(m->second).entries) {
          if (record.guard.trivial())
            continue;
          const MoveRecord same = record;
          uncount(store, record);
          changed |= joinRecord(record, same);
          count(store, record);
        }
      }
    } else {
      JoinProbe probe;
      probeJoin(m->second->entries, t->second->entries, probe);
      if (probe.changes || probe.changesUnlessEqual) {
        auto &entries = unshare(m->second).entries;
        const MoveEntries &incoming = t->second->entries;
        MoveEntries out;
        out.reserve(entries.size() + incoming.size());
        auto a = entries.begin();
        auto b = incoming.begin();
        while (a != entries.end() || b != incoming.end()) {
          if (b == incoming.end() ||
              (a != entries.end() && a->first < b->first)) {
            if (a->second.allPaths) {
              a->second.allPaths = false;
              changed = true;
            }
            out.push_back(std::move(*a));
            ++a;
          } else if (a == entries.end() || b->first < a->first) {
            out.push_back(*b);
            out.back().second.allPaths = false;
            ++store.size;
            count(store, out.back().second);
            changed = true;
            ++b;
          } else {
            uncount(store, a->second);
            changed |= joinRecord(a->second, b->second);
            count(store, a->second);
            out.push_back(std::move(*a));
            ++a;
            ++b;
          }
        }
        entries = std::move(out);
      }
    }
    merged.push_back(std::move(*m));
    ++m;
    ++t;
  }
  store.buckets = std::move(merged);
  return changed;
}

std::vector<PlaceId> MoveTracker::learn(PlaceId place, const ValueFact &fact) {
  std::vector<PlaceId> refuted;
  // Every guard learns the fact, a trivial one included, except a record of
  // unknown origin's (RFC 0030 §5.1): it is never diagnosed, so a guard could
  // only let a later test drop it, and keeping it is sound. Its guard stays
  // trivial, which keeps the guard scans short.
  if (tallies().known == 0)
    return refuted;
  const auto known = [](const MoveRecord &record) {
    return !record.unknownOrigin;
  };
  Store &store = edit();
  for (auto bucket = store.buckets.begin(); bucket != store.buckets.end();) {
    if (!anyEntry(*bucket->second, known)) {
      ++bucket;
      continue;
    }
    auto &entries = unshare(bucket->second).entries;
    for (auto it = entries.begin(); it != entries.end();) {
      if (it->second.unknownOrigin) {
        ++it;
        continue;
      }
      uncount(store, it->second);
      if (it->second.guard.learn(place, fact) == GuardRefinement::Refuted) {
        refuted.push_back(it->first);
        --store.size;
        it = entries.erase(it);
        continue;
      }
      count(store, it->second);
      ++it;
    }
    if (entries.empty())
      bucket = store.buckets.erase(bucket);
    else
      ++bucket;
  }
  return refuted;
}

void MoveTracker::dropGuardsOn(PlaceId place) {
  // Read first: the records stay shared when no guard names `place`.
  const auto depends = [place](const MoveRecord &record) {
    return record.guard.dependsOn(place);
  };
  if (tallies().guarded == 0 || !anyRecord(depends))
    return;
  Store &store = edit();
  for (auto &[index, bucket] : store.buckets) {
    if (!anyEntry(*bucket, depends))
      continue;
    for (auto &[movedPlace, record] : unshare(bucket).entries) {
      // Dropping conjuncts can only make a guard trivial.
      if (record.guard.trivial())
        continue;
      record.guard.drop(place);
      if (record.guard.trivial())
        --store.guarded;
    }
  }
}

void MoveTracker::setGuard(PlaceId place, PlaceGuard guard) {
  if (lookup(place) == nullptr)
    return;
  Store &store = edit();
  MoveRecord &record = *writable(store, place);
  uncount(store, record);
  record.guard = std::move(guard);
  count(store, record);
}

std::vector<PlaceId> MoveTracker::movedPlaces() const {
  std::vector<PlaceId> result;
  result.reserve(tallies().size);
  for (const auto &[index, bucket] : tallies().buckets)
    for (const auto &[place, record] : bucket->entries)
      result.push_back(place);
  return result;
}

bool operator==(const MoveTracker &a, const MoveTracker &b) {
  if (a.records == b.records)
    return true;
  const auto &left = a.tallies().buckets;
  const auto &right = b.tallies().buckets;
  if (a.tallies().size != b.tallies().size || left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].first != right[i].first)
      return false;
    if (left[i].second != right[i].second &&
        left[i].second->entries != right[i].second->entries)
      return false;
  }
  return true;
}

std::string_view toString(MoveReason reason) noexcept {
  switch (reason) {
  case MoveReason::Moved:
    return "moved";
  case MoveReason::Freed:
    return "freed";
  case MoveReason::Uninitialized:
    return "uninitialized";
  case MoveReason::Released:
    return "released";
  }
  return "<invalid>";
}

} // namespace weavec::core
