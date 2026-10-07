//===- EngineUnit.cpp - The object engine's unit driver -------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §3: the unit's call graph bottom up, summaries to a fixpoint per
// strongly connected component (publishing into a discarding adapter), then
// one authoritative pass per reported function.
//
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/Effects.h"
#include "weavec/Core/Scc.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>

using namespace clang;

namespace weavec::analysis {

//===----------------------------------------------------------------------===//
// ObjectEngine
//===----------------------------------------------------------------------===//

ObjectEngine::ObjectEngine() = default;
ObjectEngine::~ObjectEngine() = default;

void ObjectEngine::analyzeUnit(const EngineInput &engineInput,
                               LedgerAdapter &out) {
  input = std::make_unique<EngineInput>(engineInput);
  unit = std::make_unique<engine::UnitRun>(*input, out);
  const SiteIndex &sites = engineInput.sites;
  const SourceManager &sm = engineInput.context.getSourceManager();
  if (engineInput.options.shouldReport)
    unit->analyzeAll(engineInput.options.shouldReport);
  else
    unit->analyzeAll([&sites, &sm](const FunctionDecl &function) {
      return sites.function(function) != nullptr ||
             sm.isInMainFile(sm.getExpansionLoc(function.getLocation()));
    });
  exported = unit->exports();
}

UnitExports ObjectEngine::exports() {
  return std::move(exported);
}

const core::FunctionEffects *
ObjectEngine::summaryOf(const FunctionDecl &function) const {
  if (!unit)
    return nullptr;
  auto it = unit->summaries.find(function.getCanonicalDecl());
  return it != unit->summaries.end() ? &it->second : nullptr;
}

UnitExports ObjectEngine::discover(const EngineInput &input) {
  LedgerAdapter discarding(input.context, LedgerAdapter::Mode::Discarding);
  return engine::UnitRun(input, discarding).exports();
}

void ObjectEngine::dump(const FunctionDecl &function, llvm::raw_ostream &os) {
  // `--dump-analysis` (unstable): the function's summary, and with
  // WEAVEC_ENGINE_DUMP=3 its block states too.
  os << "function '" << function.getNameAsString() << "':\n";
  if (const core::FunctionEffects *effects = summaryOf(function)) {
    os << "  summary:\n";
    std::string text = core::toText(*effects);
    for (llvm::StringRef line :
         llvm::split(llvm::StringRef(text).rtrim('\n'), '\n'))
      if (!line.empty())
        os << "  " << line << '\n';
  }
  if (!unit)
    return;
  const char *level = std::getenv("WEAVEC_ENGINE_DUMP");
  if (level != nullptr && std::string_view(level) == "3")
    unit->dump(function, os);
}

namespace engine {

//===----------------------------------------------------------------------===//
// UnitRun
//===----------------------------------------------------------------------===//

/// RFC 0033 §9, RFC 0034 §7.1: a unit's default budget of work, a share
/// per site (its analysis costs a bounded multiple of compiling it).
static constexpr std::uint64_t MinUnitBudget = 20000000;
static constexpr std::uint64_t UnitBudgetPerSite = 400;

UnitRun::UnitRun(const EngineInput &engineInput, LedgerAdapter &adapter)
    : input(engineInput), authoritative(adapter),
      discarding(engineInput.context, LedgerAdapter::Mode::Discarding) {
  if (input.options.unitBudget) {
    unitBudget = *input.options.unitBudget;
  } else {
    std::uint64_t sites = 0;
    for (const SiteIndex::FunctionSites &function : input.sites.functions())
      sites += function.sites.size();
    unitBudget = std::max(MinUnitBudget, UnitBudgetPerSite * sites);
  }
}

std::uint32_t UnitRun::globalId(const VarDecl &var) {
  return internGlobal(var);
}

std::uint32_t UnitRun::internGlobal(const VarDecl &var) const {
  const VarDecl *canonical = var.getCanonicalDecl();
  if (auto it = globalIds.find(canonical); it != globalIds.end())
    return it->second;
  auto id = static_cast<std::uint32_t>(globalList.size());
  globalList.push_back(canonical);
  globalIds[canonical] = id;
  return id;
}

const VarDecl *UnitRun::globalDecl(std::uint32_t id) const {
  return id < globalList.size() ? globalList[id] : nullptr;
}

bool UnitRun::hasBody(const FunctionDecl &callee) const {
  const FunctionDecl *definition = nullptr;
  if (!callee.hasBody(definition) || definition == nullptr)
    return false;
  const SourceManager &sm = context().getSourceManager();
  return !sm.isInSystemHeader(sm.getExpansionLoc(definition->getLocation()));
}

const core::FunctionEffects *
UnitRun::summaryOf(const FunctionDecl &callee) const {
  auto it = summaries.find(callee.getCanonicalDecl());
  if (it != summaries.end())
    return &it->second;
  // §7: a function another unit defines, in `weavec --whole-program`.
  if (input.database == nullptr || hasBody(callee) ||
      !callee.isExternallyVisible() || callee.getIdentifier() == nullptr)
    return nullptr;
  std::string name = callee.getNameAsString();
  if (auto found = imported.find(name); found != imported.end())
    return &found->second;
  const core::FunctionEffects *effects = input.database->findEffects(name);
  if (effects == nullptr)
    return nullptr;
  return importEffects(name, *effects);
}

const core::FunctionEffects *
UnitRun::importEffects(const std::string &key,
                       const core::FunctionEffects &effects) const {
  if (auto found = imported.find(key); found != imported.end())
    return &found->second;
  std::string name = key;
  if (!globalsByName) {
    globalsByName.emplace();
    for (const Decl *decl : context().getTranslationUnitDecl()->decls())
      if (const auto *var = dyn_cast<VarDecl>(decl);
          var != nullptr && var->hasGlobalStorage())
        globalsByName->try_emplace(portableName(*var), var->getCanonicalDecl());
  }
  const GlobalNames &names = input.database->globals();
  core::FunctionEffects local = core::renumberGlobals(
      effects, [&](std::uint32_t id) -> std::optional<std::uint32_t> {
        if (id >= names.size())
          return std::nullopt;
        auto var = globalsByName->find(names.nameOf(id).str());
        if (var == globalsByName->end())
          return std::nullopt;
        return internGlobal(*var->second);
      });
  if (std::getenv("WEAVEC_ENGINE_DUMP") != nullptr)
    llvm::errs() << "imported " << name << "\n" << core::toText(local);
  return &imported.emplace(std::move(name), std::move(local)).first->second;
}

std::string UnitRun::portableName(const VarDecl &var) const {
  if (var.isExternallyVisible())
    return var.getNameAsString();
  const SourceManager &sm = context().getSourceManager();
  std::string source;
  if (const auto entry = sm.getFileEntryRefForID(sm.getMainFileID()))
    source = entry->getName().str();
  return source + "#" + var.getNameAsString();
}

namespace {
/// The functions a body calls: directly, or through a slot whose solution
/// names them (§3 step 1).
class CalleeCollector : public RecursiveASTVisitor<CalleeCollector> {
public:
  explicit CalleeCollector(const EngineInput &input) : input(input) {}
  std::set<const FunctionDecl *> callees;
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
  // RecursiveASTVisitor's CRTP hooks are found by name.
  bool VisitCallExpr(CallExpr *call) {
    if (const FunctionDecl *callee = call->getDirectCallee()) {
      callees.insert(callee->getCanonicalDecl());
      return true;
    }
    const core::SlotSolution *solution = input.slotSolution;
    if (input.database != nullptr && input.database->programFacts)
      solution = &input.database->programFacts->slots;
    if (input.slotCollection == nullptr || solution == nullptr)
      return true;
    if (auto slot = input.slotCollection->calleeSlot(*call))
      for (const std::string &name : solution->resolveCall(*slot).targets)
        if (const FunctionDecl *fn = input.slotCollection->function(name))
          callees.insert(fn->getCanonicalDecl());
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *ref) {
    // A function whose address is taken is a possible callee of the
    // indirect calls (ordered before its takers, which are few).
    if (const auto *fn = dyn_cast<FunctionDecl>(ref->getDecl()))
      callees.insert(fn->getCanonicalDecl());
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  const EngineInput &input;
};

/// §4.5 D2, RFC 0030 §9.4: the fields and globals a value released by the
/// unit is loaded from.
class OwningCollector : public RecursiveASTVisitor<OwningCollector> {
public:
  OwningCollector(const core::LibrarySpec &library, const KindTable &kinds,
                  llvm::DenseSet<const Decl *> &slots)
      : library(library), kinds(kinds), slots(slots) {}

