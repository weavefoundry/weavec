//===- ProgramDatabase.h - Summaries across translation units --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What one translation unit exports to the rest of the program and the
// database that collects those exports (RFC 0005, *Programs, units and
// exports* and *The program database*). Summaries in exports and in the
// database name globals through a `GlobalNames` table rather than a unit's
// `GlobalTable`, so they mean the same thing in every unit.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_PROGRAMDATABASE_H
#define WEAVEC_ANALYSIS_PROGRAMDATABASE_H

#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weavec::analysis {

class GlobalTable;

/// Interns global variable names for the summaries of an export set or a
/// database, mirroring `GlobalTable` without a Clang declaration behind
/// each id.
class GlobalNames {
public:
  [[nodiscard]] std::uint32_t idFor(llvm::StringRef name);
  [[nodiscard]] std::optional<std::uint32_t> find(llvm::StringRef name) const;
  [[nodiscard]] llvm::StringRef nameOf(std::uint32_t id) const;
  [[nodiscard]] std::size_t size() const noexcept { return names.size(); }

  /// If one table is a prefix of the other (ids agree wherever both have
  /// them), makes this the longer one and returns true; otherwise leaves it
  /// unchanged and returns false.
  bool extendTo(const GlobalNames &other);

  friend bool operator==(const GlobalNames &, const GlobalNames &) = default;

private:
  std::vector<std::string> names;
  std::map<std::string, std::uint32_t, std::less<>> ids;
};

/// One function a unit exports.
struct ExportedFunction {
  /// The summary a caller in the exporting unit would see (annotations
  /// applied), with globals numbered by the unit's `GlobalNames`.
  core::FunctionSummary summary;
  /// `functionTypeKey` of the definition; empty if the type has no stable
  /// spelling (an anonymous record is involved).
  std::string typeKey;
  /// External linkage: callable by name from another unit.
  bool external = true;
  /// Its address is taken somewhere in the unit: reachable through a
  /// pointer of its type from another unit.
  bool addressTaken = false;

  friend bool operator==(const ExportedFunction &,
                         const ExportedFunction &) = default;
};

/// Everything one translation unit contributes to, and needs from, the
/// program.
struct UnitExports {
  /// The main source file, for messages and the dump.
  std::string source;
  /// Exported definitions by linkage name.
  std::map<std::string, ExportedFunction> functions;
  /// Names the summaries above use for global roots.
  GlobalNames globals;
  /// External-linkage callees with no definition in the unit.
  std::set<std::string> imports;
  /// Type keys of the unit's indirect calls.
  std::set<std::string> indirectTypes;
  /// Callees `imports` contains for which the unit had no summary at all
  /// (the boundary of RFC 0003), plus indirect type keys with no
  /// candidates: what `annotation-required` would have warned about.
  std::set<std::string> unknownCallees;
  std::set<std::string> unknownIndirectTypes;

  /// True if the exported summaries are the same; the fixpoint test of RFC
  /// 0005's whole-program algorithm.
  [[nodiscard]] bool sameSummariesAs(const UnitExports &other) const;
};

/// The canonical spelling of a function type that identifies indirect-call
/// candidates across units, or an empty string when the type involves an
/// anonymous record and so has no stable spelling.
[[nodiscard]] std::string functionTypeKey(clang::QualType type,
                                          const clang::ASTContext &context);

/// The exports of every unit of a program except the one being analysed.
class ProgramDatabase {
public:
  /// Adds a unit's exports. A name defined by more than one unit gets the
  /// join of the definitions' summaries (RFC 0005, *Accepted false
  /// positives*). Summaries numbered by a table this one extends, or that
  /// extends this one (see `renumbered`), are copied rather than renumbered.
  void add(const UnitExports &unit);

  /// `unit` with its summaries numbered by this database's table, which is
  /// extended with any names it did not have; the result's `globals` is a
  /// copy of `globals()`. Rebuilding a database from such exports is a copy
  /// per summary instead of a renumbering, which is what the whole-program
  /// fixpoint does once per changed member (RFC 0005, *Performance*).
  [[nodiscard]] UnitExports renumbered(const UnitExports &unit);
  void clear();
  [[nodiscard]] bool empty() const noexcept { return functions.empty(); }

  /// Whether some unit defines `name` with external linkage.
  [[nodiscard]] bool defines(llvm::StringRef name) const;

  /// The joined summary of `name`'s external definitions, with globals
  /// numbered by `globals()`; null if no unit defines it.
  [[nodiscard]] const core::FunctionSummary *find(llvm::StringRef name) const;

  /// The joined summary of every address-taken function of type `typeKey`,
  /// or null if there is none.
  [[nodiscard]] const core::FunctionSummary *
  candidates(llvm::StringRef typeKey) const;

  [[nodiscard]] const GlobalNames &globals() const noexcept {
    return globalNames;
  }

  /// Rewrites a database summary for use in the unit `context` describes:
  /// each global root becomes the unit's external-linkage variable of that
  /// name, interned in `table`, or is dropped if the unit declares none.
  [[nodiscard]] core::FunctionSummary
  importInto(const core::FunctionSummary &summary,
             const clang::ASTContext &context, GlobalTable &table) const;

  /// Sorted names of every exported function, then every type key with
  /// candidates, in the RFC 0003 dump spelling (for `--dump-analysis`).
  void dump(llvm::raw_ostream &os) const;

private:
  std::map<std::string, core::FunctionSummary, std::less<>> functions;
  std::map<std::string, core::FunctionSummary, std::less<>> candidateSummaries;
  GlobalNames globalNames;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_PROGRAMDATABASE_H
