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
#include "weavec/Core/Diagnostic.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"

#include <memory>
#include <string>
#include <vector>

namespace weavec::test {

/// Minimal prelude so tests can call `free`/`malloc` without system headers.
/// `OWNED`/`BORROWED`/`MUT` spell the annotations without `weavec.h`.
inline constexpr const char *Prelude = R"c(
typedef unsigned long size_t;
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
void use(void *);
int cond(void);
#define NULL ((void *)0)
#define OWNED __attribute__((annotate("weavec.owned")))
#define BORROWED __attribute__((annotate("weavec.borrowed")))
#define MUT __attribute__((annotate("weavec.mut_borrowed")))
void take(void *OWNED p);
void peek(const void *BORROWED p);
void poke(void *MUT p);
#line 1
)c";

/// Parses `code` (prepended with `Prelude`) as C and runs the analyzer over
/// every function definition, collecting core diagnostics.
struct AnalysisResult {
  std::unique_ptr<clang::ASTUnit> ast;
  core::DiagnosticCollector diagnostics;
};

inline AnalysisResult analyze(const std::string &code,
                              analysis::AnalysisOptions options = {}) {
  AnalysisResult result;
  // `-w`: Clang's own warnings (e.g. -Wreturn-stack-address) are noise here.
  result.ast = clang::tooling::buildASTFromCodeWithArgs(
      std::string(Prelude) + code, {"-std=c17", "-x", "c", "-w"}, "input.c");
  if (!result.ast)
    return result;

  clang::ASTContext &context = result.ast->getASTContext();
  analysis::FunctionAnalyzer analyzer(context, result.diagnostics, options);
  for (const clang::Decl *decl : context.getTranslationUnitDecl()->decls()) {
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl))
      analyzer.analyze(*function);
  }
  return result;
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
