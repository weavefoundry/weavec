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

bool FunctionAnalyzer::analyze(const FunctionDecl &function,
                               SummaryStore &summaries, bool emitDiagnostics) {
  if (!function.doesThisDeclarationHaveABody())
    return false;

  const SourceManager &sm = context.getSourceManager();

  const AnnotationSet annotations = getAnnotations(function);
  if (annotations.invalid && emitDiagnostics) {
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
  // A `WEAVEC_UNSAFE` function is analysed like any other so its callers see
  // what it does; the dataflow itself suppresses reports inside it (RFC
  // 0004, *Unsafe regions*).
  FunctionDataflow dataflow(context, function, sink, options, summaries,
                            emitDiagnostics);
  dataflow.run();
  return summaries.setInferred(function, dataflow.summary());
}

} // namespace weavec::analysis
