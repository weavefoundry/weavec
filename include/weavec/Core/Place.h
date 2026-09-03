//===- Place.h - Abstract memory places ------------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A "place" is an abstract, frontend-neutral handle for a storage location.
// Places are structured (RFC 0002): a *base* (a variable, parameter or
// global) followed by a path of field selections, dereferences and a
// collapsing array summary:
//
//   place ::= base ('.' field | '*' | '[*]')*
//
// `p->next` is `(*p).next`, `a[i]` and `a[0]` are both `a[*]`, and `p[i]` or
// `*(p + k)` are `*p`. The Clang integration layer maps `clang::ValueDecl`s
// and expressions onto places; the core model only ever reasons about
// `PlaceId`s and the parent/child structure recorded here.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_PLACE_H
#define WEAVEC_CORE_PLACE_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

/// Opaque identifier for a place within one analysis unit.
struct PlaceId {
  std::uint32_t value = 0;

  friend constexpr bool operator==(PlaceId, PlaceId) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(PlaceId,
                                                    PlaceId) noexcept = default;
};

struct PlaceIdHash {
  std::size_t operator()(PlaceId id) const noexcept {
    return std::hash<std::uint32_t>{}(id.value);
  }
};

/// One step of a place path below its base.
enum class PathStep : std::uint8_t {
  /// `parent.field` (or `parent->field` when the parent is a dereference).
  Field,
  /// `*parent`: the object the pointer stored in `parent` refers to.
  Deref,
  /// `parent[*]`: the summary of every element of the array `parent`.
  Index,
};

/// Interns places, records their structure, and keeps user-facing names for
/// diagnostics.
///
/// Paths are interned: asking for the same child of the same parent twice
/// yields the same `PlaceId`, so identifiers stay small and dense and the
/// dataflow state can be keyed on them directly.
class PlaceTable {
public:
  /// Creates a new base place with the given display name (a variable name).
  [[nodiscard]] PlaceId create(std::string displayName);

  /// The field `fieldName` of `parent`.
  [[nodiscard]] PlaceId field(PlaceId parent, std::string_view fieldName);

  /// The object `parent` points to.
  [[nodiscard]] PlaceId deref(PlaceId parent);

  /// The element summary of the array `parent`. Indexing an index or a
  /// dereference collapses (`a[*][*]` is `a[*]`, `(*p)[*]` is `*p`), which is
  /// what makes the set of places finite for any given function.
  [[nodiscard]] PlaceId index(PlaceId parent);

  /// Display name for `id`, e.g. `p`, `p->next`, `a[*]`, `*p`.
  [[nodiscard]] std::string_view name(PlaceId id) const noexcept;

  /// The parent of `id`, or none for a base place.
  [[nodiscard]] std::optional<PlaceId> parent(PlaceId id) const noexcept;

  /// The step that produced `id` from its parent; meaningless for bases.
  [[nodiscard]] PathStep step(PlaceId id) const noexcept;

  /// The field name for a `PathStep::Field` place; empty otherwise.
  [[nodiscard]] std::string_view fieldName(PlaceId id) const noexcept;

  [[nodiscard]] bool isBase(PlaceId id) const noexcept {
    return !parent(id).has_value();
  }

  /// The base place at the top of `id`'s path.
  [[nodiscard]] PlaceId root(PlaceId id) const noexcept;

  /// Number of steps between `id` and its base (0 for a base place).
  [[nodiscard]] std::size_t depth(PlaceId id) const noexcept;

  /// True if `ancestor` is a proper prefix of `id`'s path.
  [[nodiscard]] bool isDescendantOf(PlaceId id,
                                    PlaceId ancestor) const noexcept;

  /// Every place strictly below `id`, in creation order.
  [[nodiscard]] std::vector<PlaceId> descendants(PlaceId id) const;

  /// Every proper prefix of `id`, nearest first.
  [[nodiscard]] std::vector<PlaceId> ancestors(PlaceId id) const;

  /// Rebuilds `id`'s path with the prefix `from` replaced by `to`, interning
  /// any places that do not exist yet. `id` must equal `from` or descend from
  /// it. Used to mirror facts between aliases: `p->f` under `p` becomes
  /// `q->f` under `q`.
  [[nodiscard]] PlaceId translate(PlaceId id, PlaceId from, PlaceId to);

  /// The nearest ancestor-or-self of `id` whose step is `Deref`, if any. The
  /// object holding `id` lives as long as whatever that pointer refers to.
  [[nodiscard]] std::optional<PlaceId>
  innermostDeref(PlaceId id) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

private:
  struct Entry {
    std::string name;
    std::optional<PlaceId> parent;
    PathStep step = PathStep::Field;
    std::string field;
    /// Direct children, in creation order.
    std::vector<PlaceId> children;
  };

  struct ChildKey {
    std::uint32_t parent;
    PathStep step;
    std::string field;

    friend std::strong_ordering operator<=>(const ChildKey &,
                                            const ChildKey &) = default;
  };

  std::vector<Entry> entries;
  std::map<ChildKey, PlaceId> children;

  [[nodiscard]] PlaceId intern(PlaceId parent, PathStep step,
                               std::string field);
};

} // namespace weavec::core

#endif // WEAVEC_CORE_PLACE_H
