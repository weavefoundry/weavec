//===- GuardPasses.h - Guards the backend lowers ----------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0034 §1. The prelude declares the `object` and `live` guards
// (`__weavec_chk_object`, `__weavec_chk_live`, their `_report` forms and
// verify mode's `__weavec_prv_*`), and three passes turn each call of one
// into code:
//
//   GuardCanonicalize  (PipelineStartEP) the helper's result becomes the
//                      address arithmetic it did (a `getelementptr` of the
//                      operand, which alias analysis sees through), and the
//                      check becomes a call of the runtime's slow path,
//                      `__weavec_rt_guard(from, at, width, kind, site)`,
//                      declared `nounwind memory(inaccessiblemem: read)` and
//                      not `willreturn`: the optimiser may delete a copy a
//                      dominating one makes redundant and hoist one out of a
//                      loop it always runs in, and never moves one above a
//                      condition or an access above one.
//   GuardMerge         (OptimizerLastEP) removes a guard whose bytes a
//                      dominating guard of the same pointer checked with no
//                      call between them that may release or register an
//                      object, and merges the guards of one block on one
//                      pointer into the guard of their hull.
//   GuardExpand        (OptimizerLastEP) puts the inline check of RFC 0034
//                      §2.3 in front of each slow-path call: an arena
//                      address whose shadow byte says its bytes are a live
//                      object's passes without the call.
//
// The runtime defines every declared guard and the slow path, so a call the
// passes did not rewrite (an object built without them) is checked out of
// line. `weavec-cc` registers the passes for every unit whose checks it
// emits, at every optimisation level.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_GUARDPASSES_H
#define WEAVEC_FRONTEND_GUARDPASSES_H

#include "llvm/IR/PassManager.h"

namespace clang {
class CodeGenOptions;
} // namespace clang

namespace weavec::frontend {

/// The runtime's slow path, which the canonical guards call.
inline constexpr const char *GuardSlowPath = "__weavec_rt_guard";

/// RFC 0034 §4: a registered global owns the granules it starts in.
struct GlobalPadding : llvm::PassInfoMixin<GlobalPadding> {
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Module &module,
                              llvm::ModuleAnalysisManager &analyses);
};

/// RFC 0034 §1.2.
struct GuardCanonicalize : llvm::PassInfoMixin<GuardCanonicalize> {
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses);
};

/// RFC 0034 §1.3.
struct GuardMerge : llvm::PassInfoMixin<GuardMerge> {
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses);
};

/// RFC 0034 §1.4.
struct GuardExpand : llvm::PassInfoMixin<GuardExpand> {
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses);
};

/// Adds the passes to every pipeline `options` configures.
void registerGuardPasses(clang::CodeGenOptions &options);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_GUARDPASSES_H
