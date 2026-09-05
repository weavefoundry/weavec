//===- Offset.h - Where a pointer points within its object -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0011, *Derived pointers*. A pointer value is a base object and an
// offset into it. `p + 4` is `p`'s object at `+4` elements; `&p->in` is it
// at the field `in`; `(char *)q - offsetof(struct outer, in)` composes the
// field back out again (`container_of`). The offset replaces the single
// *interior* bit earlier RFCs kept on alias edges, resource records and
// summary sources: it says not just that a pointer is inside its object but
// where, so that two derivations can cancel and a bounds check can tell how
// far from the start an access lands.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_OFFSET_H
#define WEAVEC_CORE_OFFSET_H

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace weavec::core {

struct PointerOffset {
  enum class Kind : std::uint8_t {
    /// The pointer points at the start of its object.
    Zero,
    /// A signed number of elements of the pointee type (`p + 4`, `p - 1`).
    Elements,
    /// A field path of the object's record type (`&p->in`, or its negation
    /// for `container_of`). The key is the canonical record spelling
    /// followed by the field path, as RFC 0010 spells count fields:
    /// `struct outer .in.buf`.
    Field,
    /// An unknown number of elements of the pointee type from the start
    /// (`p + n`, `&a[i]`, a join of element offsets): `*p` is still one
    /// element of the same array, so it stands for the pointee.
    Unknown,
    /// Somewhere inside the object at no known relation to the pointee
    /// (two field steps, a field and an element step, `(char *)p + k` for a
    /// non-`char` `p`, a join of a field offset with any other): the same
    /// object, but `*p` says nothing about what the pointer points at.
    Inside,
  };

  Kind kind = Kind::Zero;
  std::int64_t elements = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string field = {};
  /// `Field` only: the offset is subtracted rather than added.
  bool negative = false;

  [[nodiscard]] static PointerOffset zero() noexcept { return {}; }
  [[nodiscard]] static PointerOffset unknown() noexcept {
    return PointerOffset{
        .kind = Kind::Unknown, .elements = 0, .field = {}, .negative = false};
  }
  [[nodiscard]] static PointerOffset inside() noexcept {
    return PointerOffset{
        .kind = Kind::Inside, .elements = 0, .field = {}, .negative = false};
  }
  [[nodiscard]] static PointerOffset ofElements(std::int64_t count) noexcept {
    if (count == 0)
      return zero();
    return PointerOffset{.kind = Kind::Elements,
                         .elements = count,
                         .field = {},
                         .negative = false};
  }
  [[nodiscard]] static PointerOffset ofField(std::string key,
                                             bool negative = false) {
    return PointerOffset{.kind = Kind::Field,
                         .elements = 0,
                         .field = std::move(key),
                         .negative = negative};
  }

  [[nodiscard]] bool isZero() const noexcept { return kind == Kind::Zero; }
  [[nodiscard]] bool isUnknown() const noexcept {
    return kind == Kind::Unknown;
  }
  [[nodiscard]] bool isInside() const noexcept { return kind == Kind::Inside; }
  /// `Unknown` or `Inside`: the checker cannot say where the pointer points.
  [[nodiscard]] bool isIndefinite() const noexcept {
    return kind == Kind::Unknown || kind == Kind::Inside;
  }
  [[nodiscard]] bool isField() const noexcept { return kind == Kind::Field; }
  [[nodiscard]] bool isElements() const noexcept {
    return kind == Kind::Elements;
  }

  /// The offset of a pointer at `this` from the start, stepped by `other`:
  /// `Zero + x = x`, `Elements(a) + Elements(b) = Elements(a + b)`, element
  /// steps with `Unknown` stay `Unknown`, a field and its negation cancel,
  /// everything else is `Inside`.
  [[nodiscard]] PointerOffset plus(const PointerOffset &other) const;
  /// The offset that undoes this one.
  [[nodiscard]] PointerOffset negated() const;
  /// Two claims about one pointer: the same offset, `Unknown` when both are
  /// element counts, else `Inside`. Returns whether this changed.
  bool join(const PointerOffset &other);

  /// `0`, `+4`, `-2`, `+struct outer .in`, `-struct outer .in`, `?`, `~`.
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<PointerOffset>
  parse(std::string_view text);

  friend bool operator==(const PointerOffset &,
                         const PointerOffset &) = default;
  friend std::strong_ordering operator<=>(const PointerOffset &,
                                          const PointerOffset &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_OFFSET_H
