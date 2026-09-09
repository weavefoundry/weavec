//===- ProgramAnalysis.cpp - Whole-program driver -------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ProgramAnalysis.h"

#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/SafetyEntryPool.h"
#include "weavec/Core/Scc.h"
#include "weavec/Frontend/AnalysisCache.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/CheckedArtifacts.h"
#include "weavec/Frontend/Sidecar.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace weavec::frontend {

static std::string importedIdentity(const analysis::ProgramDatabase &database,
                                    const std::set<std::string> &dependencies);

ProgramAnalysis::ProgramAnalysis(FrontendOptions opts)
    : options(std::move(opts)) {}

void ProgramAnalysis::addUnit(std::unique_ptr<ProgramUnit> unit,
                              std::optional<analysis::UnitExports> known,
                              std::set<ReportedDiagnostic> reported) {
  units.push_back(Unit{.unit = std::move(unit),
                       .exports = std::move(known),
                       .reported = std::move(reported),
                       .sizedPairsSeen = {}});
}

void ProgramAnalysis::addExports(analysis::UnitExports exports) {
  fixed.push_back(std::move(exports));
}

void ProgramAnalysis::touchRetainedUnit(ProgramUnit &unit) {
  std::erase(retainedUnits, &unit);
  retainedUnits.push_back(&unit);
  trimRetainedUnits();
}

void ProgramAnalysis::trimRetainedUnits() {
  while (boundedRetention && retainedUnits.size() > 1) {
    auto *oldest = retainedUnits.front();
    retainedUnits.erase(retainedUnits.begin());
    if (oldest->releaseAST() && options.analysis.stats)
      options.analysis.stats->add("unit_evictions");
  }
}

std::optional<UnitResult>
ProgramAnalysis::runUnit(ProgramUnit &unit, const FrontendOptions &overrides) {
  FrontendOptions run = options;
  run.database = overrides.database;
  run.alreadyReported = overrides.alreadyReported;
  run.onlyIds = overrides.onlyIds;
  run.silent = overrides.silent;
  run.discoverOnly = overrides.discoverOnly;
  run.boundaryOnce = &boundaryOnce;
  if (run.silent)
    run.analysis.dumpStream = nullptr;

  std::optional<UnitResult> result;
  run.onResult = [&result](UnitResult r) { result = std::move(r); };
  std::string passKey;
  bool reused = false;
  bool clean = false;
  // The late sized-field reporting pass is itself settled work, but it sees
  // a fuller database than the component's initial report. Preserve that
  // separate dependency boundary instead of assuming the first run suffices.
  if (run.onlyIds && run.database && !run.analysisCache.empty()) {
    const auto input = unit.inputIdentity(run);
    const auto config = analysisOptionsIdentity(run);
    if (!input.empty() && !config.empty()) {
      std::vector<std::string> parts{input, config, "confirmed-sized-fields"};
      for (const auto id : *run.onlyIds)
        parts.emplace_back(id);
      passKey = checkedCommandDigest(parts);
      auto checkpoint = readAnalysisCheckpoint(run.analysisCache, passKey,
                                               run.analysis.stats);
      if (checkpoint && checkpoint->units.size() == 1 &&
          checkpoint->importedIdentity ==
              importedIdentity(*run.database,
                               checkpoint->units[0].dependencies)) {
        clean = unit.replay(checkpoint->units[0], run);
        reused = clean && result.has_value();
        if (reused && run.analysis.stats)
          run.analysis.stats->add("cache_hits");
      }
    }
  }
  if (!reused)
    clean = unit.analyze(run);
  if (!result)
    return std::nullopt;
  touchRetainedUnit(unit);
  if (!clean && result->errors == 0) {
    // Clang itself reported errors (the unit does not compile). Count one
    // so the run is not reported clean.
    result->errors = 1;
  }
  if (!reused && clean && !passKey.empty()) {
    AnalysisCheckpoint checkpoint;
    checkpoint.units.push_back(*result);
    checkpoint.importedIdentity =
        importedIdentity(*run.database, result->dependencies);
    (void)writeAnalysisCheckpoint(run.analysisCache, passKey, checkpoint,
                                  run.analysis.stats);
  }
  for (auto &entry : units)
    if (entry.unit.get() == &unit)
      entry.dependencies.insert(result->dependencies.begin(),
                                result->dependencies.end());
  if (!writeAnalysisStats(run.analysisStatsPath, run.analysis.stats, false))
    return std::nullopt;
  return result;
}

