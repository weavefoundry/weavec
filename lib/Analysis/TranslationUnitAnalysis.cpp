//===- TranslationUnitAnalysis.cpp - Whole-TU driver ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/TranslationUnitAnalysis.h"

#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/Ownership.h"
#include "weavec/Core/Scc.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/TargetInfo.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

namespace {

/// Collects the direct callees of one function body, and the indirect calls
/// whose candidates are resolved against the address-taken set (RFC 0004,
/// *Signatures for function pointers*).
class CalleeCollector : public RecursiveASTVisitor<CalleeCollector> {
public:
  llvm::DenseSet<const FunctionDecl *> callees;
  std::vector<const CallExpr *> indirectCalls;

  // RecursiveASTVisitor's CRTP hooks are found by name; both checks are
  // wrong about it.
  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitDeclRefExpr(DeclRefExpr *ref) {
    if (const auto *function = dyn_cast<FunctionDecl>(ref->getDecl()))
      callees.insert(function->getCanonicalDecl());
    return true;
  }
  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitCallExpr(CallExpr *call) {
    if (const FunctionDecl *callee = call->getDirectCallee())
      callees.insert(callee->getCanonicalDecl());
    else
      indirectCalls.push_back(call);
    return true;
  }
};

/// Collects every function whose name is used as a value rather than called:
/// `&f`, `f` in an initialiser, `hook = f`, `register(f)`.
class AddressTakenCollector
    : public RecursiveASTVisitor<AddressTakenCollector> {
public:
  std::vector<const FunctionDecl *> functions;

  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitCallExpr(CallExpr *call) {
    const auto *ref =
        dyn_cast<DeclRefExpr>(call->getCallee()->IgnoreParenImpCasts());
    if (ref != nullptr && isa<FunctionDecl>(ref->getDecl()))
      calledDirectly.insert(ref);
    return true;
  }

  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitDeclRefExpr(DeclRefExpr *ref) {
    const auto *function = dyn_cast<FunctionDecl>(ref->getDecl());
    if (function == nullptr || calledDirectly.contains(ref))
      return true;
    const FunctionDecl *canonical = function->getCanonicalDecl();
    if (seen.insert(canonical).second)
      functions.push_back(canonical);
    return true;
  }

private:
  llvm::DenseSet<const DeclRefExpr *> calledDirectly;
  llvm::DenseSet<const FunctionDecl *> seen;
};

} // namespace

TranslationUnitAnalyzer::TranslationUnitAnalyzer(
    ASTContext &ctx, LedgerAdapter &ledgerAdapter,
    AnalysisOptions analysisOptions)
    : context(ctx), ledger(ledgerAdapter),
      discarding(ctx, LedgerAdapter::Mode::Discarding),
      options(std::move(analysisOptions)) {
  store.setContext(&context);
  if (options.preparation)
    store.prepared = options.preparation;
  store.stats = options.stats;
}

void TranslationUnitAnalyzer::collectDefinitions(const DeclContext &dc) {
  for (const Decl *decl : dc.decls()) {
    if (const auto *function = dyn_cast<FunctionDecl>(decl)) {
      if (function->doesThisDeclarationHaveABody())
        definitions.push_back(function);
      continue;
    }
    // C has no nested namespaces, but linkage specs / extern blocks and
    // record scopes can still contain declarations worth visiting.
    if (const auto *nested = dyn_cast<DeclContext>(decl))
      collectDefinitions(*nested);
  }
}

void TranslationUnitAnalyzer::collectAddressTaken() {
  // The whole translation unit, not just function bodies: a static table of
  // callbacks at file scope is the common case.
  AddressTakenCollector collector;
  collector.TraverseDecl(context.getTranslationUnitDecl());
  for (const FunctionDecl *function : collector.functions)
    store.addAddressTaken(*function);
}

