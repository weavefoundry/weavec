//===- LedgerOutput.cpp - Where a finished ledger goes --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/LedgerOutput.h"

#include "weavec/Config/Version.h"
#include "weavec/Frontend/FrontendAction.h"

#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace weavec::frontend {

static std::string currentDirectory() {
  llvm::SmallString<256> cwd;
  if (llvm::sys::fs::current_path(cwd))
    return {};
  return cwd.str().str();
}

UnitIdentity unitIdentityOf(const clang::CompilerInstance &compiler) {
  UnitIdentity unit;
  const clang::FrontendOptions &frontend = compiler.getFrontendOpts();
  if (!frontend.Inputs.empty() && frontend.Inputs.front().isFile())
    unit.source = frontend.Inputs.front().getFile().str();
  if (frontend.OutputFile != "-")
    unit.object = frontend.OutputFile;
  unit.target = compiler.getTargetOpts().Triple;
  unit.workingDirectory = compiler.getFileSystemOpts().WorkingDir;
  if (unit.workingDirectory.empty() && compiler.hasFileManager()) {
    if (const llvm::ErrorOr<std::string> cwd =
            compiler.getFileManager()
                .getVirtualFileSystem()
                .getCurrentWorkingDirectory())
      unit.workingDirectory = *cwd;
  }
  return unit;
}

UnitIdentity unitIdentityOf(const clang::ASTUnit &ast) {
  UnitIdentity unit;
  unit.source = ast.getMainFileName().str();
  unit.target = ast.getASTContext().getTargetInfo().getTriple().str();
  const clang::FileManager &files = ast.getFileManager();
  unit.workingDirectory = files.getFileSystemOpts().WorkingDir;
  if (unit.workingDirectory.empty()) {
    if (const llvm::ErrorOr<std::string> cwd =
            files.getVirtualFileSystem().getCurrentWorkingDirectory())
      unit.workingDirectory = *cwd;
  }
  return unit;
}

void applyDiagnosticControl(core::Ledger &ledger,
                            const DiagnosticControl &control) {
  std::vector<std::optional<std::uint32_t>> moved(ledger.diagnostics.size());
  std::vector<core::LedgerDiagnostic> shown;
  shown.reserve(ledger.diagnostics.size());
  for (std::size_t i = 0; i < ledger.diagnostics.size(); ++i) {
    core::LedgerDiagnostic &diagnostic = ledger.diagnostics[i];
    const std::optional<core::Diagnostic> adjusted =
        control.apply(core::Diagnostic{.severity = diagnostic.severity,
                                       .certainty = diagnostic.certainty,
                                       .id = diagnostic.id,
                                       .message = {},
                                       .location = {},
                                       .notes = {},
                                       .fixits = {}});
    if (!adjusted)
      continue;
    diagnostic.severity = adjusted->severity;
    moved[i] = static_cast<std::uint32_t>(shown.size());
    shown.push_back(std::move(diagnostic));
  }
  ledger.diagnostics = std::move(shown);
  for (core::UnitLedger &unit : ledger.units)
    for (core::FunctionLedger &function : unit.functions)
      for (core::Site &site : function.sites)
        for (std::optional<core::FacetRecord> &record : site.facets)
          if (record && record->diagnostic)
            record->diagnostic = *record->diagnostic < moved.size()
                                     ? moved[*record->diagnostic]
                                     : std::nullopt;
}

std::string summaryName(llvm::StringRef source,
                        llvm::StringRef workingDirectory) {
  if (!llvm::sys::path::is_absolute(source))
    return source.str();
  const std::string cwd =
      workingDirectory.empty() ? currentDirectory() : workingDirectory.str();
  return pathRelativeToRoot(source, cwd, cwd);
}

/// The `producer`, `scope`, `root` and `config` of a ledger about to be
/// written; `rootSource` is the file whose nearest `.git` ancestor is the
/// fingerprint root (§12.3).
static void completeHeader(core::Ledger &ledger, core::LedgerScope scope,
                           const core::LedgerConfig &config,
                           llvm::StringRef rootSource,
                           llvm::StringRef workingDirectory) {
  ledger.producer.name = "weavec";
  ledger.producer.version = WEAVEC_VERSION_STRING;
  ledger.producer.revision = WEAVEC_GIT_REVISION;
  if (WEAVEC_GIT_DIRTY)
    ledger.producer.revision += "-dirty";
  ledger.scope = scope;
  if (ledger.root.empty())
    ledger.root = detectFingerprintRoot(rootSource, workingDirectory);
  ledger.config = config;
  core::sortDiagnostics(ledger);
}

