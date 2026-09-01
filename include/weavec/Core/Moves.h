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

#include <map>
#include <optional>
#include <vector>

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
  /// The place named in the releasing/moving expression when it differs from
  /// the place this record is attached to, i.e. the move happened through an
  /// alias (`free(q)` marking `p`). Lets diagnostics say "freed here (through
  /// 'q')".
  std::optional<PlaceId> via;

  friend bool operator==(const MoveRecord &, const MoveRecord &) = default;
};

/// Flow-insensitive record of moved-out places. Flow sensitivity is layered
/// on top by the analysis driver, which clones/joins trackers per CFG block.
class MoveTracker {
public:
  /// Marks `place` as moved out. Returns the prior record if it was already
  /// moved (i.e. a double move / double free); the original record is kept.
  std::optional<MoveRecord> markMoved(PlaceId place, MoveReason reason,
                                      SourceLocation location,
                                      std::optional<PlaceId> via = {});

  /// Reinitializes `place`, e.g. after assignment of a fresh value.
  void reinitialize(PlaceId place);

  /// Returns the move record if `place` is currently moved out.
  [[nodiscard]] std::optional<MoveRecord> movedAt(PlaceId place) const;

  [[nodiscard]] bool isMoved(PlaceId place) const {
    return movedAt(place).has_value();
  }

  /// Merges another tracker into this one, keeping the union of moved places.
  /// This is the conservative "may be moved" join used at CFG merge points.
  /// Where both sides moved the same place, this side's record is kept, so
  /// the result does not depend on evaluation order.
  void join(const MoveTracker &other);

  /// Moved places in ascending order (for dumps).
  [[nodiscard]] std::vector<PlaceId> movedPlaces() const;

  [[nodiscard]] bool empty() const noexcept { return moved.empty(); }

  friend bool operator==(const MoveTracker &, const MoveTracker &) = default;

private:
  std::map<PlaceId, MoveRecord> moved;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_MOVES_H
