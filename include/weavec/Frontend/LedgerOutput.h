//===- LedgerOutput.h - Where a finished ledger goes -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §1 step 5, §12 and §16: once a unit (or the program) has its
// ledger, the ledger is completed with what the compile job knows (the
// producer, the fingerprint root, the configuration, the unit's source,
// object and target), written to the file or directory `-fweavec-ledger`
// (`--ledger` in `weavec`) names, in the format of `-fweavec-ledger-format`,
// through a temporary file renamed into place, and summarised in one line
// on stderr (§12.4): under `-fweavec-summary`, whenever a ledger is written,
// and always in `weavec`.
//
// The WeaveC consumer calls `emitUnitLedger(ledger, compiler, options)` once
// per unit, after the unit pipeline produced the ledger; a retained unit
// (`analyzeRetainedUnit`, whole-program analysis) calls the `ASTUnit` form.
// The link step calls `emitProgramLedger` with the program ledger it
// composed.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_LEDGEROUTPUT_H
#define WEAVEC_FRONTEND_LEDGEROUTPUT_H

#include "weavec/Core/Ledger.h"
#include "weavec/Frontend/DiagnosticControl.h"
#include "weavec/Frontend/LedgerWriter.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>

namespace clang {
class ASTUnit;
class CompilerInstance;
} // namespace clang

namespace weavec::frontend {

struct FrontendOptions;

/// RFC 0030, fallback point A (*Implementation plan*): before `CheckEmitter`
/// (S5) no check was emitted, and the summary line of every mode said
/// `checkable (not enforced)`. `weavec-cc` now emits the checks, so its
/// summary line says `checked` unless the checks are `none`.
inline constexpr bool ChecksAreEmitted = true;

/// `-fweavec-ledger`, `-fweavec-ledger-format` and `-f[no-]weavec-summary`,
/// or their `weavec` equivalents.
struct LedgerOutputOptions {
  /// The `-fweavec-ledger` value: a file, or a directory (the value ends in
  /// a path separator or names an existing directory) that receives
  /// `<artifact>.ledger.json` for each ledger, `<artifact>` being the object
  /// at compile (the source when there is none) and the output at link.
  /// Empty writes no ledger.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string path = {};
  LedgerFormat format = LedgerFormat::Json;
  /// `-f[no-]weavec-summary`. Unset, the summary line is printed exactly
  /// when a ledger is written; `weavec` sets it, since it always prints it.
  std::optional<bool> summary = std::nullopt;
  /// Whether checks are enforced: false in `weavec`, with
  /// `-fweavec-checks=none`, and while `ChecksAreEmitted` is false. The
  /// summary line then says `checkable (not enforced)`.
  bool checksEnforced = false;

  [[nodiscard]] bool writesLedger() const noexcept { return !path.empty(); }
  [[nodiscard]] bool printsSummary() const noexcept {
    return summary.value_or(writesLedger());
  }

  friend bool operator==(const LedgerOutputOptions &,
                         const LedgerOutputOptions &) = default;
};

/// What the compile job knows about the unit a ledger describes.
struct UnitIdentity {
  /// The main source as the command line named it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  /// The object the job writes; empty when it writes none (`weavec`,
  /// `-fsyntax-only`, output to stdout).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string object = {};
  /// The target triple.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string target = {};
  /// The compile job's working directory; empty for the process's.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string workingDirectory = {};
};

/// The identity of the unit `compiler` is compiling.
[[nodiscard]] UnitIdentity
unitIdentityOf(const clang::CompilerInstance &compiler);
/// The identity of the unit `ast` holds; it has no object.
[[nodiscard]] UnitIdentity unitIdentityOf(const clang::ASTUnit &ast);

/// Applies the `-W` flags to the ledger's diagnostics, so the ledger and the
/// summary line count what was reported: severities are lowered or raised,
/// disabled diagnostics are dropped, and a facet linked to a dropped one
/// loses its link.
void applyDiagnosticControl(core::Ledger &ledger,
                            const DiagnosticControl &control);

/// The name the summary line gives `source`: as given when relative, else
/// relative to `workingDirectory` (the process's when empty) when inside it.
[[nodiscard]] std::string summaryName(llvm::StringRef source,
                                      llvm::StringRef workingDirectory);

/// Completes a unit ledger (producer, `scope`, the fingerprint root unless
/// set, `config`, the unit's source, object and target where the ledger
/// leaves them empty, diagnostics in source order), writes it where
/// `options` say and prints its summary line to `summaryStream` when they
/// say so. False, with `error` set, when the ledger cannot be written.
bool emitUnitLedger(core::Ledger &ledger, const UnitIdentity &unit,
                    const core::LedgerConfig &config,
                    const LedgerOutputOptions &options,
                    llvm::raw_ostream &summaryStream = llvm::errs(),
                    std::string *error = nullptr);

/// The same for a program ledger (the link step, `weavec --whole-program`):
/// `output` is the link output, which names the ledger in the directory
/// form and the program in the summary line.
bool emitProgramLedger(core::Ledger &ledger, llvm::StringRef output,
                       llvm::StringRef workingDirectory,
                       const core::LedgerConfig &config,
                       const LedgerOutputOptions &options,
                       llvm::raw_ostream &summaryStream = llvm::errs(),
                       std::string *error = nullptr);

/// The hook for the WeaveC consumer, called once per unit with the ledger
/// the unit pipeline produced. It does nothing for a silent or
/// discover-only run; otherwise it applies `options.control`, takes the
/// identity from `compiler` and the rest from `options.config` and
/// `options.ledgerOutput`, and reports a failed write as an error through
/// `compiler`'s diagnostics engine, so the compile fails. False after such
/// an error.
bool emitUnitLedger(core::Ledger &ledger,
                    const clang::CompilerInstance &compiler,
                    const FrontendOptions &options);
/// The same for a retained unit, reporting through `ast`'s engine.
bool emitUnitLedger(core::Ledger &ledger, clang::ASTUnit &ast,
                    const FrontendOptions &options);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_LEDGEROUTPUT_H
