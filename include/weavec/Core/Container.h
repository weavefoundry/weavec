//===- Container.h - Inductive linked storage (RFC 0023) -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_CONTAINER_H
#define WEAVEC_CORE_CONTAINER_H

#include "weavec/Core/ObjectType.h"
#include "weavec/Core/Place.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

inline constexpr std::size_t MaxContainerFields = 32;
inline constexpr std::size_t MaxContainerNodes = 64;
inline constexpr std::size_t MaxContainerFacts = 64;
inline constexpr std::size_t MaxContainerDescriptorBytes = 16384;

/// Endpoint-exclusive chain capabilities. Stronger capabilities include reads.
enum class ContainerAccess : std::uint8_t { Read, Write, Release };

struct ContainerField {
  std::string name;
  std::uint64_t offset = 0;
  std::uint64_t bytes = 0;
  friend auto operator<=>(const ContainerField &,
                          const ContainerField &) = default;
};

struct ContainerPayload {
  ContainerField field;
  std::string family;
  friend auto operator<=>(const ContainerPayload &,
                          const ContainerPayload &) = default;
};

/// Portable target-aware predicate. Initialized fields exclude padding bytes.
struct ContainerShape {
  ObjectType object;
  ContainerField link;
  std::vector<ContainerField> initialized;
  std::vector<ContainerPayload> payloads;
  std::string family;
  ContainerAccess access = ContainerAccess::Read;
  bool terminal = false;

  [[nodiscard]] bool valid() const;
  [[nodiscard]] bool entails(const ContainerShape &required) const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<ContainerShape>
  decode(std::string_view text);
  friend bool operator==(const ContainerShape &,
                         const ContainerShape &) = default;
};

/// Concrete witnesses and folded certificates are independent of may aliases.
/// Empty is a known null endpoint, never an unknown pointer.
struct ContainerFact {
  ContainerShape shape;
  std::set<PlaceId> members;
  std::set<PlaceId> inputs;
  bool empty = false;
  bool suffix = false;
  /// A saved successor is separated from this holder's current head node.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PlaceId> tailOf = {};
  /// Holders whose current chains definitely contain this suffix.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<PlaceId> ancestors = {};
  /// Every non-input node has malloc/free ownership compatible with inputs.
  bool allocationCompatible = false;
  /// All nodes were allocated during this function invocation.
  bool localAllocation = false;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> releasedPayloads = {};
  [[nodiscard]] bool valid() const;
  [[nodiscard]] bool entails(const ContainerShape &required) const;
  friend bool operator==(const ContainerFact &,
                         const ContainerFact &) = default;
};

/// A small must-domain. Facts survive joins only when both branches justify
/// the predicate; membership is a may-set used for conservative invalidation.
class ContainerFacts {
public:
  [[nodiscard]] const ContainerFact *find(PlaceId holder) const;
  bool set(PlaceId holder, ContainerFact fact);
  void erase(PlaceId holder);
  void invalidate(PlaceId member);
  void clear();
  void separate(PlaceId first, PlaceId second);
  [[nodiscard]] bool separated(PlaceId first, PlaceId second) const;
  [[nodiscard]] std::set<PlaceId> separatedFrom(PlaceId holder) const;
  void replace(PlaceId holder);
  void markFresh(PlaceId holder);
  [[nodiscard]] bool unlinked(PlaceId holder) const {
    return fresh.contains(holder);
  }
  void publish(PlaceId holder) { fresh.erase(holder); }
  void block(PlaceId holder);
  [[nodiscard]] bool blocked(PlaceId holder) const {
    return invalidated || exhausted || killed.contains(holder);
  }
  bool join(const ContainerFacts &other);
  [[nodiscard]] const std::map<PlaceId, ContainerFact> &all() const {
    return facts;
  }
  [[nodiscard]] bool limited() const { return exhausted; }
  friend bool operator==(const ContainerFacts &,
                         const ContainerFacts &) = default;

private:
  std::map<PlaceId, ContainerFact> facts;
  std::set<std::pair<PlaceId, PlaceId>> separation;
  std::set<PlaceId> fresh;
  std::set<PlaceId> killed;
  bool exhausted = false;
  bool invalidated = false;
};

/// An explicit graph used to establish a predicate from existing memory facts.
/// Unknown successor/payload values are distinct from known null values.
struct ContainerEdge {
  std::optional<PlaceId> node;
  bool known = false;
  static ContainerEdge null() { return {.node = {}, .known = true}; }
  static ContainerEdge to(PlaceId target) {
    return {.node = target, .known = true};
  }
  friend bool operator==(const ContainerEdge &,
                         const ContainerEdge &) = default;
};
struct ContainerNode {
  ObjectType object;
  std::set<ContainerField> initialized;
  ContainerEdge next;
  std::map<std::string, ContainerEdge> payloads;
  std::string family;
  bool live = false;
  bool writable = false;
  bool allocationBase = false;
  friend bool operator==(const ContainerNode &,
                         const ContainerNode &) = default;
};
enum class ContainerFailure : std::uint8_t {
  None,
  Unknown,
  Cycle,
  View,
  Initialization,
  Writable,
  Release,
  Overlap,
  Limit
};
[[nodiscard]] std::string_view toString(ContainerFailure failure);
struct ContainerProof {
  ContainerFailure failure = ContainerFailure::Unknown;
  std::set<PlaceId> members;
  std::set<PlaceId> payloads;
  [[nodiscard]] bool complete() const {
    return failure == ContainerFailure::None;
  }
};
class ContainerGraph {
public:
  std::map<PlaceId, ContainerNode> nodes;
  [[nodiscard]] ContainerProof
  prove(ContainerEdge first, const ContainerShape &shape,
        ContainerEdge endpoint = ContainerEdge::null()) const;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_CONTAINER_H