std::vector<std::vector<unsigned>> TranslationUnitAnalyzer::buildCallGraph() {
  llvm::DenseMap<const FunctionDecl *, unsigned> indexOf;
  for (unsigned i = 0; i < definitions.size(); ++i)
    indexOf[definitions[i]->getCanonicalDecl()] = i;

  externalCallees.clear();
  indirectTypeKeys.clear();
  llvm::DenseSet<const FunctionDecl *> seenExternal;
  std::vector<std::vector<unsigned>> adjacency(definitions.size());
  for (unsigned i = 0; i < definitions.size(); ++i) {
    CalleeCollector collector;
    collector.TraverseStmt(definitions[i]->getBody());
    const auto addEdge = [&](const FunctionDecl *callee) {
      const FunctionDecl *canonical = callee->getCanonicalDecl();
      if (const auto it = indexOf.find(canonical); it != indexOf.end()) {
        adjacency[i].push_back(it->second);
      } else if (callee->isExternallyVisible() &&
                 callee->getIdentifier() != nullptr &&
                 !callee->getName().starts_with("__builtin_") &&
                 seenExternal.insert(canonical).second) {
        // Compiler builtins are never defined by another unit.
        externalCallees.push_back(canonical);
      }
    };
    for (const FunctionDecl *callee : collector.callees)
      addEdge(callee);
    // An indirect call may reach any address-taken function of its type, so
    // those must be summarised first (or in the same component).
    for (const CallExpr *call : collector.indirectCalls) {
      for (const FunctionDecl *candidate : store.candidatesFor(*call))
        addEdge(candidate);
      if (const FunctionProtoType *type = indirectCalleeType(*call)) {
        std::string key = functionTypeKey(QualType(type, 0), context);
        if (!key.empty())
          indirectTypeKeys.insert(std::move(key));
      }
    }
    std::ranges::sort(adjacency[i]);
    adjacency[i].erase(std::ranges::unique(adjacency[i]).begin(),
                       adjacency[i].end());
  }
  // A global initializer may refer to an external callback without a call
  // expression in this unit. Its defining unit is still a dependency.
  for (const auto &[symbol, function] : store.callables) {
    if (!function->getDefinition() && function->isExternallyVisible() &&
        store.isAddressTaken(*function) && seenExternal.insert(function).second)
      externalCallees.push_back(function);
  }
  return adjacency;
}

void TranslationUnitAnalyzer::prepare() {
  definitions.clear();
  collectDefinitions(*context.getTranslationUnitDecl());
  collectAddressTaken();
  for (const FunctionDecl *function : definitions)
    store.registerCallable(*function);
}

static bool containsCallback(QualType type, unsigned depth = 0) {
  if (type.isNull() || depth > core::MaxHeapPathDepth)
    return false;
  if (type->isFunctionPointerType())
    return true;
  if (type->isPointerType())
    return containsCallback(type->getPointeeType(), depth + 1);
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return containsCallback(array->getElementType(), depth + 1);
  if (const auto *record = type->getAsRecordDecl();
      record && record->isCompleteDefinition())
    for (const auto *field : record->fields())
      if (containsCallback(field->getType(), depth + 1))
        return true;
  return false;
}

UnitExports TranslationUnitAnalyzer::skeletonExports() const {
  UnitExports result;
  const SourceManager &sm = context.getSourceManager();
  if (const auto entry = sm.getFileEntryRefForID(sm.getMainFileID()))
    result.source = entry->getName().str();

  for (const FunctionDecl *function : definitions) {
    if (function->isMain() || function->getIdentifier() == nullptr)
      continue;
    const bool external = function->isExternallyVisible();
    const bool addressTaken = store.isAddressTaken(*function);
    const auto requests =
        store.callbackRequests.find(callableSymbol(*function));
    const auto memory = store.memoryRequests.find(callableSymbol(*function));
    if (!external && !addressTaken &&
        (requests == store.callbackRequests.end() ||
         requests->second.empty()) &&
        (memory == store.memoryRequests.end() || memory->second.empty()))
      continue;
    result.functions[function->getNameAsString()] = ExportedFunction{
        .summary = {},
        .specializations = {},
        .typeKey = functionTypeKey(function->getType(), context),
        .external = external,
        .addressTaken = addressTaken,
        .acceptsCallbacks =
            std::ranges::any_of(function->parameters(),
                                [](const ParmVarDecl *param) {
                                  return containsCallback(param->getType());
                                }),
        .acceptsMemoryContexts = true,
    };
  }
  for (const FunctionDecl *callee : externalCallees)
    result.imports.insert(callee->getNameAsString());
  result.indirectTypes = indirectTypeKeys;
  result.callbackGlobals = store.exportedCallbackGlobals();
  return result;
}

