//===- Concurrency.h - What entry points share (RFC 0030 §5.3) --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §5.3: a function a `LibrarySpec` `entry` clause hands to the
// library (`pthread_create`, `signal`, `sigaction`'s `act`) runs as an entry
// point, concurrently with or in between the unit's other code. G is the set
// of places such code may share: the pointer-carrying globals any function
// reachable from an entry target reads, writes or releases (the call graph,
// and for an indirect call every address-taken function of its type until
// the slots of §9.3 are solved), together with what the argument passed to
// the target reaches (its variable, and the target's parameters). `at-exit`
// targets run after the main flow and share nothing.
//
// A facet whose pointer is loaded from a place rooted in G is not proven
// from flow facts: its temporal facet is `trusted(concurrency)`, and a null
// or spatial facet that flow facts proved is checked (the ledger adapter
// applies both). The roots are found in the syntax of the site's operand, so
// a pointer copied out of G into a local first is not covered.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_CONCURRENCY_H
#define WEAVEC_ANALYSIS_CONCURRENCY_H

#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include "llvm/ADT/DenseSet.h"

namespace weavec::analysis {

/// The roots of G: variables (globals, entry parameters, entry arguments)
/// whose places a thread or signal handler may share.
struct ConcurrencyShare {
  llvm::DenseSet<const clang::VarDecl *> roots;
  /// Every function reachable from an entry target.
  llvm::DenseSet<const clang::FunctionDecl *> entryReachable;

  [[nodiscard]] bool empty() const noexcept { return roots.empty(); }
};

/// Computes G for the unit.
[[nodiscard]] ConcurrencyShare
collectConcurrencyShare(clang::ASTContext &context,
                        const core::LibrarySpec &library);

/// Whether the pointer `operand` is loaded from a place rooted in G: its
/// lvalue path (through members, subscripts and dereferences) starts at a
/// root variable.
[[nodiscard]] bool rootedInShare(const clang::Expr &operand,
                                 const ConcurrencyShare &share);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_CONCURRENCY_H
