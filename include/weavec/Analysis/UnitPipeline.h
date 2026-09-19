//===- UnitPipeline.h - One unit's analysis (RFC 0030) ----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §1, steps 2 and 3, for one translation unit:
//
//   - `AttributeReader` reads the declared kinds (§7.2);
//   - `SiteCollector` enumerates the sites of every emitted function (§2.6);
//   - the engine (`DataflowEngine`) runs through `LedgerAdapter`;
//   - `LedgerAdapter::finish` fills the defaults, applies the ledger-side
//     rules and plans the checks;
//   - the diagnostics are reported to the caller's sink in the order the
//     engine produced them, followed by the require-level errors.
//
// `weavec` and `weavec-cc` reach this through the Frontend's
// `analyzeTranslationUnit` (lib/Frontend/FrontendAction.cpp); the result's
// `ledger` is what they write with `-fweavec-ledger` and summarise.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_UNITPIPELINE_H
#define WEAVEC_ANALYSIS_UNITPIPELINE_H

#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/SafetyEngine.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"

#include <functional>
#include <memory>

namespace weavec::analysis {

struct UnitPipelineOptions {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  EngineOptions engine = {};
  /// Other units' exports (RFC 0005); null when the unit is the program.
  const ProgramDatabase *database = nullptr;
  /// Only the RFC 0005 discovery pass: no analysis, no ledger.
  bool discoverOnly = false;
  /// Build the ledger. A silent round of the whole-program fixpoint needs
  /// only the engine's exports.
  bool buildLedger = true;
  /// §12.1 `config`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::LedgerConfig config = {};
  /// The table to use; null for `LibrarySpec::shipped()`.
  const core::LibrarySpec *library = nullptr;
  /// §3.4: whether a facet's violation was lowered to a warning.
  std::function<bool(core::SiteId, core::Facet)> lowered = nullptr;
};

struct UnitPipelineResult {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  UnitExports exports = {};
  /// The unit's ledger and check plan; null after a discovery pass.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::shared_ptr<PlannedLedger> ledger = {};
};

/// Analyses and plans one unit, reporting its diagnostics to `out`.
[[nodiscard]] UnitPipelineResult
runUnitAnalysis(clang::ASTContext &context, const UnitPipelineOptions &options,
                core::DiagnosticSink &out);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_UNITPIPELINE_H
