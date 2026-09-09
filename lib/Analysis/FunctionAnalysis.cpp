//===- FunctionAnalysis.cpp - Per-function ownership analysis -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/FunctionAnalysis.h"

#include "Dataflow.h"
#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/Summaries.h"

#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

FunctionAnalyzer::FunctionAnalyzer(ASTContext &ctx,
                                   core::DiagnosticSink &diagSink,
                                   AnalysisOptions analysisOptions)
    : context(ctx), sink(diagSink), options(std::move(analysisOptions)) {}

bool FunctionAnalyzer::analyze(const FunctionDecl &function,
                               SummaryStore &summaries, bool emitDiagnostics,
                               bool widenSummary) {
  if (!function.doesThisDeclarationHaveABody())
    return false;

  std::optional<core::AnalysisTimer> invocationTimer;
  if (options.stats)
    invocationTimer.emplace(options.stats,
                            "generic:" + callableSymbol(function));
  core::DiagnosticCollector validation;
  if (options.checkContracts) {
    FunctionAnalyzer validator(context, validation, options);
    validator.validate(function);
    if (emitDiagnostics)
      for (const auto &diagnostic : validation.diagnostics())
        sink.report(diagnostic);
  } else if (emitDiagnostics) {
    validate(function);
  }
  // A `WEAVEC_UNSAFE` function is analysed like any other so its callers see
  // what it does; the dataflow itself suppresses reports inside it (RFC
  // 0004, *Unsafe regions*).
  FunctionDataflow dataflow(context, function, sink, options, summaries,
                            emitDiagnostics);
  dataflow.run();
  if (!options.checkContracts)
    return summaries.setInferred(function, dataflow.summary(), widenSummary);
  auto summary = dataflow.summary();
  if (options.checkContracts)
    for (const auto &diagnostic : validation.diagnostics())
      if (diagnostic.severity == core::Severity::Error ||
          diagnostic.id == core::diag::InvalidAnnotation)
        summary.checked.obligations.add(
            {.property = core::SafetyProperty::Semantics,
             .outcome = core::SafetyOutcome::Violation,
             .location = diagnostic.location,
             .function = function.getNameAsString(),
             .subject = std::string(diagnostic.id),
             .reason = diagnostic.message,
             .calls = {}});
  return summaries.setInferred(function, summary, widenSummary);
}

