//===- AnalysisSummary.cpp - The analysis's summary lines -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/AnalysisSummary.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace weavec::frontend {

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
  llvm::SmallString<256> cwd(workingDirectory);
  if (cwd.empty() && llvm::sys::fs::current_path(cwd))
    return source.str();
  llvm::sys::path::remove_dots(cwd, /*remove_dot_dot=*/true);
  llvm::SmallString<256> path(source);
  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  llvm::StringRef prefix = cwd;
  if (prefix.size() > 1 && llvm::sys::path::is_separator(prefix.back()))
    prefix = prefix.drop_back();
  const llvm::StringRef full = path;
  if (full == prefix)
    return ".";
  if (full.starts_with(prefix) && full.size() > prefix.size() &&
      llvm::sys::path::is_separator(full[prefix.size()]))
    return llvm::sys::path::convert_to_slash(
        full.drop_front(prefix.size() + 1));
  return llvm::sys::path::convert_to_slash(full);
}

/// Writes one summary line to `os`.
static void printLine(const std::string &line, llvm::raw_ostream &os) {
  // The summary is one whole line on `os` (stderr), while the analysis dump
  // of `--dump-analysis` goes to the buffered `llvm::outs()`. A caller that
  // joins the two onto one file descriptor -- `2>&1 | FileCheck`, a
  // terminal, a log -- sees two independently buffered streams over one
  // descriptor, which interleave wherever a buffer happens to fill rather
  // than at a line boundary. Draining the dump stream first fixes the
  // order: the whole dump, then the summary.
  llvm::outs().flush();
  os << line << '\n';
  os.flush();
}

void printUnitSummary(core::Ledger ledger, llvm::StringRef name,
                      const DiagnosticControl &control, llvm::raw_ostream &os) {
  applyDiagnosticControl(ledger, control);
  printLine(core::unitSummaryLine(name, core::summarize(ledger)), os);
}

void printProgramSummary(core::Ledger ledger, llvm::StringRef program,
                         const DiagnosticControl &control,
                         llvm::raw_ostream &os) {
  applyDiagnosticControl(ledger, control);
  printLine(core::programSummaryLine(program, ledger.units.size(),
                                     core::summarize(ledger)),
            os);
}

} // namespace weavec::frontend
