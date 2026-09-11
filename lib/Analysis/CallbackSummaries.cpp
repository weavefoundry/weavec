//===- CallbackSummaries.cpp - Contextual callback summaries -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "weavec/Analysis/Summaries.h"

#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/ScopeExit.h"

using namespace clang;

namespace weavec::analysis {

std::string_view SummaryStore::objectView(QualType type) {
  if (!context || type.isNull() || !type->isRecordType() ||
      type->isIncompleteType())
    return {};
  const auto *record = type->getAsRecordDecl();
  const auto [it, inserted] = objectViewCache.try_emplace(record);
  if (inserted)
    it->second = recordLayoutKey(type, *context);
  return it->second;
}

static std::string globalSymbol(const VarDecl &var) {
  if (var.isExternallyVisible())
    return var.getNameAsString();
  const auto &sm = var.getASTContext().getSourceManager();
  std::string unit;
  if (const auto file = sm.getFileEntryRefForID(sm.getMainFileID()))
    unit = file->getName().str();
  return unit + "#" + var.getNameAsString();
}

static std::string globalTargetKey(const Expr &expr, const ASTContext *ctx) {
  const Expr *e = expr.IgnoreParenImpCasts();
  if (const auto *ref = dyn_cast<DeclRefExpr>(e)) {
    const auto *var = dyn_cast<VarDecl>(ref->getDecl());
    return var && var->hasGlobalStorage() ? globalSymbol(*var) : std::string{};
  }
  if (const auto *member = dyn_cast<MemberExpr>(e)) {
    std::string base = globalTargetKey(*member->getBase(), ctx);
    return base.empty()
               ? base
               : base + "." + member->getMemberDecl()->getNameAsString();
  }
  if (const auto *index = dyn_cast<ArraySubscriptExpr>(e)) {
    std::string base = globalTargetKey(*index->getBase(), ctx);
    if (base.empty())
      return base;
    Expr::EvalResult value;
    if (ctx && index->getIdx()->EvaluateAsInt(value, *ctx) &&
        value.Val.getInt().isSignedIntN(64))
      return base + "[][" + std::to_string(value.Val.getInt().getSExtValue()) +
             "]";
    return base + "[]";
  }
  if (const auto *unary = dyn_cast<UnaryOperator>(e))
    return globalTargetKey(*unary->getSubExpr(), ctx);
  return {};
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

core::CallTargets
SummaryStore::targetsForGlobal(const core::SummaryPath &path) const {
  if (!path.isGlobal())
    return {};
  const auto *global = globalTable.declFor(path.index);
  if (!global)
    return {};
  std::string name = globalTable.callbackName(path.index);
  for (const auto &step : path.steps) {
    if (step.step == core::PathStep::Field)
      name += "." + step.field;
    else if (step.step == core::PathStep::Index)
      name += "[" + step.field + "]";
    else
      return {};
  }
  const auto &local = exportedCallbackGlobals();
  core::CallTargets result;
  if (const auto it = local.find(name); it != local.end())
    result = it->second;
  if (database) {
    if (const auto it = database->callbackGlobals.find(name);
        it != database->callbackGlobals.end())
      result.join(it->second);
  }
  return result;
}

core::CallTargets SummaryStore::staticTargets(const Expr &expr,
                                              unsigned depth) {
  const auto key = globalTargetKey(expr, context);
  core::CallTargets result;
  if (!key.empty()) {
    const auto &local = exportedCallbackGlobals();
    if (const auto it = local.find(key); it != local.end())
      result.join(it->second);
    if (database) {
      if (const auto it = database->callbackGlobals.find(key);
          it != database->callbackGlobals.end())
        result.join(it->second);
    }
  }
  if (!result.empty())
    return result;
  return constantTargets(expr, depth);
}

const std::map<std::string, core::CallTargets> &
SummaryStore::exportedCallbackGlobals() const {
  noteDependency("@callback-globals");
  if (callbackGlobalCache)
    return *callbackGlobalCache;
  std::map<std::string, core::CallTargets> result;
  if (!context) {
    callbackGlobalCache.emplace();
    return *callbackGlobalCache;
  }
  std::function<void(QualType, const Expr *, const std::string &, unsigned)>
      visit;
  visit = [&](QualType type, const Expr *value, const std::string &name,
              unsigned depth) {
    if (depth > core::MaxHeapPathDepth)
      return;
    if (type->isFunctionPointerType()) {
      auto targets = value ? constantTargets(*value)
                           : core::CallTargets{.functions = {},
                                               .unknown = false,
                                               .null = true};
      result[name].join(targets.empty() ? core::CallTargets::any() : targets);
      return;
    }
    const auto *init =
        value ? dyn_cast<InitListExpr>(value->IgnoreParenImpCasts()) : nullptr;
    if (const auto *array = type->getAsArrayTypeUnsafe()) {
      if (init) {
        unsigned index = 0;
        for (const auto *item : init->inits()) {
          visit(array->getElementType(), item, name + "[]", depth + 1);
          if (index < core::MaxArrayCells)
            visit(array->getElementType(), item,
                  name + "[][" + std::to_string(index) + "]", depth + 1);
          ++index;
        }
      } else {
        visit(array->getElementType(), nullptr, name + "[]", depth + 1);
      }
    } else if (const auto *record = type->getAsRecordDecl()) {
      for (const auto *field : record->fields()) {
        if (!init || field->getFieldIndex() < init->getNumInits())
          visit(field->getType(),
                init ? init->getInit(field->getFieldIndex()) : nullptr,
                name + "." + field->getNameAsString(), depth + 1);
      }
    }
  };
  for (const auto *decl : context->getTranslationUnitDecl()->decls()) {
    if (const auto *var = dyn_cast<VarDecl>(decl);
        var && var->hasGlobalStorage() &&
        (var->hasInit() || !var->hasExternalStorage()))
      visit(var->getType(), var->getInit(), globalSymbol(*var), 0);
  }
  for (const auto &[function, summary] : inferred) {
    for (const auto &store : summary->stores) {
      if (!store.dest.isGlobal())
        continue;
      const auto *global = globalTable.declFor(store.dest.index);
      if (!global)
        continue;
      std::string name = globalTable.callbackName(store.dest.index);
      QualType type = global->getType();
      for (const auto &step : store.dest.steps) {
        if (type.isNull())
          break;
        if (step.step == core::PathStep::Field) {
          name += "." + step.field;
          const auto *record = type->getAsRecordDecl();
          type = QualType{};
          if (record)
            for (const auto *field : record->fields())
              if (field->getName() == step.field) {
                type = field->getType();
                break;
              }
        } else if (step.step == core::PathStep::Index) {
          name += "[" + step.field + "]";
          if (!step.field.empty())
            continue;
          const auto *array = type->getAsArrayTypeUnsafe();
          type = array ? array->getElementType() : QualType{};
        } else {
          type = QualType{};
          break;
        }
      }
      if (type.isNull() || !type->isFunctionPointerType())
        continue;
      auto &targets = result[name];
      if (store.value.kind == core::ValueSource::Kind::Function)
        targets.join(store.value.targets);
      else if (store.value.kind == core::ValueSource::Kind::Null)
        targets.null = true;
      else
        targets.unknown = true;
    }
  }
  callbackGlobalCache = std::move(result);
  return *callbackGlobalCache;
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

std::optional<ResolvedSummary> SummaryStore::specialize(
    const FunctionDecl &function, const core::CallbackBindings &bindings,
    const AnalysisOptions &options, core::DiagnosticSink *sink) {
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
    core::DiagnosticCollector collected;
    AnalysisOptions nestedOptions = options;
    nestedOptions.dumpStream = nullptr;
    FunctionDataflow analysis(function.getASTContext(), *definition, collected,
                              nestedOptions, *this, true);
    analysis.callbackBindings = bindings;
    analysis.run();
    auto summary = analysis.summary();
    applyContract(function, summary);
    specialized[contextKey] = publishSummary(std::move(summary));
    specializedDiagnostics[contextKey] = collected.diagnostics();
    callbackDependencies[contextKey] = std::move(dependencies);
    callbackVersions[contextKey] = dependencySnapshot();
    contextsNeedValidation = true;
  } else {
    if (options.stats)
      options.stats->add("specialization_hits");
    inheritDependencies(callbackDependencies[contextKey]);
  }
  if (sink) {
    for (const auto &diagnostic : specializedDiagnostics[contextKey])
      sink->report(diagnostic);
  }
  return ResolvedSummary{.summary = specialized.at(contextKey),
                         .source = SummarySource::Inferred};
}

} // namespace weavec::analysis
