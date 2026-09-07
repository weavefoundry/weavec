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
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/Diagnostic.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <set>
#include <string>
#include <tuple>
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

  /// Attaches the exports of the other units of the program (RFC 0005);
  /// call before `run`. The database must outlive the analyzer.
  void setDatabase(const ProgramDatabase *database) noexcept {
    store.setDatabase(database);
  }

  /// What the unit defines, imports and calls indirectly, without
  /// analysing anything: the discovery pass of RFC 0005's whole-program
  /// algorithm. Summaries in the result are empty.
  [[nodiscard]] UnitExports discover();

  /// The unit's exports after `run` (RFC 0005, *Programs, units and
  /// exports*): every external-linkage or address-taken definition with the
  /// summary a caller here would see, globals by name, plus the imports and
  /// the callees that were boundaries.
  [[nodiscard]] UnitExports exports();

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
  /// RFC 0017: reporting retains the recursive fixpoint's approximation.
  std::set<const clang::FunctionDecl *> recursiveFunctions;
  /// Direct callees with no definition in the unit and the type keys of the
  /// indirect calls, collected by `buildCallGraph` for the exports.
  std::vector<const clang::FunctionDecl *> externalCallees;
  std::set<std::string> indirectTypeKeys;

  void collectDefinitions(const clang::DeclContext &dc);
  /// Registers every function used as a value with the store (RFC 0004,
  /// *Signatures for function pointers*).
  void collectAddressTaken();
  /// Direct edges plus, for each indirect call, an edge to every
  /// address-taken function of the callee's type.
  [[nodiscard]] std::vector<std::vector<unsigned>> buildCallGraph();
  /// `collectDefinitions`, `collectAddressTaken` and `buildCallGraph`.
  void prepare();
  /// The exports without summaries: definitions, imports, indirect types.
  [[nodiscard]] UnitExports skeletonExports() const;
  void analyzeComponent(
      const std::vector<unsigned> &component, bool recursive,
      FunctionAnalyzer &analyzer,
      llvm::function_ref<bool(const clang::FunctionDecl &)> shouldReport);
  void reportUnannotatedInterface(const clang::FunctionDecl &function);

  // -- Sized fields (RFC 0012, *Two passes in a unit*) ------------------------

  /// A diagnostic by id, location and message: what "the same report"
  /// means between the two passes.
  using DiagnosticKey =
      std::tuple<std::string, std::string, unsigned, unsigned, std::string>;
  /// Forwards to the unit's sink and remembers what went through.
  class RememberingSink final : public core::DiagnosticSink {
  public:
    explicit RememberingSink(core::DiagnosticSink &sink) : inner(sink) {}
    void report(const core::Diagnostic &diagnostic) override;
    [[nodiscard]] const std::set<DiagnosticKey> &seen() const noexcept {
      return keys;
    }
    [[nodiscard]] static DiagnosticKey keyOf(const core::Diagnostic &);

  private:
    core::DiagnosticSink &inner;
    std::set<DiagnosticKey> keys;
  };
  /// Re-analyses the reported functions that load a field the unit's own
  /// witnesses confirmed, emitting the `out-of-bounds` reports the first
  /// pass did not produce.
  void reportConfirmedSizedFields(
      llvm::ArrayRef<const clang::FunctionDecl *> reported,
      const std::set<DiagnosticKey> &alreadyReported);
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_TRANSLATIONUNITANALYSIS_H
