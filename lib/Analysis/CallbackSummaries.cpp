//===- CallbackSummaries.cpp - Contextual callback summaries -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "InterfaceTypes.h"
#include "weavec/Analysis/Summaries.h"

#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"

using namespace clang;

namespace weavec::analysis {

std::string_view SummaryStore::objectView(QualType type) {
  if (!context || type.isNull() || !type->isRecordType() ||
      type->isIncompleteType())
    return {};
  const auto *record = type->getAsRecordDecl();
  const auto [it, inserted] = objectViewCache.try_emplace(record);
  if (inserted) {
    it->second = recordLayoutKey(type, *context);
    if (!it->second.empty())
      if (const auto descriptor =
              describeInterfaceType(type.getUnqualifiedType(), *context))
        objectInterfaces.emplace(it->second, *descriptor);
  }
  return it->second;
}

void SummaryStore::setDatabase(const ProgramDatabase *program) {
  const auto generation = program ? program->importGeneration() : nullptr;
  if (interfaceGeneration != generation) {
    invalidateDependency("@interfaces");
    interfaceGeneration = generation;
  }
  database = program;
}

QualType SummaryStore::interfaceType(std::string_view view) {
  setDatabase(database);
  noteDependency("@interfaces");
  const auto local = objectInterfaces.find(std::string(view));
  if (local != objectInterfaces.end() && !local->second)
    return {};
  const core::InterfaceType *description =
      local != objectInterfaces.end() && local->second ? &*local->second
                                                       : nullptr;
  if (database) {
    const auto remote = database->objectInterfaces.find(std::string(view));
    if (remote != database->objectInterfaces.end()) {
      if (!remote->second || (description && *description != *remote->second))
        return {};
      description = &*remote->second;
    }
  }
  if (!description || !context)
    return {};
  const auto key = description->encode();
  if (const auto found = interfaceAdapters.find(key);
      found != interfaceAdapters.end())
    return found->second;
  auto &arena = context->getTranslationUnitDecl()->getASTContext();
  const auto type = materializeInterfaceType(*description, arena);
  interfaceAdapters.emplace(key, type);
  if (!type.isNull() && type->isRecordType())
    objectViewCache[type->getAsRecordDecl()] = view;
  return type;
}

static const Expr *globalInitializer(const Expr &expr, unsigned depth = 0) {
  if (depth > core::MaxHeapPathDepth)
    return nullptr;
  const Expr *e = expr.IgnoreParenImpCasts();
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    const auto *var = dyn_cast<VarDecl>(ref->getDecl());
    return var && var->hasGlobalStorage() ? var->getInit() : nullptr;
  }
  if (const auto *member = dyn_cast<MemberExpr>(e)) {
    const Expr *base = globalInitializer(*member->getBase(), depth + 1);
    const auto *init = dyn_cast_or_null<InitListExpr>(base);
    const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
    return init && field && field->getFieldIndex() < init->getNumInits()
               ? init->getInit(field->getFieldIndex())
               : nullptr;
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(e))
    return globalInitializer(*unary->getSubExpr(), depth + 1);
  return nullptr;
}

static core::CallTargets constantTargets(const Expr &expr, unsigned depth = 0) {
  if (depth > core::MaxHeapPathDepth)
    return core::CallTargets::any();
  const Expr *e = expr.IgnoreParens();
  if (isa<ImplicitValueInitExpr>(e))
    return {.functions = {}, .unknown = false, .null = true};
  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    if (cast->getCastKind() == CK_NullToPointer)
      return {.functions = {}, .unknown = false, .null = true};
    if (cast->getCastKind() == CK_IntegralToPointer ||
        cast->getCastKind() == CK_BitCast)
      return core::CallTargets::any();
    return constantTargets(*cast->getSubExpr(), depth + 1);
  }
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    if (const auto *fn = dyn_cast<FunctionDecl>(ref->getDecl()))
      return core::CallTargets::function(callableSymbol(*fn));
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(e))
    return constantTargets(*unary->getSubExpr(), depth + 1);
  if (const auto *conditional = dyn_cast<ConditionalOperator>(e)) {
    auto result = constantTargets(*conditional->getTrueExpr(), depth + 1);
    result.join(constantTargets(*conditional->getFalseExpr(), depth + 1));
    return result;
  }
  if (const auto *index = dyn_cast<ArraySubscriptExpr>(e)) {
    const auto *init =
        dyn_cast_or_null<InitListExpr>(globalInitializer(*index->getBase()));
    if (!init)
      return {};
    if (const auto *literal =
            dyn_cast<IntegerLiteral>(index->getIdx()->IgnoreParenImpCasts())) {
      const auto ordinal = literal->getValue().getLimitedValue();
      if (ordinal < init->getNumInits())
        return constantTargets(*init->getInit(static_cast<unsigned>(ordinal)),
                               depth + 1);
    }
    core::CallTargets result;
    for (const auto *item : init->inits())
      result.join(constantTargets(*item, depth + 1));
    return result;
  }
  if (const auto *init = globalInitializer(*e))
    return constantTargets(*init, depth + 1);
  return {};
}

core::CallTargets SummaryStore::staticTargets(const Expr &expr,
                                              unsigned depth) {
  // RFC 0030 §9.3: what a global can hold is the solved slot's business
  // (the engine asks it); here only the constant initialisers speak.
  return constantTargets(expr, depth);
}

