//===- AnalysisState.cpp - Per-program-point dataflow state ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AnalysisState.h"

namespace weavec::core {

void AnalysisState::join(const AnalysisState &other) {
  moves.join(other.moves);
  loans.join(other.loans);
  aliases.join(other.aliases);
  raw.join(other.raw);

  // A pending realloc that is only pending on one incoming path cannot be
  // safely undone, so keep only entries both sides agree on.
  for (auto it = reallocs.begin(); it != reallocs.end();) {
    const auto theirs = other.reallocs.find(it->first);
    if (theirs == other.reallocs.end() || theirs->second != it->second)
      it = reallocs.erase(it);
    else
      ++it;
  }

  for (const auto &[place, kind] : other.kinds) {
    auto [it, inserted] = kinds.try_emplace(place, kind);
    if (!inserted)
      it->second = core::join(it->second, kind);
  }
}

OwnershipKind AnalysisState::kindOf(PlaceId place) const noexcept {
  const auto it = kinds.find(place);
  return it == kinds.end() ? OwnershipKind::Unknown : it->second;
}

void AnalysisState::forget(PlaceId place) {
  moves.reinitialize(place);
  aliases.separate(place);
  loans.dropHolder(place);
  loans.release(place);
  reallocs.erase(place);
  kinds.erase(place);
  raw.clear(place);
}

} // namespace weavec::core
