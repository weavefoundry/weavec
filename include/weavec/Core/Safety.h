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
#include "weavec/Core/Scalar.h"
#include "weavec/Core/SourceLocation.h"
#include "weavec/Core/Spatial.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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
  std::vector<SourceLocation> calls;

  /// Does not contain frontend handles, outcomes or unstable place numbers.
  [[nodiscard]] std::string identity() const;
  friend bool operator==(const SafetyObligation &,
                         const SafetyObligation &) = default;
};

class SafetyLedger {
public:
  /// Same operation on several paths retains the weakest proof outcome.
  void add(SafetyObligation obligation);
  void join(const SafetyLedger &other);
  [[nodiscard]] bool complete() const;
  [[nodiscard]] bool violated() const;
  [[nodiscard]] bool trusted() const;
  [[nodiscard]] bool limited() const { return exhausted; }
  void markLimited() { exhausted = true; }
  [[nodiscard]] const std::map<std::string, SafetyObligation> &entries() const {
    return obligations;
  }
  friend bool operator==(const SafetyLedger &, const SafetyLedger &) = default;

private:
  std::map<std::string, SafetyObligation> obligations;
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
