//===- LinkStep.h - The whole-program link step (RFC 0030) ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §13.2, the parts of the link step that work on what the units'
// records say, independently of how the records were found (`weavec-cc`'s
// link inputs) or produced (`weavec --whole-program`'s runs):
//
//   2. `solveProgramSlots`: every member's function-pointer slot
//      constraints solved together under the link's closed-slot rules (§9.3);
//   3. `verifyDeclarations`: every import with declared annotations or
//      kinds compared with the defining member's summary and kinds; a
//      contradiction is an `annotation-mismatch` error at the declaration;
//   5. `allocatorDefinedBy` and the §11 warning; the reliance Call rows,
//      the A1 and A3 counts, and every member's boundary rows
//      (`programBoundaries`), which reach the engine's re-runs through
//      `analysis::ProgramFacts`;
//   6. `composeProgramLedger`: the program ledger. Each member's rows come
//      from its last run at link when there is one (temporal facets decided
//      with the whole program in view), with the spatial, null and
//      assertion facets copied from its record, because they decided the
//      emitted code; a member that was not run contributes its record's
//      compact rows. The Call rows of step 5 are added; temporal facets that
//      rest on a contradicted declaration become `unresolved(unknown-
//      callee)`, as does every function that calls one transitively; calls
//      into functions no record defines are `trusted(external-unit)` when an
//      input without a record exists (step 1).
//
// What waits for engine facts: deciding an exported requirement at a
// cross-unit caller (every one stays unverified under A1), the §7.6
// invariant verdicts (none are recorded, so A3 lists only conflicting slot
// kinds), and applying boundary rows to other units' facets (the engine
// would mark which facets rest on which entry assumption).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_LINKSTEP_H
#define WEAVEC_FRONTEND_LINKSTEP_H

#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/Diagnostic.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Frontend/RecordPayload.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace weavec::frontend {

/// One unit of the program: what its record says at link, or what its last
/// run produced in `weavec --whole-program`.
struct ProgramMember {
  /// The main source as the unit's compile named it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  /// The compile's working directory: relative paths in the payload are
  /// relative to it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string cwd = {};
  /// The link input the record belongs to; empty in `weavec
  /// --whole-program`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string object = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string target = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  record::Payload payload = {};
};

/// §9.3 and §13.2 step 1: what the link says about code outside the
/// members.
struct LinkShape {
  /// The output is an executable: neither `-shared` nor `-r`.
  bool executable = true;
  /// `-rdynamic`, or `-export-dynamic` passed to the linker.
  bool exportDynamic = false;
  /// Non-system link inputs without a valid record.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> inputsWithoutRecords = {};
};

/// `path`, taken against `base` when relative, relative to `cwd` when
/// inside it, else absolute.
[[nodiscard]] std::string
displayPath(llvm::StringRef path, llvm::StringRef base, llvm::StringRef cwd);

/// §13.2 step 2: every member's slots solved together (§9.3).
[[nodiscard]] core::SlotSolution
solveProgramSlots(std::span<const ProgramMember> members,
                  const LinkShape &shape);

/// §13.2 step 5: every member's boundary rows, each with its unit.
[[nodiscard]] std::vector<analysis::BoundaryRow>
programBoundaries(std::span<const ProgramMember> members);

/// The program's slots, with their targets and whether they are closed,
/// for `-fweavec-dump-analysis` at link and `--dump-analysis` in `weavec
/// --whole-program` (unstable format).
void dumpProgramSlots(const core::SlotSolution &slots, llvm::raw_ostream &os);

/// A declaration §13.2 step 3 found contradicted by the definition.
struct Contradiction {
  /// The member that declares the function, and the one that defines it.
  std::size_t member = 0;
  std::size_t definer = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
  /// What the declaration states and what the definition does, for the
  /// facets that rested on it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};

  friend bool operator==(const Contradiction &,
                         const Contradiction &) = default;
};

struct DeclarationCheck {
  /// `annotation-mismatch` errors at the declarations, each with the note
  /// `defined here`, in member and import order.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<core::Diagnostic> diagnostics = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Contradiction> contradictions = {};
};

/// §13.2 step 3. `cwd` is the link's working directory, against which the
/// messages name files.
[[nodiscard]] DeclarationCheck
verifyDeclarations(std::span<const ProgramMember> members, llvm::StringRef cwd);

/// §13.2 step 5: the member that defines the allocator (`malloc`, `calloc`,
/// `realloc` or `free`) while another member lowered allocation calls.
struct AllocatorFinding {
  std::size_t member = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
};
[[nodiscard]] std::optional<AllocatorFinding>
allocatorDefinedBy(std::span<const ProgramMember> members);
/// `heap zero-initialisation assumes the system allocator, but '<unit>'
/// defines '<function>'; rebuild with -fno-weavec-zero-init`.
[[nodiscard]] std::string
allocatorWarning(std::span<const ProgramMember> members,
                 const AllocatorFinding &finding, llvm::StringRef cwd);

/// §13.2 step 6.
struct ProgramLedgerInput {
  std::span<const ProgramMember> members;
  /// Per member, the ledger of its last run at link, or null when it was
  /// not run (its record's rows are used).
  std::span<const core::Ledger *const> runs;
  /// Copy the spatial, null and assertion facets of the members' rows over
  /// their runs' (false in `weavec --whole-program`, whose rows are its
  /// runs').
  bool copyRecordRows = true;
  const DeclarationCheck *declarations = nullptr;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  LinkShape shape = {};
  /// Diagnostics the link itself reported (`unanalyzed-input`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<core::Diagnostic> linkDiagnostics = {};
  /// The link's working directory.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string cwd = {};
};

/// The program ledger, scope `program`, with its assumptions; the producer,
/// root and config are left to `emitProgramLedger`.
[[nodiscard]] core::Ledger
composeProgramLedger(const ProgramLedgerInput &input);

/// Prints the link step's own diagnostics on stderr: one with a location as
/// a compile does (`file:line:column: error: ...` and the source line, the
/// file read on first use), the others after `<prefix>: `, as the driver
/// prints its own.
class LinkDiagnosticPrinter final : public core::DiagnosticSink {
public:
  explicit LinkDiagnosticPrinter(std::string prefix);
  ~LinkDiagnosticPrinter() override;
  LinkDiagnosticPrinter(const LinkDiagnosticPrinter &) = delete;
  LinkDiagnosticPrinter &operator=(const LinkDiagnosticPrinter &) = delete;
  LinkDiagnosticPrinter(LinkDiagnosticPrinter &&) = delete;
  LinkDiagnosticPrinter &operator=(LinkDiagnosticPrinter &&) = delete;

  void report(const core::Diagnostic &diagnostic) override;

private:
  struct Engines;
  std::unique_ptr<Engines> engines;
};

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_LINKSTEP_H
