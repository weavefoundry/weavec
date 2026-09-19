//===- TestUtils.h - Shared helpers for analysis unit tests ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_UNITTESTS_ANALYSIS_TESTUTILS_H
#define WEAVEC_UNITTESTS_ANALYSIS_TESTUTILS_H

#include "weavec/Analysis/AttributeReader.h"
#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Analysis/TranslationUnitAnalysis.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace weavec::test {

/// Minimal prelude so tests can call `free`/`malloc` without system headers.
/// `OWNED`/`BORROWED`/`MUT`/`RAW`/`UNSAFE` spell the annotations without
/// `weavec.h`. `use` is the opaque "look at this pointer" helper; it is
/// annotated because an unannotated external function is an unknown callee
/// (RFC 0030 §5.1), which may have freed what it was handed.
inline constexpr const char *Prelude = R"c(
typedef unsigned long size_t;
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
void exit(int) __attribute__((noreturn));
#define OWNED __attribute__((annotate("weavec.owned")))
#define BORROWED __attribute__((annotate("weavec.borrowed")))
#define MUT __attribute__((annotate("weavec.mut_borrowed")))
#define RAW __attribute__((annotate("weavec.raw")))
#define UNSAFE __attribute__((annotate("weavec.unsafe")))
#define NULLABLE __attribute__((annotate("weavec.nullable")))
#define NONNULL __attribute__((annotate("weavec.nonnull")))
#define RETAINS __attribute__((annotate("weavec.retains")))
#define RELEASES __attribute__((annotate("weavec.releases")))
#define REFCOUNT __attribute__((annotate("weavec.refcount")))
#define OWNED_BY(f) __attribute__((annotate("weavec.family." #f)))
void use(const void *BORROWED);
int cond(void);
#define NULL ((void *)0)
void take(void *OWNED p);
void peek(const void *BORROWED p);
void poke(void *MUT p);
#line 1
)c";

/// RFC 0030 §14: what the engine publishes through, for one unit: the
/// declared kinds, the sites and the authoritative adapter.
struct UnitLedgerHarness {
  analysis::KindTable kinds;
  analysis::SiteIndex sites;
  std::unique_ptr<analysis::LedgerAdapter> ledger;

  explicit UnitLedgerHarness(clang::ASTContext &context) {
    const core::LibrarySpec &library = core::LibrarySpec::shipped();
    kinds = analysis::AttributeReader(context, library).read();
    sites = analysis::SiteCollector(context, kinds, library).collect();
    ledger = std::make_unique<analysis::LedgerAdapter>(context, sites);
  }

  /// Completes the ledger (which checks it is complete) and copies the
  /// diagnostics, in publication order, into `collector`.
  analysis::PlannedLedger finish(core::DiagnosticCollector &collector) const {
    analysis::PlannedLedger planned = ledger->finish();
    for (const core::Diagnostic &diagnostic : ledger->diagnostics())
      collector.report(diagnostic);
    return planned;
  }
};

/// Parses `code` (prepended with `Prelude`) as C and runs the analyzer over
/// the translation unit, collecting core diagnostics and summaries.
struct AnalysisResult {
  std::unique_ptr<clang::ASTUnit> ast;
  core::DiagnosticCollector diagnostics;
  std::unique_ptr<UnitLedgerHarness> harness;
  std::unique_ptr<analysis::TranslationUnitAnalyzer> analyzer;
  /// The completed unit ledger (RFC 0030 §12).
  analysis::PlannedLedger planned;

  /// The function definition named `name`, or null.
  [[nodiscard]] const clang::FunctionDecl *
  function(llvm::StringRef name) const {
    if (!ast)
      return nullptr;
    for (const clang::Decl *decl :
         ast->getASTContext().getTranslationUnitDecl()->decls()) {
      const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (fn != nullptr && fn->getName() == name)
        return fn;
    }
    return nullptr;
  }

  /// The inferred summary of the function named `name`, or null.
  [[nodiscard]] const core::FunctionSummary *
  summary(llvm::StringRef name) const {
    const clang::FunctionDecl *fn = function(name);
    if (fn == nullptr || !analyzer)
      return nullptr;
    return analyzer->summaries().inferredFor(*fn);
  }
};

/// Like `analyze`, with the exports of other units attached (RFC 0005).
/// `database` must outlive the result.
inline AnalysisResult
analyzeInProgram(const std::string &code,
                 const analysis::ProgramDatabase *database,
                 const analysis::AnalysisOptions &options = {},
                 const std::string &fileName = "input.c") {
  AnalysisResult result;
  // `-w`: Clang's own warnings (e.g. -Wreturn-stack-address) are noise here.
  result.ast = clang::tooling::buildASTFromCodeWithArgs(
      std::string(Prelude) + code, {"-std=c17", "-x", "c", "-w"}, fileName);
  if (!result.ast)
    return result;
  if (result.ast->getDiagnostics().hasErrorOccurred()) {
    result.ast.reset();
    return result;
  }

  clang::ASTContext &context = result.ast->getASTContext();
  result.harness = std::make_unique<UnitLedgerHarness>(context);
  result.analyzer = std::make_unique<analysis::TranslationUnitAnalyzer>(
      context, *result.harness->ledger, options);
  if (database != nullptr)
    result.analyzer->setDatabase(database);
  result.analyzer->run();
  result.planned = result.harness->finish(result.diagnostics);
  return result;
}

