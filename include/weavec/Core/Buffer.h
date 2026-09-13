//===- Buffer.h - Contiguous container predicates (RFC 0026) -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_CORE_BUFFER_H
#define WEAVEC_CORE_BUFFER_H

#include "weavec/Core/Container.h"
#include "weavec/Core/Scalar.h"
#include "weavec/Core/Spatial.h"

namespace weavec::core {

inline constexpr std::size_t MaxBufferShapes = 16;
inline constexpr std::size_t MaxBufferFacts = 64;

/// Count fields are unsigned target integers, measured in fixed-size elements.
/// A prefix describes initialized cells, never permission to free their values.
struct BufferShape {
  ObjectType object;
  ContainerField data;
  ContainerField length;
  ContainerField capacity;
  std::uint64_t elementBytes = 1;
  bool pointerElements = false;
  bool terminated = false;
  bool ownsBacking = false;
  bool ownsElements = false;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] bool sameLayoutAs(const BufferShape &other) const;
  [[nodiscard]] bool entails(const BufferShape &other) const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<BufferShape> decode(std::string_view text);
  friend bool operator==(const BufferShape &, const BufferShape &) = default;
};

/// Must-evidence about the values CURRENTLY in these holders. The backing
/// allocation identity is deliberately not equated across a relational join.
struct BufferFact {
  BufferShape shape;
  PlaceId object;
  PlaceId length;
  PlaceId capacity;
  bool initialized = false;
  bool nonNull = false;
  // Entry backing or a fresh replacement, used only for separation premises.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PlaceId> entryBacking = {};
  friend bool operator==(const BufferFact &, const BufferFact &) = default;
};

/// Immutable entry sequence, optionally extended by one represented pointer.
/// The source token and entry length are snapshots, not mutable field names.
struct BufferSequence {
  PlaceId origin;
  PlaceId length;
  std::optional<PlaceId> appended;
  friend bool operator==(const BufferSequence &,
                         const BufferSequence &) = default;
};

struct BufferSequencePost {
  PlaceId event;
  std::optional<BufferSequence> sequence;
  std::optional<PlaceId> value;
  std::optional<PlaceId> borrowed;
  bool nullValue = false;
  bool ownsValue = false;
  bool ownsPrefix = false;
  bool appended = false;
  Affine index;
  PlaceGuard when;
  friend bool operator==(const BufferSequencePost &,
                         const BufferSequencePost &) = default;
};

struct BufferCapacityBound {
  PlaceId capacity;
  Affine minimum;
  PlaceGuard when;
  friend bool operator==(const BufferCapacityBound &,
                         const BufferCapacityBound &) = default;
};

struct BufferPost {
  BufferFact fact;
  PlaceGuard when;
  friend bool operator==(const BufferPost &, const BufferPost &) = default;
};

struct BufferFacts {
  std::map<PlaceId, BufferFact> values;
  // Backing capacity remains valid when only logical length changes. These
  // records carry no assertion about their length or initialized prefix.
  std::map<PlaceId, BufferFact> storage;
  std::map<PlaceId, std::vector<BufferPost>> pending;
  std::map<PlaceId, std::vector<BufferCapacityBound>> bounds;
  std::map<PlaceId, BufferSequence> sequences;
  std::map<PlaceId, std::vector<BufferSequencePost>> pendingSequences;
  bool limited = false;
  void set(PlaceId data, BufferFact fact);
  void forget(PlaceId dependency);
  bool join(const BufferFacts &other);
  friend bool operator==(const BufferFacts &, const BufferFacts &) = default;
};
} // namespace weavec::core
#endif // WEAVEC_CORE_BUFFER_H