  // (The function whose body is being visited.)
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
  // RecursiveASTVisitor's CRTP hooks are found by name.
  bool TraverseFunctionDecl(FunctionDecl *function) {
    const FunctionDecl *outer = enclosing;
    enclosing = function->doesThisDeclarationHaveABody() ? function : outer;
    bool result =
        RecursiveASTVisitor<OwningCollector>::TraverseFunctionDecl(function);
    enclosing = outer;
    return result;
  }
  bool VisitVarDecl(VarDecl *var) {
    if (const Expr *init = var->getInit())
      noteSource(*var, *init);
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *op) {
    if (op->getOpcode() != BO_Assign)
      return true;
    if (const auto *ref =
            dyn_cast<DeclRefExpr>(op->getLHS()->IgnoreParenImpCasts()))
      if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
        noteSource(*var, *op->getRHS());
    return true;
  }
  bool VisitCallExpr(CallExpr *call) {
    const FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr)
      return true;
    std::vector<unsigned> released;
    if (auto match = governingLibraryEntry(*callee, library)) {
      for (unsigned i = 0; i < call->getNumArgs(); ++i)
        if (const core::LibraryParam *param = match->param(i);
            param != nullptr &&
            (param->effect == core::LibraryParam::Effect::Release ||
             param->effect == core::LibraryParam::Effect::Realloc))
          released.push_back(i);
    } else if (const OwnershipContract *contract = kinds.ownership(*callee)) {
      for (const auto &argument : contract->arguments)
        if (!argument.retains)
          released.push_back(argument.index);
    }
    for (unsigned i : released)
      if (i < call->getNumArgs())
        noteRelease(*call->getArg(i));
    // A function of the unit may release what it is handed: decided once
    // every body is seen (`finish`).
    if (released.empty() && callee->hasBody())
      for (unsigned i = 0; i < call->getNumArgs(); ++i)
        if (call->getArg(i)->getType()->isPointerType())
          handed.push_back(Handed{.callee = callee->getCanonicalDecl(),
                                  .index = i,
                                  .arg = call->getArg(i),
                                  .caller = enclosing});
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  void finish() {
    // RFC 0030 §9.4: a slot is owning when some function releases a value
    // loaded from it, itself or through a function it hands the value to
    // that releases its parameter (`free_tree(t->left)`): the releasing
    // parameters, to a fixpoint.
    for (bool changed = true; changed;) {
      changed = false;
      for (const Handed &call : handed)
        if (releasing.contains({call.callee, call.index}) && !call.counted) {
          call.counted = true;
          const FunctionDecl *outer = enclosing;
          enclosing = call.caller;
          noteRelease(*call.arg);
          enclosing = outer;
          changed = true;
        }
      for (const auto &[function, param] : pendingParams)
        if (releasing.insert({function, param}).second)
          changed = true;
      pendingParams.clear();
    }
    for (const Expr *expr : releasedExprs)
      noteReleased(*expr, 0);
  }

private:
  const core::LibrarySpec &library;
  const KindTable &kinds;
  llvm::DenseSet<const Decl *> &slots;
  std::vector<const Expr *> releasedExprs;
  /// A pointer argument handed to a function of the unit.
  struct Handed {
    const FunctionDecl *callee;
    unsigned index;
    const Expr *arg;
    const FunctionDecl *caller;
    mutable bool counted = false;
  };
  std::vector<Handed> handed;
  /// The parameters (function, index) the unit's functions release.
  std::set<std::pair<const FunctionDecl *, unsigned>> releasing;
  std::vector<std::pair<const FunctionDecl *, unsigned>> pendingParams;
  const FunctionDecl *enclosing = nullptr;

