//===- Safety.h - Checked obligations and positive evidence -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_SAFETY_H
#define WEAVEC_CORE_SAFETY_H

#include "weavec/Core/Place.h"
#include "weavec/Core/SafetyCallPath.h"
#include "weavec/Core/Scalar.h"
#include "weavec/Core/SourceLocation.h"
#include "weavec/Core/Spatial.h"

#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

/// RFC 0018: proof outcomes are independent of diagnostic severity.
enum class SafetyOutcome : std::uint8_t {
  Proven,
  Required,
  Trusted,
  Unresolved,
  Violation
};
enum class SafetyProperty : std::uint8_t {
  Bounds,
  Validity,
  Initialization,
  Release,
  Aliasing,
  Call,
  Semantics,
  Arithmetic,
  Resource
};

/// Entry assumptions remain visible even if they also supplied a local proof.
[[nodiscard]] inline SafetyOutcome safetyOutcome(bool proven, bool required) {
  if (required)
    return SafetyOutcome::Required;
  if (proven)
    return SafetyOutcome::Proven;
  return SafetyOutcome::Unresolved;
}

inline constexpr std::size_t MaxSafetyObligations = 2048;
inline constexpr std::size_t MaxSafetyRequirements = 256;
inline constexpr std::size_t MaxSafetyCallDepth = 16;
inline constexpr std::size_t MaxInitializedRanges = 32;

[[nodiscard]] std::string_view toString(SafetyOutcome value) noexcept;
[[nodiscard]] std::string_view toString(SafetyProperty value) noexcept;
[[nodiscard]] std::optional<SafetyOutcome> parseSafetyOutcome(std::string_view);
[[nodiscard]] std::optional<SafetyProperty>
    parseSafetyProperty(std::string_view);

struct SafetyObligation {
  SafetyProperty property = SafetyProperty::Semantics;
  SafetyOutcome outcome = SafetyOutcome::Unresolved;
  SourceLocation location;
  std::string function;
  std::string subject;
  std::string reason;
  SafetyCallPath calls;

  /// Does not contain frontend handles, outcomes or unstable place numbers.
  [[nodiscard]] std::string identity() const;
  friend bool operator==(const SafetyObligation &,
                         const SafetyObligation &) = default;
};

/// RFC 0020: canonical originating explanations, shared by ledger snapshots.
/// Paths already contain the callee's originating operation. Caller identity
/// and unsafe conversion are attached when applying this projection.
struct SafetyPropagation {
  std::vector<SafetyObligation> unresolved;
  std::vector<SafetyObligation> trusted;
};

/// An ordered read-only view of immutable, individually shared ledger rows.
/// RFC 0020: detaching the index never copies explanation strings or paths.
class SafetyEntries {
public:
  using Row = std::pair<const std::string, SafetyObligation>;

private:
  // Every key view points into the Row owned by the same map entry. Copying
  // the map preserves that owner, and replacement updates both together.
  using Index = std::map<std::string_view, std::shared_ptr<const Row>>;
  Index index;
  void set(std::string key, SafetyObligation obligation);
  void set(std::shared_ptr<const Row> row);
  void set(std::shared_ptr<const Row> row, Index::const_iterator position);
  friend class SafetyLedger;

public:
  class ConstIterator {
  public:
    // These spellings are required by std::iterator_traits and C++20 ranges.
    // NOLINTBEGIN(readability-identifier-naming)
    using iterator_category = std::forward_iterator_tag;
    using iterator_concept = std::forward_iterator_tag;
    using value_type = Row;
    using difference_type = std::ptrdiff_t;
    using pointer = const Row *;
    using reference = const Row &;
    // NOLINTEND(readability-identifier-naming)
    ConstIterator() = default;
    reference operator*() const { return *current->second; }
    pointer operator->() const { return current->second.get(); }
    ConstIterator &operator++() {
      ++current;
      return *this;
    }
    ConstIterator operator++(int) {
      auto previous = *this;
      ++*this;
      return previous;
    }
    friend bool operator==(const ConstIterator &,
                           const ConstIterator &) = default;

  private:
    explicit ConstIterator(Index::const_iterator current) : current(current) {}
    Index::const_iterator current;
    friend class SafetyEntries;
    friend class SafetyLedger;
  };

  [[nodiscard]] ConstIterator begin() const {
    return ConstIterator(index.begin());
  }
  [[nodiscard]] ConstIterator end() const { return ConstIterator(index.end()); }
  [[nodiscard]] ConstIterator find(std::string_view key) const {
    return ConstIterator(index.find(key));
  }
  [[nodiscard]] bool contains(std::string_view key) const {
    return index.contains(key);
  }
  [[nodiscard]] bool empty() const { return index.empty(); }
  [[nodiscard]] std::size_t size() const { return index.size(); }
  friend bool operator==(const SafetyEntries &, const SafetyEntries &);
};