UnitExports TranslationUnitAnalyzer::discover() {
  prepare();
  (void)buildCallGraph();
  return skeletonExports();
}

UnitExports TranslationUnitAnalyzer::exports() {
  UnitExports result = skeletonExports();
  const GlobalTable &table = store.globals();
  // RFC 0028: supported private roots retain identity and representation.
  const core::GlobalIdMap byName = [&](std::uint32_t id) {
    const auto name = table.portableName(id);
    return name ? std::optional(result.globals.idFor(*name)) : std::nullopt;
  };
  const auto exportSummary = [&](const core::FunctionSummary &summary) {
    return core::remapGlobals(summary, byName);
  };
  for (const FunctionDecl *function : definitions) {
    const auto resolved = store.lookup(*function);
    if (!resolved)
      continue;
    const auto it = result.functions.find(function->getNameAsString());
    if (it == result.functions.end())
      continue;
    it->second.summary.assign(exportSummary(*resolved->summary));
  }
  for (std::string &name : store.unknownCalleeNames())
    result.unknownCallees.insert(std::move(name));
  for (std::string &key : store.unknownIndirectTypeKeys())
    result.unknownIndirectTypes.insert(std::move(key));
  // RFC 0010: count fields are keyed by type spelling, so they travel as is.
  for (const auto &[symbol, requests] : store.callbackRequests)
    for (const auto &input : requests)
      if (const auto mapped = core::remapCallbackBindings(input, byName))
        result.callbackRequests[symbol].insert(*mapped);
  for (const auto &[symbol, requests] : store.memoryRequests)
    for (const auto &input : requests)
      if (const auto mapped = core::remapCallContext(input, byName))
        result.memoryRequests[symbol].insert(*mapped);
  for (const auto &[key, summary] : store.memorySpecialized) {
    const auto *function = store.callable(key.first);
    if (!function || !function->getDefinition())
      continue;
    const auto it = result.functions.find(function->getNameAsString());
    const auto mapped = core::remapCallContext(key.second, byName);
    if (it != result.functions.end() && mapped && summary)
      it->second.memorySpecializations[*mapped].assign(exportSummary(*summary));
  }
  for (const auto &[key, summary] : store.specialized) {
    const auto *function = store.callable(key.first);
    if (!function || !function->getDefinition())
      continue;
    const auto it = result.functions.find(function->getNameAsString());
    const auto mapped = core::remapCallbackBindings(key.second, byName);
    if (it != result.functions.end() && mapped && summary)
      it->second.specializations[*mapped].assign(exportSummary(*summary));
  }
  result.countFields = store.knownCountKeys();
  // RFC 0012: so are sized-field witnesses and refutations.
  result.sizedFields = store.sizedFieldFacts();
  result.sizedFieldLoads = store.sizedFieldLoads();
  result.globalInterfaces = table.interfaces;
  result.objectInterfaces = store.objectInterfaces;
  return result;
}

static void validateCheckedDeclarations(const DeclContext &dc,
                                        ASTContext &context,
                                        LedgerAdapter &ledger) {
  for (const Decl *decl : dc.decls()) {
    if (isa<FunctionDecl>(decl))
      continue; // FunctionAnalyzer checks parameters and local declarations.
    if (const auto *named = dyn_cast<NamedDecl>(decl);
        named && getAnnotations(*named).checked)
      ledger.report(
          {.severity = core::Severity::Error,
           .id = core::diag::InvalidAnnotation,
           .message = "WEAVEC_CHECKED requires a function declaration",
           .location =
               toCoreLocation(context.getSourceManager(), named->getLocation()),
           .notes = {},
           .fixits = {}},
          core::Certainty::Definite);
    if (const auto *nested = dyn_cast<DeclContext>(decl))
      validateCheckedDeclarations(*nested, context, ledger);
  }
}

