//===- KindInferenceDump.cpp - `weavec --dump-kinds` (RFC 0030) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A debugging aid (unstable format): the unit's kind table, its problems and
// suggestions, the store groups and the §7.6 candidates, one line per fact,
// for the declarations of the main file, in source order.
//
//===----------------------------------------------------------------------===//

#include "KindInferenceImpl.h"

#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// `<line>:<column>` of `loc`, or `?`.
static std::string lineColumn(const clang::SourceManager &sm,
                              clang::SourceLocation loc) {
  const clang::PresumedLoc at = sm.getPresumedLoc(sm.getExpansionLoc(loc));
  if (!at.isValid())
    return "?";
  return std::to_string(at.getLine()) + ":" + std::to_string(at.getColumn());
}

/// `<kind> [<source>[, <class>]][, shape <level>][, nullability <level>]`
/// and the flags.
static std::string describe(const KindEntry &entry) {
  std::string text = entry.kind.toString() + " [" +
                     std::string(core::toString(entry.kind.source));
  if (entry.extentClass)
    text += ", " + std::string(core::toString(*entry.extentClass));
  if (entry.shapeLevel)
    text += ", shape " + std::string(toString(*entry.shapeLevel));
  if (entry.nullabilityLevel)
    text += ", nullability " + std::string(toString(*entry.nullabilityLevel));
  text += ']';
  if (entry.extentFactor)
    text += " factor param " + std::to_string(*entry.extentFactor);
  if (entry.reliesOnSingle)
    text += " relies-on-single";
  if (entry.mainArgv)
    text += " argv";
  return text;
}

void dumpKinds(const KindTable &kinds, const KindInferenceResult &inferred,
               clang::ASTContext &context, llvm::raw_ostream &os) {
  const clang::SourceManager &sm = context.getSourceManager();
  // Lines with the location they sort by.
  std::vector<std::pair<clang::SourceLocation, std::string>> lines;
  const auto inMain = [&](clang::SourceLocation loc) {
    return loc.isValid() && sm.isInMainFile(sm.getExpansionLoc(loc));
  };
  const auto add = [&](clang::SourceLocation loc, const KindEntry &entry,
                       std::string head) {
    std::string text = std::move(head) + ": " + describe(entry);
    for (const MustAccessRequirement &requirement : entry.mustAccess)
      text += "\n    requires " + requirement.toString() + " at " +
              lineColumn(sm, requirement.access->getBeginLoc()) + " [" +
              std::string(toString(entry.enforcement.value_or(
                  RequirementEnforcement::CallerContract))) +
              "]";
    for (const KindDemotion &demotion : entry.demotedBy)
      text += "\n    demoted at " +
              (demotion.store != nullptr
                   ? lineColumn(sm, demotion.store->getBeginLoc())
                   : std::string("?")) +
              ": " + demotion.reason;
    lines.emplace_back(loc, std::move(text));
  };

  for (const auto &[key, entry] : kinds.paramEntries()) {
    const clang::FunctionDecl *function = key.first;
    const clang::FunctionDecl *shown = function->getDefinition() != nullptr
                                           ? function->getDefinition()
                                           : function;
    if (key.second >= shown->getNumParams())
      continue;
    const clang::ParmVarDecl *param = shown->getParamDecl(key.second);
    if (!inMain(param->getLocation()))
      continue;
    add(param->getLocation(), entry,
        "param " + function->getNameAsString() + " " +
            std::to_string(key.second) + " '" + param->getNameAsString() + "'");
  }
  for (const auto &[function, entry] : kinds.resultEntries()) {
    const clang::FunctionDecl *shown = function->getDefinition() != nullptr
                                           ? function->getDefinition()
                                           : function;
    if (inMain(shown->getLocation()))
      add(shown->getLocation(), entry, "result " + function->getNameAsString());
  }
  for (const auto &[field, entry] : kinds.fieldEntries())
    if (inMain(field->getLocation()))
      add(field->getLocation(), entry,
          "field " + recordName(*field->getParent()) + "." +
              field->getNameAsString());
  for (const auto &[variable, entry] : kinds.variableEntries()) {
    const clang::VarDecl *shown = variable->getDefinition() != nullptr
                                      ? variable->getDefinition()
                                      : variable;
    if (inMain(shown->getLocation()))
      add(shown->getLocation(), entry,
          "variable " + variable->getNameAsString());
  }
  for (const auto &[function, contract] : kinds.ownershipEntries()) {
    if (!inMain(function->getLocation()))
      continue;
    std::string text = "ownership " + function->getNameAsString() + ":";
    if (contract.freshResult)
      text += " fresh(" + *contract.freshResult + ")";
    for (const OwnershipContract::Argument &argument : contract.arguments)
      text += std::string(argument.retains ? " holds(" : " takes(") +
              std::to_string(argument.index) + ", " + argument.family + ")";
    text += " [" + std::string(toString(contract.level)) + "]";
    lines.emplace_back(function->getLocation(), std::move(text));
  }
  for (const clang::Decl *decl : context.getTranslationUnitDecl()->decls())
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
        function != nullptr && function->doesThisDeclarationHaveABody() &&
        inMain(function->getLocation()) && kinds.requireSafe(*function))
      lines.emplace_back(function->getLocation(),
                         "require-safe " + function->getNameAsString());
  for (const KindProblem &problem : kinds.problems())
    if (inMain(problem.location))
      lines.emplace_back(problem.location,
                         "problem " + lineColumn(sm, problem.location) + ": " +
                             problem.message);
  for (const KindSuggestion &suggestion : kinds.suggestions())
    if (inMain(suggestion.location))
      lines.emplace_back(suggestion.location,
                         "suggestion " + lineColumn(sm, suggestion.location) +
                             ": " + suggestion.message + " (insert '" +
                             suggestion.insert + "')");
  for (const StoreGroup &group : inferred.storeGroups()) {
    std::string text =
        "store group in " + group.function->getNameAsString() + " at " +
        lineColumn(sm, group.stores.front()->getBeginLoc()) + ":";
    for (const clang::FieldDecl *field : group.fields)
      text += " " + field->getNameAsString();
    text += " (" + std::to_string(group.stores.size()) + " stores, last at " +
            lineColumn(sm, group.last()->getBeginLoc()) + ")";
    lines.emplace_back(group.stores.front()->getBeginLoc(), std::move(text));
  }
  for (const ResolvedCandidate &resolved : inferred.resolvedCandidates()) {
    const FieldCandidate &candidate = resolved.candidate;
    lines.emplace_back(resolved.record->getLocation(),
                       "candidate " + candidate.record + ": " +
                           (candidate.bytes ? "bytes(" : "count(") +
                           candidate.pointer + ") == " + candidate.count +
                           " + " + std::to_string(candidate.offset));
  }
  for (const DisqualifiedRecord &record : inferred.disqualified())
    lines.emplace_back(record.record->getLocation(),
                       "disqualified " + record.key + ": " + record.reason);

  std::ranges::stable_sort(lines, [&sm](const auto &a, const auto &b) {
    return a.first != b.first && sm.isBeforeInTranslationUnit(a.first, b.first);
  });
  os << "kinds:\n";
  for (const auto &[loc, text] : lines)
    os << "  " << text << "\n";
}

} // namespace weavec::analysis
