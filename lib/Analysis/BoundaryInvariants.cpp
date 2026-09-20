//===- BoundaryInvariants.cpp - The §9.4 boundary invariants --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/BoundaryInvariants.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include "llvm/ADT/StringSet.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

namespace weavec::analysis {

/// §9.4: the class of the object a pointer of type `type` designates: the
/// key of the record it points to, or none for any other pointee. A field
/// class `struct s.buf` breaks this class too, because reaching the field
/// goes through the object.
static std::string objectClassOf(QualType type) {
  const auto *pointer = type->getAs<PointerType>();
  if (pointer == nullptr)
    return {};
  const RecordDecl *record = pointer->getPointeeType()->getAsRecordDecl();
  if (record == nullptr || record->getNameAsString().empty())
    return {};
  return record->getKindName().str() + " " + record->getNameAsString();
}

/// §9.4: the class of the place a site's operand was loaded from, spelled
/// as the engine spells it: the nearest enclosing field (`struct s.buf`),
/// else the name of the global the operand is rooted in. The operand is the
/// pointer the site's facets are about, so this is the place whose entry
/// assumption a proof of its temporal facet rests on.
static std::string classOfOperand(const Expr *operand) {
  for (const Expr *at = operand; at != nullptr;) {
    at = at->IgnoreParenImpCasts();
    if (const auto *member = dyn_cast<MemberExpr>(at)) {
      const auto *field = dyn_cast<FieldDecl>(member->getMemberDecl());
      if (field == nullptr)
        return {};
      const RecordDecl *record = field->getParent();
      const std::string name = record->getNameAsString();
      const std::string what = field->getNameAsString();
      if (name.empty() || what.empty())
        return {};
      return record->getKindName().str() + " " + name + "." + what;
    }
    if (const auto *reference = dyn_cast<DeclRefExpr>(at)) {
      const auto *var = dyn_cast<VarDecl>(reference->getDecl());
      if (var != nullptr && var->hasGlobalStorage())
        return var->getNameAsString();
      // A pointer held in a local or a parameter names no class of its own.
      // What a load through it rests on is the invariant of the *object* it
      // designates, which the record the boundary broke a field of names.
      return objectClassOf(at->getType());
    }
    // The pointer a subscript or a dereference reads is held one step up.
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(at)) {
      at = subscript->getBase();
      continue;
    }
    if (const auto *unary = dyn_cast<UnaryOperator>(at);
        unary != nullptr && unary->getOpcode() == UO_Deref) {
      at = unary->getSubExpr();
      continue;
    }
    if (const auto *cast = dyn_cast<CastExpr>(at)) {
      at = cast->getSubExpr();
      continue;
    }
    return {};
  }
  return {};
}

BoundaryVerdicts
checkBoundaryInvariants(const SiteIndex &sites,
                        llvm::ArrayRef<PublishedBoundary> published,
                        llvm::ArrayRef<BoundaryRow> program) {
  BoundaryVerdicts verdicts;
  // The classes whose entry assumption a boundary breaks, with the reason
  // that broke them. A class broken both ways takes `dangling-escape`: a
  // released pointer is the stronger statement.
  std::set<std::string> dangling;
  std::set<std::string> shared;
  const auto note = [&](core::UnresolvedReason reason, const std::string &of) {
    if (of.empty())
      return;
    std::set<std::string> &into =
        reason == core::UnresolvedReason::DanglingEscape ? dangling : shared;
    into.insert(of);
    // A broken field class breaks the object that holds it: a load of the
    // field goes through the object, so a proof about either rests on the
    // invariant the boundary broke.
    if (const std::size_t field = of.rfind('.'); field != std::string::npos)
      into.insert(of.substr(0, field));
  };
  for (const BoundaryRow &row : program)
    note(row.reason, row.placeClass);

  for (const PublishedBoundary &boundary : published) {
    const core::SiteId at = boundary.site;
    const std::string function =
        at.function < sites.functions().size() &&
                sites.functions()[at.function].decl != nullptr
            ? sites.functions()[at.function].decl->getNameAsString()
            : std::string{};
    const auto exportRow = [&](core::UnresolvedReason reason,
                               const std::string &of) {
      if (of.empty())
        return;
      verdicts.exported.push_back(BoundaryRow{.unit = {},
                                              .function = function,
                                              .site = at.ordinal,
                                              .reason = reason,
                                              .placeClass = of});
    };
    if (!boundary.facts.dangling.empty()) {
      const BoundaryFacts::Dangling &first = boundary.facts.dangling.front();
      verdicts.decisions.push_back(
          {.site = at,
           .reason = core::UnresolvedReason::DanglingEscape,
           .detail = "'" + first.name +
                     "' may hold a pointer to an object that is gone"});
      for (const BoundaryFacts::Dangling &row : boundary.facts.dangling) {
        note(core::UnresolvedReason::DanglingEscape, row.placeClass);
        exportRow(core::UnresolvedReason::DanglingEscape, row.placeClass);
      }
    } else if (!boundary.facts.sharedOwners.empty()) {
      const BoundaryFacts::SharedOwners &first =
          boundary.facts.sharedOwners.front();
      verdicts.decisions.push_back(
          {.site = at,
           .reason = core::UnresolvedReason::SecondOwner,
           .detail = "'" + first.names + "' may own the same object"});
    }
    for (const BoundaryFacts::SharedOwners &row : boundary.facts.sharedOwners) {
      note(core::UnresolvedReason::SecondOwner, row.placeClass);
      note(core::UnresolvedReason::SecondOwner, row.otherClass);
      exportRow(core::UnresolvedReason::SecondOwner, row.placeClass);
      exportRow(core::UnresolvedReason::SecondOwner, row.otherClass);
    }
  }
  std::ranges::sort(verdicts.exported);
  const auto duplicates = std::ranges::unique(verdicts.exported);
  verdicts.exported.erase(duplicates.begin(), duplicates.end());
  if (dangling.empty() && shared.empty())
    return verdicts;

  // *Propagation*. Every temporal facet the unit would otherwise prove for
  // a place of a broken class takes the boundary's reason. The site's
  // operand is the place the proof rests on; `LedgerAdapter` downgrades
  // only the facets that are in fact proven.
  for (const SiteIndex::FunctionSites &function : sites.functions())
    for (const SiteInfo &info : function.sites) {
      if (info.operand == nullptr)
        continue;
      const core::SiteId id = info.id;
      const std::string of = classOfOperand(info.operand);
      if (of.empty())
        continue;
      if (dangling.contains(of))
        verdicts.decisions.push_back(
            {.site = id,
             .reason = core::UnresolvedReason::DanglingEscape,
             .detail = "a pointer held in '" + of +
                       "' may be gone where the object is handed on",
             .propagated = true});
      else if (shared.contains(of))
        verdicts.decisions.push_back(
            {.site = id,
             .reason = core::UnresolvedReason::SecondOwner,
             .detail = "'" + of + "' may not be the only owner",
             .propagated = true});
    }
  return verdicts;
}

} // namespace weavec::analysis
