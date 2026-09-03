//===- AnalysisState.h - Per-program-point dataflow state ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The dataflow state RFC 0002 carries through a function body:
//
//   State = { moves, loans, aliases, pending, consumed, kinds, raw }
//
// Every component is a finite-height lattice whose `join` is monotone, so a
// worklist iteration over the CFG terminates without widening. Lifetimes are
// deliberately *not* part of the state: they are allocated per scope before
// the dataflow runs and only queried by it.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_ANALYSISSTATE_H
#define WEAVEC_CORE_ANALYSISSTATE_H

#include "weavec/Core/AliasRelation.h"
#include "weavec/Core/Borrow.h"
#include "weavec/Core/Moves.h"
#include "weavec/Core/Ownership.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Raw.h"
#include "weavec/Core/Resource.h"
#include "weavec/Core/Summary.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace weavec::core {

/// The consumption a call performed that depends on its result (RFC 0006,
/// *Pending outcomes*): for each outcome class the callee may produce, the
/// places consumed when the result is in that class. A test of the result
/// selects classes; a place consumed in none of the selected classes is
/// reinstated on that edge.
struct PendingOutcome {
  std::map<Outcome, std::vector<PlaceId>> consumedBy;
  /// Places whose value the callee may return as its non-null result
  /// (`if (c) { free(p); return NULL; } return p;`). One of them that is
  /// reinstated on the non-null edge *is* the result: the holder of the
  /// result and the place are exact aliases there and the result owns
  /// nothing of its own (RFC 0007, *Acquiring and losing a resource*).
  std::vector<PlaceId> returned = {};
  /// Per class, the caller places the callee left null on every path
  /// returning it (RFC 0007, *Per-outcome null stores*): once the test has
  /// narrowed the classes, a place null in all of them is null.
  std::map<Outcome, std::vector<PlaceId>> nullOn = {};

  /// The places null in every class still possible; empty when no class is.
  [[nodiscard]] std::vector<PlaceId> nullInAll() const;

  /// Every place mentioned in any class, ascending.
  [[nodiscard]] std::vector<PlaceId> places() const;

  /// Narrows to the classes in `selected`. Returns the places that are
  /// consumed in none of them (to be reinstated), or nothing at all when no
  /// selected class is possible (the edge is infeasible as far as the
  /// summary knows; nothing changes).
  std::vector<PlaceId> select(const std::set<Outcome> &selected);

  /// True if no class could still retract anything: every place is consumed
  /// in every remaining class.
  [[nodiscard]] bool settled() const;

  friend bool operator==(const PendingOutcome &,
                         const PendingOutcome &) = default;
};

struct AnalysisState {
  /// Places whose resource has been released or moved out.
  MoveTracker moves;
  /// Live borrows.
  BorrowState loans;
  /// Pointer places that may hold the same value.
  AliasRelation aliases;
  /// Calls whose consumption depends on their result, keyed by the place
  /// the result was stored in (RFC 0006). Entries are dropped on any
  /// reassignment of the result.
  std::map<PlaceId, PendingOutcome> pending;
  /// Consumption of the function's own interface (parameter roots and, per
  /// RFC 0003, paths under reassigned parameters) on the current path; the
  /// flow-sensitive record outcome classes are derived from (RFC 0006).
  std::map<SummaryPath, PlaceEffect> consumed;
  /// Inferred ownership kind per pointer place.
  std::map<PlaceId, OwnershipKind> kinds;
  /// Pointer places holding a raw pointer (RFC 0004): dereferencing or
  /// releasing one is legal only inside an unsafe region.
  RawTracker raw;
  /// Places holding an owned resource this function must account for, and
  /// places known to be null (RFC 0007).
  ResourceTracker resources;

  /// Component-wise join with the state of another incoming edge. Returns
  /// whether this state changed, so the fixpoint engine need not copy and
  /// compare whole states.
  bool join(const AnalysisState &other);

  /// Ownership kind of `place`, `Unknown` if never assigned.
  [[nodiscard]] OwnershipKind kindOf(PlaceId place) const noexcept;

  /// Forgets everything about `place` itself: its move record, its alias
  /// class membership, the loans it holds, the loans against it, its pending
  /// outcome, its kind and its raw record. Used when the place is
  /// (re)initialised or goes out of scope. Descendants are the caller's
  /// responsibility.
  void forget(PlaceId place);

  friend bool operator==(const AnalysisState &,
                         const AnalysisState &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_ANALYSISSTATE_H
