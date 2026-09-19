//===- CallContextSummaries.cpp - Bounded caller-context inference --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"

#include <algorithm>

namespace weavec::analysis {

std::optional<ResolvedSummary>
SummaryStore::specializeMemory(std::string_view symbol,
                               const core::CallContext &bindings,
                               const AnalysisOptions &options,
                               std::vector<core::Diagnostic> *diagnostics) {
  const auto *function = callable(symbol);
  if (!bindings.valid())
    return std::nullopt;
  const MemoryContextKey key{std::string(symbol), bindings};
  noteDependency(symbol);
  auto &requests = memoryRequests[key.first];
  if (!requests.contains(bindings) &&
      requests.size() >= core::MaxMemoryContexts)
    return std::nullopt;
  requests.insert(bindings);
  const auto *definition = function ? function->getDefinition() : nullptr;
  if (!context)
    return std::nullopt;
  if (!definition) {
    if (!database)
      return std::nullopt;
    const auto exported = database->exportContext(bindings, globalTable);
    if (!exported)
      return std::nullopt;
    const auto *summary = database->findMemorySpecialization(symbol, *exported);
    if (!summary)
      return std::nullopt;
    return ResolvedSummary{.summary = importSummary(*summary),
                           .source = SummarySource::Program};
  }
  if (activeMemoryContexts.contains(key) ||
      activeMemoryContexts.size() + activeContexts.size() >=
          core::MaxCallContextDepth)
    return std::nullopt;
  discardStaleContexts();
  if (!memorySpecialized.contains(key) || !memorySpecialized.at(key)) {
    if (options.stats)
      options.stats->add("specialization_misses");
    std::optional<core::AnalysisTimer> invocationTimer;
    if (options.stats)
      invocationTimer.emplace(options.stats, "memory:" + std::string(symbol));
    Dependencies dependencies{std::string(symbol)};
    beginDependencies(dependencies);
    const auto finishDependencies =
        llvm::scope_exit([&] { endDependencies(); });
    activeMemoryContexts.insert(key);
    const auto release =
        llvm::scope_exit([&] { activeMemoryContexts.erase(key); });
    // RFC 0030 §5.5: the context runs of one function share a budget.
    const auto budget = contextBudget(*definition, options);
    if (!budget)
      return std::nullopt;
    LedgerAdapter collected(definition->getASTContext(),
                            LedgerAdapter::Mode::Collecting);
    AnalysisOptions nestedOptions = options;
    nestedOptions.dumpStream = nullptr;
    nestedOptions.budget = *budget;
    FunctionDataflow analysis(definition->getASTContext(), *definition,
                              collected, nestedOptions, *this, true);
    analysis.memoryContext = bindings;
    analysis.callbackBindings = bindings.callbacks;
    analysis.run();
    contextTransfers[definition->getCanonicalDecl()] += analysis.transfers();
    if (!analysis.validMemoryContext || analysis.overBudget())
      return std::nullopt;
    auto summary = std::move(analysis).summary();
    applyContract(*function, summary);
    memorySpecialized[key] = publishSummary(std::move(summary));
    memoryDiagnostics[key] = collected.diagnostics();
    memoryDependencies[key] = std::move(dependencies);
    auto &snapshot = memoryVersions[key];
    snapshot = dependencySnapshot();
    contextsNeedValidation |= !dependenciesCurrent(snapshot);
  } else {
    if (options.stats)
      options.stats->add("specialization_hits");
    inheritDependencies(memoryDependencies[key]);
  }
  if (diagnostics != nullptr && bindings.reportDiagnostics)
    llvm::append_range(*diagnostics, memoryDiagnostics[key]);
  return ResolvedSummary{.summary = memorySpecialized.at(key),
                         .source = SummarySource::Inferred};
}

} // namespace weavec::analysis
