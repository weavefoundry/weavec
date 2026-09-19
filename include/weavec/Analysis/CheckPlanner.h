//===- CheckPlanner.h - Checks from witnesses (RFC 0030) --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §10.1, §10.3, §10.4: `CheckPlanner::plan` turns every checked
// requirement record of a unit ledger (§2.5) into `core::CheckPlanEntry`s,
// choosing each check's template, form and placement, plus a guard for each
// lowered violation (§3.4) and, in verify mode, a check for each proven
// facet with a witness (§10.7). It decides expressibility before any code
// is emitted: a record whose extra terms break a rule of §10.3 becomes
// `unresolved(inexpressible)`, so the ledger is the same whether or not
// checks are emitted. It is pure: it reads the AST and writes only the
// ledger and the plan.
//
// Placements (§10.4):
//
//   null      dereference, subscript base      nonnull  WrapOperand
//             indirect callee                  nonnull  Function form
//             call argument                    nonnull  WrapArgument
//                                                       (IfNonZero form
//                                                       for null-if-zero)
//   spatial   subscript, *(p + i)              index    WrapIndex
//             dereference                      index    WrapOperand (i = 0)
//             cursor access                    span     ReplaceAccess
//             ptr-arith, cast                  span     WrapOperand
//             call argument (count)            len      BeforeCall
//             call argument (overlap)          disjoint WrapArgument
//   assertion WEAVEC_ASSUME                    assert   ReplaceCall
//
// A `span` check traps on null, so it subsumes the `nonnull` check of the
// same operand. Operands are core terms over opaque handles, which
// `PlaceHandleTable` resolves back to Clang declarations and types (Core
// never sees Clang).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_CHECKPLANNER_H
#define WEAVEC_ANALYSIS_CHECKPLANNER_H

#include "weavec/Analysis/CheckWitness.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/CheckPlan.h"
#include "weavec/Core/Ledger.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// Resolves the opaque place and type handles of `core::CheckTerm`s to the
/// Clang declarations and types they stand for (§10.1). Handle 0 is never
/// issued.
class PlaceHandleTable {
public:
  [[nodiscard]] std::uint64_t place(const clang::ValueDecl &decl);
  [[nodiscard]] std::uint64_t type(clang::QualType type);
  /// The declaration of a place handle, or null.
  [[nodiscard]] const clang::ValueDecl *
  resolvePlace(std::uint64_t handle) const;
  /// The type of a `sizeof` handle, or a null type.
  [[nodiscard]] clang::QualType resolveType(std::uint64_t handle) const;

private:
  std::vector<const clang::ValueDecl *> placeList;
  llvm::DenseMap<const clang::ValueDecl *, std::uint64_t> placeIds;
  std::vector<clang::QualType> typeList;
  llvm::DenseMap<void *, std::uint64_t> typeIds;
};

/// The witnesses published for each facet of each site.
class WitnessTable {
public:
  void add(core::SiteId site, core::Facet facet, CheckWitness witness);
  [[nodiscard]] llvm::ArrayRef<CheckWitness> of(core::SiteId site,
                                                core::Facet facet) const;
  /// Forgets the witnesses of one function (a new authoritative pass).
  void clear(std::uint32_t function);
  [[nodiscard]] std::size_t size() const noexcept { return table.size(); }

private:
  std::map<std::pair<core::SiteId, core::Facet>, std::vector<CheckWitness>>
      table;
};

struct PlannerOptions {
  /// Verify mode plans checks of proven facets that have witnesses.
  core::ChecksMode checks = core::ChecksMode::Trap;
  /// §3.4: whether the violation of a facet was lowered to a warning, so
  /// that the site is guarded anyway. None: nothing was lowered.
  std::function<bool(core::SiteId, core::Facet)> lowered = nullptr;
  /// §5.4: facets a `setjmp` function downgraded from proven to checked;
  /// when their check is inexpressible they become `unresolved(setjmp)`
  /// rather than `unresolved(inexpressible)`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::set<std::pair<core::SiteId, core::Facet>> setjmpDowngraded = {};
};

class CheckPlanner {
public:
  CheckPlanner(clang::ASTContext &ctx, const SiteIndex &siteIndex,
               PlannerOptions plannerOptions = {})
      : context(ctx), sites(siteIndex), options(std::move(plannerOptions)) {}

  /// Plans every checked record of `unit` (whose functions and sites are
  /// `sites`'), turns the inexpressible ones `unresolved`, and records each
  /// planned check's template on its facet or requirement.
  [[nodiscard]] core::CheckPlan plan(core::UnitLedger &unit,
                                     const WitnessTable &witnesses,
                                     PlaceHandleTable &handles) const;

  /// §10.3: `term` as an extra operand of a check at `site` whose witness
  /// is `witness`, or why it cannot be one. `have` bounds a string length
  /// (`__weavec_strnlen`, rule 2).
  struct Expression {
    std::optional<core::CheckTerm> term = std::nullopt;
    /// The rule the term breaks, for the ledger's `detail`.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string failure = {};
  };
  [[nodiscard]] Expression
  express(const WitnessTerm &term, const SiteInfo &site,
          const CheckWitness &witness, PlaceHandleTable &handles,
          const std::optional<core::CheckTerm> &have = std::nullopt) const;

private:
  clang::ASTContext &context;
  const SiteIndex &sites;
  PlannerOptions options;
  /// Per function, the locals whose address is taken (§10.3 rule 1).
  mutable llvm::DenseMap<const clang::FunctionDecl *,
                         llvm::DenseSet<const clang::VarDecl *>>
      addressTaken;

  [[nodiscard]] const llvm::DenseSet<const clang::VarDecl *> &
  addressTakenIn(const clang::FunctionDecl &function) const;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_CHECKPLANNER_H
