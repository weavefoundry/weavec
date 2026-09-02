//===- FrontendAction.cpp - Clang frontend integration --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/FrontendAction.h"

#include "weavec/Analysis/TranslationUnitAnalysis.h"
#include "weavec/Frontend/ClangDiagnosticSink.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"

#include <utility>

namespace weavec::frontend {

namespace {

/// Hands the whole translation unit to the analysis driver, which orders
/// functions callees-first (RFC 0003). Every definition contributes a
/// summary; only those in the main file are reported unless
/// `--analyze-headers`.
class WeaveCConsumer final : public clang::ASTConsumer {
public:
  WeaveCConsumer(clang::CompilerInstance &compiler, FrontendOptions opts)
      : sink(compiler.getDiagnostics()), options(opts) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    analysis::TranslationUnitAnalyzer analyzer(context, sink, options.analysis);
    const clang::SourceManager &sm = context.getSourceManager();
    analyzer.run([this, &sm](const clang::FunctionDecl &function) {
      return !options.mainFileOnly || sm.isInMainFile(function.getLocation());
    });
  }

private:
  ClangDiagnosticSink sink;
  FrontendOptions options;
};

class WeaveCActionFactory final : public clang::tooling::FrontendActionFactory {
public:
  explicit WeaveCActionFactory(FrontendOptions opts) : options(opts) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<WeaveCAction>(options);
  }

private:
  FrontendOptions options;
};

} // namespace

std::unique_ptr<clang::ASTConsumer>
WeaveCAction::CreateASTConsumer(clang::CompilerInstance &compiler,
                                llvm::StringRef /*inFile*/) {
  return std::make_unique<WeaveCConsumer>(compiler, options);
}

std::unique_ptr<clang::tooling::FrontendActionFactory>
createWeaveCActionFactory(FrontendOptions options) {
  return std::make_unique<WeaveCActionFactory>(options);
}

} // namespace weavec::frontend
