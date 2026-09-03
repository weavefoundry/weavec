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

// -- UnitExports --------------------------------------------------------------

bool UnitExports::sameSummariesAs(const UnitExports &other) const {
  return functions == other.functions && globals == other.globals;
}

// -- Type keys ----------------------------------------------------------------

std::string functionTypeKey(QualType type, const ASTContext &context) {
  if (type.isNull())
    return {};
  if (const auto *pointer = type->getAs<PointerType>())
    type = pointer->getPointeeType();
  if (!type->isFunctionType())
    return {};
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
  // An anonymous record is spelled by its location, which no other unit
  // shares: no stable key (RFC 0005, *Accepted false positives*).
  for (const char *marker : {"(unnamed ", "(anonymous ", "<anonymous"}) {
    if (key.find(marker) != std::string::npos)
      return {};
  }
  return key;
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
  for (const auto &[name, function] : unit.functions) {
    const core::FunctionSummary summary =
        renumber(function.summary, unit.globals, globalNames);
    if (function.external) {
      auto [it, inserted] = functions.try_emplace(name, summary);
      if (!inserted)
        it->second.join(summary);
    }
    if (function.addressTaken && !function.typeKey.empty()) {
      auto [it, inserted] =
          candidateSummaries.try_emplace(function.typeKey, summary);
      if (!inserted)
        it->second.join(summary);
    }
  }
}

void ProgramDatabase::clear() {
  functions.clear();
  candidateSummaries.clear();
  globalNames = GlobalNames{};
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
        nulls != summary.nullOn.end()) {
      os << " null{";
      first = true;
      for (const core::SummaryPath &path : nulls->second) {
        os << (first ? "" : ", ") << core::printSummaryPath(path, namer);
        first = false;
      }
      os << "}";
    }
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
}

} // namespace weavec::analysis
