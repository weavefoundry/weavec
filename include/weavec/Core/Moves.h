//===- Moves.h - Move / deinitialization tracking --------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `MoveTracker` records which places have had their ownership moved out
// (including by being freed) so subsequent uses can be flagged.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_MOVES_H
#define WEAVEC_CORE_MOVES_H

#include "weavec/Core/Place.h"
#include "weavec/Core/SourceLocation.h"

#include <optional>
#include <unordered_map>

namespace weavec::core {

/// Why a place became uninitialized.
enum class MoveReason : std::uint8_t {
  /// Ownership transferred elsewhere (assignment, passed by value, ...).
  Moved,
  /// The resource was released (e.g. `free`).
  Freed,
};

struct MoveRecord {
  MoveReason reason = MoveReason::Moved;
  SourceLocation location;
};

/// Flow-insensitive record of moved-out places. Flow sensitivity is layered
/// on top by the analysis driver, which clones/joins trackers per CFG block.
class MoveTracker {
public:
  /// Marks `place` as moved out. Returns the prior record if it was already
  /// moved (i.e. a double move / double free).
  std::optional<MoveRecord> markMoved(PlaceId place, MoveReason reason,
                                      SourceLocation location);

  /// Reinitializes `place`, e.g. after assignment of a fresh value.
  void reinitialize(PlaceId place);

  /// Returns the move record if `place` is currently moved out.
  [[nodiscard]] std::optional<MoveRecord> movedAt(PlaceId place) const;

  [[nodiscard]] bool isMoved(PlaceId place) const {
    return movedAt(place).has_value();
  }

  /// Merges another tracker into this one, keeping the union of moved places.
  /// This is the conservative "may be moved" join used at CFG merge points.
  void join(const MoveTracker &other);

private:
  std::unordered_map<PlaceId, MoveRecord, PlaceIdHash> moved;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_MOVES_H
