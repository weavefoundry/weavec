//===- RecordPayload.h - The unit record's payload (RFC 0030) ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §13.1: the typed payload of a format-28 unit record, and its
// conversion to and from the JSON the field table of UnitRecord.h
// describes. A payload holds what one unit contributes to the program:
//
//   - the engine's `analysis::UnitExports` (summaries in SummaryIO format
//     27 text with global roots by name, the RFC 0014 and 0016 call
//     contexts, the RFC 0010, 0012 and 0028 facts), which the link step's
//     program database is built from;
//   - the `InterfaceFacts` the link step verifies (§13.2 steps 2, 3 and 5):
//     parameter and result kinds, `reliesOnSingle` flags and exported
//     requirements of the definitions (a unit's `KindTable`), the declared
//     annotations of its imports and, per call, which pointer arguments
//     were Single-valid, the function-pointer slot constraints and rules
//     (a `SlotCollection`), the slot kinds, the §7.6 invariants and the
//     boundary rows;
//   - the compact site rows of its ledger (§13.1 `sites`), the diagnostics
//     already reported, and what its zero-initialisation lowered (§11).
//
// Records carry no evidence: no traces, notes or explanation text.
// Everything a reader cannot accept makes the record stale; the error names
// the offending key.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_RECORDPAYLOAD_H
#define WEAVEC_FRONTEND_RECORDPAYLOAD_H

#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/PointerKind.h"
#include "weavec/Core/SourceLocation.h"
#include "weavec/Frontend/DiagnosticControl.h"

#include "llvm/Support/JSON.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weavec::frontend::record {

/// RFC 0005 transport bound: context requests of one symbol a record may
/// carry, independent of the computed-context limits (RFC 0028).
inline constexpr std::size_t MaxContextRequests = 65536;
/// Entries of the global name table and of each interface map.
inline constexpr std::size_t MaxGlobalNames = 65536;
inline constexpr std::size_t MaxInterfaces = 4096;

/// A kind with its source, spelled `<source> <kind>`: `declared counted(param
/// 1 scale 1 plus 0) nonnull`, `default single nullable`.
[[nodiscard]] std::string spellKind(const core::PointerKind &kind);
[[nodiscard]] std::optional<core::PointerKind> parseKind(std::string_view text);

/// §13.1 `functions[].requirements[]`: an inferred requirement of an
/// exported or address-taken function (§7.5), which the link step verifies
/// at every caller the records contain.
struct ExportedRequirement {
  std::uint32_t param = 0;
  /// `PointerKind::toString`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string kind = {};
  /// `RequirementGuard::toString`; none when the requirement always holds.
  std::optional<std::string> guard = std::nullopt;

  friend bool operator==(const ExportedRequirement &,
                         const ExportedRequirement &) = default;
};

/// What the link step knows about one definition beyond its summary.
struct FunctionInterface {
  /// Per parameter: `spellKind` of its kind, or none for a non-pointer.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::optional<std::string>> params = {};
  std::optional<std::string> result = std::nullopt;
  /// §7.3: the parameters whose Single default the body relies on.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::uint32_t> reliesOnSingle = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<ExportedRequirement> requirements = {};
  /// The definition, for the note `defined here`.
  std::optional<core::SourceLocation> location = std::nullopt;

  friend bool operator==(const FunctionInterface &,
                         const FunctionInterface &) = default;
};

/// §13.1 `imports[].declared`: what the unit's declarations of an external
/// function state. Ownership annotations are spelled as the macros are
/// (`WEAVEC_BORROWED`), kinds by `PointerKind::toString`.
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

/// §13.1 `imports[].calls[]`: one call of the import. `args[i]` says
/// whether pointer argument `i` was Single-valid (§7.3); none for a
/// non-pointer argument.
struct ImportCall {
  /// The calling function and the call's Call-site ordinal in it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
  std::optional<std::uint32_t> site = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::optional<bool>> args = {};

  friend bool operator==(const ImportCall &, const ImportCall &) = default;
};
struct ImportInterface {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  DeclaredInterface declared = {};
  /// The declaration the link step blames (the one with annotations).
  std::optional<core::SourceLocation> location = std::nullopt;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<ImportCall> calls = {};

  friend bool operator==(const ImportInterface &,
                         const ImportInterface &) = default;
};

/// §13.1 `globals[]` beyond the name: the variable's type and slot kind.
struct GlobalInterface {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string typeKey = {};
  /// `spellKind`, or empty for a non-pointer or an unknown variable.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string kind = {};