  /// `expr` is released: its slots are owning, and a parameter of the
  /// function around it that it is makes that parameter releasing.
  void noteRelease(const Expr &expr) {
    releasedExprs.push_back(&expr);
    if (enclosing == nullptr)
      return;
    if (const auto *ref = dyn_cast<DeclRefExpr>(expr.IgnoreParenCasts()))
      if (const auto *param = dyn_cast<ParmVarDecl>(ref->getDecl()))
        pendingParams.emplace_back(enclosing->getCanonicalDecl(),
                                   param->getFunctionScopeIndex());
  }
  llvm::DenseMap<const VarDecl *, llvm::SmallVector<const Decl *, 2>> sources;

  static const Decl *slotOf(const Expr &expr) {
    const Expr *stripped = expr.IgnoreParenCasts();
    if (const auto *member = dyn_cast<MemberExpr>(stripped))
      return dyn_cast<FieldDecl>(member->getMemberDecl());
    if (const auto *ref = dyn_cast<DeclRefExpr>(stripped))
      if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
          var != nullptr && var->hasGlobalStorage())
        return var;
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(stripped))
      return slotOf(*subscript->getBase());
    return nullptr;
  }
  void noteSource(const VarDecl &var, const Expr &value) {
    if (const Decl *slot = slotOf(value))
      sources[&var].push_back(slot->getCanonicalDecl());
    else if (const auto *ref = dyn_cast<DeclRefExpr>(value.IgnoreParenCasts()))
      if (const auto *from = dyn_cast<VarDecl>(ref->getDecl()))
        sources[&var].push_back(from);
  }
  void noteReleased(const Expr &expr, int depth) {
    if (depth > 4)
      return;
    if (const Decl *slot = slotOf(expr)) {
      slots.insert(slot->getCanonicalDecl());
      return;
    }
    if (const auto *ref = dyn_cast<DeclRefExpr>(expr.IgnoreParenCasts()))
      if (const auto *var = dyn_cast<VarDecl>(ref->getDecl()))
        noteVar(*var, depth);
  }
  void noteVar(const VarDecl &var, int depth) {
    // (Locals copied into each other in a loop: each is followed once.)
    if (depth > MaxCopyDepth || !followed.insert(&var).second)
      return;
    auto it = sources.find(&var);
    if (it == sources.end())
      return;
    for (const Decl *source : it->second) {
      if (const auto *from = dyn_cast<VarDecl>(source);
          from != nullptr && !from->hasGlobalStorage()) {
        noteVar(*from, depth + 1);
        continue;
      }
      slots.insert(source);
    }
  }
  static constexpr int MaxCopyDepth = 64;
  llvm::DenseSet<const VarDecl *> followed;
};

/// RFC 0031 §4.6: the unit's references to variables with static storage
/// and internal linkage, and those of them that only read a scalar value
/// (`table[1]`, `s.f` loaded) or sit in an unevaluated operand. A variable
/// no other reference reaches holds its initializer for the whole run.
class GlobalReads : public RecursiveASTVisitor<GlobalReads> {
public:
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-convert-member-functions-to-static):
  // RecursiveASTVisitor's CRTP hooks are found by name.
  bool VisitDeclRefExpr(DeclRefExpr *ref) {
    if (const auto *var = dyn_cast<VarDecl>(ref->getDecl());
        var != nullptr && var->hasGlobalStorage())
      references.push_back(ref);
    return true;
  }
  bool VisitImplicitCastExpr(ImplicitCastExpr *cast) {
    if (cast->getCastKind() != CK_LValueToRValue)
      return true;
    // Down through subscripts of arrays (not of pointers) and `.` members.
    const Expr *lvalue = cast->getSubExpr()->IgnoreParens();
    while (true) {
      if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(lvalue)) {
        const auto *decay =
            dyn_cast<ImplicitCastExpr>(subscript->getBase()->IgnoreParens());
        if (decay == nullptr || decay->getCastKind() != CK_ArrayToPointerDecay)
          return true;
        lvalue = decay->getSubExpr()->IgnoreParens();
        continue;
      }
      if (const auto *member = dyn_cast<MemberExpr>(lvalue);
          member != nullptr && !member->isArrow()) {
        lvalue = member->getBase()->IgnoreParens();
        continue;
      }
      break;
    }
    if (const auto *ref = dyn_cast<DeclRefExpr>(lvalue))
      reads.insert(ref);
    return true;
  }
  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *) {
    return true; // Unevaluated (a variable-length operand has no global).
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-convert-member-functions-to-static)

  std::vector<const DeclRefExpr *> references;
  llvm::DenseSet<const DeclRefExpr *> reads;
};
} // namespace

