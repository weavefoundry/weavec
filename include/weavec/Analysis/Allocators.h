//===- Allocators.h - Ownership effects of a call --------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Classifies calls by their ownership effect (RFC 0002, *Events*; RFC 0003,
// *Applying a summary at a call*). The effects come from the callee's
// summary, which `SummaryStore` resolves from annotations, the body in this
// translation unit, or the shipped C library table.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_ALLOCATORS_H
#define WEAVEC_ANALYSIS_ALLOCATORS_H

#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/Borrow.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// What a call does to the ownership of its pointer arguments and result.
struct CallEffects {
  /// The callee's summary; never null. Sub-path effects, stores and return
  /// alternatives are read from here.
  const core::FunctionSummary *summary = nullptr;
  /// Where the summary came from.
  SummarySource source = SummarySource::Inferred;
  /// The call may return a fresh owned allocation.
  bool producesOwned = false;
  /// Arguments whose ownership the callee takes (released or moved).
  std::vector<unsigned> consumedArgs;
  /// Arguments borrowed for the duration of the call, with the kind of
  /// borrow. Consumed arguments are not listed.
  std::vector<std::pair<unsigned, core::BorrowKind>> borrowedArgs;

  [[nodiscard]] bool consumes(unsigned arg) const noexcept;
  /// True if the consumed argument is *released* rather than moved to
  /// another owner; decides between `use-after-free` and `use-after-move`.
  [[nodiscard]] bool frees(unsigned arg) const noexcept;
};

/// Returns the ownership effects of `call`, or `std::nullopt` for calls with
/// no known effect: callees `summaries` cannot resolve, directly or (RFC
/// 0004) through a function pointer.
[[nodiscard]] std::optional<CallEffects>
classifyCall(const clang::CallExpr &call, SummaryStore &summaries);

/// True if `function` is a C library function that returns a fresh
/// allocation (`malloc`, `calloc`, `realloc`, `strdup`, `fopen`, ...).
[[nodiscard]] bool isKnownAllocator(const clang::FunctionDecl &function);

/// True if `function` is a C library function that releases its first
/// argument (`free`, `fclose`).
[[nodiscard]] bool isKnownReleaser(const clang::FunctionDecl &function);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_ALLOCATORS_H
