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

#include <string>

using namespace clang;

namespace weavec::analysis {

FunctionAnalyzer::FunctionAnalyzer(ASTContext &ctx,
                                   core::DiagnosticSink &diagSink,
                                   AnalysisOptions analysisOptions)
    : context(ctx), sink(diagSink), options(analysisOptions) {}

void FunctionAnalyzer::analyze(const FunctionDecl &function) {
  if (!function.doesThisDeclarationHaveABody())
    return;

  const SourceManager &sm = context.getSourceManager();

  const AnnotationSet annotations = getAnnotations(function);
  if (annotations.invalid) {
    sink.report(core::Diagnostic{
        .severity = core::Severity::Warning,
        .id = core::diag::InvalidAnnotation,
        .message = "unrecognised weavec annotation on '" +
                   function.getNameAsString() + "'",
        .location = toCoreLocation(sm, function.getLocation()),
        .notes = {},
    });
  }
  if (annotations.unsafe)
    return;

  if (options.reportUnannotated) {
    for (const ParmVarDecl *param : function.parameters()) {
      if (!param->getType()->isPointerType() || getAnnotations(*param).any())
        continue;
      sink.report(core::Diagnostic{
          .severity = core::Severity::Warning,
          .id = core::diag::AnnotationRequired,
          .message = "pointer parameter '" + param->getNameAsString() +
                     "' has no inferable ownership; annotate it with "
                     "WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT",
          .location = toCoreLocation(sm, param->getLocation()),
          .notes = {},
      });
    }
  }

  FunctionDataflow dataflow(context, function, sink, options);
  dataflow.run();
}

} // namespace weavec::analysis
