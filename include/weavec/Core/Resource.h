//===- Resource.h - Owned resource tracking --------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `ResourceTracker` records which places hold an owned resource *this
// function is responsible for* (RFC 0007): the result of an allocating call
// or the value of a `WEAVEC_OWNED` parameter, until it is released, moved,
// returned, stored where the caller can see it, or handed to code the
// checker cannot follow ("escaped"). A holder that dies with a record, no
// move record and no other live alias has leaked its resource.
//
// The record also carries the *release family* (`free`, `fclose`, ...) so a
// release by the wrong family can be reported, and the tracker keeps a set
// of places known to hold null so a declared-owned field that was nulled is
// not reported when its container is freed.
//
// This is not ownership *kind*: `kinds` says a place is `Owned` (RFC 0001);
// the record says which resource it holds and whether it is still on this
// function's books.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_RESOURCE_H
#define WEAVEC_CORE_RESOURCE_H

#include "weavec/Core/Place.h"
#include "weavec/Core/Scalar.h"
#include "weavec/Core/SourceLocation.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

/// How a place came to hold a resource.
enum class ResourceOrigin : std::uint8_t {
  /// An allocating call (`malloc`, `fopen`, a callee returning `fresh`).
  Allocated,
  /// A parameter or field declared `WEAVEC_OWNED`.
  Declared,
  /// RFC 0010: a share taken by a reference-count increment on a value this
  /// function does not otherwise own (a parameter, a global, a load through
  /// one). Releasing the last owned share leaves the holder valid: the
  /// caller's share underlies it.
  Retained,
};

struct ResourceRecord {
  ResourceOrigin origin = ResourceOrigin::Allocated;
  /// The allocating call, the declaration for `Declared`, the increment for
  /// `Retained`.
  SourceLocation location = {};
  /// RFC 0010, *Shares*: how many shares of the object this function owns
  /// through the holder. One for every record made by the RFC 0007 rules; a
  /// count increment on the holder adds one; a copy of a holder with a
  /// surplus takes one away. Joins to the smaller count.
  std::uint32_t shares = 1;
  /// RFC 0010: the count field the shares were taken through (the canonical
  /// spelling of the record type and the field path, `struct obj .rc`);
  /// empty when the record is not share-counted. A `Retained` record leaks
  /// only when its field is a known count.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string countField = {};
  /// The release family the resource must be released with; empty when
  /// unknown.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string family = {};
  /// The resource was handed to code the checker cannot follow (an unknown
  /// callee, an integer cast, a raw destination): its holder's death is not
  /// a leak.
  bool escaped = false;
  /// The holder points *into* the resource rather than at its start (`q = p
  /// + 1`, `q = strchr(p, c)`; RFC 0008, *Invalid releases*): releasing
  /// through it is invalid. Cleared by a fresh allocation.
  bool interior = false;
  /// RFC 0009: the place holds the resource only when the guard holds (the
  /// facts on the path that acquired it, and the callee's guard on a
  /// conditional `fresh` result). Refuted by a later test, the record is
  /// gone and its holder's death is not a leak.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PlaceGuard guard = {};

  friend bool operator==(const ResourceRecord &,
                         const ResourceRecord &) = default;
};

/// Flow-insensitive record of resource holders and known-null places; the
/// analysis driver clones and joins trackers per CFG block, exactly as for
/// `MoveTracker`.
class ResourceTracker {
public:
  /// `place` now holds the resource described by `record`, replacing any
  /// earlier record (a reassignment is a new resource). Clears the null mark.
  void hold(PlaceId place, ResourceRecord record);

  /// The record if `place` holds a tracked resource.
  [[nodiscard]] std::optional<ResourceRecord> recordOf(PlaceId place) const;
  [[nodiscard]] bool holds(PlaceId place) const {
    return owned.contains(place);
  }

  /// Flags the resource at `place` as escaped; no-op without a record.
  void escape(PlaceId place);
  /// RFC 0010, *Per-outcome stores*: the store that escaped the resource at
  /// `place` was retracted; the flag is cleared. No-op without a record.
  void unescape(PlaceId place);
  [[nodiscard]] bool isEscaped(PlaceId place) const;

  /// Forgets the resource at `place` (released, lost, reassigned).
  void clear(PlaceId place);

  /// RFC 0010: `place` gains one share of the resource it holds, taken
  /// through `countField`. Without a record, `place` gets a `Retained` one
  /// with a single share at `location`. Returns the record afterwards.
  ResourceRecord retain(PlaceId place, std::string countField,
                        SourceLocation location);
  /// RFC 0010: `place` gives up one share. Returns the shares left; the
  /// record is cleared when none are (the caller decides whether the holder
  /// is then dead). No-op returning 0 without a record.
  std::uint32_t release(PlaceId place);

  /// `place` is known to hold a null pointer (an assignment of a null
  /// constant, or the edge on which its null test holds). Drops any record.
  void markNull(PlaceId place);
  [[nodiscard]] bool isNull(PlaceId place) const {
    return null.contains(place);
  }
  void forgetNull(PlaceId place) { null.erase(place); }

  /// Forgets everything about `place`.
  void forget(PlaceId place);

  /// Records join by union (a place *may* hold a resource): for a place on
  /// both sides this side's record is kept, `escaped` and `interior` are
  /// or-ed, the family and count field cleared when the sides disagree, the
  /// share count is the smaller (RFC 0010) and the guards joined (RFC 0009).
  /// Null facts join by intersection (a place *must* be null). Returns
  /// whether this tracker changed.
  bool join(const ResourceTracker &other);

  /// `place` now satisfies `fact`: records whose guard is refuted are
  /// cleared and returned (RFC 0009, *Refuting guards*).
  std::vector<PlaceId> learn(PlaceId place, const ValueFact &fact);
  /// `place` was overwritten: no guard may speak about it any more.
  void dropGuardsOn(PlaceId place);

  /// Holders in ascending order (for dumps and the leak scan).
  [[nodiscard]] std::vector<PlaceId> holders() const;
  /// Known-null places in ascending order (for dumps).
  [[nodiscard]] std::vector<PlaceId> nullPlaces() const;

  [[nodiscard]] bool empty() const noexcept {
    return owned.empty() && null.empty();
  }

  friend bool operator==(const ResourceTracker &,
                         const ResourceTracker &) = default;

private:
  std::map<PlaceId, ResourceRecord> owned;
  std::set<PlaceId> null;
};

/// Stable spelling used in dumps: `allocated`, `declared`, `retained`.
[[nodiscard]] std::string_view toString(ResourceOrigin origin) noexcept;

} // namespace weavec::core

#endif // WEAVEC_CORE_RESOURCE_H
