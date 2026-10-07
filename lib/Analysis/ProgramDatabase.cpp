//===- ProgramDatabase.cpp - Summaries across translation units -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/ProgramDatabase.h"

#include "weavec/Core/Effects.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/Version.h"

#include "llvm/ADT/StringExtras.h"

#include <optional>
#include <string>
#include <string_view>
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

llvm::StringRef GlobalNames::nameOf(std::uint32_t id) const {
  return id < names.size() ? llvm::StringRef(names[id]) : "<global>";
}

// -- UnitExports --------------------------------------------------------------

bool UnitExports::sameFunctionsAs(const UnitExports &other) const {
  return functions == other.functions && globals == other.globals;
}

bool UnitExports::sameSummariesAs(const UnitExports &other) const {
  return sameFunctionsAs(other) && contextEffects == other.contextEffects &&
         contextRequests == other.contextRequests;
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
  for (const char *marker :
       {"(unnamed ", "(unnamed)", "(anonymous ", "<anonymous"}) {
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

void ProgramDatabase::add(const UnitExports &unit) {
  const auto toDatabase =
      [&](std::uint32_t id) -> std::optional<std::uint32_t> {
    if (id >= unit.globals.size())
      return std::nullopt;
    return globalNames.idFor(unit.globals.nameOf(id));
  };
  const auto fold =
      [](std::map<std::string, core::FunctionEffects, std::less<>> &into,
         llvm::StringRef key, core::FunctionEffects effects) {
        auto [it, inserted] = into.try_emplace(key.str(), effects);
        if (!inserted)
          it->second = core::joinEffects(it->second, effects);
      };
  for (const auto &[name, function] : unit.functions) {
    core::FunctionEffects effects =
        core::renumberGlobals(function.effects, toDatabase);
    if (function.external)
      fold(byName, name, effects);
    // (An internal function another unit may be handed as a callback, by
    // its portable name.)
    else if (function.addressTaken && !unit.source.empty())
      fold(byName, unit.source + "#" + name, effects);
    if (function.addressTaken && !function.typeKey.empty())
      fold(byType, function.typeKey, std::move(effects));
  }
  for (const auto &[request, effects] : unit.contextEffects) {
    core::FunctionEffects renumbered =
        core::renumberGlobals(effects, toDatabase);
    auto [it, inserted] = byContext.try_emplace(request, renumbered);
    if (!inserted)
      it->second = core::joinEffects(it->second, renumbered);
  }
  requested.insert(unit.contextRequests.begin(), unit.contextRequests.end());
}

const core::FunctionEffects *
ProgramDatabase::contextEffects(const ContextRequest &request) const {
  auto it = byContext.find(request);
  return it != byContext.end() ? &it->second : nullptr;
}

void ProgramDatabase::clear() {
  globalNames = GlobalNames{};
  byName.clear();
  byType.clear();
  byContext.clear();
  requested.clear();
}

const core::FunctionEffects *
ProgramDatabase::findEffects(llvm::StringRef name) const {
  auto it = byName.find(name);
  return it != byName.end() ? &it->second : nullptr;
}

const core::FunctionEffects *
ProgramDatabase::candidateEffects(llvm::StringRef typeKey) const {
  auto it = byType.find(typeKey);
  return it != byType.end() ? &it->second : nullptr;
}

void ProgramDatabase::dump(llvm::raw_ostream &os) const {
  os << "program:\n";
  auto summary = [&](const core::FunctionEffects &effects) {
    std::string text = core::toText(effects);
    for (llvm::StringRef line :
         llvm::split(llvm::StringRef(text).rtrim('\n'), '\n'))
      os << "  " << line << '\n';
  };
  for (const auto &[name, effects] : byName) {
    os << "  function '" << name << "':\n";
    summary(effects);
  }
  for (const auto &[key, effects] : byType) {
    os << "  candidate '" << key << "':\n";
    summary(effects);
  }
  for (std::size_t id = 0; id < globalNames.size(); ++id)
    os << "  global" << id << " '"
       << globalNames.nameOf(static_cast<std::uint32_t>(id)) << "'\n";
  for (const ContextRequest &request : requested) {
    os << "  context '" << request.callee << "' '" << request.key << "':\n";
    if (const core::FunctionEffects *effects = contextEffects(request))
      summary(*effects);
    else
      os << "    unserved\n";
  }
}

} // namespace weavec::analysis
