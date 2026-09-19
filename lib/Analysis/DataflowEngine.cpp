//===- DataflowEngine.cpp - SafetyEngine over FunctionDataflow ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/DataflowEngine.h"

#include "weavec/Analysis/TranslationUnitAnalysis.h"

#include <utility>

namespace weavec::analysis {

std::optional<core::Facet> facetOfDiagnostic(std::string_view id) {
  if (id == core::diag::UseAfterFree || id == core::diag::UseAfterMove ||
      id == core::diag::DoubleFree || id == core::diag::MismatchedRelease ||
      id == core::diag::ConflictingBorrow || id == core::diag::LifetimeTooShort)
    return core::Facet::Temporal;
  if (id == core::diag::NullDereference || id == core::diag::UseOfUninitialized)
    return core::Facet::Null;
  if (id == core::diag::OutOfBounds || id == core::diag::InvalidRelease ||
      id == core::diag::UnsafeOperation)
    return core::Facet::Spatial;
  if (id == core::diag::ContradictedAssumption)
    return core::Facet::Assertion;
  return std::nullopt;
}

DataflowEngine::DataflowEngine() = default;
DataflowEngine::~DataflowEngine() = default;

void DataflowEngine::analyzeUnit(const EngineInput &input, LedgerAdapter &out) {
  context = &input.context;
  analysisOptions = input.options.analysis;
  // Everything the engine publishes goes through `out` (§14): the
  // authoritative pass of each reported function opens with
  // `beginFunction`, and the fixpoint rounds publish into a discarding
  // adapter of the analyzer's own.
  analyzer = std::make_unique<TranslationUnitAnalyzer>(input.context, out,
                                                       analysisOptions);
  analyzer->setDatabase(input.database);
  if (input.options.dependencies != nullptr)
    analyzer->summaries().beginDependencies(*input.options.dependencies);
  if (input.options.shouldReport)
    analyzer->run(input.options.shouldReport);
  else
    analyzer->run();
  // The exports read summaries, which count as dependencies (RFC 0020).
  exported = analyzer->exports();
  if (input.options.dependencies != nullptr)
    analyzer->summaries().endDependencies();
}

UnitExports DataflowEngine::exports() {
  return std::move(exported);
}

void DataflowEngine::dump(const clang::FunctionDecl &function,
                          llvm::raw_ostream &os) {
  if (analyzer == nullptr || context == nullptr)
    return;
  LedgerAdapter ignored(*context, LedgerAdapter::Mode::Discarding);
  AnalysisOptions describe = analysisOptions;
  describe.dumpStream = &os;
  FunctionAnalyzer single(*context, ignored, describe);
  single.analyze(function, analyzer->summaries(), /*emitDiagnostics=*/true,
                 /*widenSummary=*/false);
}

UnitExports DataflowEngine::discover(clang::ASTContext &unitContext,
                                     const EngineOptions &options,
                                     const ProgramDatabase *database) {
  LedgerAdapter ignored(unitContext, LedgerAdapter::Mode::Discarding);
  TranslationUnitAnalyzer discovery(unitContext, ignored, options.analysis);
  discovery.setDatabase(database);
  if (options.dependencies != nullptr)
    discovery.summaries().beginDependencies(*options.dependencies);
  UnitExports result = discovery.discover();
  if (options.dependencies != nullptr)
    discovery.summaries().endDependencies();
  return result;
}

} // namespace weavec::analysis
