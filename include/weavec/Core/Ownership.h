//===- Ownership.h - Ownership lattice --------------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines the ownership kinds WeaveC assigns to pointer-typed values and the
// lattice operations used by inference. This header is deliberately free of
// any Clang/LLVM dependency so the model can be reused outside the frontend.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_OWNERSHIP_H
#define WEAVEC_CORE_OWNERSHIP_H

#include <cstdint>
#include <string_view>

namespace weavec::core {

/// The ownership discipline associated with a pointer-typed value.
///
/// The kinds form a lattice used during inference:
///
///                Raw            (top: no guarantees, requires `unsafe`)
///             /   |   \
///        Owned  Shared  Mutable
///             \   |   /
///               Unknown         (bottom: not yet inferred)
///
/// `join` moves up the lattice; two conflicting concrete kinds join to `Raw`.
/// The checker does not act on the lattice value: whether a *place* holds a
/// raw pointer is a separate fact (`RawTracker`, RFC 0004), and a
/// contradictory join is a precision loss visible in `--dump-analysis`.
enum class OwnershipKind : std::uint8_t {
  /// No information yet; the bottom element.
  Unknown,
  /// Unique ownership. The holder is responsible for releasing the resource
  /// exactly once, and may move it to another owner.
  Owned,
  /// Shared (read-only) borrow. Any number may coexist; none may mutate.
  Shared,
  /// Exclusive (mutable) borrow. Excludes all other borrows of the place.
  Mutable,
  /// No ownership guarantee (RFC 0004): cast from an integer, declared
  /// `WEAVEC_RAW`, or handed out by unchecked code. Also the top element,
  /// reached by joining contradictory facts.
  Raw,
};

/// Least upper bound of two ownership kinds.
[[nodiscard]] constexpr OwnershipKind join(OwnershipKind lhs,
                                           OwnershipKind rhs) noexcept {
  if (lhs == rhs)
    return lhs;
  if (lhs == OwnershipKind::Unknown)
    return rhs;
  if (rhs == OwnershipKind::Unknown)
    return lhs;
  return OwnershipKind::Raw;
}

/// Returns true if `kind` denotes a borrow (shared or mutable).
[[nodiscard]] constexpr bool isBorrow(OwnershipKind kind) noexcept {
  return kind == OwnershipKind::Shared || kind == OwnershipKind::Mutable;
}

/// Returns true if `kind` is a fully-inferred, safe discipline.
[[nodiscard]] constexpr bool isSafe(OwnershipKind kind) noexcept {
  return kind == OwnershipKind::Owned || isBorrow(kind);
}

/// Human-readable, stable spelling used in diagnostics and dumps.
[[nodiscard]] std::string_view toString(OwnershipKind kind) noexcept;

} // namespace weavec::core

#endif // WEAVEC_CORE_OWNERSHIP_H
