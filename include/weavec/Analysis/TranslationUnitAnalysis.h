//===- TranslationUnitAnalysis.h - Whole-TU driver -------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Analyses every function definition in a translation unit in an order that
// lets callers see their callees' summaries (RFC 0003, *The translation-unit
// driver*): the direct call graph is split into strongly connected
// components, visited callees-first, and recursive components are iterated
// to a fixpoint before their diagnostics are emitted.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_TRANSLATIONUNITANALYSIS_H
#define WEAVEC_ANALYSIS_TRANSLATIONUNITANALYSIS_H

#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/Diagnostic.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <vector>

namespace weavec::analysis {

/// Runs WeaveC over a whole translation unit.
class TranslationUnitAnalyzer {
public:
  TranslationUnitAnalyzer(clang::ASTContext &ctx,
                          core::DiagnosticSink &diagSink,
                          AnalysisOptions analysisOptions = {});

  /// Analyses every function definition in the TU. Summaries are computed
  /// for all of them; diagnostics are emitted only for those `shouldReport`
  /// accepts (the frontend uses this for `mainFileOnly`).
  void run(llvm::function_ref<bool(const clang::FunctionDecl &)> shouldReport);

  /// Analyses and reports everything.
  void run() {
    run([](const clang::FunctionDecl &) { return true; });
  }

  /// The summaries inferred by `run`, plus the store's lookup facilities.
  [[nodiscard]] SummaryStore &summaries() noexcept { return store; }
  [[nodiscard]] const SummaryStore &summaries() const noexcept { return store; }

  /// Upper bound on fixpoint rounds for a recursive component. The summary
  /// lattice is finite, so this is a guard, not a budget.
  static constexpr unsigned MaxFixpointRounds = 16;

private:
  clang::ASTContext &context;
  core::DiagnosticSink &sink;
  AnalysisOptions options;
  SummaryStore store;

  /// Function definitions in source order.
  std::vector<const clang::FunctionDecl *> definitions;

  void collectDefinitions(const clang::DeclContext &dc);
  /// Registers every function used as a value with the store (RFC 0004,
  /// *Signatures for function pointers*).
  void collectAddressTaken();
  /// Direct edges plus, for each indirect call, an edge to every
  /// address-taken function of the callee's type.
  [[nodiscard]] std::vector<std::vector<unsigned>> buildCallGraph() const;
  void analyzeComponent(
      const std::vector<unsigned> &component, bool recursive,
      FunctionAnalyzer &analyzer,
      llvm::function_ref<bool(const clang::FunctionDecl &)> shouldReport);
  void reportUnannotatedInterface(const clang::FunctionDecl &function);
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_TRANSLATIONUNITANALYSIS_H
