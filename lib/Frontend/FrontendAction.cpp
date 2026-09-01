//===- FrontendAction.cpp - Clang frontend integration --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/FrontendAction.h"

#include "weavec/Analysis/FunctionAnalysis.h"
#include "weavec/Frontend/ClangDiagnosticSink.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclBase.h"
#include "clang/Frontend/CompilerInstance.h"

#include <utility>

namespace weavec::frontend {

namespace {

/// Visits every function definition in the translation unit and runs the
/// analyses over it.
class WeaveCConsumer final : public clang::ASTConsumer {
public:
  WeaveCConsumer(clang::CompilerInstance &compiler, FrontendOptions opts)
      : sink(compiler.getDiagnostics()), options(opts) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    analysis::FunctionAnalyzer analyzer(context, sink, options.analysis);
    visitContext(context, *context.getTranslationUnitDecl(), analyzer);
  }

private:
  ClangDiagnosticSink sink;
  FrontendOptions options;

  void visitContext(clang::ASTContext &context, const clang::DeclContext &dc,
                    analysis::FunctionAnalyzer &analyzer) {
    const clang::SourceManager &sm = context.getSourceManager();
    for (const clang::Decl *decl : dc.decls()) {
      if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if (!function->doesThisDeclarationHaveABody())
          continue;
        if (options.mainFileOnly && !sm.isInMainFile(function->getLocation()))
          continue;
        analyzer.analyze(*function);
        continue;
      }
      // C has no nested namespaces, but linkage specs / extern blocks and
      // record scopes can still contain declarations worth visiting.
      if (const auto *nested = llvm::dyn_cast<clang::DeclContext>(decl))
        visitContext(context, *nested, analyzer);
    }
  }
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