void TranslationUnitAnalyzer::run(
    llvm::function_ref<bool(const FunctionDecl &)> shouldReport) {
  prepare();
  validateCheckedDeclarations(*context.getTranslationUnitDecl(), context,
                              ledger);

  const std::vector<std::vector<unsigned>> adjacency = buildCallGraph();
  const std::vector<std::vector<unsigned>> components =
      core::stronglyConnectedComponents(adjacency);

  // RFC 0012, *Sized fields*: the rounds below take inferred sized fields
  // from the program database only, while they collect the unit's own
  // witnesses. Every round publishes into a discarding adapter (RFC 0030
  // §2.6).
  store.setUnitSizedFactsInForce(false);
  FunctionAnalyzer silent(context, discarding, options);
  std::vector<const FunctionDecl *> reported;
  for (const auto *function : definitions)
    if (shouldReport(*function))
      reported.push_back(function);
  // RFC 0014: stores in a later function can change the target of a global
  // used by an earlier helper. Settle these entry values before reporting.
  for (unsigned round = 0; round < MaxFixpointRounds; ++round) {
    if (options.stats)
      options.stats->add("unit_fixpoint_rounds");
    const auto globalsBefore = store.exportedCallbackGlobals();
    for (const std::vector<unsigned> &component : components) {
      const bool recursive =
          component.size() > 1 ||
          llvm::is_contained(adjacency[component.front()], component.front());
      analyzeComponent(component, recursive, silent);
    }
    if (globalsBefore == store.exportedCallbackGlobals())
      break;
    if (round + 1 == MaxFixpointRounds)
      for (const auto *function : definitions)
        store.markIncomplete(*function);
  }
  for (const FunctionDecl *function : definitions) {
    const std::string symbol = callableSymbol(*function);
    if (const auto *program = store.programDatabase()) {
      const auto &requests = program->requestsFor(symbol);
      for (const auto &input : requests) {
        core::CallContext callbacks;
        callbacks.callbacks = input;
        const auto mapped =
            program->importContext(callbacks, context, store.globals());
        if (mapped)
          store.callbackRequests[symbol].insert(mapped->callbacks);
      }
      for (const auto &input : program->memoryRequestsFor(symbol)) {
        const auto mapped =
            program->importContext(input, context, store.globals());
        auto &memory = store.memoryRequests[symbol];
        if (mapped && (memory.contains(*mapped) ||
                       memory.size() < core::MaxMemoryContexts))
          memory.insert(*mapped);
      }
    }
    const auto requests = store.callbackRequests[symbol];
    for (const auto &bindings : requests)
      (void)store.specialize(*function, bindings, options, nullptr);
    const auto memory = store.memoryRequests[symbol];
    for (const auto &input : memory)
      (void)store.specializeMemory(symbol, input, options, nullptr);
  }

  // RFC 0030 §2.6, §15 item 18: the one authoritative pass over each
  // reported function, the context-insensitive analysis of the body CodeGen
  // emits for every caller, with the unit's sized-field facts in force (the
  // RFC 0012 second reporting pass is folded into it). A call that requests
  // a context run reports that run's findings, linked to the call.
  store.setUnitSizedFactsInForce(true);
  FunctionAnalyzer authoritative(context, ledger, options);
  // A function its callers check through contexts got no generic reporting
  // pass before RFC 0030; the unknown targets of its generic pass are not
  // `annotation-required` boundaries (stage S3-B3 replaces that warning
  // with `unresolved(callback)` rows for every function).
  AnalysisOptions contextOptions = options;
  contextOptions.deferBoundary = true;
  FunctionAnalyzer contextChecked(context, ledger, contextOptions);
  for (const FunctionDecl *function : reported) {
    ledger.beginFunction(*function);
    const std::string symbol = callableSymbol(*function);
    const bool checkedInContexts = !store.callbackRequests[symbol].empty() ||
                                   !store.memoryRequests[symbol].empty();
    (checkedInContexts ? contextChecked : authoritative)
        .analyze(*function, store, /*emitDiagnostics=*/true,
                 recursiveFunctions.contains(function->getCanonicalDecl()));
    if (options.dumpStream)
      dumpMemoryContexts(*function);
    if (options.reportUnannotated)
      reportUnannotatedInterface(*function);
  }
  for (const FunctionDecl *function : reported)
    reportUnclaimedContexts(*function);
}

