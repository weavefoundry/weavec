//===- FrontendAction.cpp - Clang frontend integration --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/FrontendAction.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/TranslationUnitAnalysis.h"
#include "weavec/Core/SafetyEntryPool.h"
#include "weavec/Frontend/CheckedArtifacts.h"
#include "weavec/Frontend/ClangDiagnosticSink.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <utility>

namespace weavec::frontend {

UnitResult replayUnitResult(UnitResult result,
                            clang::DiagnosticsEngine &diagnostics,
                            const FrontendOptions &options) {
  ClangDiagnosticSink clangSink(diagnostics);
  FilteringSink sink(clangSink, options.control, options.alreadyReported,
                     options.boundaryOnce, options.onlyIds);
  if (!options.silent)
    for (const auto &diagnostic : result.diagnostics)
      sink.report(diagnostic);
  const bool failure =
      !options.silent &&
      CheckedReport::failed(result.exports, options.analysis.deferCheckedCalls);
  if (!options.silent && options.checkedReport)
    options.checkedReport->record(result.exports);
  if (failure)
    diagnostics.Report(diagnostics.getCustomDiagID(
        clang::DiagnosticsEngine::Error,
        "checked safety requirements were not established"));
  result.reported = sink.reported();
  result.errors = sink.errors() + (failure ? 1 : 0);
  result.warnings = sink.warnings();
  return result;
}

UnitResult analyzeTranslationUnit(clang::ASTContext &context,
                                  clang::DiagnosticsEngine &diagnostics,
                                  const FrontendOptions &options) {
  core::SafetyEntryPool explanations(options.analysis.stats);
  analysis::AnalysisOptions analysisOptions = options.analysis;
  analysisOptions.checkedMainFileOnly = options.mainFileOnly;
  if (options.silent)
    analysisOptions.dumpStream = nullptr;
  core::DiagnosticCollector collected;
  analysis::TranslationUnitAnalyzer analyzer(context, collected,
                                             analysisOptions);
  analyzer.setDatabase(options.database);
  UnitResult result;
  const bool trackImports =
      options.database != nullptr || !options.analysisCache.empty();
  if (trackImports)
    analyzer.summaries().beginDependencies(result.dependencies);
  if (options.discoverOnly) {
    result.exports = analyzer.discover();
    if (trackImports)
      analyzer.summaries().endDependencies();
    return result;
  }
  const clang::SourceManager &sm = context.getSourceManager();
  analyzer.run([&](const clang::FunctionDecl &function) {
    return !options.silent &&
           (!options.mainFileOnly || sm.isInMainFile(function.getLocation()) ||
            options.analysis.checkedFunctions.contains(
                function.getNameAsString()) ||
            analysis::getAnnotations(function).checked);
  });
  result.exports = analyzer.exports();
  if (options.bindCheckedInputs || !result.exports.checkedDefinitions.empty() ||
      !options.analysisCache.empty()) {
    for (auto file = sm.fileinfo_begin(); file != sm.fileinfo_end(); ++file) {
      const auto buffer = file->second->getBufferIfLoaded();
      if (!buffer)
        continue;
      llvm::SmallString<256> absolute;
      const auto name = file->first.getName();
      if (llvm::sys::fs::real_path(name, absolute))
        absolute = name;
      result.exports.checkedInputs[absolute.str().str()] =
          checkedDigest(buffer->getBuffer());
    }
  }
  result.diagnostics = collected.diagnostics();
  if (trackImports)
    analyzer.summaries().endDependencies();
  return replayUnitResult(std::move(result), diagnostics, options);
}

UnitResult analyzeRetainedUnit(clang::ASTUnit &ast,
                               const FrontendOptions &options,
                               const UnitResult *checkpoint) {
  auto &diagnostics = ast.getDiagnostics();
  auto *previous = diagnostics.getClient();
  auto owned = diagnostics.takeClient();
  clang::TextDiagnosticPrinter printer(llvm::errs(),
                                       diagnostics.getDiagnosticOptions());
  diagnostics.setClient(&printer, false);
  diagnostics.Reset(true);
  printer.BeginSourceFile(ast.getLangOpts(), &ast.getPreprocessor());
  auto result = checkpoint ? replayUnitResult(*checkpoint, diagnostics, options)
                           : analyzeTranslationUnit(ast.getASTContext(),
                                                    diagnostics, options);
  if (diagnostics.hasErrorOccurred() && result.errors == 0)
    result.errors = 1;
  printer.EndSourceFile();
  const auto warnings = printer.getNumWarnings();
  const auto errors = printer.getNumErrors();
  if (warnings || errors) {
    if (warnings)
      llvm::errs() << warnings << " warning" << (warnings == 1 ? "" : "s");
    if (warnings && errors)
      llvm::errs() << " and ";
    if (errors)
      llvm::errs() << errors << " error" << (errors == 1 ? "" : "s");
    llvm::errs() << " generated.\n";
  }
  const bool owns = static_cast<bool>(owned);
  diagnostics.setClient(owns ? owned.release() : previous, owns);
  return result;
}

namespace {

class WeaveCConsumer final : public clang::ASTConsumer {
public:
  WeaveCConsumer(clang::CompilerInstance &compiler, FrontendOptions opts)
      : compiler(compiler), options(std::move(opts)) {
    if (options.analysis.stats)
      options.analysis.stats->add("unit_parses");
  }
  void HandleTranslationUnit(clang::ASTContext &context) override {
    auto result =
        analyzeTranslationUnit(context, compiler.getDiagnostics(), options);
    if (options.onResult)
      options.onResult(std::move(result));
  }

private:
  clang::CompilerInstance &compiler;
  FrontendOptions options;
};

class WeaveCActionFactory final : public clang::tooling::FrontendActionFactory {
public:
  explicit WeaveCActionFactory(FrontendOptions opts)
      : options(std::move(opts)) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<WeaveCAction>(options);
  }

private:
  FrontendOptions options;
};

} // namespace

std::unique_ptr<clang::ASTConsumer>
createWeaveCConsumer(clang::CompilerInstance &compiler,
                     const FrontendOptions &options) {
  return std::make_unique<WeaveCConsumer>(compiler, options);
}

std::unique_ptr<clang::ASTConsumer>
WeaveCAction::CreateASTConsumer(clang::CompilerInstance &compiler,
                                llvm::StringRef /*inFile*/) {
  return createWeaveCConsumer(compiler, options);
}

std::unique_ptr<clang::tooling::FrontendActionFactory>
createWeaveCActionFactory(FrontendOptions options) {
  return std::make_unique<WeaveCActionFactory>(std::move(options));
}

} // namespace weavec::frontend
