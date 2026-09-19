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
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/Diagnostic.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <functional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// Runs WeaveC over a whole translation unit.
///
/// RFC 0030 §2.6, §15 item 18: every fixpoint round (the callback-global
/// rounds, recursive components, specialisations) publishes into a
/// discarding adapter. Then each reported function gets one *authoritative*
/// pass, the context-insensitive analysis of its body with the unit's
/// sized-field facts in force (RFC 0012's second pass folded in), opened by
/// `LedgerAdapter::beginFunction`. Context-specialised runs decide no rows;
/// the call that requests one reports its diagnostics, linked to that
/// call's site, and what no call here reported (a request from another
/// unit) is reported last, linked to nothing.
class TranslationUnitAnalyzer {
public:
  TranslationUnitAnalyzer(clang::ASTContext &ctx, LedgerAdapter &ledgerAdapter,
                          AnalysisOptions analysisOptions = {});

  /// Analyses every function definition in the TU. Summaries are computed
  /// for all of them; the authoritative pass runs only for those
  /// `shouldReport` accepts (the frontend uses this for `mainFileOnly`).
  void run(llvm::function_ref<bool(const clang::FunctionDecl &)> shouldReport);

  /// Analyses and reports everything.
  void run() {
    run([](const clang::FunctionDecl &) { return true; });
  }

  /// Attaches the exports of the other units of the program (RFC 0005);
  /// call before `run`. The database must outlive the analyzer.
  void setDatabase(const ProgramDatabase *database) {
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
  LedgerAdapter &ledger;
  /// Where the fixpoint rounds publish (RFC 0030 §2.6).
  LedgerAdapter discarding;
  AnalysisOptions options;
  SummaryStore store;
  struct SilentAnalysis {
    bool widen;
    SummaryStore::DependencyVersions dependencies;
  };
  std::map<const clang::FunctionDecl *, SilentAnalysis> silentAnalyses;
  bool analyzeSilently(const clang::FunctionDecl &function,
                       FunctionAnalyzer &analyzer, bool widen);

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
  void analyzeComponent(const std::vector<unsigned> &component, bool recursive,
                        FunctionAnalyzer &analyzer);
  /// `--dump-analysis`: the memory contexts of `function` and their
  /// summaries.
  void dumpMemoryContexts(const clang::FunctionDecl &function);
  /// The findings of `function`'s context runs that no call in this unit
  /// reported (requests from other units), linked to nothing.
  void reportUnclaimedContexts(const clang::FunctionDecl &function);
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_TRANSLATIONUNITANALYSIS_H
