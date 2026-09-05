//===- ProgramDatabase.cpp - Summaries across translation units -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/ProgramDatabase.h"

#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/SummaryIO.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/Version.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

// -- GlobalNames --------------------------------------------------------------

std::uint32_t GlobalNames::idFor(llvm::StringRef name) {
  const auto [it, inserted] =
      ids.try_emplace(name.str(), static_cast<std::uint32_t>(names.size()));
  if (inserted)
    names.push_back(name.str());
  return it->second;
}

std::optional<std::uint32_t> GlobalNames::find(llvm::StringRef name) const {
  const auto it = ids.find(name);
  return it == ids.end() ? std::nullopt : std::optional(it->second);
}

llvm::StringRef GlobalNames::nameOf(std::uint32_t id) const {
  return id < names.size() ? llvm::StringRef(names[id]) : "<global>";
}

bool GlobalNames::extendTo(const GlobalNames &other) {
  const std::size_t common = std::min(names.size(), other.names.size());
  if (!std::equal(names.begin(),
                  names.begin() + static_cast<std::ptrdiff_t>(common),
                  other.names.begin()))
    return false;
  for (std::size_t i = names.size(); i < other.names.size(); ++i)
    (void)idFor(other.names[i]);
  return true;
}

// -- UnitExports --------------------------------------------------------------

bool UnitExports::sameSummariesAs(const UnitExports &other) const {
  return functions == other.functions && globals == other.globals &&
         countFields == other.countFields;
}

// -- Type keys ----------------------------------------------------------------

/// The canonical spelling of `type`, or empty when an anonymous record is
/// involved (spelled by its location, which no other unit shares: no stable
/// key; RFC 0005, *Accepted false positives*).
static std::string stableTypeKey(QualType type, const ASTContext &context) {
  PrintingPolicy policy(context.getLangOpts());
  policy.SuppressTagKeyword = false;
#if CLANG_VERSION_MAJOR >= 23
  policy.AnonymousTagNameStyle =
      llvm::to_underlying(PrintingPolicy::AnonymousTagMode::SourceLocation);
#else
  policy.AnonymousTagLocations = true;
#endif
  policy.SuppressScope = true;
  policy.Bool = false;
  std::string key = type.getCanonicalType().getAsString(policy);
  for (const char *marker : {"(unnamed ", "(anonymous ", "<anonymous"}) {
    if (key.find(marker) != std::string::npos)
      return {};
  }
  return key;
}

std::string functionTypeKey(QualType type, const ASTContext &context) {
  if (type.isNull())
    return {};
  if (const auto *pointer = type->getAs<PointerType>())
    type = pointer->getPointeeType();
  if (!type->isFunctionType())
    return {};
  return stableTypeKey(type, context);
}

std::string recordTypeKey(QualType type, const ASTContext &context) {
  if (type.isNull())
    return {};
  const QualType canonical = type.getCanonicalType().getUnqualifiedType();
  if (!canonical->isRecordType())
    return {};
  return stableTypeKey(canonical, context);
}

// -- ProgramDatabase ----------------------------------------------------------

static core::FunctionSummary renumber(const core::FunctionSummary &summary,
                                      const GlobalNames &from,
                                      GlobalNames &to) {
  return core::remapGlobals(summary, [&](std::uint32_t id) {
    return std::optional(to.idFor(from.nameOf(id)));
  });
}

void ProgramDatabase::add(const UnitExports &unit) {
  // Exports already numbered by (a prefix or an extension of) this table
  // mean the same thing verbatim; renumbering them would rebuild every
  // summary's maps for nothing.
  const bool sameNumbering = globalNames.extendTo(unit.globals);
  for (const auto &[name, function] : unit.functions) {
    std::optional<core::FunctionSummary> renumbered;
    const core::FunctionSummary &summary =
        sameNumbering ? function.summary
                      : renumbered.emplace(renumber(function.summary,
                                                    unit.globals, globalNames));
    // RFC 0009: a definition that returns makes the join return. Settled
    // here because the join cannot tell a definition that does nothing
    // from the empty summary it treats as bottom.
    const auto fold = [&summary](core::FunctionSummary &into) {
      const bool bothNeverReturn = into.neverReturns && summary.neverReturns;
      into.join(summary);
      into.neverReturns = bothNeverReturn;
    };
    if (function.external) {
      auto [it, inserted] = functions.try_emplace(name, summary);
      if (!inserted)
        fold(it->second);
    }
    if (function.addressTaken && !function.typeKey.empty()) {
      auto [it, inserted] =
          candidateSummaries.try_emplace(function.typeKey, summary);
      if (!inserted)
        fold(it->second);
    }
  }
  countFields.insert(unit.countFields.begin(), unit.countFields.end());
}

UnitExports ProgramDatabase::renumbered(const UnitExports &unit) {
  UnitExports result = unit;
  if (!globalNames.extendTo(unit.globals)) {
    for (auto &[name, function] : result.functions)
      function.summary = renumber(function.summary, unit.globals, globalNames);
  }
  result.globals = globalNames;
  return result;
}

void ProgramDatabase::clear() {
  functions.clear();
  candidateSummaries.clear();
  globalNames = GlobalNames{};
  countFields.clear();
}

bool ProgramDatabase::defines(llvm::StringRef name) const {
  return functions.contains(name);
}

