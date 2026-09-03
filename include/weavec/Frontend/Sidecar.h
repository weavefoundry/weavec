//===- Sidecar.h - The per-object summary file -----------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `weavec-cc` writes what a translation unit exports to `<output>.weavec`
// next to the object it compiled, and reads those files back at link time
// (RFC 0005, *`weavec-cc`*). The file is line oriented:
//
//   weavec-summaries 2
//   source <path>
//   cwd <path>
//   arg <one cc1 argument>              (repeated, in order)
//   import <name>                       (repeated)
//   indirect <type key>                 (repeated)
//   unknown <name>                      (repeated)
//   unknown-indirect <type key>         (repeated)
//   reported <id> <line> <column> <file>   (repeated)
//   function <name> <external|internal> <address-taken|plain> <type key>
//   summary ... end                     (RFC 0005 record, globals by name)
//
// Everything after the header is optional and order-independent except that
// a `summary` record belongs to the `function` line before it.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_SIDECAR_H
#define WEAVEC_FRONTEND_SIDECAR_H

#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Frontend/DiagnosticControl.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weavec::frontend {

/// Version 2 (RFC 0006): summaries in format 2 (`outcome`, `interior`).
/// Version 3 (RFC 0007): summaries in format 3 (release families).
inline constexpr unsigned SidecarFormatVersion = 3;

/// Everything the driver remembers about one compiled unit.
struct UnitRecord {
  analysis::UnitExports exports;
  /// The `-cc1` command line (without `-cc1`) that produced the unit, so the
  /// link step can analyse it again.
  std::vector<std::string> command;
  /// The directory the command ran in.
  std::string workingDirectory;
  /// Diagnostics already shown to the user for this unit.
  std::set<ReportedDiagnostic> reported;
};

/// `<output>.weavec`.
[[nodiscard]] std::string sidecarPathFor(llvm::StringRef output);

[[nodiscard]] std::string printUnitRecord(const UnitRecord &record);

/// Parses a sidecar. `nullopt` with `error` set on a version this build
/// cannot read or a malformed line.
[[nodiscard]] std::optional<UnitRecord>
parseUnitRecord(llvm::StringRef text, std::string *error = nullptr);

/// Writes `record` to `path` atomically enough for a build: a temporary
/// next to it, then a rename.
bool writeSidecar(llvm::StringRef path, const UnitRecord &record,
                  std::string *error = nullptr);

[[nodiscard]] std::optional<UnitRecord>
readSidecar(llvm::StringRef path, std::string *error = nullptr);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_SIDECAR_H
