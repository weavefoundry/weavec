//===- ProgramAnalysis.cpp - Whole-program driver -------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ProgramAnalysis.h"

#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Scc.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/LinkStep.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <utility>

namespace weavec::frontend {

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
  run.collectInterface = overrides.collectInterface;
  if (run.silent)
    run.analysis.dumpStream = nullptr;

  std::optional<UnitResult> result;
  run.onResult = [&result](UnitResult r) { result = std::move(r); };
  const bool clean = unit.analyze(run);
  if (!result)
    return std::nullopt;
  touchRetainedUnit(unit);
  if (!clean && result->errors == 0) {
    // Clang itself reported errors (the unit does not compile). Count one
    // so the run is not reported clean.
    result->errors = 1;
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
    function.summary = analysis::ExportedSummary{};
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
  overrides.collectInterface = interfaces;
  std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
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
  settle(unit, settled, std::move(*run));
}

void ProgramAnalysis::settle(Unit &unit, const analysis::ProgramDatabase &db,
                             UnitResult run) const {
  unit.exports = std::move(run.exports);
  unit.reported.insert(run.reported.begin(), run.reported.end());
  unit.sizedPairsSeen = db.sizedFieldFacts().confirmedPairs();
  // RFC 0030 §2.6: only the last reporting run publishes.
  if (run.ledger && (ledgers || interfaces))
    unit.ledger = std::make_shared<const core::Ledger>(run.ledger->ledger);
  if (run.interface)
    unit.interface = std::move(run.interface);
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
    overrides.collectInterface = interfaces;
    std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
    if (!run) {
      result.failed.push_back(unit.unit->name());
      continue;
    }
    result.errors += run->errors;
    result.warnings += run->warnings;
    settled.add(run->exports);
    settle(unit, settled, std::move(*run));
  }
}

