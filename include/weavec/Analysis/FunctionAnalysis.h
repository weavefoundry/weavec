//===- FunctionAnalysis.h - Per-function ownership analysis ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The analysis layer walks Clang ASTs, translates them into facts about core
// `PlaceId`s, and drives the core model to produce diagnostics. It is the only
// layer that knows about both Clang and the core model.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_FUNCTIONANALYSIS_H
#define WEAVEC_ANALYSIS_FUNCTIONANALYSIS_H

#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/Diagnostic.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/Support/raw_ostream.h"

namespace weavec::analysis {

/// Tunables for the analyses.
struct AnalysisOptions {
  /// `--report-unannotated` (RFC 0003): for every exported function
  /// definition, report pointer parameters and results without an
  /// annotation, offering the inferred one as a fix-it; and include callees
  /// declared in system headers in the external-boundary
  /// `annotation-required` report.
  bool reportUnannotated = false;
  /// `--strict-externs` (RFC 0004, *Boundaries*): a call into code with no
  /// definition, annotation or library summary is a raw operation. It is an
  /// `unsafe-operation` error outside an unsafe region and its pointer
  /// result is raw, instead of the RFC 0003 once-per-callee warning.
  bool strictExterns = false;
  /// RFC 0005, *`weavec-cc`*, compile step: record the callees that are
  /// boundaries (for the unit's exports) but do not emit the
  /// `annotation-required` warning for them; the link step decides whether
  /// the program defines them.
  bool deferBoundary = false;
  /// If set, print the inferred facts for every analysed function
  /// (`--dump-analysis`): places and their kinds, lifetimes, and the state
  /// at function exit. Intended for debugging and lit tests; the format is
  /// not stable.
  llvm::raw_ostream *dumpStream = nullptr;
};

/// Runs every WeaveC check over a single function definition.
///
/// Implements the sound intra-procedural checker of RFC 0002 (model:
/// RFC 0001): a forward dataflow over the function's `clang::CFG` whose
/// state is `core::AnalysisState`, followed by one final pass that reports
/// and records the function's summary (RFC 0003). Calls are interpreted
/// through the summaries in the `SummaryStore` handed to `analyze`;
/// `TranslationUnitAnalyzer` orders functions so callees come first.
class FunctionAnalyzer {
public:
  FunctionAnalyzer(clang::ASTContext &ctx, core::DiagnosticSink &diagSink,
                   AnalysisOptions analysisOptions = {});

  /// Analyzes `function`, which must have a body, resolving callees from
  /// `summaries` and recording the inferred summary into it. Diagnostics
  /// are emitted only if `emitDiagnostics`. Functions annotated
  /// `weavec.unsafe` are skipped entirely (their signature is their
  /// summary). Returns true if the recorded summary changed.
  bool analyze(const clang::FunctionDecl &function, SummaryStore &summaries,
               bool emitDiagnostics = true);

private:
  clang::ASTContext &context;
  core::DiagnosticSink &sink;
  AnalysisOptions options;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_FUNCTIONANALYSIS_H
