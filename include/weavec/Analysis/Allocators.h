//===- Allocators.h - Recognised allocation and release functions -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Classifies calls by their ownership effect (RFC 0002, *Events*): libc
// allocators by name, plus any function whose declaration carries WeaveC
// ownership annotations on its return type or parameters.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_ALLOCATORS_H
#define WEAVEC_ANALYSIS_ALLOCATORS_H

#include "weavec/Core/Borrow.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace weavec::analysis {

/// What a call does to the ownership of its pointer arguments and result.
struct CallEffects {
  /// The call returns a fresh owned allocation.
  bool producesOwned = false;
  /// The call is `realloc`-shaped: consumes argument 0 and produces a fresh
  /// allocation that must be null-tested before the argument is dead for
  /// certain.
  bool isRealloc = false;
  /// Arguments whose ownership the callee takes; `free`'s argument, and every
  /// parameter annotated `WEAVEC_OWNED`.
  std::vector<unsigned> consumedArgs;
  /// Arguments borrowed for the duration of the call, with the kind of
  /// borrow (`WEAVEC_BORROWED` -> shared, `WEAVEC_MUT` -> mutable).
  std::vector<std::pair<unsigned, core::BorrowKind>> borrowedArgs;
  /// True if the consumed arguments are released (`free`) rather than moved
  /// to another owner; decides between `use-after-free` and
  /// `use-after-move` later on.
  bool releasesArgs = false;

  [[nodiscard]] bool consumes(unsigned arg) const noexcept;
};

/// Returns the ownership effects of `call`, or `std::nullopt` for calls with
/// no recognised effect (unannotated callees, indirect calls).
[[nodiscard]] std::optional<CallEffects>
classifyCall(const clang::CallExpr &call);

/// True if `function` is one of the libc allocation functions recognised by
/// name (`malloc`, `calloc`, `realloc`, `strdup`, `strndup`,
/// `aligned_alloc`).
[[nodiscard]] bool isKnownAllocator(const clang::FunctionDecl &function);

/// True if `function` is `free`.
[[nodiscard]] bool isKnownReleaser(const clang::FunctionDecl &function);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_ALLOCATORS_H