void FunctionAnalyzer::validate(const FunctionDecl &function) {
  const SourceManager &sm = context.getSourceManager();

  const auto invalidChecked = [&](const NamedDecl &decl) {
    if (!getAnnotations(decl).checked)
      return;
    sink.report(core::Diagnostic{
        .severity = core::Severity::Error,
        .id = core::diag::InvalidAnnotation,
        .message = "WEAVEC_CHECKED requires a function declaration",
        .location = toCoreLocation(sm, decl.getLocation()),
        .notes = {},
        .fixits = {}});
  };
  for (const auto *param : function.parameters())
    invalidChecked(*param);
  std::vector<const Stmt *> declarations{function.getBody()};
  for (std::size_t i = 0;
       i < declarations.size() && declarations.size() < 65536; ++i) {
    if (!declarations[i])
      continue;
    if (const auto *statement = dyn_cast<DeclStmt>(declarations[i]))
      for (const auto *decl : statement->decls())
        if (const auto *named = dyn_cast<NamedDecl>(decl))
          invalidChecked(*named);
    for (const auto *child : declarations[i]->children())
      declarations.push_back(child);
  }
  const AnnotationSet annotations = getAnnotations(function);
  if (annotations.invalid) {
    sink.report(core::Diagnostic{
        .severity = core::Severity::Warning,
        .id = core::diag::InvalidAnnotation,
        .message = "unrecognised weavec annotation on '" +
                   function.getNameAsString() + "'",
        .location = toCoreLocation(sm, function.getLocation()),
        .notes = {},
        .fixits = {},
    });
  }
  // `WEAVEC_NULLABLE` and `WEAVEC_NONNULL` on one declaration contradict
  // each other (RFC 0008, *Annotation surface*).
  {
    const auto reportContradiction = [&](const NamedDecl &decl) {
      sink.report(core::Diagnostic{
          .severity = core::Severity::Warning,
          .id = core::diag::InvalidAnnotation,
          .message = "'" + decl.getNameAsString() +
                     "' is declared both WEAVEC_NULLABLE and WEAVEC_NONNULL",
          .location = toCoreLocation(sm, decl.getLocation()),
          .notes = {},
          .fixits = {},
      });
    };
    // RFC 0010, *Annotations*: `WEAVEC_OWNED_BY` says which family an owned
    // pointer belongs to, so it needs `WEAVEC_OWNED`; retaining and
    // releasing the same argument contradict each other.
    const auto reportShareContradictions = [&](const NamedDecl &decl,
                                               const AnnotationSet &set) {
      if (set.retains && set.releases) {
        sink.report(core::Diagnostic{
            .severity = core::Severity::Warning,
            .id = core::diag::InvalidAnnotation,
            .message = "'" + decl.getNameAsString() +
                       "' is declared both WEAVEC_RETAINS and WEAVEC_RELEASES",
            .location = toCoreLocation(sm, decl.getLocation()),
            .notes = {},
            .fixits = {},
        });
      }
      if (!set.family.empty() && !set.owned) {
        sink.report(core::Diagnostic{
            .severity = core::Severity::Warning,
            .id = core::diag::InvalidAnnotation,
            .message = "'" + decl.getNameAsString() +
                       "' is declared WEAVEC_OWNED_BY(" + set.family +
                       ") without WEAVEC_OWNED",
            .location = toCoreLocation(sm, decl.getLocation()),
            .notes = {},
            .fixits = {},
        });
      }
    };
    if (annotations.nullable && annotations.nonNull)
      reportContradiction(function);
    reportShareContradictions(function, annotations);
    // RFC 0012, *`WEAVEC_ASSUME`*: `weavec.assume` belongs to the header's
    // `weavec_assume_` alone.
    if (annotations.assume && function.getName() != "weavec_assume_") {
      sink.report(core::Diagnostic{
          .severity = core::Severity::Warning,
          .id = core::diag::InvalidAnnotation,
          .message = "'weavec.assume' is not an annotation for '" +
                     function.getNameAsString() + "'",
          .location = toCoreLocation(sm, function.getLocation()),
          .notes = {},
          .fixits = {},
      });
    }
    for (const ParmVarDecl *param : function.parameters()) {
      const AnnotationSet onParam = getAnnotations(*param);
      if (onParam.nullable && onParam.nonNull)
        reportContradiction(*param);
      reportShareContradictions(*param, onParam);
      // RFC 0011, *Annotation surface*: `WEAVEC_SIZED_BY(n)` goes on a
      // pointer parameter and names an integer parameter.
      if (!onParam.sizedBy.empty() &&
          !sizedByOf(function, param->getFunctionScopeIndex())) {
        sink.report(core::Diagnostic{
            .severity = core::Severity::Warning,
            .id = core::diag::InvalidAnnotation,
            .message = "'" + param->getNameAsString() +
                       "' is declared WEAVEC_SIZED_BY(" + onParam.sizedBy +
                       ")" +
                       (param->getType()->isPointerType()
                            ? " but '" + onParam.sizedBy +
                                  "' is not an integer parameter"
                            : " but is not a pointer"),
            .location = toCoreLocation(sm, param->getLocation()),
            .notes = {},
            .fixits = {},
        });
      }
      if (onParam.invalid) {
        sink.report(core::Diagnostic{
            .severity = core::Severity::Warning,
            .id = core::diag::InvalidAnnotation,
            .message = "unrecognised weavec annotation on '" +
                       param->getNameAsString() + "'",
            .location = toCoreLocation(sm, param->getLocation()),
            .notes = {},
            .fixits = {},
        });
      }
    }
  }
}

} // namespace weavec::analysis
