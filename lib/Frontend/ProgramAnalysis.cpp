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
#include "weavec/Core/EffectsIO.h"
#include "weavec/Core/Scc.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/LedgerOutput.h"
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
    : options(std::move(opts)),
      // A unit may run again after it reported (to serve a context): its
      // summary line is its last run's, printed when the program is done.
      unitSummaries(options.ledgerOutput.printsSummary()) {
  options.ledgerOutput.summary = false;
}

void ProgramAnalysis::addUnit(std::unique_ptr<ProgramUnit> unit,
                              std::optional<analysis::UnitExports> known,
                              std::set<ReportedDiagnostic> reported) {
  units.push_back(Unit{.unit = std::move(unit),
                       .exports = std::move(known),
                       .reported = std::move(reported)});
}

void ProgramAnalysis::addServingUnit(std::unique_ptr<ProgramUnit> unit,
                                     analysis::UnitExports known,
                                     std::set<ReportedDiagnostic> reported) {
  units.push_back(Unit{.unit = std::move(unit),
                       .exports = std::move(known),
                       .reported = std::move(reported),
                       .dormant = true});
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
    if (oldest->releaseAST() && options.engine.stats)
      options.engine.stats->add("unit_evictions");
  }
}

std::optional<UnitResult>
ProgramAnalysis::runUnit(ProgramUnit &unit, const FrontendOptions &overrides) {
  FrontendOptions run = options;
  run.database = overrides.database;
  run.alreadyReported = overrides.alreadyReported;
  run.onlyIds = overrides.onlyIds;
  run.silent = overrides.silent;
  run.holdFor = overrides.holdFor;
  run.discoverOnly = overrides.discoverOnly;
  run.collectInterface = overrides.collectInterface;
  if (run.silent)
    run.engine.dumpStream = nullptr;

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
  if (!writeAnalysisStats(run.analysisStatsPath, run.engine.stats, false))
    return std::nullopt;
  return result;
}

/// Exports with every summary at the bottom: the start of a fixpoint.
static analysis::UnitExports skeleton(const analysis::UnitExports &exports) {
  analysis::UnitExports result = exports;
  for (auto &[name, function] : result.functions)
    function.effects = core::FunctionEffects{};
  result.unknownCallees.clear();
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

bool ProgramAnalysis::exhausted() const {
  return budgetSeconds > 0 && std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - started)
                                      .count() > budgetSeconds;
}

void ProgramAnalysis::analyzeAcyclic(unsigned index, Result &result) {
  Unit &unit = units[index];
  // RFC 0033 §7: past the budget the compile-time view stands.
  if (exhausted()) {
    result.unfinished.push_back(unit.unit->name());
    if (unit.exports)
      settled.add(*unit.exports);
    return;
  }
  announce(options.engine.dumpStream, *unit.unit);

  FrontendOptions overrides;
  overrides.database = &settled;
  overrides.alreadyReported = &unit.reported;
  overrides.collectInterface = interfaces;
  overrides.holdFor = holdForUnserved();
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
  settle(unit, std::move(*run));
}

void ProgramAnalysis::settle(Unit &unit, UnitResult run) const {
  unit.exports = std::move(run.exports);
  unit.held = run.held;
  unit.reported.insert(run.reported.begin(), run.reported.end());
  // RFC 0030 §2.6: only the last reporting run publishes.
  if (run.ledger && (ledgers || interfaces || unitSummaries))
    unit.ledger = std::make_shared<const core::Ledger>(run.ledger->ledger);
  if (run.interface)
    unit.interface = std::move(run.interface);
}