/// Runs a fresh analyzer over `context`, reporting only the functions
/// `shouldReport` accepts, and returns its diagnostics.
inline core::DiagnosticCollector analyzeFiltered(
    clang::ASTContext &context,
    llvm::function_ref<bool(const clang::FunctionDecl &)> shouldReport,
    const analysis::AnalysisOptions &options = {}) {
  UnitLedgerHarness harness(context);
  analysis::TranslationUnitAnalyzer analyzer(context, *harness.ledger, options);
  analyzer.run(shouldReport);
  core::DiagnosticCollector collected;
  (void)harness.finish(collected);
  return collected;
}

inline AnalysisResult analyze(const std::string &code,
                              const analysis::AnalysisOptions &options = {}) {
  return analyzeInProgram(code, nullptr, options);
}

/// RFC 0030 §15 item 3: where the engine could not model a construct, as
/// `"<line>: <facet> <reason>: <what>"` for every unresolved facet whose
/// detail is an incompleteness some function's summary records, in ledger
/// order. (Before RFC 0030 each was an `analysis-incomplete` warning.)
inline std::vector<std::string> incomplete(const AnalysisResult &result) {
  std::vector<std::string> out;
  if (!result.ast)
    return out;
  std::set<std::string> recorded;
  for (const clang::Decl *decl :
       result.ast->getASTContext().getTranslationUnitDecl()->decls()) {
    const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (fn == nullptr || !fn->doesThisDeclarationHaveABody())
      continue;
    if (const core::FunctionSummary *summary = result.summary(fn->getName()))
      recorded.insert(summary->incomplete.begin(), summary->incomplete.end());
  }
  for (const core::UnitLedger &unit : result.planned.ledger.units)
    for (const core::FunctionLedger &function : unit.functions)
      for (const core::Site &site : function.sites)
        for (const core::Facet facet : core::AllFacets) {
          const core::FacetRecord *record = site.facet(facet);
          if (record == nullptr ||
              record->outcome() != core::SiteOutcome::Unresolved ||
              !recorded.contains(record->decision.detail))
            continue;
          out.push_back(std::to_string(site.location.line) + ": " +
                        std::string(core::toString(facet)) + " " +
                        std::string(record->decision.reasonText()) + ": " +
                        record->decision.detail);
        }
  return out;
}

/// RFC 0030 §5.1: the call sites whose temporal facet is
/// `unresolved(unknown-callee)` (a call into code the analysis cannot see),
/// as `"<line>: <site text>"` in ledger order. (Before RFC 0030 such a callee
/// was an `annotation-required` warning, once per callee.)
inline std::vector<std::string> unknownCalls(const AnalysisResult &result) {
  std::vector<std::string> out;
  for (const core::UnitLedger &unit : result.planned.ledger.units)
    for (const core::FunctionLedger &function : unit.functions)
      for (const core::Site &site : function.sites) {
        const core::FacetRecord *record = site.facet(core::Facet::Temporal);
        if (site.kind == core::SiteKind::Call &&
            site.boundary == core::Boundary::Call && record != nullptr &&
            record->decision.unresolved ==
                core::UnresolvedReason::UnknownCallee)
          out.push_back(std::to_string(site.location.line) + ": " + site.text);
      }
  return out;
}

/// Returns the ids of all reported (non-note) diagnostics, in order.
inline std::vector<std::string>
ids(const core::DiagnosticCollector &collector) {
  std::vector<std::string> result;
  for (const core::Diagnostic &d : collector.diagnostics())
    result.emplace_back(d.id);
  return result;
}

/// Returns `"<line>: <message>"` for every diagnostic, in order. Lines count
/// from the first line of the test snippet (the prelude resets them with
/// `#line 1`), so the raw string's opening `R"c(` newline is line 1.
inline std::vector<std::string>
messages(const core::DiagnosticCollector &collector) {
  std::vector<std::string> result;
  for (const core::Diagnostic &d : collector.diagnostics())
    result.push_back(std::to_string(d.location.line) + ": " + d.message);
  return result;
}

/// Returns the note messages attached to the `index`-th diagnostic.
inline std::vector<std::string>
notes(const core::DiagnosticCollector &collector, std::size_t index = 0) {
  std::vector<std::string> result;
  if (index >= collector.size())
    return result;
  for (const core::Diagnostic &note : collector.diagnostics()[index].notes)
    result.push_back(note.message);
  return result;
}

} // namespace weavec::test

#endif // WEAVEC_UNITTESTS_ANALYSIS_TESTUTILS_H
