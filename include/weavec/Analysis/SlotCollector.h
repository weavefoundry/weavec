//===- SlotCollector.h - Function-pointer slots of a unit (RFC 0030) -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §9.3: flow-insensitive, field-based constraints over every
// function-pointer value of a unit, for `core::FnSlots` to solve.
//
//   - `f ∈ S`: a function designator stored, assigned, passed or returned
//     into `S`, static initialisers included;
//   - `S ⊆ T`: a copy of a value from slot `S` into `T`, through casts
//     between function-pointer types; a direct call copies its arguments
//     into `param g i` and `result g` into what receives it; an indirect
//     call through `S` copies them into `call-param i S` and out of
//     `call-result S` (the solver adds the dynamic constraints);
//   - `open(S)`: `S` receives a value from outside the solved program: an
//     integer or data-pointer conversion, a load the analysis cannot name, a
//     `va_arg`, a byte-wise write, a store to another member of a union, or
//     a slot whose address escapes. Parameters of exported functions and
//     results of callees without a body are open by the solver's own rules.
//
// A function designator converted to a data pointer or an integer, or
// passed where no parameter receives it (a variadic argument), escapes: it
// becomes a member of `param <unknown> 0`, so its parameters are open.
//
// Names: functions with external linkage keep their name; internal ones,
// `static` variables and the records a main file defines are qualified with
// the unit (`<unit>:l_alloc`, `static <unit>:hook`, `<unit>:struct ops`).
// Array elements share their array's slot.
//
// Closedness per TU (§9.3): a field of a main-file record is closed when no
// object of the record flows through a parameter or result of a function
// that may be called from outside, through a callee without a body, through
// an externally visible variable, or through a conversion from another
// pointer type; a `static` variable is closed while its address does not
// escape. Everything else follows the solver's seed rules.
//
// Like every §14 component outside the engine, this one depends on no part
// of `FunctionDataflow`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_SLOTCOLLECTOR_H
#define WEAVEC_ANALYSIS_SLOTCOLLECTOR_H

#include "weavec/Core/FnSlots.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace weavec::analysis {

struct SlotCollectorOptions {
  /// The unit that qualifies internal names; empty for the main file's name
  /// as the user gave it.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string unit = {};
};

/// One unit's constraints, and how its code maps onto them.
class SlotCollection {
public:
  /// Every constraint, local slots included.
  [[nodiscard]] const core::FnSlots &constraints() const noexcept {
    return all;
  }
  /// §9.3's closed-slot rules for a per-TU compile.
  [[nodiscard]] const core::SlotRules &rules() const noexcept {
    return unitRules;
  }
  /// Solves the unit's constraints under its rules.
  [[nodiscard]] core::SlotSolution solve() const {
    return all.solve(unitRules);
  }
  /// §13.1 `slots`: the constraints with local slots eliminated.
  [[nodiscard]] core::FnSlots exported() const { return all.withoutLocals(); }

  /// The slot an indirect call calls through; none for a direct call.
  [[nodiscard]] std::optional<core::SlotKey>
  calleeSlot(const clang::CallExpr &call) const;
  /// Every indirect call of the unit with its callee slot, in source order.
  [[nodiscard]] const std::vector<
      std::pair<const clang::CallExpr *, core::SlotKey>> &
  indirectCalls() const noexcept {
    return calls;
  }
  /// The declaration a function name of the constraints stands for, or
  /// null (another unit's function, or `<unknown>`).
  [[nodiscard]] const clang::FunctionDecl *function(llvm::StringRef name) const;
  /// The unit that qualifies internal names.
  [[nodiscard]] const std::string &unit() const noexcept { return unitName; }

private:
  friend class SlotCollector;
  friend class SlotWalker;
  core::FnSlots all;
  core::SlotRules unitRules;
  std::vector<std::pair<const clang::CallExpr *, core::SlotKey>> calls;
  llvm::DenseMap<const clang::CallExpr *, std::size_t> callIndex;
  std::map<std::string, const clang::FunctionDecl *, std::less<>> functions;
  std::string unitName;
};

/// Builds the §9.3 constraints of a unit.
class SlotCollector {
public:
  SlotCollector(clang::ASTContext &ctx, const core::LibrarySpec &spec,
                SlotCollectorOptions collectorOptions = {})
      : context(ctx), library(spec), options(std::move(collectorOptions)) {}

  [[nodiscard]] SlotCollection collect();

  /// The name `function` has in the constraints: its own, or
  /// `<unit>:<name>` with internal linkage.
  [[nodiscard]] static std::string
  functionName(const clang::FunctionDecl &function, llvm::StringRef unit);
  /// The record type key of a field slot: `struct ops`, or
  /// `<unit>:struct ops` for a record a main file defines.
  [[nodiscard]] static std::string
  recordSlotKey(const clang::RecordDecl &record,
                const clang::ASTContext &context, llvm::StringRef unit);

private:
  clang::ASTContext &context;
  const core::LibrarySpec &library;
  SlotCollectorOptions options;
};

/// `weavec --dump-kinds`: the constraints, the indirect calls with their
/// resolution, and the open slots with their reasons (unstable format).
void dumpSlots(const SlotCollection &slots, const core::SlotSolution &solution,
               clang::ASTContext &context, llvm::raw_ostream &os);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_SLOTCOLLECTOR_H
