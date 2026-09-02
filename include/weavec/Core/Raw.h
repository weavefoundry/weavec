//===- Raw.h - Raw pointer tracking ----------------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `RawTracker` records which places hold a *raw* pointer (RFC 0004): one the
// model has no ownership knowledge about, because it was cast from an
// integer, declared `WEAVEC_RAW`, loaded through another raw pointer, or
// handed out by code WeaveC cannot see. Dereferencing or releasing a raw
// pointer is legal only inside an unsafe region; the checker consults this
// tracker to decide.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_RAW_H
#define WEAVEC_CORE_RAW_H

#include "weavec/Core/Place.h"
#include "weavec/Core/SourceLocation.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

/// Why a pointer is raw (RFC 0004, *Raw pointers*).
enum class RawReason : std::uint8_t {
  /// Converted from an integer: provenance was lost.
  IntegerCast,
  /// Read from a place declared `WEAVEC_RAW`.
  Declared,
  /// Loaded from memory reached through a raw pointer.
  LoadedThroughRaw,
  /// Returned or stored by a callee whose summary says `raw`.
  Callee,
  /// Returned by a call into code with no summary (`--strict-externs`).
  UnknownCallee,
};

struct RawRecord {
  RawReason reason = RawReason::IntegerCast;
  /// Where the pointer became raw.
  SourceLocation location;
  /// The place the raw value was copied from when this record was
  /// propagated by a copy (`q = p`), so notes can say "(through 'p')".
  std::optional<PlaceId> via;
  /// Free-form detail for notes, filled in by the analysis layer: the
  /// callee for `Callee`/`UnknownCallee`, the pointer's name for
  /// `LoadedThroughRaw`. Empty otherwise.
  std::string detail;

  friend bool operator==(const RawRecord &, const RawRecord &) = default;
};

/// Flow-insensitive record of raw places; the analysis driver clones and
/// joins trackers per CFG block, exactly as for `MoveTracker`.
class RawTracker {
public:
  /// Marks `place` raw. If it already is, the existing record is kept so
  /// later diagnostics point at the first site; returns false in that case.
  bool markRaw(PlaceId place, RawReason reason, SourceLocation location,
               std::optional<PlaceId> via = {});

  /// Marks `place` raw with a copy of `record` (a pointer copy).
  bool markRaw(PlaceId place, const RawRecord &record);

  /// Forgets that `place` is raw, e.g. after it is reassigned.
  void clear(PlaceId place);

  /// The record if `place` is currently raw.
  [[nodiscard]] std::optional<RawRecord> rawAt(PlaceId place) const;

  [[nodiscard]] bool isRaw(PlaceId place) const {
    return rawAt(place).has_value();
  }

  /// Set union ("may be raw"); this side's record wins for shared places.
  void join(const RawTracker &other);

  /// Raw places in ascending order (for dumps).
  [[nodiscard]] std::vector<PlaceId> rawPlaces() const;

  [[nodiscard]] bool empty() const noexcept { return raw.empty(); }

  friend bool operator==(const RawTracker &, const RawTracker &) = default;

private:
  std::map<PlaceId, RawRecord> raw;
};

/// Stable spelling used in dumps: `integer-cast`, `declared`, ...
[[nodiscard]] std::string_view toString(RawReason reason) noexcept;

} // namespace weavec::core

#endif // WEAVEC_CORE_RAW_H
