//===- DispatchEdges.h - Split critical edges into dispatches ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §9.2 (RFC 0030 G14). A computed-goto interpreter keeps its speed
// because LLVM's tail duplicator copies the block ending in the `indirectbr`
// into its predecessors, but only into predecessors with a single
// successor. An inserted check whose continuation `SimplifyCFG` folded away
// leaves a conditional branch straight into the dispatch, a critical edge
// the duplicator skips. `SplitDispatchEdges` splits every critical edge into
// a block that ends in an `indirectbr` and has more than
// `DispatchPredecessors` predecessors, which gives the duplicator its
// single-successor predecessors back. It is keyed on that shape alone.
//
// `weavec-cc` registers it through `CodeGenOptions::PassBuilderCallbacks` at
// `OptimizerLastEP`, and only for a unit whose checks are emitted, so an
// object built with `-fweavec-checks=none` is Clang's (gate G7).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DISPATCHEDGES_H
#define WEAVEC_FRONTEND_DISPATCHEDGES_H

#include "llvm/IR/PassManager.h"

namespace clang {
class CodeGenOptions;
} // namespace clang

namespace weavec::frontend {

/// A dispatch block has more predecessors than this.
inline constexpr unsigned DispatchPredecessors = 8;

/// The function pass of §9.2.
struct SplitDispatchEdges : llvm::PassInfoMixin<SplitDispatchEdges> {
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses);
};

/// Adds `SplitDispatchEdges` at the end of the optimisation pipeline of
/// every optimised build `options` configures.
void registerDispatchEdgeSplit(clang::CodeGenOptions &options);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DISPATCHEDGES_H