/// Writes `ledger` for `artifact` where `options` say, creating the
/// directory it goes to.
static bool writeFor(const core::Ledger &ledger, llvm::StringRef artifact,
                     llvm::StringRef workingDirectory,
                     const LedgerOutputOptions &options, std::string *error) {
  const std::string path = ledgerPathFor(options.path, artifact);
  const llvm::StringRef directory = llvm::sys::path::parent_path(path);
  if (!directory.empty()) {
    if (const std::error_code ec =
            llvm::sys::fs::create_directories(directory)) {
      if (error != nullptr)
        *error = "cannot create the ledger directory '" + directory.str() +
                 "': " + ec.message();
      return false;
    }
  }
  return writeLedger(
      path, ledger, options.format,
      LedgerWriteOptions{.workingDirectory = workingDirectory.str()}, error);
}

static void printSummary(const core::Ledger &ledger, llvm::StringRef name,
                         const LedgerOutputOptions &options,
                         llvm::raw_ostream &summaryStream) {
  // The summary is one whole line on `summaryStream` (stderr), while the
  // analysis dump of `--dump-analysis` (`-fweavec-dump-analysis`) goes to the
  // buffered `llvm::outs()`. A caller that joins the two onto one file
  // descriptor -- `2>&1 | FileCheck`, a terminal, a log -- sees two
  // independently buffered streams over one descriptor, which interleave
  // wherever a buffer happens to fill rather than at a line boundary. The
  // buffer size follows the destination's `st_blksize`, so on glibc, where a
  // pipe reports 4 KiB, the flush lands mid-line and the summary is written
  // into the middle of a dump line. Draining the dump stream first fixes the
  // order: the whole dump, then the summary.
  llvm::outs().flush();
  const core::SummaryLineOptions line{
      .checksEnforced = options.checksEnforced &&
                        ledger.config.checks != core::ChecksMode::None};
  summaryStream << core::summaryLine(ledger, name, line) << '\n';
  summaryStream.flush();
}

bool emitUnitLedger(core::Ledger &ledger, const UnitIdentity &unit,
                    const core::LedgerConfig &config,
                    const LedgerOutputOptions &options,
                    llvm::raw_ostream &summaryStream, std::string *error) {
  if (ledger.units.empty())
    ledger.units.emplace_back();
  if (ledger.units.size() == 1) {
    core::UnitLedger &only = ledger.units.front();
    if (only.source.empty())
      only.source = unit.source;
    if (only.object.empty())
      only.object = unit.object;
    if (only.target.empty())
      only.target = unit.target;
  }
  completeHeader(ledger, core::LedgerScope::Unit, config, unit.source,
                 unit.workingDirectory);
  bool written = true;
  if (options.writesLedger())
    written = writeFor(ledger, unit.object.empty() ? unit.source : unit.object,
                       unit.workingDirectory, options, error);
  if (options.printsSummary())
    printSummary(ledger, summaryName(unit.source, unit.workingDirectory),
                 options, summaryStream);
  return written;
}

bool emitProgramLedger(core::Ledger &ledger, llvm::StringRef output,
                       llvm::StringRef workingDirectory,
                       const core::LedgerConfig &config,
                       const LedgerOutputOptions &options,
                       llvm::raw_ostream &summaryStream, std::string *error) {
  const std::string rootSource =
      ledger.units.empty() ? output.str() : ledger.units.front().source;
  completeHeader(ledger, core::LedgerScope::Program, config, rootSource,
                 workingDirectory);
  bool written = true;
  if (options.writesLedger())
    written = writeFor(ledger, output, workingDirectory, options, error);
  if (options.printsSummary())
    printSummary(ledger, llvm::sys::path::filename(output), options,
                 summaryStream);
  return written;
}

/// Whether a run with `options` writes or prints anything about its unit.
static bool emitsUnitOutput(const FrontendOptions &options) {
  const LedgerOutputOptions &output = options.ledgerOutput;
  return !options.silent && !options.discoverOnly &&
         (output.writesLedger() || output.printsSummary());
}

/// The hooks' common part: a failed write is an error of `diagnostics`.
static bool emitUnitLedgerReporting(core::Ledger &ledger,
                                    const UnitIdentity &unit,
                                    clang::DiagnosticsEngine &diagnostics,
                                    const FrontendOptions &options) {
  applyDiagnosticControl(ledger, options.control);
  std::string error;
  if (emitUnitLedger(ledger, unit, options.config, options.ledgerOutput,
                     llvm::errs(), &error))
    return true;
  diagnostics.Report(diagnostics.getCustomDiagID(
      clang::DiagnosticsEngine::Error, "cannot write the WeaveC ledger: %0"))
      << error;
  return false;
}

bool emitUnitLedger(core::Ledger &ledger,
                    const clang::CompilerInstance &compiler,
                    const FrontendOptions &options) {
  return !emitsUnitOutput(options) ||
         emitUnitLedgerReporting(ledger, unitIdentityOf(compiler),
                                 compiler.getDiagnostics(), options);
}

bool emitUnitLedger(core::Ledger &ledger, clang::ASTUnit &ast,
                    const FrontendOptions &options) {
  return !emitsUnitOutput(options) ||
         emitUnitLedgerReporting(ledger, unitIdentityOf(ast),
                                 ast.getDiagnostics(), options);
}

} // namespace weavec::frontend
