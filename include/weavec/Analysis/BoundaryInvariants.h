//===- BoundaryInvariants.h - The §9.4 boundary invariants ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §9.4: two invariants hold at every boundary — every Call site,
// every library call whose row hands an argument to a callback, and every
// function exit, a call that does not return included.
//
//   - *Validity*: no place the other side of the boundary can reach holds a
//     released pointer, or a pointer to storage whose lifetime has ended.
//     A boundary that breaks it is `unresolved(dangling-escape)`.
//   - *Owner uniqueness*: no two owning places it can reach hold the same
//     object. A boundary that breaks it is `unresolved(second-owner)`.
//
// Both are assumed at function entry (A1 and A3), which is what makes
// `free(s->a); free(s->b)` proven inside a cleanup function and the
// temporal facets of loads from parameters sound. An assumption a boundary
// elsewhere breaks cannot prove anything, so each row names the *place
// class* it concerns — a global `g`, or a field path `<struct>.<field>` —
// and every temporal facet the unit *proved* for a place of that class
// takes the boundary's reason instead. `LedgerAdapter::finish` applies both
// the rows and the propagation; the link step (§13.2 step 5) supplies the
// other units' rows, so the propagation is program-wide there.
//
// This component runs after the engine and never includes `Dataflow.h`
// (gate H2).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_BOUNDARYINVARIANTS_H
#define WEAVEC_ANALYSIS_BOUNDARYINVARIANTS_H

#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/Ledger.h"

#include "llvm/ADT/ArrayRef.h"

#include <string>
#include <vector>

namespace weavec::analysis {

/// §9.4: what the invariants decided for one unit.
struct BoundaryVerdicts {
  /// The boundaries whose invariant broke, and the propagation that
  /// follows from them (`BoundaryDecision::propagated`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<BoundaryDecision> decisions = {};
  /// The rows the unit record carries for the program-wide propagation
  /// (§13.1 `boundaries`, §13.2 step 5).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<BoundaryRow> exported = {};
};

/// §9.4: judges what the engine published at the unit's boundaries and
/// works out the propagation. `program` holds the other units' rows at
/// link, and is empty when a unit is compiled on its own.
[[nodiscard]] BoundaryVerdicts
checkBoundaryInvariants(const SiteIndex &sites,
                        llvm::ArrayRef<PublishedBoundary> published,
                        llvm::ArrayRef<BoundaryRow> program);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_BOUNDARYINVARIANTS_H
