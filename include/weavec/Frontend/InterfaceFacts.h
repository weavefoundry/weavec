//===- InterfaceFacts.h - A unit's interface facts -------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What `weavec --whole-program` checks across units beyond the summaries
// (RFC 0030 §13.2, RFC 0035 §8), from the components that run before the
// engine and depend on no engine fact:
//
//   - `AttributeReader` and `KindInference` (§7.2–§7.5) give the kinds of
//     the parameters of the unit's definitions and the declared kinds of its
//     imports;
//   - the ownership annotations of the imports' declarations (RFC 0003);
//   - `SlotCollector` (§9.3) gives the function-pointer slot constraints
//     with local slots eliminated and the unit's closed-slot rules.
//
// §9.4's boundary rows come from `BoundaryInvariants`, which runs after the
// engine, through `UnitExports`. The facts stay in memory: there are no
// unit records.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_INTERFACEFACTS_H
#define WEAVEC_FRONTEND_INTERFACEFACTS_H

#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Core/PointerKind.h"
#include "weavec/Core/SourceLocation.h"

#include "clang/AST/ASTContext.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::frontend {

/// A kind with its source, spelled `<source> <kind>`: `declared counted(param
/// 1 scale 1 plus 0) nonnull`, `default single nullable`.
[[nodiscard]] std::string spellKind(const core::PointerKind &kind);
[[nodiscard]] std::optional<core::PointerKind> parseKind(std::string_view text);

/// What the program check knows about one definition beyond its summary.
struct FunctionInterface {
  /// Per parameter: `spellKind` of its kind, or none for a non-pointer.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::optional<std::string>> params = {};
  /// The definition, for the note `defined here`.
  std::optional<core::SourceLocation> location = std::nullopt;

  friend bool operator==(const FunctionInterface &,
                         const FunctionInterface &) = default;
};

/// What the unit's declarations of an external function state. Ownership
/// annotations are spelled as the macros are (`WEAVEC_BORROWED`), kinds by
/// `PointerKind::toString`.
struct DeclaredParam {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string name = {};
  std::optional<std::string> kind = std::nullopt;
  std::optional<std::string> ownership = std::nullopt;

  friend bool operator==(const DeclaredParam &,
                         const DeclaredParam &) = default;
};
struct DeclaredInterface {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<DeclaredParam> params = {};
  /// The declared result kind and ownership annotation.
  std::optional<std::string> result = std::nullopt;
  std::optional<std::string> ownership = std::nullopt;

  /// Whether any annotation or kind is declared at all.
  [[nodiscard]] bool empty() const noexcept;
  friend bool operator==(const DeclaredInterface &,
                         const DeclaredInterface &) = default;
};

struct ImportInterface {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  DeclaredInterface declared = {};
  /// The declaration a contradiction is reported at (the one with
  /// annotations).
  std::optional<core::SourceLocation> location = std::nullopt;

  friend bool operator==(const ImportInterface &,
                         const ImportInterface &) = default;
};

/// The unit's function-pointer slot constraints with local slots
/// eliminated, and its inputs to §9.3's closed-slot rules (functions named
/// as `SlotCollector` names them).
struct SlotFacts {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<core::SlotRow> rows = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string unit = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> defined = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> exported = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> confinedRecords = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::string> escapedStatics = {};

  friend bool operator==(const SlotFacts &, const SlotFacts &) = default;
};

/// Everything the program check verifies about a unit beyond its summaries.
struct InterfaceFacts {
  /// By the name `UnitExports::functions` uses.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::map<std::string, FunctionInterface> functions = {};
  /// By the name `UnitExports::imports` uses.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::map<std::string, ImportInterface> imports = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SlotFacts slots = {};
  /// `BoundaryRow::unit` is empty here.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<analysis::BoundaryRow> boundaries = {};

  friend bool operator==(const InterfaceFacts &,
                         const InterfaceFacts &) = default;
};

struct FactsInput {
  /// The unit's exports: which functions and imports to describe.
  const analysis::UnitExports &exports;
  /// The kinds the unit's analysis used; null to build them again.
  const analysis::UnitKinds *kinds = nullptr;
};

/// The interface facts of the unit `context` holds.
[[nodiscard]] InterfaceFacts collectInterfaceFacts(clang::ASTContext &context,
                                                   const FactsInput &input);
/// Only the function-pointer slots, for a discovery run: what the
/// whole-program driver solves before any unit is analysed.
[[nodiscard]] InterfaceFacts collectSlotFacts(clang::ASTContext &context);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_INTERFACEFACTS_H
