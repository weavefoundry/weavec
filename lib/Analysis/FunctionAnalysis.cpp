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
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/Summaries.h"

#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

FunctionAnalyzer::FunctionAnalyzer(ASTContext &ctx,
                                   LedgerAdapter &ledgerAdapter,
                                   AnalysisOptions analysisOptions)
    : context(ctx), ledger(ledgerAdapter), options(std::move(analysisOptions)) {
}

/// A declaration-level diagnostic: linked to no site, definite when it is
/// an error.
static void reportDeclaration(LedgerAdapter &ledger,
                              core::Diagnostic diagnostic) {
  const core::Certainty certainty = diagnostic.severity == core::Severity::Error
                                        ? core::Certainty::Definite
                                        : core::Certainty::Possible;
  ledger.report(std::move(diagnostic), certainty);
}

bool FunctionAnalyzer::analyze(const FunctionDecl &function,
                               SummaryStore &summaries, bool emitDiagnostics,
                               bool widenSummary) {
  if (!function.doesThisDeclarationHaveABody())
    return false;

  std::optional<core::AnalysisTimer> invocationTimer;
  if (options.stats)
    invocationTimer.emplace(options.stats,
                            "generic:" + callableSymbol(function));
  if (emitDiagnostics)
    validate(function);
  // A `WEAVEC_UNSAFE` function is analysed like any other so its callers see
  // what it does; the dataflow itself suppresses reports inside it (RFC
  // 0004, *Unsafe regions*).
  FunctionDataflow dataflow(context, function, ledger, options, summaries,
                            emitDiagnostics);
  dataflow.run();
  return summaries.setInferred(function, std::move(dataflow).summary(),
                               widenSummary);
}

void FunctionAnalyzer::validate(const FunctionDecl &function) {
  const SourceManager &sm = context.getSourceManager();

  const AnnotationSet annotations = getAnnotations(function);
  if (annotations.invalid) {
    reportDeclaration(
        ledger, core::Diagnostic{
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
  // each other (RFC 0008, *Annotation surface*): `AttributeReader` reports
  // that, with every other malformed kind (RFC 0030 §7.2).
  {
    // RFC 0010, *Annotations*: `WEAVEC_OWNED_BY` says which family an owned
    // pointer belongs to, so it needs `WEAVEC_OWNED`; retaining and
    // releasing the same argument contradict each other.
    const auto reportShareContradictions = [&](const NamedDecl &decl,
                                               const AnnotationSet &set) {
      if (set.retains && set.releases) {
        reportDeclaration(
            ledger,
            core::Diagnostic{
                .severity = core::Severity::Warning,
                .id = core::diag::InvalidAnnotation,
                .message =
                    "'" + decl.getNameAsString() +
                    "' is declared both WEAVEC_RETAINS and WEAVEC_RELEASES",
                .location = toCoreLocation(sm, decl.getLocation()),
                .notes = {},
                .fixits = {},
            });
      }
      if (!set.family.empty() && !set.owned) {
        reportDeclaration(
            ledger, core::Diagnostic{
                        .severity = core::Severity::Warning,
                        .id = core::diag::InvalidAnnotation,
                        .message = "'" + decl.getNameAsString() +
                                   "' is declared WEAVEC_OWNED_BY(" +
                                   set.family + ") without WEAVEC_OWNED",
                        .location = toCoreLocation(sm, decl.getLocation()),
                        .notes = {},
                        .fixits = {},
                    });
      }
    };
    reportShareContradictions(function, annotations);
    // RFC 0012, *`WEAVEC_ASSUME`*: `weavec.assume` belongs to the header's
    // `weavec_assume_` alone.
    if (annotations.assume && function.getName() != "weavec_assume_") {
      reportDeclaration(
          ledger, core::Diagnostic{
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
      reportShareContradictions(*param, onParam);
      // A malformed `WEAVEC_SIZED_BY` is `AttributeReader`'s (RFC 0030
      // §7.2), reported before the engine runs.
      if (onParam.invalid) {
        reportDeclaration(
            ledger, core::Diagnostic{
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
