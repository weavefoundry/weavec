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
#include "weavec/Frontend/ClangDiagnosticSink.h"
#include "weavec/Frontend/LedgerOutput.h"
#include "weavec/Frontend/Prelude.h"
#include "weavec/Frontend/RecordFacts.h"
#include "weavec/Frontend/ZeroInit.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include <array>
#include <string_view>
#include <utility>

namespace weavec::frontend {

// RFC 0030 (S5, begin): lowered violations and zero-initialisation.

/// The ids whose error reports a violation of `facet` (§3, §3.4).
static llvm::ArrayRef<std::string_view> violationIds(core::Facet facet) {
  static constexpr std::array Temporal{
      core::diag::UseAfterFree,       core::diag::UseAfterMove,
      core::diag::DoubleFree,         core::diag::MismatchedRelease,
      core::diag::ConflictingBorrow,  core::diag::LifetimeTooShort,
      core::diag::AnnotationMismatch,
  };
  static constexpr std::array Null{
      core::diag::NullDereference,
      core::diag::UseOfUninitialized,
      core::diag::AnnotationMismatch,
  };
  static constexpr std::array Spatial{
      core::diag::OutOfBounds,
      core::diag::InvalidRelease,
      core::diag::UnsafeOperation,
      core::diag::AnnotationMismatch,
  };
  static constexpr std::array Assertion{
      core::diag::ContradictedAssumption,
      core::diag::AnnotationMismatch,
  };
  switch (facet) {
  case core::Facet::Temporal:
    return Temporal;
  case core::Facet::Null:
    return Null;
  case core::Facet::Spatial:
    return Spatial;
  case core::Facet::Assertion:
    return Assertion;
  }
  return {};
}

/// §3.4: a definite violation whose error the `-W` flags lower to a warning
/// still traps, since the unit then produces an object. The planner asks
/// per facet; an id of the facet lowered is enough, since a violation whose
/// error stands fails the compile before any check is emitted.
static std::function<bool(core::SiteId, core::Facet)>
loweredViolations(const DiagnosticControl &control) {
  return [control](core::SiteId /*site*/, core::Facet facet) {
    for (const std::string_view id : violationIds(facet)) {
      const core::Diagnostic probe{.severity = core::Severity::Error,
                                   .certainty = core::Certainty::Definite,
                                   .id = id,
                                   .message = {},
                                   .location = {},
                                   .notes = {},
                                   .fixits = {}};
      const std::optional<core::Diagnostic> shown = control.apply(probe);
      if (shown && shown->severity == core::Severity::Warning)
        return true;
    }
    return false;
  };
}

/// §11: which references the unit lowers, and the ledger's A5 counts.
static std::shared_ptr<ZeroInitPlan>
zeroInitPlanOf(clang::ASTContext &context, const FrontendOptions &options) {
  // A freestanding unit's allocator is not the C library's, whose calloc
  // and usable-size query the wrappers call.
  const UsableSizeQuery query =
      usableSizeQueryFor(context.getTargetInfo().getTriple());
  const bool heap =
      query != UsableSizeQuery::None && !context.getLangOpts().Freestanding;
  return std::make_shared<ZeroInitPlan>(
      planZeroInit(context, core::LibrarySpec::shipped(),
                   options.config.checks != core::ChecksMode::None &&
                       options.config.zeroInit,
                   ZeroInitOptions{.heap = heap, .stack = true}));
}

// RFC 0030 (S5, end).

UnitResult analyzeTranslationUnit(clang::ASTContext &context,
                                  clang::DiagnosticsEngine &diagnostics,
                                  const FrontendOptions &options) {
  // RFC 0030 §1 steps 2 and 3: kinds, sites, the engine through the ledger
  // adapter, planning; the diagnostics come back in the engine's order.
  UnitResult result;
  analysis::UnitPipelineOptions pipeline;
  pipeline.engine.analysis = options.analysis;
  // RFC 0030 §5.5: the budget the ledger records is the one the engine
  // counts against.
  pipeline.engine.budget = options.config.budget;
  pipeline.engine.zeroInit = options.analysis.zeroInit;
  pipeline.engine.strictAliasing = options.analysis.strictAliasing;
  if (options.silent)
    pipeline.engine.analysis.dumpStream = nullptr;
  // RFC 0030 §5.6: every emitted function is analysed and reported, those
  // of user headers included (the engine asks the unit's sites); a silent
  // round reports nothing.
  if (options.silent)
    pipeline.engine.shouldReport = [](const clang::FunctionDecl &) {
      return false;
    };
  if (options.database != nullptr)
    pipeline.engine.dependencies = &result.dependencies;
  pipeline.database = options.database;
  pipeline.discoverOnly = options.discoverOnly;
  pipeline.config = options.config;
  pipeline.buildLedger = !options.silent;
  pipeline.lowered = loweredViolations(options.control);
  core::DiagnosticCollector collected;
  analysis::UnitPipelineResult unit =
      analysis::runUnitAnalysis(context, pipeline, collected);
  result.exports = std::move(unit.exports);
  result.ledger = std::move(unit.ledger);
  if (options.discoverOnly) {
    // RFC 0030 §13.2 step 2: the whole-program driver solves the slots of
    // every unit before it analyses any.
    if (options.collectInterface)
      result.interface = std::make_shared<const record::InterfaceFacts>(
          record::collectSlotFacts(context));
    return result;
  }
  // RFC 0030 §11: the zero-initialisation plan, decided before anything is
  // emitted so that the ledger's A5 counts are known when it is written.
  if (result.ledger && !result.ledger->ledger.units.empty()) {
    result.zeroInit = zeroInitPlanOf(context, options);
    result.ledger->ledger.units.front().a5 = result.zeroInit->a5;
  }
  // RFC 0030 §13.1: what the unit record carries beyond the summaries.
  if (options.collectInterface && !options.silent) {
    std::uint64_t lowered = 0;
    if (result.zeroInit)
      lowered = static_cast<std::uint64_t>(std::ranges::count_if(
          result.zeroInit->rewrites, [](const ZeroInitRewrite &rewrite) {
            return rewrite.kind != ZeroInitRewrite::Kind::Alloca;
          }));
    const record::FactsInput input{
        .exports = result.exports,
        .sites = result.ledger ? result.ledger->sites.get() : nullptr,
        .loweredAllocations = lowered,
        .library = nullptr};
    result.interface = std::make_shared<const record::InterfaceFacts>(
        record::collectInterfaceFacts(context, input));
  }

  ClangDiagnosticSink clangSink(diagnostics);
  FilteringSink sink(clangSink, options.control, options.alreadyReported,
                     options.onlyIds);
  if (!options.silent)
    for (const auto &diagnostic : collected.diagnostics())
      sink.report(diagnostic);
  result.reported = sink.reported();
  result.errors = sink.errors();
  result.warnings = sink.warnings();
  return result;
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
  // RFC 0030 §1 step 5: the unit ledger and the summary line.
  if (result.ledger)
    emitUnitLedger(result.ledger->ledger, ast, options);
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
    if (options.analysis.stats)
      options.analysis.stats->add("unit_parses");
    // RFC 0030 §3.1: under `-fno-strict-aliasing` any two pointee types may
    // designate one object.
    options.analysis.strictAliasing =
        !compiler.getCodeGenOpts().RelaxedAliasing;
  }
  void HandleTranslationUnit(clang::ASTContext &context) override {
    auto result =
        analyzeTranslationUnit(context, compiler.getDiagnostics(), options);
    // RFC 0030 §1 step 5: the unit ledger and the summary line.
    if (result.ledger)
      emitUnitLedger(result.ledger->ledger, compiler, options);
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
