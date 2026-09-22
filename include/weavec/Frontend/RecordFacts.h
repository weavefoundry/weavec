//===- RecordFacts.h - A unit's interface facts (RFC 0030) ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §13.1: the `record::InterfaceFacts` of one unit, from the
// components that run before the engine and depend on no engine fact:
//
//   - `AttributeReader` and `KindInference` (§7.2–§7.5) give the kinds of
//     the parameters and results of the unit's definitions, their
//     `reliesOnSingle` flags and exported requirements, the declared kinds
//     of its imports, whether each pointer argument of a call to an import
//     was Single-valid, and the slot kinds of header structs and external
//     variables;
//   - the ownership annotations of the imports' declarations (RFC 0003);
//   - `SlotCollector` (§9.3) gives the function-pointer slot constraints
//     with local slots eliminated and the unit's closed-slot rules;
//   - the unit's `SiteIndex` names the Call site of each such call.
//
// §9.4's boundary rows come from `BoundaryInvariants`, which runs after the
// engine, through `UnitExports`. §7.6 verdicts are not published through the
// pipeline yet and stay empty.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_RECORDFACTS_H
#define WEAVEC_FRONTEND_RECORDFACTS_H

#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Frontend/FrontendAction.h"
#include "weavec/Frontend/RecordPayload.h"

#include "clang/AST/ASTContext.h"

#include <cstdint>

namespace weavec::analysis {
class SiteIndex;
} // namespace weavec::analysis

namespace weavec::frontend::record {

struct FactsInput {
  /// The unit's exports: which functions, globals and imports to describe.
  const analysis::UnitExports &exports;
  /// The unit's sites, for the Call-site ordinals of calls to imports; may
  /// be null.
  const analysis::SiteIndex *sites = nullptr;
  /// §11: the allocation calls and references the unit lowered.
  std::uint64_t loweredAllocations = 0;
  /// The table to use; null for `LibrarySpec::shipped()`.
  const core::LibrarySpec *library = nullptr;
  /// The kinds the unit's analysis used; null to build them again.
  const analysis::UnitKinds *kinds = nullptr;
};

/// The interface facts of the unit `context` holds.
[[nodiscard]] InterfaceFacts collectInterfaceFacts(clang::ASTContext &context,
                                                   const FactsInput &input);
/// Only the function-pointer slots, for a discovery run: what the
/// whole-program driver solves before any unit is analysed.
[[nodiscard]] InterfaceFacts
collectSlotFacts(clang::ASTContext &context,
                 const core::LibrarySpec *library = nullptr);

/// The payload of what a reporting run of a unit produced: its exports and
/// interface facts, the compact rows of its ledger, the diagnostics it
/// reported and its A5 counts.
[[nodiscard]] Payload payloadOf(const UnitResult &result);

} // namespace weavec::frontend::record

#endif // WEAVEC_FRONTEND_RECORDFACTS_H