void ProgramAnalysis::widen(analysis::UnitExports &exports,
                            const analysis::UnitExports &previous) {
  for (auto &[name, function] : exports.functions) {
    const auto before = previous.functions.find(name);
    if (before != previous.functions.end()) {
      auto joined = function.summary.get();
      joined.join(before->second.summary.get());
      function.summary.assign(std::move(joined));
      for (const auto &[input, summary] : before->second.memorySpecializations)
        if (function.memorySpecializations.contains(input) ||
            function.memorySpecializations.size() < core::MaxMemoryContexts) {
          auto specialized = function.memorySpecializations[input].get();
          specialized.join(summary.get());
          function.memorySpecializations[input].assign(std::move(specialized));
        }
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
  InputChanges changes{.globals = before.globals != after.globals ||
                                  before.countFields != after.countFields ||
                                  before.sizedFields != after.sizedFields,
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
    member = db.renumbered(std::move(member));

  // RFC 0010, *Whole-program fixpoint*: a member whose inputs did not change
  // since it last ran produces the same exports, so only members with a
  // changed dependency are re-run. The dependencies are the unit graph's
  // edges restricted to the group (`imports` and `indirectTypes` against
  // the members' definitions); a member with no known dependency inside the
  // group runs once.
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

  // RFC 0005, *The whole-program algorithm*: a member's new summaries reach
  // the members that import them **within the same round** only when those
  // come later in the order, so a group visited in the order its units were
  // named advances one fact by about two members per round. Visit it in
  // reverse post-order of the definer-to-consumer graph instead — the
  // schedule every iterative dataflow solver uses — so one round carries a
  // fact the length of a call chain. Only the order changes: the round loop,
  // the widening and the reporting pass below (which keeps the component's
  // own order) are untouched.
  const std::vector<unsigned> schedule = [&] {
    std::vector<unsigned> visitOrder(component.size());
    std::vector<unsigned> incoming(component.size(), 0);
    for (const auto &consumers : dependents)
      for (const unsigned consumer : consumers)
        ++incoming[consumer];
    std::iota(visitOrder.begin(), visitOrder.end(), 0U);
    std::ranges::stable_sort(visitOrder, [&](unsigned a, unsigned b) {
      return incoming[a] < incoming[b];
    });
    std::vector<bool> seen(component.size(), false);
    std::vector<unsigned> postOrder;
    postOrder.reserve(component.size());
    std::vector<std::pair<unsigned, std::size_t>> frames;
    for (const unsigned root : visitOrder) {
      if (seen[root])
        continue;
      seen[root] = true;
      frames.emplace_back(root, 0);
      while (!frames.empty()) {
        const auto [node, next] = frames.back();
        if (next < dependents[node].size()) {
          frames.back().second = next + 1;
          const unsigned consumer = dependents[node][next];
          if (!seen[consumer]) {
            seen[consumer] = true;
            frames.emplace_back(consumer, 0);
          }
          continue;
        }
        postOrder.push_back(node);
        frames.pop_back();
      }
    }
    return std::vector<unsigned>(postOrder.rbegin(), postOrder.rend());
  }();

  bool stale = false;
  bool changed = true;
  for (unsigned round = 0; round < MaxRounds && changed; ++round) {
    if (options.analysis.stats)
      options.analysis.stats->add("program_fixpoint_rounds");
    changed = false;
    for (const unsigned k : schedule) {
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
      std::optional<UnitResult> run =
          runUnit(*units[component[k]].unit, overrides);
      if (!run) {
        broken[k] = true;
        result.failed.push_back(units[component[k]].unit->name());
        continue;
      }
      // Both sides are numbered by the database's table (`renumbered` only
      // ever appends to it), so the summaries alone decide the fixpoint.
      analysis::UnitExports exports = db.renumbered(std::move(run->exports));
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
    overrides.collectInterface = interfaces;
    std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
    if (!run) {
      broken[k] = true;
      result.failed.push_back(unit.unit->name());
      continue;
    }
    result.errors += run->errors;
    result.warnings += run->warnings;
    settle(unit, db, std::move(*run));
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

void ProgramAnalysis::analyzeComponent(const std::vector<unsigned> &component,
                                       Result &result) {
  if (component.size() == 1 && !units[component.front()].exports)
    return;
  if (component.size() == 1)
    analyzeAcyclic(component.front(), result);
  else
    analyzeCyclic(component, result);
}

ProgramAnalysis::Result ProgramAnalysis::run() {
  Result result;
  settled.clear();
  settled.programFacts = programFacts;
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
    overrides.collectInterface = interfaces;
    if (std::optional<UnitResult> run = runUnit(*unit.unit, overrides)) {
      unit.exports = std::move(run->exports);
      unit.interface = std::move(run->interface);
    } else {
      result.failed.push_back(unit.unit->name());
    }
  }
  // RFC 0030 §13.2 step 2 in `weavec --whole-program`.
  if (interfaces && !programFacts)
    solveDiscoveredSlots();
  settled.programFacts = programFacts;

  // RFC 0020: after discovery, keep one retained AST at a time unless an
  // analysis dump needs them all.
  boundedRetention = options.analysis.dumpStream == nullptr;
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

  if (llvm::raw_ostream *dump = options.analysis.dumpStream) {
    settled.dump(*dump);
    if (programFacts)
      dumpProgramSlots(programFacts->slots, *dump);
  }
  return result;
}

void ProgramAnalysis::solveDiscoveredSlots() {
  std::vector<ProgramMember> members;
  LinkShape shape;
  shape.executable = false;
  for (const Unit &unit : units) {
    if (!unit.interface)
      continue;
    ProgramMember member;
    member.source = unit.unit->name();
    member.payload.facts.slots = unit.interface->slots;
    shape.executable =
        shape.executable || unit.interface->slots.defined.contains("main");
    members.push_back(std::move(member));
  }
  auto facts = std::make_shared<analysis::ProgramFacts>();
  facts->slots = solveProgramSlots(members, shape);
  facts->boundaries = programBoundaries(members);
  programFacts = std::move(facts);
}

const core::Ledger *ProgramAnalysis::ledgerOf(std::size_t index) const {
  return index < units.size() ? units[index].ledger.get() : nullptr;
}

const record::InterfaceFacts *
ProgramAnalysis::interfaceOf(std::size_t index) const {
  return index < units.size() ? units[index].interface.get() : nullptr;
}

const analysis::UnitExports *
ProgramAnalysis::exportsOf(std::size_t index) const {
  return index < units.size() && units[index].exports ? &*units[index].exports
                                                      : nullptr;
}

const std::set<ReportedDiagnostic> &
ProgramAnalysis::reportedOf(std::size_t index) const {
  static const std::set<ReportedDiagnostic> None;
  return index < units.size() ? units[index].reported : None;
}

std::string ProgramAnalysis::unitName(std::size_t index) const {
  return index < units.size() ? units[index].unit->name() : std::string();
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

bool CompilationDatabaseUnit::releaseAST() {
  if (asts.empty())
    return false;
  preparation->functions.clear();
  // ClangTool fills this vector in uninstrumented LLVM. Discard its backing
  // storage too, so reparsing cannot reuse ASan-poisoned spare capacity.
  decltype(asts){}.swap(asts);
  attemptedParse = false;
  multipleCommands = false;
  return true;
}

} // namespace weavec::frontend