  friend bool operator==(const GlobalInterface &,
                         const GlobalInterface &) = default;
};

/// §13.1 `slots` and `slotRules`: the unit's function-pointer slot
/// constraints with local slots eliminated, and its inputs to §9.3's
/// closed-slot rules (functions named as `SlotCollector` names them).
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

/// §13.1 `slotKinds[]`: a §7.3 slot kind of a header struct field or an
/// external variable, with the stores that demoted it.
struct SlotKindRow {
  /// `field <record> <field>` or `global <name>`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string slot = {};
  /// `PointerKind::toString`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string kind = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<core::SourceLocation> demotedBy = {};

  friend bool operator==(const SlotKindRow &, const SlotKindRow &) = default;
};

/// §13.1 `invariants[]`: a §7.6 invariant of a header struct, the unit's
/// verdict on its stores and whether the unit relied on it.
struct InvariantRow {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string record = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string field = {};
  /// `count(<d>) == <f> + <c>` or `bytes(<d>) == <f> + <c>`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string templ = {};
  core::Verdict verdict = core::Verdict::Unknown;
  bool relied = false;
  /// The store a `violated` verdict names.
  std::optional<core::SourceLocation> store = std::nullopt;

  friend bool operator==(const InvariantRow &, const InvariantRow &) = default;
};

/// Everything the link step verifies about a unit beyond its summaries.
/// Producers fill what they know; the rest stays empty (the engine
/// publishes no invariant verdicts or boundary rows through the pipeline
/// yet).
struct InterfaceFacts {
  /// By the name `UnitExports::functions` uses.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::map<std::string, FunctionInterface> functions = {};
  /// By the name `UnitExports::globals` uses.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::map<std::string, GlobalInterface> globals = {};
  /// By the name `UnitExports::imports` uses.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::map<std::string, ImportInterface> imports = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SlotFacts slots = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<SlotKindRow> slotKinds = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<InvariantRow> invariants = {};
  /// `BoundaryRow::unit` is empty here.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<analysis::BoundaryRow> boundaries = {};
  /// §11, §13.2 step 5: the allocator function (`malloc`, `calloc`,
  /// `realloc` or `free`) the unit defines, if any.
  std::optional<std::string> allocator = std::nullopt;
  /// §11: allocation calls and references the unit lowered.
  std::uint64_t loweredAllocations = 0;

  friend bool operator==(const InterfaceFacts &,
                         const InterfaceFacts &) = default;
};

/// §13.1 `sites[]`: one function's compact rows.
struct SiteRow {
  std::uint32_t ordinal = 0;
  core::SiteKind kind = core::SiteKind::Deref;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  /// Indexed by `core::Facet`; none where the facet does not apply.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::array<std::optional<core::FacetDecision>, core::FacetCount> facets = {};

  friend bool operator==(const SiteRow &, const SiteRow &) = default;
};
struct FunctionRows {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string function = {};
  /// The file of the definition; empty for the unit's source.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string file = {};
  std::uint32_t line = 0;
  core::Linkage linkage = core::Linkage::External;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<SiteRow> rows = {};

  friend bool operator==(const FunctionRows &, const FunctionRows &) = default;
};

/// The compact rows of a unit ledger (§13.1 `sites`), and back: every
/// facet is decided, with no detail, check, requirement or fix-it.
[[nodiscard]] std::vector<FunctionRows> siteRows(const core::UnitLedger &unit);
[[nodiscard]] core::UnitLedger unitLedgerOf(std::span<const FunctionRows> rows);

/// A typed payload.
struct Payload {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  analysis::UnitExports exports = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  InterfaceFacts facts = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<FunctionRows> sites = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<ReportedDiagnostic> reported = {};
  /// §12.1 unit `summary.a5`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::A5Counts a5 = {};
};

/// The payload's JSON, which `validate` accepts against `payloadSchema()`.
[[nodiscard]] llvm::json::Object toJson(const Payload &payload);
/// Reads a payload that `validate` accepted, or fails with `error` naming
/// what cannot be read (an unknown spelling, a malformed summary or
/// context, a duplicate, a bound exceeded). `source` becomes
/// `exports.source`.
[[nodiscard]] std::optional<Payload>
payloadFromJson(const llvm::json::Object &json, std::string_view source,
                std::string &error);

} // namespace weavec::frontend::record

#endif // WEAVEC_FRONTEND_RECORDPAYLOAD_H
