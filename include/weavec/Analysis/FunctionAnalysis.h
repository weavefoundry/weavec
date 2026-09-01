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

namespace weavec::analysis {

/// Tunables for the per-function analyses.
struct AnalysisOptions {
  /// Report pointer-typed parameters/locals whose ownership could not be
  /// inferred and that carry no annotation.
  bool reportUnannotated = false;
};

/// Runs every WeaveC check over a single function definition.
///
/// The current implementation is an intentionally small, path-insensitive
/// checker for local heap ownership (use-after-free / double-free of locals
/// released with `free`). It exists to exercise the end-to-end pipeline and
/// will be superseded by a CFG-based dataflow analysis; see
/// docs/design/ownership-model.md.
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