void TranslationUnitAnalyzer::dumpMemoryContexts(const FunctionDecl &function) {
  const std::string symbol = callableSymbol(function);
  const auto memory = store.memoryRequests[symbol];
  for (const auto &input : memory) {
    *options.dumpStream << "  call-context " << symbol
                        << (input.reportDiagnostics ? " checked" : " unsafe")
                        << "\n";
    const core::GlobalNamer names = [&](std::uint32_t id) {
      const auto *global = store.globals().declFor(id);
      return global ? global->getNameAsString() : "<unmapped>";
    };
    for (const auto &alias : input.aliases)
      *options.dumpStream << "    "
                          << core::printSummaryPath(alias.first, names) << " = "
                          << core::printSummaryPath(alias.second, names) << " @"
                          << alias.offset.toString()
                          << (alias.definite ? " definite" : " possible")
                          << (alias.sameShare ? " same-share"
                                              : " distinct-shares")
                          << "\n";
    for (const auto &[first, second] : input.separations)
      *options.dumpStream << "    " << core::printSummaryPath(first, names)
                          << " distinct-object "
                          << core::printSummaryPath(second, names) << "\n";
    for (const auto &[path, fact] : input.facts)
      *options.dumpStream << "    " << core::printSummaryPath(path, names)
                          << " " << fact.toString() << "\n";
    if (const auto selected =
            store.specializeMemory(symbol, input, options, nullptr))
      *options.dumpStream << core::printSummary(*selected->summary, names);
  }
}

void TranslationUnitAnalyzer::reportUnclaimedContexts(
    const FunctionDecl &function) {
  const std::string symbol = callableSymbol(function);
  std::vector<core::Diagnostic> found;
  const auto memory = store.memoryRequests[symbol];
  for (const auto &input : memory)
    if (!store.claimedMemoryContexts.contains({symbol, input}))
      (void)store.specializeMemory(symbol, input, options, &found);
  const auto requests = store.callbackRequests[symbol];
  for (const auto &bindings : requests)
    if (std::ranges::none_of(
            memory,
            [&](const auto &input) { return input.callbacks == bindings; }) &&
        !store.claimedCallbackContexts.contains({symbol, bindings}))
      (void)store.specialize(function, bindings, options, &found);
  for (core::Diagnostic &diagnostic : found) {
    const core::Certainty certainty = diagnostic.certainty;
    ledger.report(std::move(diagnostic), certainty);
  }
}

bool TranslationUnitAnalyzer::analyzeSilently(const FunctionDecl &function,
                                              FunctionAnalyzer &analyzer,
                                              bool widen) {
  const auto *key = function.getCanonicalDecl();
  if (!options.dumpStream) {
    const auto found = silentAnalyses.find(key);
    if (found != silentAnalyses.end() && found->second.widen == widen &&
        store.dependenciesCurrent(found->second.dependencies)) {
      if (options.stats)
        options.stats->add("silent_function_reuses");
      for (const auto &[dependency, revision] : found->second.dependencies) {
        (void)revision;
        store.noteDependency(dependency);
      }
      return false;
    }
  }
  SummaryStore::Dependencies dependencies;
  store.beginDependencies(dependencies);
  // Capture before analysis: a recursive read must see the newly computed
  // summary again if this run changes it (RFC 0020).
  store.noteDependency(callableSymbol(function));
  const bool changed = analyzer.analyze(function, store, false, widen);
  auto snapshot = store.dependencySnapshot();
  store.endDependencies();
  silentAnalyses.insert_or_assign(
      key, SilentAnalysis{.widen = widen, .dependencies = std::move(snapshot)});
  return changed;
}

void TranslationUnitAnalyzer::analyzeComponent(
    const std::vector<unsigned> &component, bool recursive,
    FunctionAnalyzer &analyzer) {
  if (recursive) {
    for (const unsigned member : component)
      recursiveFunctions.insert(definitions[member]->getCanonicalDecl());
  }
  bool settled = false;
  if (recursive) {
    // Start every member at the bottom summary and iterate silently until
    // nothing changes; the final, reporting run then sees the fixpoint.
    for (const unsigned member : component)
      store.setInferred(*definitions[member], core::FunctionSummary{});
    for (unsigned round = 0; round < MaxFixpointRounds; ++round) {
      if (options.stats)
        options.stats->add("function_fixpoint_rounds");
      bool changed = false;
      for (const unsigned member : component) {
        changed =
            analyzeSilently(*definitions[member], analyzer, true) || changed;
      }
      if (!changed) {
        settled = true;
        break;
      }
      if (round + 1 == MaxFixpointRounds)
        for (const unsigned member : component)
          store.markIncomplete(*definitions[member]);
    }
  }

  const bool previousRefresh = store.refreshingRecursiveValueOutcomes;
  store.refreshingRecursiveValueOutcomes = recursive && settled;
  const auto restoreRefresh = llvm::scope_exit(
      [&] { store.refreshingRecursiveValueOutcomes = previousRefresh; });
  for (const unsigned member : component) {
    const FunctionDecl &function = *definitions[member];
    if (store.refreshingRecursiveValueOutcomes)
      silentAnalyses.erase(function.getCanonicalDecl());
    analyzeSilently(function, analyzer, recursive);
  }
}