/// Exports with every summary at the bottom: the start of a fixpoint.
static analysis::UnitExports skeleton(const analysis::UnitExports &exports) {
  analysis::UnitExports result = exports;
  for (auto &[name, function] : result.functions) {
    function.summary = core::FunctionSummary{};
    function.memorySpecializations.clear();
  }
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
      if (const auto it = definers.find(name); it != definers.end()) {
        edges.insert(edges.end(), it->second.begin(), it->second.end());
        // RFC 0014/0016: callback and memory contexts travel from caller to
        // definer, so these units converge together before either is reported.
        for (const unsigned definer : it->second)
          if (units[definer].exports->functions.at(name).acceptsCallbacks ||
              units[definer].exports->functions.at(name).acceptsMemoryContexts)
            adjacency[definer].push_back(i);
      }
    }
    for (const std::string &key : units[i].exports->indirectTypes) {
      if (const auto it = candidates.find(key); it != candidates.end()) {
        edges.insert(edges.end(), it->second.begin(), it->second.end());
        for (const unsigned definer : it->second)
          adjacency[definer].push_back(i);
      }
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
  settled.add(run->exports);
  settle(unit, settled, *run);
}

void ProgramAnalysis::settle(Unit &unit, const analysis::ProgramDatabase &db,
                             const UnitResult &run) const {
  unit.exports = run.exports;
  if (!options.analysisCache.empty())
    unit.checkpoint = run;
  unit.reported.insert(run.reported.begin(), run.reported.end());
  unit.sizedPairsSeen = db.sizedFieldFacts().confirmedPairs();
}

void ProgramAnalysis::reportConfirmedSizedFields(Result &result) {
  // A unit reported on before the program confirmed a pair (the witnesses
  // came from units it does not call, so the unit order did not put them
  // first), and that looked up the extent of the pair's field, is analysed
  // once more against the whole program and shows what it did not show
  // before. The pass is one: a witness the pass itself adds can only widen
  // the next program's view (RFC 0012, *Sized fields*, "Inference").
  const std::set<analysis::SizedFieldWitness> confirmed =
      settled.sizedFieldFacts().confirmedPairs();
  if (confirmed.empty())
    return;
  // A synthesised extent enables bounds reports and nothing else; the run
  // is against a fuller database than the first, which is not this pass's
  // business to report on (RFC 0012, *Two passes in a unit*).
  static const std::set<std::string_view> OnlyBounds{core::diag::OutOfBounds};
  for (Unit &unit : units) {
    if (!unit.exports)
      continue;
    const bool more = llvm::any_of(
        confirmed, [&unit](const analysis::SizedFieldWitness &pair) {
          return !unit.sizedPairsSeen.contains(pair) &&
                 unit.exports->sizedFieldLoads.contains(pair.field);
        });
    if (!more)
      continue;
    announce(options.analysis.dumpStream, *unit.unit);
    FrontendOptions overrides;
    overrides.database = &settled;
    overrides.alreadyReported = &unit.reported;
    overrides.onlyIds = &OnlyBounds;
    const std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
    if (!run) {
      result.failed.push_back(unit.unit->name());
      continue;
    }
    result.errors += run->errors;
    result.warnings += run->warnings;
    settled.add(run->exports);
    settle(unit, settled, *run);
  }
}

