//===- BypassedDeclarations.h - Jumps past a declaration -------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §11: `-ftrivial-auto-var-init=zero` initialises a local where its
// declaration runs. A `goto`, a computed `goto` or a `switch` that jumps past
// the declaration skips that initialisation too, so such a local holds
// whatever the stack held, which no null check can catch. Both readers of
// this fact share one answer: the A5 measurement counts the declarations
// (`lib/Frontend/ZeroInit.cpp`), and the engine gives a use of one the
// `no-zero-init` reason instead of a null check (§2.3).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_BYPASSEDDECLARATIONS_H
#define WEAVEC_ANALYSIS_BYPASSEDDECLARATIONS_H

#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"

#include <vector>

namespace weavec::analysis {

/// The locals declared in `body` that may hold a pointer and whose
/// declaration a jump within `body` can bypass, in the order the body
/// declares them. A jump from J to label L bypasses the declaration D when
/// L follows D within D's parent and J is not in that part of the tree.
[[nodiscard]] std::vector<const clang::VarDecl *>
bypassedDeclarations(const clang::Stmt &body);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_BYPASSEDDECLARATIONS_H
