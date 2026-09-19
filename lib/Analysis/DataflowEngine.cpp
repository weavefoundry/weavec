//===- DataflowEngine.cpp - SafetyEngine over FunctionDataflow ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/DataflowEngine.h"

#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/TranslationUnitAnalysis.h"

#include "clang/Basic/SourceManager.h"

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
  if (id == rfc0030::ContradictedAssumption)
    return core::Facet::Assertion;
  return std::nullopt;
}

/// Forwards the engine's diagnostics to the adapter, linked to the site at
/// their location.
class DataflowEngine::AdapterSink final : public core::DiagnosticSink {
public:
  AdapterSink(const clang::SourceManager &sourceManager,
              const SiteIndex &siteIndex, LedgerAdapter &ledgerAdapter)
      : sm(sourceManager), sites(siteIndex), adapter(ledgerAdapter) {}

  void report(const core::Diagnostic &diagnostic) override {
    // A placeholder until the engine publishes certainty (S3-B, §3).
    const core::Certainty certainty =
        diagnostic.severity == core::Severity::Error
            ? core::Certainty::Definite
            : core::Certainty::Possible;
    const std::optional<core::Facet> facet = facetOfDiagnostic(diagnostic.id);
    const clang::Stmt *site = nullptr;
    const clang::SourceLocation at = toClangLocation(diagnostic.location);
    if (facet && at.isValid())
      if (const auto id = sites.innermostAt(at, facet, sm))
        site = sites.info(*id)->stmt;
    adapter.report(diagnostic, certainty, site,
                   site != nullptr ? facet : std::nullopt);
  }

private:
  const clang::SourceManager &sm;
  const SiteIndex &sites;
  LedgerAdapter &adapter;
};

DataflowEngine::DataflowEngine() = default;
DataflowEngine::~DataflowEngine() = default;

void DataflowEngine::analyzeUnit(const EngineInput &input, LedgerAdapter &out) {
  context = &input.context;
  analysisOptions = input.options.analysis;
  sink = std::make_unique<AdapterSink>(input.context.getSourceManager(),
                                       input.sites, out);
  analyzer = std::make_unique<TranslationUnitAnalyzer>(input.context, *sink,
                                                       analysisOptions);
  analyzer->setDatabase(input.database);
  analyzer->setReportingObserver([&out](const clang::FunctionDecl &function) {
    out.beginFunction(function);
  });
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
  analyzer->setReportingObserver(nullptr);
}

UnitExports DataflowEngine::exports() {
  return std::move(exported);
}

void DataflowEngine::dump(const clang::FunctionDecl &function,
                          llvm::raw_ostream &os) {
  if (analyzer == nullptr || context == nullptr)
    return;
  core::DiagnosticCollector ignored;
  AnalysisOptions describe = analysisOptions;
  describe.dumpStream = &os;
  FunctionAnalyzer single(*context, ignored, describe);
  single.analyze(function, analyzer->summaries(), /*emitDiagnostics=*/true,
                 /*widenSummary=*/false);
}

UnitExports DataflowEngine::discover(clang::ASTContext &unitContext,
                                     const EngineOptions &options,
                                     const ProgramDatabase *database) {
  core::DiagnosticCollector ignored;
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
