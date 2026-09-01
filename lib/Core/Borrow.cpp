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

std::string_view toString(BorrowKind kind) noexcept {
  switch (kind) {
  case BorrowKind::Shared:
    return "shared";
  case BorrowKind::Mutable:
    return "mutable";
  }
  return "?";
}

static bool conflicts(const Loan &existing, const Loan &attempted) {
  if (existing.place != attempted.place)
    return false;
  // Two shared borrows never conflict; anything involving a mutable borrow
  // does.
  return existing.kind == BorrowKind::Mutable ||
         attempted.kind == BorrowKind::Mutable;
}

std::optional<BorrowConflict>
BorrowState::findConflict(const Loan &loan) const {
  for (const Loan &existing : live) {
    if (conflicts(existing, loan))
      return BorrowConflict{.existing = existing, .attempted = loan};
  }
  return std::nullopt;
}

std::optional<BorrowConflict> BorrowState::addLoan(const Loan &loan) {
  if (auto conflict = findConflict(loan))
    return conflict;
  addLoanUnchecked(loan);
  return std::nullopt;
}

void BorrowState::addLoanUnchecked(const Loan &loan) {
  if (!contains(loan))
    live.push_back(loan);
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

void BorrowState::dropHolder(PlaceId holder) {
  std::erase_if(live,
                [holder](const Loan &loan) { return loan.holder == holder; });
}

void BorrowState::copyHolder(PlaceId from, PlaceId to) {
  if (from == to)
    return;
  for (Loan loan : heldBy(from)) {
    loan.holder = to;
    addLoanUnchecked(loan);
  }
}

std::vector<Loan> BorrowState::heldBy(PlaceId holder) const {
  std::vector<Loan> result;
  for (const Loan &loan : live) {
    if (loan.holder == holder)
      result.push_back(loan);
  }
  return result;
}

void BorrowState::join(const BorrowState &other) {
  for (const Loan &loan : other.live)
    addLoanUnchecked(loan);
}

bool BorrowState::hasLoans(PlaceId place) const noexcept {
  return std::ranges::any_of(
      live, [place](const Loan &loan) { return loan.place == place; });
}

bool BorrowState::contains(const Loan &loan) const noexcept {
  return std::ranges::find(live, loan) != live.end();
}

bool operator==(const BorrowState &lhs, const BorrowState &rhs) {
  if (lhs.live.size() != rhs.live.size())
    return false;
  return std::ranges::all_of(
      lhs.live, [&rhs](const Loan &loan) { return rhs.contains(loan); });
}

} // namespace weavec::core
