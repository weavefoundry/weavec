//===- ProgramChecks.h - What a whole program checks -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §13.2 as RFC 0035 §8 keeps it: the parts of `weavec
// --whole-program` that work on every unit's interface facts at once.
//
//   - `solveProgramSlots`: every member's function-pointer slot constraints
//     solved together under the program's closed-slot rules (§9.3);
//   - `programBoundaries`: every member's boundary rows, which reach the
//     engine's runs through `analysis::ProgramFacts`;
//   - `verifyDeclarations`: every import with declared annotations or kinds
//     compared with the defining member's summary and kinds; a
//     contradiction is an `annotation-mismatch` error at the declaration.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_PROGRAMCHECKS_H
#define WEAVEC_FRONTEND_PROGRAMCHECKS_H

#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Frontend/InterfaceFacts.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace weavec::frontend {

/// One unit of the program, as its last run left it.
struct ProgramMember {
  /// The main source as the unit's compile command named it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  /// The compile command's working directory: relative paths in the facts
  /// are relative to it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string cwd = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  analysis::UnitExports exports = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  InterfaceFacts facts = {};
};

/// `path`, taken against `base` when relative, relative to `cwd` when
/// inside it, else absolute.
[[nodiscard]] std::string
displayPath(llvm::StringRef path, llvm::StringRef base, llvm::StringRef cwd);

/// Every member's slots solved together (§9.3). The slots of an executable
/// (a program that defines `main`) are closed.
[[nodiscard]] core::SlotSolution
solveProgramSlots(std::span<const ProgramMember> members, bool executable);

/// Every member's boundary rows, each with its unit.
[[nodiscard]] std::vector<analysis::BoundaryRow>
programBoundaries(std::span<const ProgramMember> members);

/// The program's slots, with their targets and whether they are closed,
/// for `--dump-analysis` in `weavec --whole-program` (unstable format).
void dumpProgramSlots(const core::SlotSolution &slots, llvm::raw_ostream &os);

/// The `annotation-mismatch` errors at the declarations a definition
/// contradicts, each with the note `defined here`, in member and import
/// order. `cwd` is the working directory against which the messages name
/// files.
[[nodiscard]] std::vector<core::Diagnostic>
verifyDeclarations(std::span<const ProgramMember> members, llvm::StringRef cwd);

/// Prints the program's own diagnostics on stderr: one with a location as a
/// compile does (`file:line:column: error: ...` and the source line, the
/// file read on first use), the others after `<prefix>: `.
class ProgramDiagnosticPrinter final : public core::DiagnosticSink {
public:
  explicit ProgramDiagnosticPrinter(std::string prefix);
  ~ProgramDiagnosticPrinter() override;
  ProgramDiagnosticPrinter(const ProgramDiagnosticPrinter &) = delete;
  ProgramDiagnosticPrinter &
  operator=(const ProgramDiagnosticPrinter &) = delete;
  ProgramDiagnosticPrinter(ProgramDiagnosticPrinter &&) = delete;
  ProgramDiagnosticPrinter &operator=(ProgramDiagnosticPrinter &&) = delete;

  void report(const core::Diagnostic &diagnostic) override;

private:
  struct Engines;
  std::unique_ptr<Engines> engines;
};

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_PROGRAMCHECKS_H
