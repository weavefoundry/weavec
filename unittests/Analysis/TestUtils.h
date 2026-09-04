//===- TestUtils.h - Shared helpers for analysis unit tests ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_UNITTESTS_ANALYSIS_TESTUTILS_H
#define WEAVEC_UNITTESTS_ANALYSIS_TESTUTILS_H

#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Analysis/Summaries.h"
#include "weavec/Analysis/TranslationUnitAnalysis.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"

#include <memory>
#include <string>
#include <vector>

namespace weavec::test {

/// Minimal prelude so tests can call `free`/`malloc` without system headers.
/// `OWNED`/`BORROWED`/`MUT`/`RAW`/`UNSAFE` spell the annotations without
/// `weavec.h`. `use` is the opaque "look at this pointer" helper; it is
/// annotated because an unannotated external function warns by default (RFC
/// 0003).
inline constexpr const char *Prelude = R"c(
typedef unsigned long size_t;
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
#define OWNED __attribute__((annotate("weavec.owned")))
#define BORROWED __attribute__((annotate("weavec.borrowed")))
#define MUT __attribute__((annotate("weavec.mut_borrowed")))
#define RAW __attribute__((annotate("weavec.raw")))
#define UNSAFE __attribute__((annotate("weavec.unsafe")))
#define NULLABLE __attribute__((annotate("weavec.nullable")))
#define NONNULL __attribute__((annotate("weavec.nonnull")))
void use(const void *BORROWED);
int cond(void);
#define NULL ((void *)0)
void take(void *OWNED p);
void peek(const void *BORROWED p);
void poke(void *MUT p);
#line 1
)c";

/// Parses `code` (prepended with `Prelude`) as C and runs the analyzer over
/// the translation unit, collecting core diagnostics and summaries.
struct AnalysisResult {
  std::unique_ptr<clang::ASTUnit> ast;
  core::DiagnosticCollector diagnostics;
  std::unique_ptr<analysis::TranslationUnitAnalyzer> analyzer;

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
                 analysis::AnalysisOptions options = {},
                 const std::string &fileName = "input.c") {
  AnalysisResult result;
  // `-w`: Clang's own warnings (e.g. -Wreturn-stack-address) are noise here.
  result.ast = clang::tooling::buildASTFromCodeWithArgs(
      std::string(Prelude) + code, {"-std=c17", "-x", "c", "-w"}, fileName);
  if (!result.ast)
    return result;

  clang::ASTContext &context = result.ast->getASTContext();
  result.analyzer = std::make_unique<analysis::TranslationUnitAnalyzer>(
      context, result.diagnostics, options);
  if (database != nullptr)
    result.analyzer->setDatabase(database);
  result.analyzer->run();
  return result;
}

inline AnalysisResult analyze(const std::string &code,
                              analysis::AnalysisOptions options = {}) {
  return analyzeInProgram(code, nullptr, options);
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