bool UnitRun::keepsInitializer(const VarDecl &var) const {
  if (!initializerOnly) {
    GlobalReads collector;
    collector.TraverseDecl(context().getTranslationUnitDecl());
    llvm::DenseSet<const VarDecl *> written;
    for (const DeclRefExpr *ref : collector.references)
      if (!collector.reads.contains(ref))
        written.insert(cast<VarDecl>(ref->getDecl())->getCanonicalDecl());
    initializerOnly.emplace();
    for (const DeclRefExpr *ref : collector.references) {
      const auto *read = cast<VarDecl>(ref->getDecl())->getCanonicalDecl();
      if (!written.contains(read))
        initializerOnly->insert(read);
    }
  }
  const VarDecl *canonical = var.getCanonicalDecl();
  return !canonical->isExternallyVisible() &&
         !canonical->getType().isVolatileQualified() &&
         initializerOnly->contains(canonical);
}

void UnitRun::computeOwningSlots() {
  OwningCollector collector(library(), input.kinds, owningSlots);
  collector.TraverseDecl(context().getTranslationUnitDecl());
  collector.finish();
}

/// `component`'s members in a postorder of the call edges between them.
static std::vector<unsigned>
calleesFirst(const std::vector<unsigned> &component,
             const std::vector<std::vector<unsigned>> &adjacency) {
  llvm::DenseSet<unsigned> members(component.begin(), component.end());
  llvm::DenseSet<unsigned> seen;
  std::vector<unsigned> order;
  for (unsigned root : component) {
    if (!seen.insert(root).second)
      continue;
    std::vector<std::pair<unsigned, std::size_t>> frames{{root, 0}};
    while (!frames.empty()) {
      auto &[node, next] = frames.back();
      if (next < adjacency[node].size()) {
        unsigned callee = adjacency[node][next++];
        if (members.contains(callee) && seen.insert(callee).second)
          frames.emplace_back(callee, 0);
        continue;
      }
      order.push_back(node);
      frames.pop_back();
    }
  }
  return order;
}

