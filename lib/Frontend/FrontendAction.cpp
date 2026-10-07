//===- FrontendAction.cpp - Clang frontend integration --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/FrontendAction.h"

#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/UnitPipeline.h"
#include "weavec/Frontend/AnalysisSummary.h"
#include "weavec/Frontend/ClangDiagnosticSink.h"
#include "weavec/Frontend/InterfaceFacts.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace weavec::frontend {

UnitResult analyzeTranslationUnit(clang::ASTContext &context,
                                  clang::DiagnosticsEngine &diagnostics,
                                  const FrontendOptions &options) {
  // RFC 0030 §1 steps 2 and 3: kinds, sites, the engine through the ledger
  // adapter, planning; the diagnostics come back in the engine's order.
  UnitResult result;
  // As Clang's own analyzer does: a unit that failed to parse is not
  // analysed (its records may have no layout).
  if (diagnostics.hasUncompilableErrorOccurred()) {
    result.errors = 1;
    return result;
  }
  analysis::UnitPipelineOptions pipeline;
  pipeline.engine = options.engine;
  if (options.silent)
    pipeline.engine.dumpStream = nullptr;
  // RFC 0030 §5.6: every emitted function is analysed and reported, those
  // of user headers included (the engine asks the unit's sites); a silent
  // round reports nothing.
  if (options.silent)
    pipeline.engine.shouldReport = [](const clang::FunctionDecl &) {
      return false;
    };
  pipeline.database = options.database;
  pipeline.discoverOnly = options.discoverOnly;
  pipeline.buildLedger = !options.silent;
  core::DiagnosticCollector collected;
  analysis::UnitPipelineResult unit =
      analysis::runUnitAnalysis(context, pipeline, collected);
  result.exports = std::move(unit.exports);
  result.ledger = std::move(unit.ledger);
  if (options.holdFor && !options.silent)
    result.held =
        std::ranges::any_of(result.exports.contextRequests, options.holdFor);
  if (result.held) {
    result.ledger = nullptr;
    return result;
  }
  if (options.discoverOnly) {
    // RFC 0030 §13.2 step 2: the whole-program driver solves the slots of
    // every unit before it analyses any.
    if (options.collectInterface)
      result.interface =
          std::make_shared<const InterfaceFacts>(collectSlotFacts(context));
    return result;
  }
  // What `weavec --whole-program` checks across units.
  if (options.collectInterface && !options.silent)
    result.interface = std::make_shared<const InterfaceFacts>(
        collectInterfaceFacts(context, FactsInput{.exports = result.exports,
                                                  .kinds = unit.kinds.get()}));

  ClangDiagnosticSink clangSink(diagnostics);
  FilteringSink sink(clangSink, options.control, options.alreadyReported);
  if (!options.silent)
    for (const auto &diagnostic : collected.diagnostics())
      sink.report(diagnostic);
  result.reported = sink.reported();
  result.errors = sink.errors();
  result.warnings = sink.warnings();
  return result;
}

/// The directory the unit's relative paths are relative to.
static std::string workingDirectoryOf(clang::FileManager &files) {
  std::string cwd = files.getFileSystemOpts().WorkingDir;
  if (cwd.empty())
    if (const llvm::ErrorOr<std::string> current =
            files.getVirtualFileSystem().getCurrentWorkingDirectory())
      cwd = *current;
  return cwd;
}

/// RFC 0035 §8: the summary line of a reporting run, under
/// `FrontendOptions::summary`.
static void printSummary(const UnitResult &result, llvm::StringRef source,
                         clang::FileManager &files,
                         const FrontendOptions &options) {
  if (!options.summary || options.silent || options.discoverOnly ||
      !result.ledger)
    return;
  printUnitSummary(*result.ledger,
                   summaryName(source, workingDirectoryOf(files)),
                   options.control);
}

UnitResult analyzeRetainedUnit(clang::ASTUnit &ast,
                               const FrontendOptions &options) {
  auto &diagnostics = ast.getDiagnostics();
  auto *previous = diagnostics.getClient();
  auto owned = diagnostics.takeClient();
  // RFC 0020: Clang renders each diagnostic in many small writes. A private
  // bounded stderr stream combines them without changing global stream state
  // or retaining a translation unit's entire output. Descriptor 2 stays open.
  class DiagnosticStream final : public llvm::raw_ostream {
  public:
    DiagnosticStream() : llvm::raw_ostream(true), destination(2, false) {
      destination.SetBufferSize(16384);
      enable_colors(llvm::errs().colors_enabled());
      destination.enable_colors(colors_enabled());
    }
    void finishDiagnostic() { destination.flush(); }
    bool is_displayed() const override { return destination.is_displayed(); }
    bool has_colors() const override { return destination.has_colors(); }

  private:
    // Clang's formatted stream takes its immediate sink's buffer. Keep this
    // facade unbuffered so locationless diagnostics cannot bypass a pending
    // source snippet in that formatted stream. Only the destination buffers.
    llvm::raw_fd_ostream destination;
    void write_impl(const char *data, std::size_t size) override {
      destination.write(data, size);
    }
    std::uint64_t current_pos() const override { return destination.tell(); }
  };
  DiagnosticStream diagnosticOutput;
  class BufferedDiagnosticPrinter final : public clang::TextDiagnosticPrinter {
  public:
    BufferedDiagnosticPrinter(DiagnosticStream &output,
                              clang::DiagnosticOptions &options)
        : clang::TextDiagnosticPrinter(output, options), output(output) {}
    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic &info) override {
      clang::TextDiagnosticPrinter::HandleDiagnostic(level, info);
      // Preserve prompt delivery, including an isolated error before a later
      // long-running analysis or fatal exit.
      output.finishDiagnostic();
    }

  private:
    DiagnosticStream &output;
  };
  BufferedDiagnosticPrinter printer(diagnosticOutput,
                                    diagnostics.getDiagnosticOptions());
  diagnostics.setClient(&printer, false);
  diagnostics.Reset(true);
  printer.BeginSourceFile(ast.getLangOpts(), &ast.getPreprocessor());
  auto result =
      analyzeTranslationUnit(ast.getASTContext(), diagnostics, options);
  printSummary(result, ast.getMainFileName(), ast.getFileManager(), options);
  if (diagnostics.hasErrorOccurred() && result.errors == 0)
    result.errors = 1;
  printer.EndSourceFile();
  diagnosticOutput.finishDiagnostic();
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
    if (options.engine.stats)
      options.engine.stats->add("unit_parses");
    // RFC 0030 §3.1: under `-fno-strict-aliasing` any two pointee types may
    // designate one object.
    options.engine.strictAliasing = !compiler.getCodeGenOpts().RelaxedAliasing;
  }
  void HandleTranslationUnit(clang::ASTContext &context) override {
    auto result =
        analyzeTranslationUnit(context, compiler.getDiagnostics(), options);
    const clang::FrontendOptions &frontend = compiler.getFrontendOpts();
    if (!frontend.Inputs.empty() && frontend.Inputs.front().isFile() &&
        compiler.hasFileManager())
      printSummary(result, frontend.Inputs.front().getFile(),
                   compiler.getFileManager(), options);
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
