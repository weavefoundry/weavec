//===- Borrow.cpp - Loan tracking and conflict detection ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Borrow.h"

#include <algorithm>
#include <vector>

namespace weavec::core {

static bool conflicts(const Loan &existing, const Loan &attempted) {
  if (existing.place != attempted.place)
    return false;
  // Two shared borrows never conflict; anything involving a mutable borrow
  // does.
  return existing.kind == BorrowKind::Mutable ||
         attempted.kind == BorrowKind::Mutable;
}

std::optional<BorrowConflict> BorrowState::addLoan(const Loan &loan) {
  for (const Loan &existing : live) {
    if (conflicts(existing, loan))
      return BorrowConflict{.existing = existing, .attempted = loan};
  }
  live.push_back(loan);
  return std::nullopt;
}

std::optional<BorrowConflict> BorrowState::checkMove(PlaceId place) const {
  // Any live loan, shared or mutable, pins the place.
  for (const Loan &existing : live) {
    if (existing.place == place)
      return BorrowConflict{.existing = existing, .attempted = std::nullopt};
  }
  return std::nullopt;
}

std::optional<BorrowConflict> BorrowState::checkMutation(PlaceId place) const {
  // Direct mutation through the owner is a write, so it conflicts with every
  // outstanding borrow exactly like a move does.
  return checkMove(place);
}

void BorrowState::expire(LifetimeId lifetime) {
  std::erase_if(
      live, [lifetime](const Loan &loan) { return loan.lifetime == lifetime; });
}

void BorrowState::release(PlaceId place) {
  std::erase_if(live,
                [place](const Loan &loan) { return loan.place == place; });
}

bool BorrowState::hasLoans(PlaceId place) const noexcept {
  return std::ranges::any_of(
      live, [place](const Loan &loan) { return loan.place == place; });
}

} // namespace weavec::core