void UnitRun::analyzeAll(
    const std::function<bool(const FunctionDecl &)> &shouldReport) {
  computeOwningSlots();
  // The unit's definitions.
  std::vector<const FunctionDecl *> definitions;
  llvm::DenseMap<const FunctionDecl *, unsigned> index;
  const SourceManager &sm = context().getSourceManager();
  for (const Decl *decl : context().getTranslationUnitDecl()->decls()) {
    const auto *fn = dyn_cast<FunctionDecl>(decl);
    if (fn == nullptr || !fn->doesThisDeclarationHaveABody())
      continue;
    if (sm.isInSystemHeader(sm.getExpansionLoc(fn->getLocation())))
      continue;
    const FunctionDecl *canonical = fn->getCanonicalDecl();
    if (index.contains(canonical))
      continue;
    index[canonical] = static_cast<unsigned>(definitions.size());
    definitions.push_back(fn);
  }
  // The call graph and its components, callees first.
  std::vector<std::vector<unsigned>> adjacency(definitions.size());
  for (unsigned i = 0; i < definitions.size(); ++i) {
    CalleeCollector collector(input);
    collector.TraverseStmt(definitions[i]->getBody());
    for (const FunctionDecl *callee : collector.callees)
      if (auto it = index.find(callee); it != index.end())
        adjacency[i].push_back(it->second);
  }
  std::vector<std::vector<unsigned>> components =
      core::stronglyConnectedComponents(adjacency);
  functionsLeft = std::max<std::uint64_t>(1, definitions.size());
  for (std::vector<unsigned> &component : components) {
    // A component's members run callees first (a postorder of its own
    // edges), so a round sees the summaries its callees made in it rather
    // than the last round's, and fewer rounds settle.
    if (component.size() > 2)
      component = calleesFirst(component, adjacency);
    bool recursive = component.size() > 1;
    if (!recursive)
      for (unsigned callee : adjacency[component.front()])
        recursive = recursive || callee == component.front();
    if (recursive) {
      // Summary rounds to a fixpoint (§3 step 2).
      for (unsigned member : component) {
        summaries[definitions[member]->getCanonicalDecl()] = {};
        unsettled.insert(definitions[member]->getCanonicalDecl());
      }
      // RFC 0031 §6.4: after three rounds a summary that still changes
      // widens (joined with the last round's, its moving integer bounds
      // dropped); after eight the rest are incomplete.
      static constexpr int WidenFrom = 3;
      static constexpr int MaxRounds = 8;
      // A member runs again only when a summary it calls changed since its
      // last run: its summary is a function of theirs.
      llvm::DenseMap<unsigned, std::vector<unsigned>> callers;
      llvm::DenseSet<unsigned> members(component.begin(), component.end());
      for (unsigned member : component)
        for (unsigned callee : adjacency[member])
          if (members.contains(callee))
            callers[callee].push_back(member);
      llvm::DenseSet<unsigned> dirty = members;
      // A summary that says unknown code may write any global has callers
      // forget them all (§5.1: what they hold, and what they reach may be
      // released), so its writes to one global say nothing more, except to
      // the C library's own, which forgetting keeps; inside a component
      // they climb a call edge a round and keep it from settling.
      auto dropCoveredGlobals = [&](core::FunctionEffects &effects) {
        if (!effects.unknownGlobals)
          return;
        auto covered = [&](const core::SummaryPath &path) {
          if (!path.isGlobal())
            return false;
          const VarDecl *var = globalDecl(path.index);
          return var != nullptr &&
                 !sm.isInSystemHeader(sm.getExpansionLoc(var->getLocation()));
        };
        std::erase_if(effects.effects, [&](const core::PathEffect &effect) {
          return effect.kind == core::PathEffect::Kind::Unknown &&
                 covered(effect.path);
        });
        std::erase_if(effects.stores, [&](const core::StoreEffect &store) {
          return covered(store.dest);
        });
      };
      for (int round = 0; round < MaxRounds && !dirty.empty(); ++round) {
        for (unsigned member : component) {
          if (!dirty.erase(member))
            continue;
          const FunctionDecl *fn = definitions[member];
          // (One that spent the per-function budget keeps its incomplete
          // summary: run again, it spends it again. A run cut short by its
          // share of the unit's budget may get a larger share later.)
          if (exhausted.contains(fn->getCanonicalDecl()))
            continue;
          FunctionRun run(*this, *fn, discarding, RunMode::Summary);
          RunResult result = run.run();
          if (result.spentBudget)
            exhausted.insert(fn->getCanonicalDecl());
          core::FunctionEffects &slot = summaries[fn->getCanonicalDecl()];
          dropCoveredGlobals(result.effects);
          core::FunctionEffects next =
              round >= WidenFrom ? core::widenEffects(slot, result.effects)
                                 : std::move(result.effects);
          if (!(next == slot)) {
            slot = std::move(next);
            for (unsigned caller : callers[member])
              dirty.insert(caller);
          }
        }
      }
      const bool stable = dirty.empty();
      // (One that did not settle may return even where its last round
      // said it never does.)
      if (!stable)
        for (unsigned member : component) {
          core::FunctionEffects &slot =
              summaries[definitions[member]->getCanonicalDecl()];
          slot.incomplete = "the recursive summaries did not converge";
          if (slot.returns == core::FunctionEffects::Returns::Never)
            slot.returns = core::FunctionEffects::Returns::May;
        }
    }
    // The authoritative pass of each member; its summary stands for the
    // members outside a cycle (§3 step 3).
    for (unsigned member : component) {
      const FunctionDecl *fn = definitions[member];
      bool report = shouldReport(*fn);
      if (report)
        authoritative.beginFunction(*fn);
      // A member whose summary round spent the per-function budget spends
      // it in its authoritative pass too, which then publishes nothing: it
      // is over budget without that run.
      RunResult result;
      if (exhausted.contains(fn->getCanonicalDecl())) {
        result.overBudget = true;
        result.transfers = std::numeric_limits<std::uint64_t>::max();
        result.effects = summaries[fn->getCanonicalDecl()];
      } else {
        FunctionRun run(*this, *fn, report ? authoritative : discarding,
                        report ? RunMode::Authoritative : RunMode::Summary);
        result = run.run();
      }
      if (functionsLeft > 1)
        --functionsLeft;
      transfersOf[fn->getCanonicalDecl()] = result.transfers;
      workOf[fn->getCanonicalDecl()] =
          result.overBudget ? std::numeric_limits<std::uint64_t>::max()
                            : result.work;
      if (result.overBudget) {
        overBudget.insert(fn->getCanonicalDecl());
        if (report)
          authoritative.overBudget(*fn);
      }
      if (!recursive)
        summaries[fn->getCanonicalDecl()] = std::move(result.effects);
      if (std::getenv("WEAVEC_ENGINE_DUMP") != nullptr)
        llvm::errs() << "summary " << fn->getNameAsString() << "\n"
                     << core::toText(summaries[fn->getCanonicalDecl()]);
    }
    unsettled.clear();
  }
  // RFC 0030 §7.6: only a record the unit alone can make (defined in its
  // main file) has every store in view; a header's is another unit's too,
  // whose stores only the link step would see (A3, not verified).
  if (input.inferred != nullptr)
    for (const ResolvedCandidate &candidate :
         input.inferred->resolvedCandidates())
      if (candidate.record != nullptr && candidate.pointer != nullptr &&
          candidate.count != nullptr &&
          sm.isInMainFile(sm.getExpansionLoc(candidate.record->getLocation())))
        standing.push_back(&candidate);
  inferInvariants(definitions, shouldReport);
  serveContextRequests(shouldReport);
}

