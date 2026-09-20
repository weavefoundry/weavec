//===- Borrow.cpp - Loan tracking and conflict detection ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Borrow.h"

#include <algorithm>
#include <iterator>
#include <tuple>
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

/// The borrow itself: what is borrowed, by whom, how, for how long. Two
/// loans with the same key are the same borrow recorded at different sites.
static auto borrowKey(const Loan &loan) noexcept {
  return std::tie(loan.place, loan.holder, loan.kind, loan.lifetime.value);
}

static auto locationKey(const Loan &loan) noexcept {
  return std::tie(loan.location.line, loan.location.column,
                  loan.location.opaque, loan.location.file);
}

bool BorrowState::sameBorrow(const Loan &lhs, const Loan &rhs) noexcept {
  return borrowKey(lhs) == borrowKey(rhs);
}

bool BorrowState::before(const Loan &lhs, const Loan &rhs) noexcept {
  const auto l = borrowKey(lhs);
  const auto r = borrowKey(rhs);
  if (l != r)
    return l < r;
  return locationKey(lhs) < locationKey(rhs);
}

void BorrowState::addLoanUnchecked(const Loan &loan) {
  const auto at = std::ranges::lower_bound(
      live, loan, [](const Loan &lhs, const Loan &rhs) {
        return borrowKey(lhs) < borrowKey(rhs);
      });
  if (at == live.end() || !sameBorrow(*at, loan)) {
    live.insert(at, loan);
    return;
  }
  // The same borrow from another site: one record, the earliest site. It
  // holds on every path through here if the new record does (RFC 0030).
  const bool allPaths = at->allPaths || loan.allPaths;
  if (locationKey(loan) < locationKey(*at))
    *at = loan;
  at->allPaths = allPaths;
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

void BorrowState::drop(PlaceId holder, PlaceId place) {
  std::erase_if(live, [holder, place](const Loan &loan) {
    return loan.holder == holder && loan.place == place;
  });
}

void BorrowState::expireHolders(const std::function<bool(PlaceId)> &dead) {
  std::erase_if(live, [&dead](const Loan &loan) { return dead(loan.holder); });
}

void BorrowState::copyHolder(PlaceId from, PlaceId to,
                             std::optional<SourceLocation> at) {
  if (from == to)
    return;
  for (Loan loan : heldBy(from)) {
    loan.holder = to;
    if (at)
      loan.location = *at;
    addLoanUnchecked(loan);
  }
}

void BorrowState::weakenHolder(PlaceId holder) {
  for (Loan &loan : live)
    if (loan.holder == holder)
      loan.allPaths = false;
}

std::vector<Loan> BorrowState::heldBy(PlaceId holder) const {
  std::vector<Loan> result;
  for (const Loan &loan : live) {
    if (loan.holder == holder)
      result.push_back(loan);
  }
  return result;
}

bool BorrowState::join(const BorrowState &other) {
  // Both sides hold one loan per borrow, sorted by borrow then site. The
  // union keeps one per borrow, at the earliest site; a borrow on one side
  // only holds on some paths (RFC 0030 §3.1). At the fixpoint nothing is
  // new; find that out first, without copying a loan (each carries a file
  // name).
  const auto hereOnly = [&](auto mine, auto theirs) {
    return theirs == other.live.end() ||
           (mine != live.end() && borrowKey(*mine) < borrowKey(*theirs));
  };
  const auto thereOnly = [&](auto mine, auto theirs) {
    return mine == live.end() || borrowKey(*theirs) < borrowKey(*mine);
  };
  {
    auto mine = live.begin();
    auto theirs = other.live.begin();
    bool unchanged = true;
    while (unchanged && (mine != live.end() || theirs != other.live.end())) {
      if (hereOnly(mine, theirs)) {
        unchanged = !mine->allPaths;
        ++mine;
      } else if (thereOnly(mine, theirs)) {
        unchanged = false;
      } else {
        unchanged = locationKey(*mine) <= locationKey(*theirs) &&
                    (!mine->allPaths || theirs->allPaths);
        ++mine;
        ++theirs;
      }
    }
    if (unchanged)
      return false;
  }
  std::vector<Loan> merged;
  merged.reserve(live.size() + other.live.size());
  auto mine = live.begin();
  auto theirs = other.live.begin();
  while (mine != live.end() || theirs != other.live.end()) {
    if (hereOnly(mine, theirs)) {
      merged.push_back(*mine++);
      merged.back().allPaths = false;
    } else if (thereOnly(mine, theirs)) {
      merged.push_back(*theirs++);
      merged.back().allPaths = false;
    } else {
      const bool allPaths = mine->allPaths && theirs->allPaths;
      merged.push_back(locationKey(*mine) <= locationKey(*theirs) ? *mine
                                                                  : *theirs);
      merged.back().allPaths = allPaths;
      ++mine;
      ++theirs;
    }
  }
  live = std::move(merged);
  return true;
}

bool BorrowState::hasLoans(PlaceId place) const noexcept {
  return std::ranges::any_of(
      live, [place](const Loan &loan) { return loan.place == place; });
}

bool BorrowState::contains(const Loan &loan) const noexcept {
  return std::ranges::binary_search(live, loan, before);
}

bool operator==(const BorrowState &lhs, const BorrowState &rhs) {
  return lhs.live == rhs.live;
}

} // namespace weavec::core