std::string callableSymbol(const FunctionDecl &function) {
  if (function.isExternallyVisible())
    return function.getNameAsString();
  const SourceManager &sm = function.getASTContext().getSourceManager();
  std::string unit;
  if (const auto file = sm.getFileEntryRefForID(sm.getMainFileID()))
    unit = file->getName().str();
  return unit + "#" + function.getNameAsString();
}

void SummaryStore::registerCallable(const FunctionDecl &function) {
  callables[callableSymbol(function)] = function.getCanonicalDecl();
}

const FunctionDecl *SummaryStore::callable(std::string_view symbol) const {
  const auto it = callables.find(std::string(symbol));
  return it == callables.end() ? nullptr : it->second;
}

std::optional<ResolvedSummary>
SummaryStore::lookupSymbol(std::string_view symbol) {
  noteDependency(symbol);
  if (const auto *function = callable(symbol))
    return lookup(*function);
  if (!database || !context)
    return std::nullopt;
  const auto *summary = database->findCallable(symbol);
  if (!summary)
    return std::nullopt;
  return ResolvedSummary{.summary = importSummary(*summary),
                         .source = SummarySource::Program};
}

std::optional<ResolvedSummary> SummaryStore::lookupCall(const CallExpr &call) {
  if (callResolver)
    return callResolver(call);
  if (const auto *callee = call.getDirectCallee())
    return lookup(*callee);
  return lookupIndirect(call);
}

std::optional<std::uint64_t>
SummaryStore::contextBudget(const FunctionDecl &definition,
                            const AnalysisOptions &options) const {
  if (options.budget == 0)
    return 0;
  const auto spent = contextTransfers.find(definition.getCanonicalDecl());
  const std::uint64_t used =
      spent == contextTransfers.end() ? 0 : spent->second;
  if (used >= options.budget)
    return std::nullopt;
  return options.budget - used;
}

std::optional<ResolvedSummary>
SummaryStore::specialize(const FunctionDecl &function,
                         const core::CallbackBindings &bindings,
                         const AnalysisOptions &options,
                         std::vector<core::Diagnostic> *diagnostics) {
  if (bindings.empty())
    return lookup(function);
  const std::string symbol = callableSymbol(function);
  noteDependency(symbol);
  const ContextKey contextKey{symbol, bindings};
  auto &requests = callbackRequests[symbol];
  if (!requests.contains(bindings) &&
      requests.size() >= core::MaxCallbackContexts)
    return std::nullopt;
  requests.insert(bindings);
  const FunctionDecl *definition = function.getDefinition();
  if (!context)
    return std::nullopt;
  if (!definition) {
    if (!database)
      return std::nullopt;
    core::CallContext input;
    input.callbacks = bindings;
    const auto exported = database->exportContext(input, globalTable);
    if (!exported)
      return std::nullopt;
    const auto *summary =
        database->findSpecialization(symbol, exported->callbacks);
    if (!summary)
      return std::nullopt;
    return ResolvedSummary{.summary = importSummary(*summary),
                           .source = SummarySource::Program};
  }
  if (activeContexts.contains(contextKey))
    return std::nullopt;
  discardStaleContexts();
  if (!specialized.contains(contextKey) || !specialized.at(contextKey)) {
    if (options.stats)
      options.stats->add("specialization_misses");
    std::optional<core::AnalysisTimer> invocationTimer;
    if (options.stats)
      invocationTimer.emplace(options.stats, "callback:" + symbol);
    Dependencies dependencies{symbol};
    beginDependencies(dependencies);
    const auto finishDependencies =
        llvm::scope_exit([&] { endDependencies(); });
    activeContexts.insert(contextKey);
    const auto release =
        llvm::scope_exit([&] { activeContexts.erase(contextKey); });
    // RFC 0030 §5.5: the context runs of one function share a budget.
    const auto budget = contextBudget(*definition, options);
    if (!budget)
      return std::nullopt;
    LedgerAdapter collected(function.getASTContext(),
                            LedgerAdapter::Mode::Collecting);
    AnalysisOptions nestedOptions = options;
    nestedOptions.dumpStream = nullptr;
    nestedOptions.budget = *budget;
    FunctionDataflow analysis(function.getASTContext(), *definition, collected,
                              nestedOptions, *this, true);
    analysis.callbackBindings = bindings;
    analysis.run();
    contextTransfers[definition->getCanonicalDecl()] += analysis.transfers();
    if (analysis.overBudget())
      return std::nullopt;
    auto summary = std::move(analysis).summary();
    applyContract(function, summary);
    specialized[contextKey] = publishSummary(std::move(summary));
    specializedDiagnostics[contextKey] = collected.diagnostics();
    callbackDependencies[contextKey] = std::move(dependencies);
    auto &snapshot = callbackVersions[contextKey];
    snapshot = dependencySnapshot();
    contextsNeedValidation |= !dependenciesCurrent(snapshot);
  } else {
    if (options.stats)
      options.stats->add("specialization_hits");
    inheritDependencies(callbackDependencies[contextKey]);
  }
  if (diagnostics != nullptr)
    llvm::append_range(*diagnostics, specializedDiagnostics[contextKey]);
  return ResolvedSummary{.summary = specialized.at(contextKey),
                         .source = SummarySource::Inferred};
}

} // namespace weavec::analysis
