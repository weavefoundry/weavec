//===- ProgramAnalysis.cpp - Whole-program driver -------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ProgramAnalysis.h"

#include "weavec/Core/Scc.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <utility>

namespace weavec::frontend {

ProgramAnalysis::ProgramAnalysis(FrontendOptions opts)
    : options(std::move(opts)) {}

void ProgramAnalysis::addUnit(std::unique_ptr<ProgramUnit> unit,
                              std::optional<analysis::UnitExports> known,
                              std::set<ReportedDiagnostic> reported) {
  units.push_back(Unit{.unit = std::move(unit),
                       .exports = std::move(known),
                       .reported = std::move(reported)});
}

void ProgramAnalysis::addExports(analysis::UnitExports exports) {
  fixed.push_back(std::move(exports));
}

std::optional<UnitResult>
ProgramAnalysis::runUnit(ProgramUnit &unit, const FrontendOptions &overrides) {
  FrontendOptions run = options;
  run.database = overrides.database;
  run.alreadyReported = overrides.alreadyReported;
  run.silent = overrides.silent;
  run.discoverOnly = overrides.discoverOnly;
  run.boundaryOnce = &boundaryOnce;
  if (run.silent)
    run.analysis.dumpStream = nullptr;

  std::optional<UnitResult> result;
  run.onResult = [&result](UnitResult r) { result = std::move(r); };
  const auto factory = createWeaveCActionFactory(std::move(run));
  const bool clean = unit.run(*factory);
  if (!result)
    return std::nullopt;
  if (!clean && result->errors == 0) {
    // Clang itself reported errors (the unit does not compile). Count one
    // so the run is not reported clean.
    result->errors = 1;
  }
  return result;
}

/// Exports with every summary at the bottom: the start of a fixpoint.
static analysis::UnitExports skeleton(const analysis::UnitExports &exports) {
  analysis::UnitExports result = exports;
  for (auto &[name, function] : result.functions)
    function.summary = core::FunctionSummary{};
  result.unknownCallees.clear();
  result.unknownIndirectTypes.clear();
  return result;
}

std::vector<std::vector<unsigned>> ProgramAnalysis::unitGraph() const {
  std::map<std::string, std::vector<unsigned>, std::less<>> definers;
  std::map<std::string, std::vector<unsigned>, std::less<>> candidates;
  for (unsigned i = 0; i < units.size(); ++i) {
    if (!units[i].exports)
      continue;
    for (const auto &[name, function] : units[i].exports->functions) {
      if (function.external)
        definers[name].push_back(i);
      if (function.addressTaken && !function.typeKey.empty())
        candidates[function.typeKey].push_back(i);
    }
  }

  std::vector<std::vector<unsigned>> adjacency(units.size());
  for (unsigned i = 0; i < units.size(); ++i) {
    if (!units[i].exports)
      continue;
    std::vector<unsigned> &edges = adjacency[i];
    for (const std::string &name : units[i].exports->imports) {
      if (const auto it = definers.find(name); it != definers.end())
        edges.insert(edges.end(), it->second.begin(), it->second.end());
    }
    for (const std::string &key : units[i].exports->indirectTypes) {
      if (const auto it = candidates.find(key); it != candidates.end())
        edges.insert(edges.end(), it->second.begin(), it->second.end());
    }
    std::erase(edges, i);
    std::ranges::sort(edges);
    edges.erase(std::ranges::unique(edges).begin(), edges.end());
  }
  return adjacency;
}

analysis::ProgramDatabase ProgramAnalysis::databaseFor(
    const std::vector<analysis::UnitExports> &members) const {
  analysis::ProgramDatabase db = settled;
  for (const analysis::UnitExports &exports : members)
    db.add(exports);
  return db;
}

static void announce(llvm::raw_ostream *dump, const ProgramUnit &unit) {
  if (dump != nullptr)
    *dump << "unit '" << unit.name() << "':\n";
}

void ProgramAnalysis::analyzeAcyclic(unsigned index, Result &result) {
  Unit &unit = units[index];
  announce(options.analysis.dumpStream, *unit.unit);

  FrontendOptions overrides;
  overrides.database = &settled;
  overrides.alreadyReported = &unit.reported;
  const std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
  if (!run) {
    result.failed.push_back(unit.unit->name());
    // The compile-time view is the best the rest of the program can get.
    if (unit.exports)
      settled.add(*unit.exports);
    return;
  }
  result.errors += run->errors;
  result.warnings += run->warnings;
  unit.exports = run->exports;
  settled.add(run->exports);
}

void ProgramAnalysis::analyzeCyclic(const std::vector<unsigned> &component,
                                    Result &result) {
  // Every member starts at the bottom and the group is iterated silently
  // until no member's exports change; the reporting pass then sees the
  // fixpoint (RFC 0005, *The whole-program algorithm*).
  std::vector<analysis::UnitExports> current;
  std::vector<bool> broken(component.size(), false);
  current.reserve(component.size());
  for (const unsigned member : component)
    current.push_back(skeleton(*units[member].exports));

  bool changed = true;
  for (unsigned round = 0; round < MaxRounds && changed; ++round) {
    changed = false;
    for (unsigned k = 0; k < component.size(); ++k) {
      if (broken[k])
        continue;
      const analysis::ProgramDatabase db = databaseFor(current);
      FrontendOptions overrides;
      overrides.database = &db;
      overrides.silent = true;
      const std::optional<UnitResult> run =
          runUnit(*units[component[k]].unit, overrides);
      if (!run) {
        broken[k] = true;
        result.failed.push_back(units[component[k]].unit->name());
        continue;
      }
      if (!run->exports.sameSummariesAs(current[k])) {
        changed = true;
        current[k] = run->exports;
      }
    }
  }
  if (changed) {
    std::vector<std::string> names;
    names.reserve(component.size());
    for (const unsigned member : component)
      names.push_back(units[member].unit->name());
    result.nonConverging.push_back(std::move(names));
  }

  const analysis::ProgramDatabase db = databaseFor(current);
  for (unsigned k = 0; k < component.size(); ++k) {
    Unit &unit = units[component[k]];
    if (broken[k])
      continue;
    announce(options.analysis.dumpStream, *unit.unit);
    FrontendOptions overrides;
    overrides.database = &db;
    overrides.alreadyReported = &unit.reported;
    const std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
    if (!run) {
      broken[k] = true;
      result.failed.push_back(unit.unit->name());
      continue;
    }
    result.errors += run->errors;
    result.warnings += run->warnings;
    current[k] = run->exports;
  }
  for (unsigned k = 0; k < component.size(); ++k) {
    units[component[k]].exports = current[k];
    settled.add(current[k]);
  }
}

ProgramAnalysis::Result ProgramAnalysis::run() {
  Result result;
  settled.clear();
  boundaryOnce.clear();
  for (const analysis::UnitExports &exports : fixed)
    settled.add(exports);

  // Discovery: one parse, no analysis, for units whose exports are not
  // already on hand.
  for (Unit &unit : units) {
    if (unit.exports)
      continue;
    FrontendOptions overrides;
    overrides.discoverOnly = true;
    if (std::optional<UnitResult> run = runUnit(*unit.unit, overrides)) {
      unit.exports = std::move(run->exports);
    } else {
      result.failed.push_back(unit.unit->name());
    }
  }

  const std::vector<std::vector<unsigned>> adjacency = unitGraph();
  for (const std::vector<unsigned> &component :
       core::stronglyConnectedComponents(adjacency)) {
    if (component.size() == 1) {
      // A self-edge (an indirect call to one of its own address-taken
      // functions) is resolved inside the unit by RFC 0004; no rounds.
      if (units[component.front()].exports)
        analyzeAcyclic(component.front(), result);
      continue;
    }
    analyzeCyclic(component, result);
  }

  if (llvm::raw_ostream *dump = options.analysis.dumpStream)
    settled.dump(*dump);
  return result;
}

bool CompilationDatabaseUnit::run(
    clang::tooling::FrontendActionFactory &factory) {
  clang::tooling::ClangTool tool(compilations, {source});
  for (const clang::tooling::ArgumentsAdjuster &adjuster : adjusters)
    tool.appendArgumentsAdjuster(adjuster);
  return tool.run(&factory) == 0;
}

} // namespace weavec::frontend
