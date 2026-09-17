//===- CallContextSummaries.cpp - Bounded caller-context inference --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>

namespace weavec::analysis {

std::optional<ResolvedSummary> SummaryStore::specializeMemory(
    std::string_view symbol, const core::CallContext &bindings,
    const AnalysisOptions &options, core::DiagnosticSink *sink) {
  const bool checkedCase =
      options.stats != nullptr && options.checkContracts &&
      (!bindings.facts.empty() || !bindings.nonNan.empty());
  if (checkedCase)
    options.stats->add("checked_case_requests");
  const auto decline = [&]() -> std::optional<ResolvedSummary> {
    if (checkedCase)
      options.stats->add("checked_case_declines");
    return std::nullopt;
  };
  const auto *function = callable(symbol);
  // Private induction premises belong to the enclosing proof transaction.
  // A nested context must not publish a member before that group verifies.
  if (function != nullptr &&
      activeRecursiveContracts.members.contains(function->getCanonicalDecl()))
    return decline();
  if (!bindings.valid())
    return decline();
  if (options.checkContracts && checkingRecursiveApproximation &&
      activeMemoryContexts.empty() && bindings.aliases.empty() &&
      bindings.orders.empty()) {
    if (options.stats)
      options.stats->add("recursive_provisional_context_declines");
    return decline();
  }
  if (options.checkContracts && function &&
      recursiveFunctions.contains(function->getCanonicalDecl()) &&
      !verifiedRecursiveContracts.contains(function->getCanonicalDecl()) &&
      bindings.aliases.empty() && bindings.orders.empty()) {
    auto base = lookup(*function);
    if (base) {
      std::set<unsigned> readOnlyRecords;
      for (unsigned i = 0; i < function->getNumParams(); ++i) {
        const auto type = function->getParamDecl(i)->getType();
        if (!type->isPointerType() || !type->getPointeeType()->isRecordType())
          continue;
        bool read = false;
        bool changed = false;
        for (const auto &[path, effect] : base->summary->effects) {
          if (!path.isParam() || path.index != i)
            continue;
          read |= path.hasDeref() && effect.read;
          changed |= effect.written || effect.replaced || effect.consumed();
        }
        if (read && !changed)
          readOnlyRecords.insert(i);
      }
      const auto overwritten = [&](const core::SummaryPath &path) {
        return path.hasDeref() &&
               std::ranges::any_of(
                   base->summary->effects, [&](const auto &effect) {
                     return (effect.second.written || effect.second.replaced) &&
                            (effect.first == path ||
                             effect.first.isProperPrefixOf(path));
                   });
      };
      const bool refinesInput =
          std::ranges::any_of(bindings.facts, [&](const auto &entry) {
            const auto &[path, fact] = entry;
            const bool exact =
                fact.constant || (fact.integer && fact.integer->constant()) ||
                (fact.isPointer() &&
                 fact.implies(core::ValueFact::of(core::Outcome::Null)));
            if (!path.isParam() || !exact)
              return false;
            if (!readOnlyRecords.empty() && path.hasDeref() &&
                !readOnlyRecords.contains(path.index))
              return false;
            return !overwritten(path);
          });
      if (!refinesInput) {
        if (options.stats)
          options.stats->add("recursive_context_nomination_declines");
        if (base->summary->checked.complete())
          return base;
        return decline();
      }
      const auto stableInputs = [&](core::CallContext input) {
        std::erase_if(input.facts, [&](const auto &fact) {
          return overwritten(fact.first);
        });
        return input;
      };
      const auto stable = stableInputs(bindings);
      if (std::ranges::any_of(activeMemoryContexts, [&](const auto &active) {
            return active.first == symbol && active.second != bindings &&
                   stableInputs(active.second) == stable;
          })) {
        if (options.stats)
          options.stats->add("recursive_output_context_declines");
        if (base->summary->checked.complete())
          return base;
        return decline();
      }
    }
  }
  const MemoryContextKey key{std::string(symbol), bindings};
  noteDependency(symbol);
  auto &requests = memoryRequests[key.first];
  if (!requests.contains(bindings) &&
      requests.size() >= core::MaxMemoryContexts)
    return decline();
  requests.insert(bindings);
  const auto *definition = function ? function->getDefinition() : nullptr;
  if (!context)
    return decline();
  if (!definition) {
    if (!database)
      return decline();
    const auto exported = database->exportContext(bindings, globalTable);
    if (!exported)
      return decline();
    const auto *summary = database->findMemorySpecialization(symbol, *exported);
    if (!summary)
      return decline();
    if (checkedCase)
      options.stats->add("checked_case_hits");
    return ResolvedSummary{.summary = importSummary(*summary),
                           .source = SummarySource::Program};
  }
  if (activeMemoryContexts.contains(key) ||
      activeMemoryContexts.size() + activeContexts.size() >=
          core::MaxCallContextDepth)
    return decline();
  discardStaleContexts();
  if (!memorySpecialized.contains(key) || !memorySpecialized.at(key)) {
    if (options.stats) {
      options.stats->add("specialization_misses");
      if (checkedCase)
        options.stats->add("checked_case_analyses");
    }
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
    core::DiagnosticCollector collected;
    AnalysisOptions nestedOptions = options;
    nestedOptions.dumpStream = nullptr;
    FunctionDataflow analysis(definition->getASTContext(), *definition,
                              collected, nestedOptions, *this, true);
    analysis.memoryContext = bindings;
    analysis.callbackBindings = bindings.callbacks;
    analysis.run();
    if (!analysis.validMemoryContext)
      return decline();
    auto summary = std::move(analysis).summary();
    applyContract(*function, summary);
    memorySpecialized[key] = publishSummary(std::move(summary));
    memoryDiagnostics[key] = std::move(collected).diagnostics();
    memoryDependencies[key] = std::move(dependencies);
    auto &snapshot = memoryVersions[key];
    snapshot = dependencySnapshot();
    contextsNeedValidation |= !dependenciesCurrent(snapshot);
  } else {
    if (options.stats)
      options.stats->add("specialization_hits");
    if (checkedCase)
      options.stats->add("checked_case_hits");
    inheritDependencies(memoryDependencies[key]);
  }
  if (sink && bindings.reportDiagnostics)
    for (const auto &diagnostic : memoryDiagnostics[key])
      sink->report(diagnostic);
  return ResolvedSummary{.summary = memorySpecialized.at(key),
                         .source = SummarySource::Inferred};
}

} // namespace weavec::analysis