const core::FunctionSummary *ProgramDatabase::find(llvm::StringRef name) const {
  const auto it = functions.find(name);
  return it == functions.end() ? nullptr : &it->second;
}

const core::FunctionSummary *
ProgramDatabase::candidates(llvm::StringRef typeKey) const {
  const auto it = candidateSummaries.find(typeKey);
  return it == candidateSummaries.end() ? nullptr : &it->second;
}

/// The external-linkage variable named `name` at file scope, if the unit
/// declares one.
static const VarDecl *externalVariable(llvm::StringRef name,
                                       const ASTContext &context) {
  const TranslationUnitDecl *tu = context.getTranslationUnitDecl();
  for (const NamedDecl *decl :
       tu->lookup(DeclarationName(&context.Idents.get(name)))) {
    const auto *var = dyn_cast<VarDecl>(decl);
    if (var != nullptr && var->hasGlobalStorage() && var->isExternallyVisible())
      return var;
  }
  return nullptr;
}

core::FunctionSummary
ProgramDatabase::importInto(const core::FunctionSummary &summary,
                            const ASTContext &context,
                            GlobalTable &table) const {
  return core::remapGlobals(summary, [&](std::uint32_t id) {
    const VarDecl *var = externalVariable(globalNames.nameOf(id), context);
    return var == nullptr ? std::nullopt : std::optional(table.idFor(*var));
  });
}

static void describe(llvm::raw_ostream &os,
                     const core::FunctionSummary &summary,
                     const GlobalNames &globals) {
  const core::GlobalNamer namer = [&globals](std::uint32_t id) {
    return globals.nameOf(id).str();
  };
  for (const auto &[path, effect] : summary.effects) {
    os << " " << core::printSummaryPath(path, namer) << ": "
       << core::printFlags(effect) << ";";
  }
  os << " stores{";
  bool first = true;
  for (const core::Store &store : summary.stores) {
    os << (first ? "" : ", ") << core::printSummaryPath(store.dest, namer)
       << " = " << core::printValueSource(store.value, namer);
    first = false;
  }
  os << "} returns{";
  first = true;
  for (const core::ValueSource &source : summary.returns) {
    os << (first ? "" : ", ") << core::printValueSource(source, namer);
    first = false;
  }
  os << "}";
  if (!summary.requiresNonNull.empty()) {
    os << " requires{";
    first = true;
    for (const std::uint32_t param : summary.requiresNonNull) {
      os << (first ? "" : ", ")
         << core::printSummaryPath(core::SummaryPath::param(param), namer);
      first = false;
    }
    os << "}";
  }
  const auto describePaths =
      [&os, &namer, &first](const char *label,
                            const std::set<core::SummaryPath> &paths) {
        os << " " << label << "{";
        first = true;
        for (const core::SummaryPath &path : paths) {
          os << (first ? "" : ", ") << core::printSummaryPath(path, namer);
          first = false;
        }
        os << "}";
      };
  for (const auto &[outcome, effects] : summary.outcomes) {
    os << " outcome " << core::toString(outcome) << "{";
    first = true;
    for (const auto &[path, effect] : effects) {
      os << (first ? "" : ", ") << core::printSummaryPath(path, namer) << ": "
         << core::printFlags(effect);
      first = false;
    }
    os << "}";
    if (const auto nulls = summary.nullOn.find(outcome);
        nulls != summary.nullOn.end())
      describePaths("null", nulls->second);
    if (const auto nonNulls = summary.nonNullOn.find(outcome);
        nonNulls != summary.nonNullOn.end())
      describePaths("notnull", nonNulls->second);
    if (const auto stored = summary.storesOn.find(outcome);
        stored != summary.storesOn.end())
      describePaths("stored", stored->second);
    if (const auto facts = summary.factOn.find(outcome);
        facts != summary.factOn.end()) {
      os << " facts{";
      first = true;
      for (const auto &[path, fact] : facts->second) {
        os << (first ? "" : ", ") << core::printSummaryPath(path, namer) << " "
           << fact.toString();
        first = false;
      }
      os << "}";
    }
  }
  if (!summary.increments.empty())
    describePaths("increments", summary.increments);
  if (!summary.decrements.empty())
    describePaths("decrements", summary.decrements);
  if (!summary.counts.empty())
    describePaths("counts", summary.counts);
  if (!summary.requiresExtent.empty()) {
    os << " requires-extent{";
    first = true;
    for (const auto &[param, requirements] : summary.requiresExtent) {
      for (const core::ExtentRequirement &requirement : requirements) {
        os << (first ? "" : ", ")
           << core::printSummaryPath(core::SummaryPath::param(param), namer)
           << ": " << core::printAffine(requirement.need, namer)
           << core::printGuard(requirement.when, namer);
        first = false;
      }
    }
    os << "}";
  }
  os << "\n";
}

void ProgramDatabase::dump(llvm::raw_ostream &os) const {
  os << "program:\n";
  for (const auto &[name, summary] : functions) {
    os << "  function '" << name << "':";
    describe(os, summary, globalNames);
  }
  for (const auto &[key, summary] : candidateSummaries) {
    os << "  candidate '" << key << "':";
    describe(os, summary, globalNames);
  }
  for (const std::string &key : countFields)
    os << "  count-field '" << key << "'\n";
}

} // namespace weavec::analysis