namespace {
/// §7: what a unit's definitions call that it does not define, and which
/// of its functions have their address taken.
class InterfaceCollector : public RecursiveASTVisitor<InterfaceCollector> {
public:
  explicit InterfaceCollector(const ASTContext &context) : context(context) {}
  std::set<const FunctionDecl *> called;
  std::set<const FunctionDecl *> addressTaken;
  std::set<std::string> indirectTypes;

  // Pre-order: a call is visited before the reference that names its
  // callee, so that reference is known to be no address taken.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method):
  // RecursiveASTVisitor's CRTP hooks are found by name.
  bool VisitCallExpr(CallExpr *call) {
    if (const FunctionDecl *callee = call->getDirectCallee()) {
      called.insert(callee->getCanonicalDecl());
      directCallees.insert(call->getCallee()->IgnoreParenImpCasts());
      return true;
    }
    QualType type = call->getCallee()->getType();
    if (type->isPointerType())
      type = type->getPointeeType();
    if (std::string key = functionTypeKey(type, context); !key.empty())
      indirectTypes.insert(std::move(key));
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr *ref) {
    if (const auto *fn = dyn_cast<FunctionDecl>(ref->getDecl());
        fn != nullptr && !directCallees.contains(ref))
      addressTaken.insert(fn->getCanonicalDecl());
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  const ASTContext &context;
  std::set<const Expr *> directCallees;
};
} // namespace

const std::vector<const FunctionDecl *> &
UnitRun::localCandidates(const std::string &typeKey) const {
  if (!candidatesByType) {
    candidatesByType.emplace();
    InterfaceCollector collector(context());
    collector.TraverseDecl(context().getTranslationUnitDecl());
    for (const FunctionDecl *fn : collector.addressTaken)
      if (std::string key = functionTypeKey(fn->getType(), context());
          !key.empty())
        (*candidatesByType)[key].push_back(fn);
  }
  static const std::vector<const FunctionDecl *> None;
  auto it = candidatesByType->find(typeKey);
  return it != candidatesByType->end() ? it->second : None;
}

UnitExports UnitRun::exports() {
  UnitExports exports;
  const SourceManager &sm = context().getSourceManager();
  if (const auto entry = sm.getFileEntryRefForID(sm.getMainFileID()))
    exports.source = entry->getName().str();
  InterfaceCollector collector(context());
  collector.TraverseDecl(context().getTranslationUnitDecl());
  // Globals by the export table's numbering.
  auto toExport = [&](std::uint32_t id) -> std::optional<std::uint32_t> {
    const VarDecl *var = globalDecl(id);
    if (var == nullptr)
      return std::nullopt;
    return exports.globals.idFor(portableName(*var));
  };
  for (const Decl *decl : context().getTranslationUnitDecl()->decls()) {
    const auto *fn = dyn_cast<FunctionDecl>(decl);
    if (fn == nullptr || !fn->doesThisDeclarationHaveABody() || fn->isMain() ||
        fn->getIdentifier() == nullptr ||
        sm.isInSystemHeader(sm.getExpansionLoc(fn->getLocation())))
      continue;
    const bool external = fn->isExternallyVisible();
    const bool taken = collector.addressTaken.contains(fn->getCanonicalDecl());
    if (!external && !taken)
      continue;
    ExportedFunction exported;
    exported.typeKey = functionTypeKey(fn->getType(), context());
    exported.external = external;
    exported.addressTaken = taken;
    if (auto it = summaries.find(fn->getCanonicalDecl()); it != summaries.end())
      exported.effects = core::renumberGlobals(it->second, toExport);
    exports.functions[fn->getNameAsString()] = std::move(exported);
  }
  // What the predefines declare is the check prelude's (RFC 0030 §10.2): the
  // runtime entry points its helpers call are no callee of the program.
  auto inPredefines = [&](const FunctionDecl &fn) {
    return sm.isWrittenInBuiltinFile(sm.getExpansionLoc(fn.getLocation()));
  };
  for (const FunctionDecl *callee : collector.called)
    if (!hasBody(*callee) && callee->isExternallyVisible() &&
        callee->getIdentifier() != nullptr && callee->getBuiltinID() == 0 &&
        !inPredefines(*callee))
      exports.imports.insert(callee->getNameAsString());
  // A function of another unit this one only refers to (a callback it
  // hands out): its summary is what calls through the value apply.
  for (const FunctionDecl *referenced : collector.addressTaken)
    if (!hasBody(*referenced) && referenced->isExternallyVisible() &&
        referenced->getIdentifier() != nullptr &&
        referenced->getBuiltinID() == 0)
      exports.imports.insert(referenced->getNameAsString());
  exports.indirectTypes = std::move(collector.indirectTypes);
  // §7 *Amendment (cross-unit contexts)*.
  exports.contextRequests = contextRequests;
  for (const auto &[request, effects] : servedContexts)
    exports.contextEffects[request] = core::renumberGlobals(effects, toExport);
  return exports;
}

void UnitRun::dump(const FunctionDecl &function, llvm::raw_ostream &os) {
  FunctionRun run(*this, function, discarding, RunMode::Summary);
  (void)run.run();
  run.dump(os);
}

} // namespace engine
} // namespace weavec::analysis
