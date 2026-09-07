//===- CallContextSummaries.cpp - Bounded caller-context inference --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "llvm/ADT/ScopeExit.h"

namespace weavec::analysis {

std::optional<ResolvedSummary> SummaryStore::specializeMemory(
    std::string_view symbol, const core::CallContext &bindings,
    const AnalysisOptions &options, core::DiagnosticSink *sink) {
  if (!bindings.valid())
    return std::nullopt;
  const MemoryContextKey key{std::string(symbol), bindings};
  auto &requests = memoryRequests[key.first];
  if (!requests.contains(bindings) &&
      requests.size() >= core::MaxMemoryContexts)
    return std::nullopt;
  requests.insert(bindings);
  const auto *function = callable(symbol);
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
    memorySpecialized[key] =
        database->importInto(*summary, *context, globalTable);
    return ResolvedSummary{.summary = &memorySpecialized.at(key),
                           .source = SummarySource::Program};
  }
  if (activeMemoryContexts.contains(key) ||
      activeMemoryContexts.size() + activeContexts.size() >=
          core::MaxCallContextDepth)
    return std::nullopt;
  if (!memorySpecialized.contains(key)) {
    activeMemoryContexts.insert(key);
    const auto release =
        llvm::scope_exit([&] { activeMemoryContexts.erase(key); });
    core::DiagnosticCollector collected;
    AnalysisOptions nestedOptions = options;
    nestedOptions.dumpStream = nullptr;
    FunctionDataflow analysis(definition->getASTContext(), *definition,
                              collected, nestedOptions, *this, true);
    analysis.memoryContext = bindings;
    analysis.callbackBindings = bindings.callbacks;
    analysis.run();
    if (!analysis.validMemoryContext)
      return std::nullopt;
    memorySpecialized[key] = analysis.summary();
    applyContract(*function, memorySpecialized[key]);
    memoryDiagnostics[key] = collected.diagnostics();
  }
  if (sink && bindings.reportDiagnostics)
    for (const auto &diagnostic : memoryDiagnostics[key])
      sink->report(diagnostic);
  return ResolvedSummary{.summary = &memorySpecialized.at(key),
                         .source = SummarySource::Inferred};
}

} // namespace weavec::analysis