class SafetyLedger {
public:
  SafetyLedger() = default;
  ~SafetyLedger() = default;
  SafetyLedger(const SafetyLedger &) = default;
  SafetyLedger &operator=(const SafetyLedger &) = default;
  SafetyLedger(SafetyLedger &&other) noexcept { *this = std::move(other); }
  SafetyLedger &operator=(SafetyLedger &&other) noexcept {
    if (this != &other) {
      obligations = std::move(other.obligations);
      weakest = std::exchange(other.weakest, SafetyOutcome::Proven);
      trustedEntries = std::exchange(other.trustedEntries, 0);
      exhausted = std::exchange(other.exhausted, false);
    }
    return *this;
  }
  /// Same operation on several paths retains the weakest proof outcome.
  void add(SafetyObligation obligation);
  /// RFC 0020: apply a canonical call-origin projection without copying paths
  /// that cannot change this ledger. Equivalent to ordinary call insertion.
  void addCalls(std::span<const SafetyObligation> origins,
                const SourceLocation &location, std::string_view function,
                std::string_view callee, bool unsafe);
  /// Reuse callee-specific explanation preparation from an immutable ledger.
  void addCalls(const SafetyLedger &source, bool trusted,
                const SourceLocation &location, std::string_view function,
                std::string_view callee, bool unsafe);
  void join(const SafetyLedger &other);
  [[nodiscard]] bool complete() const;
  [[nodiscard]] bool violated() const;
  [[nodiscard]] bool trusted() const;
  [[nodiscard]] bool limited() const { return exhausted; }
  void markLimited() { exhausted = true; }
  [[nodiscard]] const SafetyEntries &entries() const;
  [[nodiscard]] const SafetyPropagation &propagation() const;
  /// RFC 0020: weakly intern a completed immutable snapshot when a pool exists.
  void shareSnapshot();
  /// RFC 0020: convergence compares proof outcomes, not explanation routes.
  friend bool operator==(const SafetyLedger &, const SafetyLedger &);
  [[nodiscard]] bool sameExplanationsAs(const SafetyLedger &other) const {
    return exhausted == other.exhausted && entries() == other.entries();
  }

private:
  bool rejects(SafetyEntries::ConstIterator found, SafetyOutcome outcome,
               std::string_view reason);
  void addPrepared(std::string key, SafetyObligation obligation,
                   SafetyEntries::ConstIterator found);
  /// Immutable across copies; mutation detaches only when necessary.
  struct Storage;
  friend class SafetyEntryPool;
  std::shared_ptr<Storage> obligations;
  // Derived from the map: repeated completeness queries must not rescan every
  // propagated explanation. Outcomes only weaken; trust can be replaced.
  SafetyOutcome weakest = SafetyOutcome::Proven;
  std::size_t trustedEntries = 0;
  bool exhausted = false;
};

struct InitializedRange {
  Affine begin = Affine::ofConstant(0);
  Affine end = Affine::ofConstant(0);
  /// RFC 0019: a must-fact conditional on unchanged place values.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PlaceGuard when = {};
  /// RFC 0019: preserve initialization from this function-entry object.
  /// A relational fact, not unconditional initialized bytes.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PlaceId> source = {};
  bool zeroed = false;
  friend auto operator<=>(const InitializedRange &,
                          const InitializedRange &) = default;
};

/// RFC 0018: must-facts. Missing entries are unknown, never initialized.
struct SafetyState {
  std::set<PlaceId> initialized;
  std::set<PlaceId> pointers;
  /// May-fact: these objects require unresolved external effects at link time.
  std::set<PlaceId> deferred;
  std::map<PlaceId, std::vector<InitializedRange>> memory;
  /// RFC 0019: pointer holders name storage, independently of their own bytes.
  std::map<PlaceId, PlaceId> objects;
  /// A bounded disjunction of incoming path conditions. Empty means unknown.
  std::vector<PlaceGuard> paths;
  bool havoc = false;
  void refinePaths(const PlaceGuard &guard);

  void initialize(PlaceId storage, InitializedRange range);
  void forgetZeros();
  void copyMemory(PlaceId source, PlaceId destination);
  void forget(PlaceId place);
  void forgetDependency(PlaceId place);
  bool join(const SafetyState &other, const PlaceGuard &left = {},
            const PlaceGuard &right = {});
  friend bool operator==(const SafetyState &, const SafetyState &) = default;
};

/// JSON string escaping shared by reports and stable portable records.
[[nodiscard]] std::string safetyJsonString(std::string_view value);

} // namespace weavec::core

#endif // WEAVEC_CORE_SAFETY_H
