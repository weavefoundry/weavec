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
#include <optional>
#include <vector>

namespace weavec::core {

enum class BorrowKind : std::uint8_t {
  Shared,
  Mutable,
};

/// A live borrow of `place` valid for `lifetime`.
struct Loan {
  PlaceId place;
  BorrowKind kind = BorrowKind::Shared;
  LifetimeId lifetime;
  SourceLocation location;
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
class BorrowState {
public:
  /// Attempts to record `loan`. Returns the conflict on failure; the loan is
  /// only recorded on success.
  [[nodiscard]] std::optional<BorrowConflict> addLoan(const Loan &loan);

  /// Returns the first live loan preventing a move of `place`, if any.
  [[nodiscard]] std::optional<BorrowConflict> checkMove(PlaceId place) const;

  /// Returns the first live loan preventing direct mutation of `place`.
  [[nodiscard]] std::optional<BorrowConflict>
  checkMutation(PlaceId place) const;

  /// Drops every loan whose lifetime is exactly `lifetime`.
  void expire(LifetimeId lifetime);

  /// Drops every loan against `place`.
  void release(PlaceId place);

  [[nodiscard]] const std::vector<Loan> &loans() const noexcept { return live; }
  [[nodiscard]] bool hasLoans(PlaceId place) const noexcept;

private:
  std::vector<Loan> live;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_BORROW_H
