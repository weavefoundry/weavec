//===- AnalysisSummary.h - The analysis's summary lines --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §8: `weavec` prints one line on stderr for every unit it
// analyses, and `weavec --whole-program` one more for the program, from
// the sites the analysis decided and the diagnostics it reported:
//
//   weavec: cJSON.c: 4,210 sites: 3,050 proven, 1,130 not proven,
//     0 violations, 30 trusted; 0 errors, 2 warnings
//
// The counts follow the `-W` flags, as the diagnostics shown do.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_ANALYSISSUMMARY_H
#define WEAVEC_FRONTEND_ANALYSISSUMMARY_H

#include "weavec/Core/Ledger.h"
#include "weavec/Frontend/DiagnosticControl.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace weavec::frontend {

/// Applies the `-W` flags to the ledger's diagnostics, so the summary line
/// counts what was reported: severities are lowered or raised, disabled
/// diagnostics are dropped, and a facet linked to a dropped one loses its
/// link.
void applyDiagnosticControl(core::Ledger &ledger,
                            const DiagnosticControl &control);

/// The name the summary line gives `source`: as given when relative, else
/// relative to `workingDirectory` (the process's when empty) when inside it.
[[nodiscard]] std::string summaryName(llvm::StringRef source,
                                      llvm::StringRef workingDirectory);

/// Prints the unit form of the summary line of `ledger` (one unit) to `os`,
/// with its diagnostics counted after `control`.
void printUnitSummary(core::Ledger ledger, llvm::StringRef name,
                      const DiagnosticControl &control,
                      llvm::raw_ostream &os = llvm::errs());

/// Prints the program form, for a ledger of every unit of the program.
void printProgramSummary(core::Ledger ledger, llvm::StringRef program,
                         const DiagnosticControl &control,
                         llvm::raw_ostream &os = llvm::errs());

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_ANALYSISSUMMARY_H
