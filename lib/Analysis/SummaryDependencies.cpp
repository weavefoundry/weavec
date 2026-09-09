//===- SummaryDependencies.cpp - Context invalidation (RFC 0020) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Analysis/Summaries.h"

namespace weavec::analysis {

SummarySnapshot
SummaryStore::publishSummary(core::FunctionSummary summary) const {
  if (stats)
    stats->add("summary_publications");
  return std::make_shared<const core::FunctionSummary>(std::move(summary));
}

SummarySnapshot
SummaryStore::retainSummary(const ResolvedSummary &summary) const {
  if (stats)
    stats->add("summary_shared_uses");
  return summary.summary;
}

void SummaryStore::markIncomplete(const clang::FunctionDecl &function) {
  if (incompleteFunctions.insert(function.getCanonicalDecl()).second)
    invalidateDependency(callableSymbol(function));
}

void SummaryStore::noteDependency(std::string_view name) const {
  if (dependencyFrames.empty())
    return;
  const std::string key(name);
  const auto found = revisions.find(key);
  const auto revision = found == revisions.end() ? 0 : found->second;
  for (auto *frame : dependencyFrames)
    frame->insert(key);
  for (auto &frame : dependencySnapshots)
    frame.try_emplace(key, revision);
}
void SummaryStore::inheritDependencies(const Dependencies &dependencies) const {
  for (const auto &dependency : dependencies)
    noteDependency(dependency);
}
void SummaryStore::beginDependencies(Dependencies &dependencies) {
  dependencyFrames.push_back(&dependencies);
  dependencySnapshots.emplace_back();
  for (const auto &dependency : dependencies)
    noteDependency(dependency);
}
void SummaryStore::endDependencies() {
  dependencyFrames.pop_back();
  dependencySnapshots.pop_back();
}
void SummaryStore::endAnalysis() {
  if (--analysisDepth == 0) {
    retiredMemory.clear();
    retiredCallbacks.clear();
  }
}

SummaryStore::DependencyVersions SummaryStore::dependencySnapshot() const {
  return dependencySnapshots.empty() ? DependencyVersions{}
                                     : dependencySnapshots.back();
}

bool SummaryStore::dependenciesCurrent(
    const DependencyVersions &snapshot) const {
  return std::ranges::all_of(snapshot, [&](const auto &entry) {
    const auto current = revisions.find(entry.first);
    return entry.second == (current == revisions.end() ? 0 : current->second);
  });
}

void SummaryStore::discardStaleContexts() {
  if (!contextsNeedValidation)
    return;
  contextsNeedValidation = false;
  const auto stale = [&](const auto &versions, auto &dependencies,
                         auto &summaries, auto &diagnostics, auto &retired) {
    for (const auto &[key, snapshot] : versions) {
      const bool changed = !dependenciesCurrent(snapshot);
      if (!changed || !summaries.contains(key))
        continue;
      if (analysisDepth)
        retired.push_back(summaries.extract(key));
      else
        summaries.erase(key);
      dependencies.erase(key);
      diagnostics.erase(key);
      if (stats)
        stats->add("specialization_invalidations");
    }
  };
  stale(memoryVersions, memoryDependencies, memorySpecialized,
        memoryDiagnostics, retiredMemory);
  stale(callbackVersions, callbackDependencies, specialized,
        specializedDiagnostics, retiredCallbacks);
}

void SummaryStore::invalidateDependency(std::string_view name) {
  ++revisions[std::string(name)];
  contextsNeedValidation = true;
  const auto invalidate = [&](auto &dependencies, auto &summaries,
                              auto &diagnostics, auto &retired) {
    for (auto it = dependencies.begin(); it != dependencies.end();) {
      if (!it->second.contains(std::string(name))) {
        ++it;
        continue;
      }
      // An active caller may still be applying this node. Keep its address
      // stable until the outermost dataflow has finished (RFC 0020).
      if (analysisDepth && summaries.contains(it->first))
        retired.push_back(summaries.extract(it->first));
      else
        summaries.erase(it->first);
      diagnostics.erase(it->first);
      it = dependencies.erase(it);
      if (stats)
        stats->add("specialization_invalidations");
    }
  };
  invalidate(memoryDependencies, memorySpecialized, memoryDiagnostics,
             retiredMemory);
  invalidate(callbackDependencies, specialized, specializedDiagnostics,
             retiredCallbacks);
}

} // namespace weavec::analysis
