//===- AnalysisCache.h - Settled checkpoints (RFC 0020) --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_FRONTEND_ANALYSISCACHE_H
#define WEAVEC_FRONTEND_ANALYSISCACHE_H

#include "weavec/Frontend/FrontendAction.h"

namespace weavec::frontend {

/// Preprocesses the effective invocation and fingerprints its expanded output,
/// loaded input bytes and command. Unsupported external AST inputs yield no
/// key.
bool supportsInputIdentity(const clang::CompilerInvocation &invocation);
std::unique_ptr<clang::tooling::FrontendActionFactory>
createInputIdentityFactory(std::string &identity);
std::string analysisOptionsIdentity(const FrontendOptions &options);

struct AnalysisCheckpoint {
  std::vector<UnitResult> units;
  std::vector<std::set<analysis::SizedFieldWitness>> sizedPairsSeen;
  std::string importedIdentity;
};

/// Lossless explanation identity for imported facts (private format 2).
std::string checkpointExportsIdentity(const analysis::UnitExports &exports);

/// A component is published only after all its units have settled. No active
/// approximation is exposed as a reusable proof. Bounds and a content digest
/// protect reads; failed I/O simply loses the optimization.
std::optional<AnalysisCheckpoint>
readAnalysisCheckpoint(std::string_view directory, std::string_view key,
                       core::AnalysisStats *stats);
bool writeAnalysisCheckpoint(std::string_view directory, std::string_view key,
                             const AnalysisCheckpoint &checkpoint,
                             core::AnalysisStats *stats);

} // namespace weavec::frontend
#endif // WEAVEC_FRONTEND_ANALYSISCACHE_H
