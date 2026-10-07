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
// exports* and *The program database*; RFC 0031 §7). Summaries in exports
// and in the database name globals through a `GlobalNames` table, so they
// mean the same thing in every unit.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_PROGRAMDATABASE_H
#define WEAVEC_ANALYSIS_PROGRAMDATABASE_H

#include "weavec/Core/Effects.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Core/Ledger.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weavec::analysis {

/// Names of the globals a set of summaries refers to, by id.
class GlobalNames {
public:
  [[nodiscard]] std::uint32_t idFor(llvm::StringRef name);
  [[nodiscard]] llvm::StringRef nameOf(std::uint32_t id) const;
  [[nodiscard]] std::size_t size() const noexcept { return names.size(); }

  friend bool operator==(const GlobalNames &, const GlobalNames &) = default;

private:
  std::vector<std::string> names;
  std::map<std::string, std::uint32_t, std::less<>> ids;
};

/// One function a unit exports.
struct ExportedFunction {
  /// The summary (RFC 0031 §6), with globals numbered by the unit's
  /// `GlobalNames`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::FunctionEffects effects = {};
  /// `functionTypeKey` of the definition; empty if the type has no stable
  /// spelling (an anonymous record is involved).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string typeKey = {};
  /// External linkage: callable by name from another unit.
  bool external = true;
  /// Its address is taken somewhere in the unit: reachable through a
  /// pointer of its type from another unit.
  bool addressTaken = false;

  friend bool operator==(const ExportedFunction &,
                         const ExportedFunction &) = default;
};

/// RFC 0030 §9.4, §13.1 `boundaries`: one `dangling-escape` or
/// `second-owner` row of a unit, with the place class it concerns, for
/// program-wide propagation (§13.2 step 5).
struct BoundaryRow {
  /// The unit's main source; empty inside the unit's own record.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string unit = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
  /// The boundary's site ordinal within `function`.
  std::uint32_t site = 0;
  core::UnresolvedReason reason = core::UnresolvedReason::DanglingEscape;
  /// A global `g`, or a field path `<struct>.<field>...`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string placeClass = {};

  friend auto operator<=>(const BoundaryRow &, const BoundaryRow &) = default;
};

/// RFC 0031 §6.6, §7 *Amendment (cross-unit contexts)*: a caller's request
/// that a function another unit defines be analysed in the context of a
/// call: the arguments it makes one object, the integers it knows (and the
/// integers their objects hold), and the callbacks it passes.
struct ContextRequest {
  /// The callee's portable name (its own for external linkage,
  /// `<source>#<name>` otherwise).
  std::string callee;
  /// The context, spelled by the engine (`contextKeyText`).
  std::string key;

  friend auto operator<=>(const ContextRequest &,
                          const ContextRequest &) = default;
};

/// What a unit exports (RFC 0031 §7).
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
  /// RFC 0030 §9.4: the unit's boundary rows, for the program-wide
  /// propagation of §13.2 step 5. Filled after the engine, from what
  /// `BoundaryInvariants` made of the boundaries the engine published.
  std::vector<BoundaryRow> boundaries;

  /// The contexts this unit's calls into other units asked for.
  std::set<ContextRequest> contextRequests;
  /// The summaries of this unit's functions in the contexts other units
  /// asked for, globals numbered by `globals`.
  std::map<ContextRequest, core::FunctionEffects> contextEffects;
  /// True if the exported summaries are the same; the fixpoint test of RFC
  /// 0005's whole-program algorithm.
  [[nodiscard]] bool sameSummariesAs(const UnitExports &other) const;
  /// The same, leaving out the contexts asked and served (RFC 0031 §7):
  /// what a cyclic component's fixpoint iterates on.
  [[nodiscard]] bool sameFunctionsAs(const UnitExports &other) const;
};

/// RFC 0030 §13.2: what `weavec --whole-program` knows about the whole
/// program beyond the summaries, for the engine's runs over its units: the
/// function-pointer slots solved over every unit (step 2, §9.3) and every
/// unit's boundary rows (step 5, §9.4).
struct ProgramFacts {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::SlotSolution slots = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<BoundaryRow> boundaries = {};
};

/// The canonical spelling of a function type that identifies indirect-call
/// candidates across units, or an empty string when the type involves an
/// anonymous record and so has no stable spelling.
[[nodiscard]] std::string functionTypeKey(clang::QualType type,
                                          const clang::ASTContext &context);

[[nodiscard]] std::string recordTypeKey(clang::QualType type,
                                        const clang::ASTContext &context);

/// The exports of every unit of a program except the one being analysed.
class ProgramDatabase {
public:
  /// RFC 0030 §13.2: the program's solved slots and boundary rows, set by
  /// the whole-program driver for every run it makes; null otherwise (and
  /// never cleared by `clear`). Copies share it.
  std::shared_ptr<const ProgramFacts> programFacts = nullptr;

  /// Adds a unit's exports. A name defined by more than one unit gets the
  /// join of the definitions' summaries (RFC 0005, *Accepted false
  /// positives*); the globals are renumbered into `globals()`.
  void add(const UnitExports &unit);
  void clear();
  [[nodiscard]] bool empty() const noexcept { return byName.empty(); }

  /// The joined summary of `name`'s external definitions, or of every
  /// address-taken function of type `typeKey`, with globals numbered by
  /// `globals()`; null if there is none.
  [[nodiscard]] const core::FunctionEffects *
  findEffects(llvm::StringRef name) const;
  [[nodiscard]] const core::FunctionEffects *
  candidateEffects(llvm::StringRef typeKey) const;
  /// The summary of `request`'s callee in its context, when its unit ran
  /// it; the contexts other units asked of `callee`.
  [[nodiscard]] const core::FunctionEffects *
  contextEffects(const ContextRequest &request) const;
  /// Every context some unit asked for.
  [[nodiscard]] const std::set<ContextRequest> &requests() const noexcept {
    return requested;
  }

  [[nodiscard]] const GlobalNames &globals() const noexcept {
    return globalNames;
  }

  /// Sorted names of every exported function, then every type key with
  /// candidates, with their summaries (for `--dump-analysis`).
  void dump(llvm::raw_ostream &os) const;

private:
  GlobalNames globalNames;
  std::map<std::string, core::FunctionEffects, std::less<>> byName;
  std::map<std::string, core::FunctionEffects, std::less<>> byType;
  std::map<ContextRequest, core::FunctionEffects> byContext;
  std::set<ContextRequest> requested;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_PROGRAMDATABASE_H
