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
//   State = { moves, loans, aliases, reallocs, kinds, raw }
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

#include <map>
#include <optional>
#include <vector>

namespace weavec::core {

struct AnalysisState {
  /// Places whose resource has been released or moved out.
  MoveTracker moves;
  /// Live borrows.
  BorrowState loans;
  /// Pointer places that may hold the same value.
  AliasRelation aliases;
  /// Pending `realloc` results: the place holding the result maps to the
  /// places whose resource the call consumed. A direct null test of the
  /// result reinstates them on the null edge (RFC 0002, *`realloc` and the
  /// null edge*). Entries are dropped on any reassignment of the result.
  std::map<PlaceId, std::vector<PlaceId>> reallocs;
  /// Inferred ownership kind per pointer place.
  std::map<PlaceId, OwnershipKind> kinds;
  /// Pointer places holding a raw pointer (RFC 0004): dereferencing or
  /// releasing one is legal only inside an unsafe region.
  RawTracker raw;

  /// Component-wise join with the state of another incoming edge.
  void join(const AnalysisState &other);

  /// Ownership kind of `place`, `Unknown` if never assigned.
  [[nodiscard]] OwnershipKind kindOf(PlaceId place) const noexcept;

  /// Forgets everything about `place` itself: its move record, its alias
  /// class membership, the loans it holds, the loans against it, its pending
  /// realloc, its kind and its raw record. Used when the place is
  /// (re)initialised or goes out of scope. Descendants are the caller's
  /// responsibility.
  void forget(PlaceId place);

  friend bool operator==(const AnalysisState &,
                         const AnalysisState &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_ANALYSISSTATE_H
