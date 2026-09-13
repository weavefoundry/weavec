//===- Union.h - Overlapping member evidence (RFC 0025) ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_UNION_H
#define WEAVEC_CORE_UNION_H

#include "weavec/Core/ObjectType.h"
#include "weavec/Core/Scalar.h"
#include "weavec/Core/Spatial.h"

#include <map>
#include <set>
#include <vector>

namespace weavec::core {

inline constexpr std::size_t MaxUnionObjects = 64;
inline constexpr std::size_t MaxUnionAlternatives = 8;
inline constexpr std::size_t MaxUnionMembers = 32;

/// A member view is independent of the value and any pointee permissions.
struct UnionMember {
  ObjectType object;
  ObjectType value;
  std::string name;
  bool pointer = false;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<UnionMember> decode(std::string_view text);
  friend bool operator==(const UnionMember &, const UnionMember &) = default;
};

/// Independently established pointer evidence, carried through the same
/// branch alternatives as a member view. This never initializes pointee bytes.
struct UnionPointer {
  PlaceId holder;
  PlaceId storage;
  Affine offset;
  std::optional<Affine> extent;
  std::optional<PlaceId> input;
  bool valid = false;
  friend auto operator<=>(const UnionPointer &, const UnionPointer &) = default;
};

struct UnionWitness {
  std::string member;
  PlaceGuard when;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<UnionPointer> pointer = {};
  friend auto operator<=>(const UnionWitness &, const UnionWitness &) = default;
};

/// Must-evidence under stable predicates. Written/unknown storage cannot
/// re-acquire an input requirement after its original value was invalidated.
struct UnionState {
  std::map<PlaceId, std::vector<UnionWitness>> members;
  std::set<PlaceId> written;
  bool havoc = false;
  void set(PlaceId storage, std::string member, PlaceGuard when = {});
  void invalidate(PlaceId storage);
  void invalidateAll();
  void forgetDependency(PlaceId place);
  void forgetPointer(PlaceId holder);
  void copy(PlaceId source, PlaceId destination);
  [[nodiscard]] bool mayRequire(PlaceId storage) const;
  bool join(const UnionState &other, const std::vector<PlaceGuard> &left,
            const std::vector<PlaceGuard> &right);
  friend bool operator==(const UnionState &, const UnionState &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_UNION_H
