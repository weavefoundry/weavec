//===- Definitions.h - Safe units agree when they are linked ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0051 §5 (N6). A unit with safe functions records what its safe code
// believes of the names it shares with other units: the layout and the
// annotations of each type other units can name, the declaration of each
// function it defines or calls, the type of each global it defines or
// names; each as a name, a hash and a place. The record goes in a section
// of the unit's object (`__DATA,__weavec_defs`, `.weavec.defs`), and
// `weavec-cc` compares the records of every input of a link before the
// linker runs: two that disagree are an error, and the program is not
// linked. Nothing is inferred from another unit: it is a comparison.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DEFINITIONS_H
#define WEAVEC_FRONTEND_DEFINITIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace clang {
class ASTContext;
} // namespace clang

namespace weavec::frontend {

struct SafeUnit;

/// The section's names: Mach-O's segment and section, and ELF's.
inline constexpr llvm::StringLiteral DefinitionsSectionMachO =
    "__DATA,__weavec_defs";
inline constexpr llvm::StringLiteral DefinitionsSectionName = "__weavec_defs";
inline constexpr llvm::StringLiteral DefinitionsSectionELF = ".weavec.defs";
/// The first line of each unit's record.
inline constexpr llvm::StringLiteral DefinitionsHeader = "weavec-defs 1";

/// §5.1: the record of a unit with safe functions, from its AST at the end
/// of the unit; empty when it has none.
[[nodiscard]] std::string recordDefinitions(clang::ASTContext &ast,
                                            const SafeUnit &unit);

/// What one comparison found.
struct DefinitionsCompared {
  unsigned objects = 0;
  unsigned safe = 0;
  unsigned types = 0;
  unsigned functions = 0;
  unsigned globals = 0;
  unsigned mismatches = 0;
};

/// §5.2: compares the records of `inputs` (objects, archives, linked
/// images), writes an error and its notes to `errors` for each name two
/// of them disagree on, and returns what it found.
[[nodiscard]] DefinitionsCompared
compareDefinitions(llvm::ArrayRef<std::string> inputs,
                   llvm::raw_ostream &errors);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DEFINITIONS_H