static const char *macroFor(core::OwnershipKind kind) {
  switch (kind) {
  case core::OwnershipKind::Owned:
    return "WEAVEC_OWNED";
  case core::OwnershipKind::Shared:
    return "WEAVEC_BORROWED";
  case core::OwnershipKind::Mutable:
    return "WEAVEC_MUT";
  case core::OwnershipKind::Raw:
    return "WEAVEC_RAW";
  case core::OwnershipKind::Unknown:
    break;
  }
  return nullptr;
}

void TranslationUnitAnalyzer::reportUnannotatedInterface(
    const FunctionDecl &function) {
  // Only the exported surface: a `static` helper's callers are all here and
  // already checked against its inferred summary, and nobody annotates
  // `main`.
  if (!function.isGlobal() || function.isMain() ||
      getAnnotations(function).unsafe)
    return;
  const core::FunctionSummary *summary = store.inferredFor(function);
  if (summary == nullptr)
    return;

  const SourceManager &sm = context.getSourceManager();
  const SignatureAnnotations annotations = collectAnnotations(function);
  const std::string name = function.getNameAsString();

  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const ParmVarDecl *param = function.getParamDecl(i);
    // A nullness annotation alone says nothing about ownership (RFC 0008).
    if (!param->getType()->isPointerType() ||
        (i < annotations.params.size() &&
         (annotations.params[i].ownership() || annotations.params[i].invalid)))
      continue;
    const std::string paramName = param->getNameAsString();
    const core::SourceLocation at = toCoreLocation(sm, param->getLocation());
    if (const char *macro = macroFor(summary->inferredKind(i))) {
      core::Diagnostic diagnostic{
          .severity = core::Severity::Warning,
          .id = core::diag::AnnotationRequired,
          .message = llvm::formatv("pointer parameter '{0}' of '{1}' is "
                                   "inferred {2}; add the annotation to its "
                                   "declaration",
                                   paramName, name, macro)
                         .str(),
          .location = at,
          .notes = {},
          .fixits = {},
      };
      if (!paramName.empty())
        diagnostic.addFixIt(at, std::string(macro) + " ");
      ledger.report(std::move(diagnostic), core::Certainty::Possible);
      continue;
    }
    ledger.report(
        core::Diagnostic{
            .severity = core::Severity::Warning,
            .id = core::diag::AnnotationRequired,
            .message = "pointer parameter '" + paramName +
                       "' has no inferable ownership; annotate it with "
                       "WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT",
            .location = at,
            .notes = {},
            .fixits = {},
        },
        core::Certainty::Possible);
  }

  if (!function.getReturnType()->isPointerType() ||
      annotations.result.ownership() || annotations.result.invalid ||
      annotations.result.unsafe)
    return;
  const char *macro = macroFor(summary->inferredReturnKind());
  if (macro == nullptr)
    return;
  const core::SourceLocation at = toCoreLocation(sm, function.getLocation());
  core::Diagnostic diagnostic{
      .severity = core::Severity::Warning,
      .id = core::diag::AnnotationRequired,
      .message = llvm::formatv("return value of '{0}' is inferred {1}; add "
                               "the annotation to its declaration",
                               name, macro)
                     .str(),
      .location = at,
      .notes = {},
      .fixits = {},
  };
  diagnostic.addFixIt(at, std::string(macro) + " ");
  ledger.report(std::move(diagnostic), core::Certainty::Possible);
}

} // namespace weavec::analysis