void ProgramAnalysis::widen(analysis::UnitExports &exports,
                            const analysis::UnitExports &previous) {
  for (auto &[name, contract] : exports.checkedDefinitions)
    if (const auto before = previous.checkedDefinitions.find(name);
        before != previous.checkedDefinitions.end())
      contract.join(before->second);
  for (auto &[name, function] : exports.functions) {
    const auto before = previous.functions.find(name);
    if (before != previous.functions.end()) {
      function.summary.join(before->second.summary);
      for (const auto &[input, summary] : before->second.memorySpecializations)
        if (function.memorySpecializations.contains(input) ||
            function.memorySpecializations.size() < core::MaxMemoryContexts)
          function.memorySpecializations[input].join(summary);
    }
  }
  for (const auto &[symbol, requests] : previous.memoryRequests)
    for (const auto &input : requests)
      if (exports.memoryRequests[symbol].size() < core::MaxMemoryContexts)
        exports.memoryRequests[symbol].insert(input);
  exports.countFields.insert(previous.countFields.begin(),
                             previous.countFields.end());
  // RFC 0012: sized-field facts widen the same way; a refutation once
  // seen stays.
  exports.sizedFields.merge(previous.sizedFields);
}

/// RFC 0020: component edges carry summaries and context requests in opposite
/// directions. Compute changed inputs once, retaining no copied summaries.
static auto changedUnitInputs(const analysis::UnitExports &before,
                              const analysis::UnitExports &after) {
  struct InputChanges {
    bool globals;
    std::set<std::string> functions;
    std::set<std::string> indirectTypes;
    std::set<std::string> requests;

    bool affects(const analysis::UnitExports &consumer,
                 const std::set<std::string> &dependencies) const {
      if (globals)
        return true;
      for (const auto &symbol : functions)
        if (dependencies.contains(symbol) || consumer.imports.contains(symbol))
          return true;
      for (const auto &type : indirectTypes)
        if (consumer.indirectTypes.contains(type))
          return true;
      return std::ranges::any_of(consumer.functions, [&](const auto &entry) {
        const auto &[name, function] = entry;
        const auto symbol =
            function.external ? name : consumer.source + "#" + name;
        return requests.contains(symbol);
      });
    }
  };
  InputChanges changes{.globals =
                           before.globals != after.globals ||
                           before.countFields != after.countFields ||
                           before.sizedFields != after.sizedFields ||
                           before.callbackGlobals != after.callbackGlobals,
                       .functions = {},
                       .indirectTypes = {},
                       .requests = {}};
  const auto changed = [](const auto &a, const auto &b, const auto &visit) {
    for (const auto &[key, value] : a) {
      const auto found = b.find(key);
      if (found == b.end() || found->second != value)
        visit(key, value);
    }
  };
  const auto functions = [&](const auto &unit, const auto &other) {
    changed(unit.functions, other.functions,
            [&](const auto &name, const auto &function) {
              changes.functions.insert(
                  function.external ? name : unit.source + "#" + name);
              if (function.addressTaken && !function.typeKey.empty())
                changes.indirectTypes.insert(function.typeKey);
            });
  };
  functions(before, after);
  functions(after, before);
  const auto requests = [&](const auto &a, const auto &b) {
    changed(a, b, [&](const auto &symbol, const auto &values) {
      (void)values;
      changes.requests.insert(symbol);
    });
  };
  requests(before.callbackRequests, after.callbackRequests);
  requests(after.callbackRequests, before.callbackRequests);
  requests(before.memoryRequests, after.memoryRequests);
  requests(after.memoryRequests, before.memoryRequests);
  return changes;
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

  // Each member sees the newest exports of every other member; the database
  // is rebuilt only after some member's exports changed, which in the last
  // round (and for most of the members of a large one) is never. Members
  // are kept numbered by the database's own table, so a rebuild copies
  // their summaries instead of renumbering each of them.
  analysis::ProgramDatabase db = databaseFor(current);
  for (analysis::UnitExports &member : current)
    member = db.renumbered(member);

  // RFC 0010, *Whole-program fixpoint*: a member whose inputs did not change
  // since it last ran produces the same exports, so only members with a
  // changed dependency are re-run. The dependencies are the unit graph's
  // edges restricted to the group (`imports` and `indirectTypes` against
  // the members' definitions); a member with no known dependency inside the
  // group runs once.
  for (const auto &unit : units)
    if (unit.exports && !unit.exports->checkedDefinitions.empty())
      options.analysis.checkContracts = true;
  const std::vector<std::vector<unsigned>> adjacency = unitGraph();
  std::map<unsigned, unsigned> position;
  for (unsigned k = 0; k < component.size(); ++k)
    position[component[k]] = k;
  std::vector<std::vector<unsigned>> dependents(component.size());
  for (unsigned k = 0; k < component.size(); ++k) {
    for (const unsigned target : adjacency[component[k]]) {
      if (const auto it = position.find(target); it != position.end())
        dependents[it->second].push_back(k);
    }
  }
  std::vector<bool> dirty(component.size(), true);

  bool stale = false;
  bool changed = true;
  for (unsigned round = 0; round < MaxRounds && changed; ++round) {
    if (options.analysis.stats)
      options.analysis.stats->add("program_fixpoint_rounds");
    changed = false;
    for (unsigned k = 0; k < component.size(); ++k) {
      if (broken[k] || !dirty[k])
        continue;
      dirty[k] = false;
      if (stale) {
        // RFC 0020: no analyzer is active between unit runs. Release the
        // old summaries before constructing their complete replacement.
        db.clear();
        db = databaseFor(current);
        stale = false;
      }
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
      // Both sides are numbered by the database's table (`renumbered` only
      // ever appends to it), so the summaries alone decide the fixpoint.
      analysis::UnitExports exports = db.renumbered(run->exports);
      // RFC 0011, *Whole-program widening*: a group that oscillates (a
      // must-fact one member drops makes another add one back) is joined
      // towards what every round agreed on; `join` only ever weakens.
      if (round >= WidenAfter)
        widen(exports, current[k]);
      if (!exports.sameSummariesAs(current[k])) {
        changed = true;
        stale = true;
        const auto inputs = changedUnitInputs(current[k], exports);
        for (unsigned dependent = 0; dependent < component.size();
             ++dependent) {
          if (dependent == k || broken[dependent])
            continue;
          const auto &consumer = units[component[dependent]];
          const bool graphEdge = llvm::is_contained(dependents[k], dependent);
          const bool affected =
              consumer.dependencies.empty()
                  ? graphEdge
                  : inputs.affects(current[dependent], consumer.dependencies);
          if (affected) {
            dirty[dependent] = true;
          } else if (graphEdge && options.analysis.stats) {
            options.analysis.stats->add("unit_invalidation_skips");
          }
        }
        current[k] = std::move(exports);
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

  if (stale) {
    db.clear();
    db = databaseFor(current);
  }
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
    settle(unit, db, *run);
    // The unit now owns its completed export; the approximation has no
    // remaining reader. Keep failed members' previous exports below.
    current[k] = analysis::UnitExports{};
  }
  db.clear();
  for (unsigned k = 0; k < component.size(); ++k) {
    auto &unit = units[component[k]];
    if (broken[k])
      unit.exports = std::move(current[k]);
    settled.add(*unit.exports);
  }
}

static std::string importedIdentity(const analysis::ProgramDatabase &database,
                                    const std::set<std::string> &dependencies) {
  return checkpointExportsIdentity(database.checkpointInputs(dependencies));
}

void ProgramAnalysis::analyzeComponent(const std::vector<unsigned> &component,
                                       Result &result) {
  if (component.size() == 1 && !units[component.front()].exports)
    return;
  if (component.size() > 1)
    for (const auto &unit : units)
      if (unit.exports && !unit.exports->checkedDefinitions.empty())
        options.analysis.checkContracts = true;
  std::string key;
  std::optional<analysis::ProgramDatabase> imported;
  if (!options.analysisCache.empty()) {
    std::vector<std::string> inputs{analysisOptionsIdentity(options)};
    bool eligible = !inputs.front().empty();
    for (const auto index : component) {
      auto input = units[index].unit->inputIdentity(options);
      eligible &= !input.empty();
      inputs.push_back(units[index].unit->name());
      inputs.push_back(std::move(input));
    }
    if (eligible) {
      key = checkedCommandDigest(inputs);
      imported = settled;
      auto checkpoint = readAnalysisCheckpoint(options.analysisCache, key,
                                               options.analysis.stats);
      if (checkpoint && checkpoint->units.size() == component.size()) {
        std::set<std::string> dependencies;
        for (const auto &unit : checkpoint->units)
          dependencies.insert(unit.dependencies.begin(),
                              unit.dependencies.end());
        if (checkpoint->importedIdentity ==
            importedIdentity(settled, dependencies)) {
          bool replayed = true;
          for (unsigned k = 0; k < component.size(); ++k) {
            auto &unit = units[component[k]];
            auto replay = options;
            replay.database = &settled;
            replay.alreadyReported = &unit.reported;
            replay.boundaryOnce = &boundaryOnce;
            replay.onResult = [&](const UnitResult &run) {
              result.errors += run.errors;
              result.warnings += run.warnings;
              settle(unit, settled, run);
              unit.sizedPairsSeen = checkpoint->sizedPairsSeen[k];
            };
            replayed &= unit.unit->replay(checkpoint->units[k], replay);
          }
          // Production units validate replay support before providing an input
          // identity. A failed custom unit cannot publish a successful cache
          // hit.
          if (replayed) {
            for (const auto index : component)
              settled.add(*units[index].exports);
            if (options.analysis.stats)
              options.analysis.stats->add("cache_hits", component.size());
            return;
          }
        } else if (options.analysis.stats) {
          options.analysis.stats->add("cache_dependency_misses",
                                      component.size());
        }
      }
    }
    if (options.analysis.stats)
      options.analysis.stats->add("cache_misses", component.size());
  }
  const auto failures = result.failed.size();
  const auto unsettled = result.nonConverging.size();
  if (component.size() == 1)
    analyzeAcyclic(component.front(), result);
  else
    analyzeCyclic(component, result);
  if (key.empty() || result.failed.size() != failures ||
      result.nonConverging.size() != unsettled)
    return;
  AnalysisCheckpoint checkpoint;
  std::set<std::string> dependencies;
  for (const auto index : component) {
    const auto &unit = units[index];
    if (!unit.checkpoint)
      return;
    checkpoint.units.push_back(*unit.checkpoint);
    checkpoint.sizedPairsSeen.push_back(unit.sizedPairsSeen);
    checkpoint.units.back().dependencies = unit.dependencies;
    dependencies.insert(unit.dependencies.begin(), unit.dependencies.end());
  }
  checkpoint.importedIdentity = importedIdentity(*imported, dependencies);
  (void)writeAnalysisCheckpoint(options.analysisCache, key, checkpoint,
                                options.analysis.stats);
}

ProgramAnalysis::Result ProgramAnalysis::run() {
  core::SafetyEntryPool explanations(options.analysis.stats);
  Result result;
  settled.clear();
  boundaryOnce.clear();
  boundedRetention = false;
  retainedUnits.clear();
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

  // RFC 0020: discover every annotation before allowing ordinary eviction.
  // Bound neither checked input bindings nor a pending persistent cache key.
  const auto checkedExports = [](const auto &exports) {
    return !exports.checkedDefinitions.empty();
  };
  boundedRetention =
      options.analysisCache.empty() && !options.analysis.checkContracts &&
      !options.analysis.checked && options.analysis.checkedFunctions.empty() &&
      !options.bindCheckedInputs && options.analysis.dumpStream == nullptr &&
      std::ranges::none_of(fixed, checkedExports) &&
      std::ranges::none_of(units, [&](const auto &unit) {
        return unit.exports && checkedExports(*unit.exports);
      });
  // Include ASTs retained by a previous invocation of this ProgramAnalysis.
  retainedUnits.clear();
  for (const auto &unit : units)
    retainedUnits.push_back(unit.unit.get());
  trimRetainedUnits();
  const std::vector<std::vector<unsigned>> adjacency = unitGraph();
  for (const std::vector<unsigned> &component :
       core::stronglyConnectedComponents(adjacency)) {
    analyzeComponent(component, result);
  }
  reportConfirmedSizedFields(result);
  if (!result.failed.empty() || !result.nonConverging.empty())
    options.checkedReport->invalidate(
        "whole-program inputs did not produce settled checked contracts");

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

bool CompilationDatabaseUnit::analyze(const FrontendOptions &options) {
  if (!attemptedParse) {
    attemptedParse = true;
    if (!options.analysisCache.empty())
      identity = preprocessingInput(options);
    core::AnalysisTimer timer(options.analysis.stats, "parsing");
    clang::tooling::ClangTool tool(compilations, {source});
    for (const auto &adjuster : adjusters)
      tool.appendArgumentsAdjuster(adjuster);
    if (options.analysis.stats)
      options.analysis.stats->add("unit_parses");
    if (tool.buildASTs(asts) != 0 || asts.empty()) {
      asts.clear();
      return false;
    }
    multipleCommands = asts.size() != 1;
    if (multipleCommands)
      asts.clear();
    if (identity && !identity->empty() &&
        (multipleCommands || preprocessingInput(options) != *identity)) {
      identity->clear();
      if (options.analysis.stats)
        options.analysis.stats->add("cache_input_changes");
    }
  } else if (options.analysis.stats) {
    options.analysis.stats->add("unit_reuses");
  }
  if (multipleCommands)
    return ProgramUnit::analyze(options);
  if (asts.empty())
    return false;
  auto &ast = *asts.front();
  auto run = options;
  run.analysis.preparation = preparation;
  auto result = analyzeRetainedUnit(ast, run);
  if (options.onResult)
    options.onResult(std::move(result));
  return true;
}

std::string
CompilationDatabaseUnit::inputIdentity(const FrontendOptions &options) {
  if (!attemptedParse) {
    auto discovery = options;
    discovery.discoverOnly = true;
    discovery.onResult = {};
    if (!analyze(discovery))
      return {};
  }
  if (asts.size() != 1)
    return {};
  // Never attach current filesystem bytes to an AST parsed earlier.
  return identity.value_or("");
}

std::string
CompilationDatabaseUnit::preprocessingInput(const FrontendOptions &options) {
  core::AnalysisTimer timer(options.analysis.stats, "cache_validation");
  if (options.analysis.stats)
    options.analysis.stats->add("cache_input_validations");
  std::string observed;
  auto factory = createInputIdentityFactory(observed);
  clang::tooling::ClangTool tool(compilations, {source});
  for (const auto &adjuster : adjusters)
    tool.appendArgumentsAdjuster(adjuster);
  clang::IgnoringDiagConsumer ignored;
  tool.setDiagnosticConsumer(&ignored);
  if (tool.run(factory.get()) != 0)
    observed.clear();
  return observed;
}

bool CompilationDatabaseUnit::replay(const UnitResult &result,
                                     const FrontendOptions &options) {
  if (asts.size() != 1)
    return false;
  auto replayed = analyzeRetainedUnit(*asts.front(), options, &result);
  if (options.onResult)
    options.onResult(std::move(replayed));
  return true;
}

bool CompilationDatabaseUnit::releaseAST() {
  if (asts.empty())
    return false;
  preparation->functions.clear();
  // ClangTool fills this vector in uninstrumented LLVM. Discard its backing
  // storage too, so reparsing cannot reuse ASan-poisoned spare capacity.
  decltype(asts){}.swap(asts);
  attemptedParse = false;
  multipleCommands = false;
  identity.reset();
  return true;
}

} // namespace weavec::frontend
