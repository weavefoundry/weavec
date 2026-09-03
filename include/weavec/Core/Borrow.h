//===- Borrow.h - Loan tracking and conflict detection ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `BorrowState` tracks the set of live loans against places and answers the
// classic borrow-checking questions: may this new borrow be created, and may
// this place be moved/mutated right now?
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_BORROW_H
#define WEAVEC_CORE_BORROW_H

#include "weavec/Core/Lifetime.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/SourceLocation.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace weavec::core {

enum class BorrowKind : std::uint8_t {
  Shared,
  Mutable,
};

[[nodiscard]] std::string_view toString(BorrowKind kind) noexcept;

/// A live borrow of `place` valid for `lifetime`, held by the pointer place
/// `holder` (the variable or field the borrowing pointer was stored in).
struct Loan {
  PlaceId place;
  BorrowKind kind = BorrowKind::Shared;
  LifetimeId lifetime;
  SourceLocation location;
  PlaceId holder;

  friend bool operator==(const Loan &, const Loan &) = default;
};

/// Describes why a borrow or access was rejected.
struct BorrowConflict {
  /// The pre-existing loan that conflicts.
  Loan existing;
  /// The borrow that was attempted, if any (empty for moves/mutations).
  std::optional<Loan> attempted;
};

/// Tracks live loans and detects aliasing violations.
///
/// Rules mirror Rust's:
///   * any number of shared borrows may coexist;
///   * a mutable borrow excludes every other borrow of the same place;
///   * a place with any live loan may not be moved or mutated directly.
///
/// `BorrowState` knows nothing about place structure: conflicts between a
/// place and its fields (`&s` vs `&s.f`) are the analysis layer's job, which
/// asks about each related place in turn.
class BorrowState {
public:
  /// Attempts to record `loan`. Returns the conflict on failure; the loan is
  /// only recorded on success.
  [[nodiscard]] std::optional<BorrowConflict> addLoan(const Loan &loan);

  /// Returns the loan that would conflict with `loan`, without recording
  /// anything. Used for borrows that last only for a call.
  [[nodiscard]] std::optional<BorrowConflict>
  findConflict(const Loan &loan) const;

  /// Records `loan` unconditionally. Used when an existing borrow is shared
  /// with another holder (a pointer copy), which is not a new borrow and so
  /// cannot conflict with itself.
  void addLoanUnchecked(const Loan &loan);

  /// Returns the first live loan preventing a move of `place`, if any.
  [[nodiscard]] std::optional<BorrowConflict> checkMove(PlaceId place) const;

  /// Returns the first live loan preventing direct mutation of `place`.
  [[nodiscard]] std::optional<BorrowConflict>
  checkMutation(PlaceId place) const;

  /// Drops every loan whose lifetime is exactly `lifetime`.
  void expire(LifetimeId lifetime);

  /// Drops every loan against `place`.
  void release(PlaceId place);

  /// Drops every loan held by `holder`, e.g. because it was reassigned.
  void dropHolder(PlaceId holder);

  /// Drops every loan whose holder satisfies `dead`: the holder will not be
  /// read again (RFC 0006, *Loans end at the last use of their holder*).
  void expireHolders(const std::function<bool(PlaceId)> &dead);

  /// Gives `to` a copy of every loan held by `from`.
  void copyHolder(PlaceId from, PlaceId to);

  /// Loans held by `holder`.
  [[nodiscard]] std::vector<Loan> heldBy(PlaceId holder) const;

  /// Set union with `other`: a loan live on either incoming path is live.
  /// Returns whether this state changed.
  bool join(const BorrowState &other);

  /// The live loans, ascending by place, then holder.
  [[nodiscard]] const std::vector<Loan> &loans() const noexcept { return live; }
  [[nodiscard]] bool hasLoans(PlaceId place) const noexcept;
  [[nodiscard]] bool contains(const Loan &loan) const noexcept;

  friend bool operator==(const BorrowState &lhs, const BorrowState &rhs);

private:
  /// Kept sorted (see `before`) so membership is a binary search and `join`
  /// a merge: a large function holds hundreds of loans, and both run at
  /// every CFG edge.
  std::vector<Loan> live;

  [[nodiscard]] static bool before(const Loan &lhs, const Loan &rhs) noexcept;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_BORROW_H