void ProgramAnalysis::widen(analysis::UnitExports &exports,
                            const analysis::UnitExports &previous) {
  // Summaries join by global id, which means the same only under one table.
  if (!(exports.globals == previous.globals))
    return;
  for (auto &[name, function] : exports.functions)
    if (const auto before = previous.functions.find(name);
        before != previous.functions.end())
      function.effects =
          core::joinEffects(function.effects, before->second.effects);
  exports.countFields.insert(previous.countFields.begin(),
                             previous.countFields.end());
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
  // is rebuilt only after some member's exports changed.
  analysis::ProgramDatabase db = databaseFor(current);

  // RFC 0010, *Whole-program fixpoint*: a member whose inputs did not change
  // since it last ran produces the same exports, so only the members that
  // depend on a changed one are re-run: the unit graph's edges restricted to
  // the group (`imports` and `indirectTypes` against the members'
  // definitions). A member with no dependency inside the group runs once.
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
  bool cut = false;
  for (unsigned round = 0; round < MaxRounds && changed && !cut; ++round) {
    if (options.engine.stats)
      options.engine.stats->add("program_fixpoint_rounds");
    changed = false;
    for (const unsigned k : schedule) {
      if (broken[k] || !dirty[k])
        continue;
      if (exhausted()) {
        cut = true;
        break;
      }
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
      analysis::UnitExports exports = std::move(run->exports);
      // RFC 0011, *Whole-program widening*: a group that oscillates (a
      // must-fact one member drops makes another add one back) is joined
      // towards what every round agreed on; `join` only ever weakens.
      if (round >= WidenAfter)
        widen(exports, current[k]);
      // (The contexts are served after the fixpoint, `serveContexts`.)
      if (!exports.sameFunctionsAs(current[k])) {
        changed = true;
        stale = true;
        for (const unsigned dependent : dependents[k])
          if (!broken[dependent])
            dirty[dependent] = true;
        current[k] = std::move(exports);
      }
    }
  }
  if (changed && !cut) {
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
    // RFC 0033 §7: a group the budget cut keeps its compile-time view.
    if (cut || exhausted()) {
      broken[k] = true;
      current[k] = *unit.exports;
      result.unfinished.push_back(unit.unit->name());
      continue;
    }
    announce(options.engine.dumpStream, *unit.unit);
    FrontendOptions overrides;
    overrides.database = &db;
    overrides.alreadyReported = &unit.reported;
    overrides.collectInterface = interfaces;
    overrides.holdFor = holdForUnserved();
    std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
    if (!run) {
      broken[k] = true;
      result.failed.push_back(unit.unit->name());
      continue;
    }
    result.errors += run->errors;
    result.warnings += run->warnings;
    settle(unit, std::move(*run));
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
  // (A serving unit calls into no other unit: a component of its own.)
  if (component.size() == 1 && units[component.front()].dormant) {
    settled.add(*units[component.front()].exports);
    return;
  }
  if (component.size() == 1)
    analyzeAcyclic(component.front(), result);
  else
    analyzeCyclic(component, result);
}

ProgramAnalysis::Result ProgramAnalysis::run() {
  Result result;
  started = std::chrono::steady_clock::now();
  settled.clear();
  settled.programFacts = programFacts;
  attempted.clear();
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
  boundedRetention = options.engine.dumpStream == nullptr;
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
  serveContexts(result);
  if (unitSummaries) {
    LedgerOutputOptions summaryOnly = options.ledgerOutput;
    summaryOnly.path.clear();
    summaryOnly.summary = true;
    for (const Unit &unit : units) {
      if (!unit.ledger || unit.ledger->units.empty())
        continue;
      core::Ledger ledger = *unit.ledger;
      (void)emitUnitLedger(ledger,
                           UnitIdentity{.source = ledger.units.front().source},
                           options.config, summaryOnly);
    }
  }

  if (llvm::raw_ostream *dump = options.engine.dumpStream) {
    settled.dump(*dump);
    if (programFacts)
      dumpProgramSlots(programFacts->slots, *dump);
  }
  return result;
}

/// Whether `exports` defines the function named `portable`.
static bool definesName(const analysis::UnitExports &exports,
                        const std::string &portable) {
  return std::ranges::any_of(exports.functions, [&](const auto &entry) {
    const auto &[name, function] = entry;
    return (function.external && name == portable) ||
           (!function.external && exports.source + "#" + name == portable);
  });
}

bool ProgramAnalysis::runsDefinitionOf(const std::string &portable) const {
  return std::ranges::any_of(units, [&](const Unit &unit) {
    return unit.exports && definesName(*unit.exports, portable);
  });
}

std::function<bool(const analysis::ContextRequest &)>
ProgramAnalysis::holdForUnserved() const {
  return [this](const analysis::ContextRequest &request) {
    return settled.contextEffects(request) == nullptr &&
           !attempted.contains(request) && runsDefinitionOf(request.callee);
  };
}

void ProgramAnalysis::serveContexts(Result &result) {
  static constexpr unsigned MaxContextRounds = 8;
  // The database of every unit's newest exports.
  auto rebuild = [&] {
    settled.clear();
    settled.programFacts = programFacts;
    for (const analysis::UnitExports &exports : fixed)
      settled.add(exports);
    for (const Unit &unit : units)
      if (unit.exports)
        settled.add(*unit.exports);
  };
  auto reportingRun = [&](unsigned index, bool hold) -> std::optional<bool> {
    rebuild();
    Unit &unit = units[index];
    announce(options.engine.dumpStream, *unit.unit);
    FrontendOptions overrides;
    overrides.database = &settled;
    overrides.alreadyReported = &unit.reported;
    overrides.collectInterface = interfaces;
    if (hold)
      overrides.holdFor = holdForUnserved();
    std::optional<UnitResult> run = runUnit(*unit.unit, overrides);
    if (!run) {
      result.failed.push_back(unit.unit->name());
      return std::nullopt;
    }
    result.errors += run->errors;
    result.warnings += run->warnings;
    const analysis::UnitExports before = std::move(*unit.exports);
    unit.dormant = false;
    settle(unit, std::move(*run));
    return !unit.exports->sameSummariesAs(before);
  };
  const std::vector<std::vector<unsigned>> adjacency = unitGraph();
  std::vector<std::vector<unsigned>> dependents(units.size());
  for (unsigned i = 0; i < units.size(); ++i)
    for (const unsigned definer : adjacency[i])
      dependents[definer].push_back(i);
  const std::vector<std::vector<unsigned>> order =
      core::stronglyConnectedComponents(adjacency);
  std::set<unsigned> pending;
  for (unsigned round = 0; round < MaxContextRounds && !exhausted(); ++round) {
    rebuild();
    // Requests no unit has served yet: their definers run, once per
    // request (one the definer cannot serve stays unserved).
    for (const analysis::ContextRequest &request : settled.requests())
      if (settled.contextEffects(request) == nullptr &&
          attempted.insert(request).second)
        for (unsigned i = 0; i < units.size(); ++i)
          if (units[i].exports &&
              definesName(*units[i].exports, request.callee))
            pending.insert(i);
    if (pending.empty())
      break;
    std::set<unsigned> next;
    for (const std::vector<unsigned> &component : order)
      for (const unsigned index : component) {
        if (!pending.contains(index) || !units[index].exports || exhausted())
          continue;
        const std::optional<bool> changed = reportingRun(index, true);
        // Its callers use the contexts it now serves, or its new summaries:
        // those still held report, the others see what changed.
        if (changed && *changed)
          for (const unsigned dependent : dependents[index])
            next.insert(dependent);
      }
    pending = std::move(next);
  }
  // Every unit still held reports now, with whatever is served.
  for (const std::vector<unsigned> &component : order)
    for (const unsigned index : component)
      if (units[index].held && units[index].exports) {
        // RFC 0033 §7: past the budget its compile-time results stand.
        if (exhausted()) {
          result.unfinished.push_back(units[index].unit->name());
          units[index].ledger.reset();
          continue;
        }
        (void)reportingRun(index, false);
      }
  rebuild();
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
    core::AnalysisTimer timer(options.engine.stats, "parsing");
    clang::tooling::ClangTool tool(compilations, {source});
    for (const auto &adjuster : adjusters)
      tool.appendArgumentsAdjuster(adjuster);
    if (options.engine.stats)
      options.engine.stats->add("unit_parses");
    // (A unit that failed to parse is not analysed: see
    // `analyzeTranslationUnit`.)
    if (tool.buildASTs(asts) != 0 || asts.empty() ||
        llvm::any_of(asts, [](const std::unique_ptr<clang::ASTUnit> &ast) {
          return ast->getDiagnostics().hasUncompilableErrorOccurred();
        })) {
      asts.clear();
      return false;
    }
    multipleCommands = asts.size() != 1;
    if (multipleCommands)
      asts.clear();
  } else if (options.engine.stats) {
    options.engine.stats->add("unit_reuses");
  }
  if (multipleCommands)
    return ProgramUnit::analyze(options);
  if (asts.empty())
    return false;
  auto &ast = *asts.front();
  auto result = analyzeRetainedUnit(ast, options);
  if (options.onResult)
    options.onResult(std::move(result));
  return true;
}

bool CompilationDatabaseUnit::releaseAST() {
  if (asts.empty())
    return false;
  // ClangTool fills this vector in uninstrumented LLVM. Discard its backing
  // storage too, so reparsing cannot reuse ASan-poisoned spare capacity.
  decltype(asts){}.swap(asts);
  attemptedParse = false;
  multipleCommands = false;
  return true;
}

} // namespace weavec::frontend
