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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
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
    ASTContext &ctx, core::DiagnosticSink &diagSink,
    AnalysisOptions analysisOptions)
    : context(ctx), sink(diagSink), options(analysisOptions) {
  store.setContext(&context);
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
  return adjacency;
}

void TranslationUnitAnalyzer::prepare() {
  definitions.clear();
  collectDefinitions(*context.getTranslationUnitDecl());
  collectAddressTaken();
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
    if (!external && !addressTaken)
      continue;
    result.functions[function->getNameAsString()] = ExportedFunction{
        .summary = {},
        .typeKey = functionTypeKey(function->getType(), context),
        .external = external,
        .addressTaken = addressTaken,
    };
  }
  for (const FunctionDecl *callee : externalCallees)
    result.imports.insert(callee->getNameAsString());
  result.indirectTypes = indirectTypeKeys;
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
  // Globals travel by name; a `static` one means nothing elsewhere and is
  // dropped (RFC 0005, *The program database*).
  const core::GlobalIdMap byName = [&](std::uint32_t id) {
    const VarDecl *var = table.declFor(id);
    if (var == nullptr || !var->isExternallyVisible())
      return std::optional<std::uint32_t>();
    return std::optional(result.globals.idFor(var->getName()));
  };
  for (const FunctionDecl *function : definitions) {
    const auto it = result.functions.find(function->getNameAsString());
    if (it == result.functions.end())
      continue;
    if (const auto resolved = store.lookup(*function))
      it->second.summary = core::remapGlobals(*resolved->summary, byName);
  }
  for (std::string &name : store.unknownCalleeNames())
    result.unknownCallees.insert(std::move(name));
  for (std::string &key : store.unknownIndirectTypeKeys())
    result.unknownIndirectTypes.insert(std::move(key));
  // RFC 0010: count fields are keyed by type spelling, so they travel as is.
  result.countFields = store.knownCountKeys();
  return result;
}

void TranslationUnitAnalyzer::run(
    llvm::function_ref<bool(const FunctionDecl &)> shouldReport) {
  prepare();

  const std::vector<std::vector<unsigned>> adjacency = buildCallGraph();
  const std::vector<std::vector<unsigned>> components =
      core::stronglyConnectedComponents(adjacency);

  FunctionAnalyzer analyzer(context, sink, options);
  for (const std::vector<unsigned> &component : components) {
    const bool recursive =
        component.size() > 1 ||
        llvm::is_contained(adjacency[component.front()], component.front());
    analyzeComponent(component, recursive, analyzer, shouldReport);
  }
}

void TranslationUnitAnalyzer::analyzeComponent(
    const std::vector<unsigned> &component, bool recursive,
    FunctionAnalyzer &analyzer,
    llvm::function_ref<bool(const FunctionDecl &)> shouldReport) {
  if (recursive) {
    // Start every member at the bottom summary and iterate silently until
    // nothing changes; the final, reporting run then sees the fixpoint.
    for (const unsigned member : component)
      store.setInferred(*definitions[member], core::FunctionSummary{});
    for (unsigned round = 0; round < MaxFixpointRounds; ++round) {
      bool changed = false;
      for (const unsigned member : component) {
        changed = analyzer.analyze(*definitions[member], store,
                                   /*emitDiagnostics=*/false) ||
                  changed;
      }
      if (!changed)
        break;
    }
  }

  for (const unsigned member : component) {
    const FunctionDecl &function = *definitions[member];
    const bool report = shouldReport(function);
    analyzer.analyze(function, store, report);
    if (report && options.reportUnannotated)
      reportUnannotatedInterface(function);
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
      sink.report(diagnostic);
      continue;
    }
    sink.report(core::Diagnostic{
        .severity = core::Severity::Warning,
        .id = core::diag::AnnotationRequired,
        .message = "pointer parameter '" + paramName +
                   "' has no inferable ownership; annotate it with "
                   "WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT",
        .location = at,
        .notes = {},
        .fixits = {},
    });
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
  sink.report(diagnostic);
}

} // namespace weavec::analysis
