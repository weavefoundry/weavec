//===- GuardPass.h - Every memory access guarded ----------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035. `weavec-cc` adds four passes to the pipeline of every C unit it
// enforces (GuardLoop when optimising, the others at every level):
//
//   GuardInsert  (PipelineStartEP) before every access (load, store, atomic
//                operation, memory intrinsic, masked or gathered lane) and
//                every memory argument of a C library call, a guard: a call
//                of `__weavec.guard`, `__weavec.range` or `__weavec.strlen`
//                that only reads inaccessible memory and is not `willreturn`,
//                so the optimiser keeps it, merges and hoists it as it does a
//                load, and never assumes the access after it executes. An
//                access inside a stack or global object at a constant
//                offset gets none, so that the object can still be promoted
//                to registers. It also redirects the calls only the runtime
//                can check (section 2.5).
//   GuardPrune   (PeepholeEP) removes the guards inlining made of the same
//                kind, before the scalar replacement of aggregates runs again.
//   GuardLoop    (VectorizerStartEP) versions the loops whose guarded
//                accesses one range each covers (section 6.4): the copy
//                without those guards runs when the ranges are addressable.
//   GuardExpand  (OptimizerLastEP) removes the guards a local rule proves
//                redundant (section 6), lays out the stack objects a guard can
//                reach in frames with redzones (section 3), expands each
//                remaining guard into an inline check of the runtime's shadow
//                memory that calls the slow path when a shadow byte is not 0
//                (section 2.3), clears the stack's shadow before calls that
//                do not return, and gives the unit's globals redzones
//                (section 4).
//
// Guards are inserted first and expanded last because the optimiser
// exploits undefined behaviour: an out-of-bounds read of a heap object of
// known size may make a loop infinite, and a read after `free` may become
// `undef`, before any pass at the end of the pipeline sees the access.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_GUARDPASS_H
#define WEAVEC_FRONTEND_GUARDPASS_H

#include "weavec/Frontend/EnforcementLedger.h"

#include "llvm/IR/PassManager.h"

#include <cstdint>
#include <memory>

namespace clang {
class CodeGenOptions;
} // namespace clang

namespace weavec::frontend {

class UnsafeRegions;

struct GuardOptions {
  /// `-fweavec-checks=`: a failed guard traps, reports and continues, or
  /// traps and every removed guard is emitted as a monitor.
  enum class Mode : std::uint8_t { Trap, Report, Verify };
  Mode mode = Mode::Trap;
  /// The unsafe regions of the unit (section 2.6); null for none.
  std::shared_ptr<const UnsafeRegions> unsafe;
  /// Where the rows go; null to collect nothing.
  std::shared_ptr<EnforcementLedger> ledger;
};

/// RFC 0035 §2.1, §2.5: guards before the optimiser.
class GuardInsertPass : public llvm::PassInfoMixin<GuardInsertPass> {
public:
  explicit GuardInsertPass(GuardOptions options)
      : options(std::move(options)) {}
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Module &module,
                              llvm::ModuleAnalysisManager &analyses);
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  static bool isRequired() { return true; }

private:
  GuardOptions options;
};

/// RFC 0035 §6.1 after inlining.
class GuardPrunePass : public llvm::PassInfoMixin<GuardPrunePass> {
public:
  explicit GuardPrunePass(GuardOptions options) : options(std::move(options)) {}
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses);

private:
  GuardOptions options;
};

/// RFC 0035 §6.4: loop ranges, before the vectoriser.
class GuardLoopPass : public llvm::PassInfoMixin<GuardLoopPass> {
public:
  explicit GuardLoopPass(GuardOptions options) : options(std::move(options)) {}
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses);

private:
  GuardOptions options;
};

/// RFC 0035 §2.3, §3, §4, §6: guards after the optimiser.
class GuardExpandPass : public llvm::PassInfoMixin<GuardExpandPass> {
public:
  explicit GuardExpandPass(GuardOptions options)
      : options(std::move(options)) {}
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  llvm::PreservedAnalyses run(llvm::Module &module,
                              llvm::ModuleAnalysisManager &analyses);
  // NOLINTNEXTLINE(readability-identifier-naming): the pass manager's name
  static bool isRequired() { return true; }

private:
  GuardOptions options;
};

/// Adds the passes to every pipeline `codegen` configures.
void registerGuardPasses(clang::CodeGenOptions &codegen,
                         const GuardOptions &options);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_GUARDPASS_H
