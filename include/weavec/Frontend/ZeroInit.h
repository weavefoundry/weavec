//===- ZeroInit.h - Zero-initialisation plan (RFC 0030) ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §11. In the enforcing modes `weavec-cc` compiles with
// `-ftrivial-auto-var-init=zero`, which covers locals, and lowers every
// reference to a function whose `LibrarySpec` row has the `zero-init` flag
// so that no byte of a lowered block's usable region is left uninitialised:
//
//   malloc(n)          -> __weavec_malloc_zero(n)         (same signature;
//   calloc, realloc,      likewise __weavec_calloc_zero, _realloc_zero,
//   reallocarray,         _reallocarray_zero, _strdup_zero, _strndup_zero)
//   strdup, strndup
//   aligned_alloc(a, n)-> __weavec_zero_tail(aligned_alloc(a, n), 0)
//                         (every other allocator of the heap family)
//   <string row>(...)  -> __weavec_zero_string(<call>)    (zero after the
//                                                          terminator)
//   getline(&l, ...)   -> __weavec_zero_line(getline(&l, ...), &l)
//   posix_memalign(..) -> __weavec_posix_memalign_zero(posix_memalign, ...)
//   alloca(n)          -> __builtin_memset(alloca(n), 0, n)
//   p = malloc         -> p = __weavec_malloc_zero_fn     (a static,
//                                                          non-inline wrapper)
//
// The plan is pure: it reads the AST and decides which references are
// lowered, so the ledger's A5 counts (`summary.a5`: allocation calls that
// are not lowered, and pointer locals whose declaration a jump can bypass)
// are known before anything is rewritten, and are the same whether or not
// checks are emitted. `CheckEmitter::lowerZeroInit` applies it.
//
// Nothing is lowered in a unit that defines an allocator or a releaser of
// the heap family (`malloc`, `free`, or any `zero-init` row), so that an
// allocator is never wrapped into recursion; nor, for the heap family, on a
// target without a usable-size query (the driver warns once). Rows the
// table marks `no-zero-init`, wide-string rows, calls whose arguments would
// have to be evaluated twice and have side effects, and references whose
// declaration does not match the wrapper's type are left alone and counted.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_ZEROINIT_H
#define WEAVEC_FRONTEND_ZEROINIT_H

#include "weavec/Core/Ledger.h"
#include "weavec/Core/LibrarySpec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class CallExpr;
class Decl;
class DeclRefExpr;
class Expr;
class FunctionDecl;
} // namespace clang

namespace weavec::frontend {

struct ZeroInitOptions {
  /// Lower the heap family: the target has a usable-size query.
  bool heap = true;
  /// Lower `alloca`.
  bool stack = true;
};

/// One lowered reference.
struct ZeroInitRewrite {
  enum class Kind : std::uint8_t {
    /// The callee becomes `helper`, which has the callee's own signature.
    SwapCallee,
    /// `call` becomes `(T)helper(call)` (`__weavec_zero_string`) or
    /// `(T)helper(call, 0)` (`__weavec_zero_tail`).
    WrapResult,
    /// `call` becomes `(T)__weavec_zero_line(call, <argument>)`; the
    /// argument is rebuilt (it is `&object` or an unmodified variable).
    ZeroLine,
    /// `call` becomes `helper(<callee>, <arguments>...)`.
    PassCallee,
    /// `call` becomes `__builtin_memset(call, 0, <argument>)`; the size is
    /// rebuilt (it is side-effect free).
    Alloca,
    /// A reference that is not a callee becomes one to `helper`, a static
    /// non-inline wrapper with the same signature.
    AddressOf,
  };

  Kind kind = Kind::SwapCallee;
  /// The reference to the table's function.
  const clang::DeclRefExpr *reference = nullptr;
  /// The call, for every kind but `AddressOf`.
  const clang::CallExpr *call = nullptr;
  /// The prelude function.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string helper = {};
  /// `ZeroLine`: the slot argument; `Alloca`: the size argument.
  unsigned argument = 0;
  /// That argument as planned, which the lowering rebuilds: a check may
  /// have wrapped the argument by then.
  const clang::Expr *operand = nullptr;
  /// The function, block or variable whose body or initialiser holds it.
  const clang::Decl *owner = nullptr;
};

struct ZeroInitPlan {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<ZeroInitRewrite> rewrites = {};
  /// §12.1 `summary.a5`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::A5Counts a5 = {};
  /// A definition that makes the unit an allocator; nothing is then
  /// lowered.
  const clang::FunctionDecl *allocator = nullptr;
};

/// Plans the §11 lowering of the unit. With `enabled` false (the checks
/// are `none`, or `-fno-weavec-zero-init`) nothing is lowered and only the
/// A5 counts are computed.
[[nodiscard]] ZeroInitPlan planZeroInit(clang::ASTContext &context,
                                        const core::LibrarySpec &library,
                                        bool enabled,
                                        const ZeroInitOptions &options = {});

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_ZEROINIT_H
