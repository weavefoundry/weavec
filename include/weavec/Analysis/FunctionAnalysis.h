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

#include "weavec/Core/Diagnostic.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/Support/raw_ostream.h"

namespace weavec::analysis {

/// Tunables for the per-function analyses.
struct AnalysisOptions {
  /// Report pointer-typed parameters/locals whose ownership could not be
  /// inferred and that carry no annotation.
  bool reportUnannotated = false;
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
/// state is `core::AnalysisState`, followed by one reporting pass. Function
/// signatures are not inferred; parameters are treated per their annotations.
class FunctionAnalyzer {
public:
  FunctionAnalyzer(clang::ASTContext &ctx, core::DiagnosticSink &diagSink,
                   AnalysisOptions analysisOptions = {});

  /// Analyzes `function`, which must have a body. Functions annotated
  /// `weavec.unsafe` are skipped entirely.
  void analyze(const clang::FunctionDecl &function);

private:
  clang::ASTContext &context;
  core::DiagnosticSink &sink;
  AnalysisOptions options;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_FUNCTIONANALYSIS_H
