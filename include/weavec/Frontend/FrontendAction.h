//===- FrontendAction.h - Clang frontend integration -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Glue between Clang's frontend/tooling machinery and the WeaveC analyses.
// `WeaveCAction` can be run by `clang::tooling::ClangTool`, embedded in a
// custom driver (`weavec-cc` multiplexes `createWeaveCConsumer` with
// Clang's own consumer), or (eventually) loaded as a plugin.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_FRONTENDACTION_H
#define WEAVEC_FRONTEND_FRONTENDACTION_H

#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Frontend/DiagnosticControl.h"
#include "weavec/Frontend/LedgerOutput.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>

namespace weavec::analysis {
struct PlannedLedger;
} // namespace weavec::analysis

namespace weavec::frontend {

struct ZeroInitPlan;
namespace record {
struct InterfaceFacts;
} // namespace record

/// What one run of the consumer over a unit produced (RFC 0005).
struct UnitResult {
  analysis::UnitExports exports;
  /// The callee summaries the unit's analysis read (RFC 0020), so a cyclic
  /// component re-runs only the members an export change can affect.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> dependencies = {};
  /// The diagnostics shown for the unit in this run.
  std::set<ReportedDiagnostic> reported;
  std::size_t errors = 0;
  std::size_t warnings = 0;
  /// RFC 0030: the unit's ledger and check plan (§14); null for a silent or
  /// discovery run.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::shared_ptr<analysis::PlannedLedger> ledger = {};
  /// RFC 0030 §11: the unit's zero-initialisation plan, whose A5 counts the
  /// ledger holds; null when there is no ledger.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::shared_ptr<ZeroInitPlan> zeroInit = {};
  /// RFC 0030 §13.1: the unit's interface facts, under
  /// `FrontendOptions::collectInterface`; after a discovery run only the
  /// function-pointer slots.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::shared_ptr<const record::InterfaceFacts> interface = {};
};

struct FrontendOptions;
UnitResult analyzeTranslationUnit(clang::ASTContext &context,
                                  clang::DiagnosticsEngine &diagnostics,
                                  const FrontendOptions &options);
/// Runs against an ASTUnit with a fresh diagnostic consumer and counters.
UnitResult analyzeRetainedUnit(clang::ASTUnit &ast,
                               const FrontendOptions &options);

/// User-configurable behaviour of the frontend action.
struct FrontendOptions {
  analysis::AnalysisOptions analysis;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string analysisStatsPath = {};
  /// Only analyse declarations in the main file (not in included headers).
  /// RFC 0030 §5.6 removed `--analyze-headers`, the flag that cleared it.
  bool mainFileOnly = true;
  /// `-W` overrides applied before a diagnostic reaches Clang.
  DiagnosticControl control;
  // RFC 0030 (S3-C, begin): the configuration the ledger records and the
  // analysis plans with (`-fweavec-checks`, `-f[no-]weavec-zero-init`,
  // `-fweavec-require`, `-fweavec-budget`; `--require`, `--budget` and
  // `--no-zero-init` in `weavec`), and where the unit ledger and the
  // summary line go (`emitUnitLedger` in LedgerOutput.h).
  core::LedgerConfig config;
  LedgerOutputOptions ledgerOutput;
  // RFC 0030 (S3-C, end).

  // RFC 0005, whole-program analysis. Every pointer must outlive the run.

  /// Exports of the other units of the program; null when the unit is the
  /// whole program.
  const analysis::ProgramDatabase *database = nullptr;
  /// Diagnostics not to show again (already printed by an earlier step).
  const std::set<ReportedDiagnostic> *alreadyReported = nullptr;
  /// When set, only diagnostics with these ids are shown (RFC 0012: the
  /// pass that re-analyses a unit for a sized field the program confirmed
  /// shows only what the field's extent can change).
  const std::set<std::string_view> *onlyIds = nullptr;
  /// Messages of `annotation-required` reports already made for the
  /// program, so each boundary is reported once per program. Updated.
  std::set<std::string> *boundaryOnce = nullptr;
  /// Analyse but report nothing (a fixpoint round).
  bool silent = false;
  /// Collect the unit's definitions, imports and indirect types without
  /// analysing anything; `onResult` receives exports with empty summaries.
  bool discoverOnly = false;
  /// RFC 0030 §13.1: also collect the unit's interface facts for its record
  /// (`UnitResult::interface`).
  bool collectInterface = false;
  /// Receives the unit's exports and reporting statistics.
  std::function<void(UnitResult)> onResult;
};

/// The AST consumer that runs WeaveC over a translation unit, for embedding
/// in another action (`weavec-cc` runs it beside Clang's code generator).
std::unique_ptr<clang::ASTConsumer>
createWeaveCConsumer(clang::CompilerInstance &compiler,
                     const FrontendOptions &options);

/// Runs WeaveC's analyses over each translation unit.
class WeaveCAction final : public clang::ASTFrontendAction {
public:
  explicit WeaveCAction(FrontendOptions opts) : options(std::move(opts)) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override;

private:
  FrontendOptions options;
};

/// Creates a factory usable with `clang::tooling::ClangTool::run`.
std::unique_ptr<clang::tooling::FrontendActionFactory>
createWeaveCActionFactory(FrontendOptions options);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_FRONTENDACTION_H
