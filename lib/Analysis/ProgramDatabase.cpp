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
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/Version.h"

#include <algorithm>
#include <limits>
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
  if (callbackGlobals != other.callbackGlobals ||
      callbackRequests != other.callbackRequests ||
      memoryRequests != other.memoryRequests)
    return false;
  return functions == other.functions && globals == other.globals &&
         countFields == other.countFields && sizedFields == other.sizedFields;
}

// -- SizedFieldFacts ----------------------------------------------------------

void SizedFieldFacts::merge(const SizedFieldFacts &other) {
  witnesses.insert(other.witnesses.begin(), other.witnesses.end());
  unsizedFields.insert(other.unsizedFields.begin(), other.unsizedFields.end());
  unsizedPairs.insert(other.unsizedPairs.begin(), other.unsizedPairs.end());
}

void SizedFieldFacts::clear() {
  witnesses.clear();
  unsizedFields.clear();
  unsizedPairs.clear();
}

std::optional<std::pair<std::string, std::int64_t>>
SizedFieldFacts::confirmed(std::string_view field) const {
  // RFC 0012, *Sized fields*, "Inference": exactly one `(count, scale)`
  // witnessed, the field in no refutation, the pair in none.
  if (unsizedFields.contains(std::string(field)))
    return std::nullopt;
  std::optional<std::pair<std::string, std::int64_t>> found;
  for (auto it = witnesses.lower_bound(SizedFieldWitness{
           .field = std::string(field),
           .count = {},
           .scale = std::numeric_limits<std::int64_t>::min()});
       it != witnesses.end() && it->field == field; ++it) {
    if (found)
      return std::nullopt;
    found.emplace(it->count, it->scale);
  }
  if (!found || unsizedPairs.contains(UnsizedPair{.field = std::string(field),
                                                  .count = found->first}))
    return std::nullopt;
  return found;
}

std::set<SizedFieldWitness> SizedFieldFacts::confirmedPairs() const {
  std::set<SizedFieldWitness> result;
  for (const SizedFieldWitness &witness : witnesses) {
    if (const auto pair = confirmed(witness.field)) {
      result.insert(SizedFieldWitness{
          .field = witness.field, .count = pair->first, .scale = pair->second});
    }
  }
  return result;
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

std::string recordLayoutKey(QualType type, const ASTContext &context) {
  if (type.isNull() || !type->isRecordType() || type->isIncompleteType())
    return {};
  const auto *record = type->getAsRecordDecl();
  const auto &layout = context.getASTRecordLayout(record);
  // Top-level cv-qualification changes access, not the object layout.
  std::string shape = stableTypeKey(type.getUnqualifiedType(), context);
  if (shape.empty())
    shape = record->isUnion() ? "union" : "struct";
  shape += ":" + std::to_string(layout.getSize().getQuantity()) + ":" +
           std::to_string(layout.getAlignment().getQuantity());
  for (const auto *field : record->fields()) {
    shape += ":" + field->getNameAsString() + ":" +
             std::to_string(layout.getFieldOffset(field->getFieldIndex())) +
             ":" + stableTypeKey(field->getType(), context);
  }
  return core::CallTargets::function(std::move(shape)).toString();
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
  addCallbackInformation(unit);
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
  sizedFields.merge(unit.sizedFields);
}

UnitExports ProgramDatabase::renumbered(const UnitExports &unit) {
  UnitExports result = unit;
  if (!globalNames.extendTo(unit.globals)) {
    for (auto &[name, function] : result.functions) {
      function.summary = renumber(function.summary, unit.globals, globalNames);
      for (auto &[bindings, summary] : function.specializations)
        summary = renumber(summary, unit.globals, globalNames);
      decltype(function.memorySpecializations) contexts;
      for (const auto &[input, summary] : function.memorySpecializations) {
        const auto mapped =
            core::remapCallContext(input, [&](std::uint32_t id) {
              return id < unit.globals.size() ? std::optional(globalNames.idFor(
                                                    unit.globals.nameOf(id)))
                                              : std::nullopt;
            });
        if (mapped)
          contexts.emplace(*mapped,
                           renumber(summary, unit.globals, globalNames));
      }
      function.memorySpecializations = std::move(contexts);
    }
    result.memoryRequests.clear();
    for (const auto &[symbol, requests] : unit.memoryRequests)
      for (const auto &input : requests)
        if (const auto mapped =
                core::remapCallContext(input, [&](std::uint32_t id) {
                  return id < unit.globals.size()
                             ? std::optional(
                                   globalNames.idFor(unit.globals.nameOf(id)))
                             : std::nullopt;
                }))
          result.memoryRequests[symbol].insert(*mapped);
  }
  result.globals = globalNames;
  return result;
}

void ProgramDatabase::addCallbackInformation(const UnitExports &unit) {
  for (const auto &[name, targets] : unit.callbackGlobals)
    callbackGlobals[name].join(targets);
  for (const auto &[symbol, requests] : unit.callbackRequests)
    callbackRequests[symbol].insert(requests.begin(), requests.end());
  const core::GlobalIdMap map = [&](std::uint32_t id) {
    return id < unit.globals.size()
               ? std::optional(globalNames.idFor(unit.globals.nameOf(id)))
               : std::nullopt;
  };
  for (const auto &[symbol, requests] : unit.memoryRequests)
    for (const auto &input : requests)
      if (const auto mapped = core::remapCallContext(input, map))
        memoryRequests[symbol].insert(*mapped);
  for (const auto &[name, function] : unit.functions) {
    const std::string symbol =
        function.external ? name : unit.source + "#" + name;
    callableSummaries[symbol] =
        renumber(function.summary, unit.globals, globalNames);
    for (const auto &[bindings, summary] : function.specializations)
      contextSummaries[{symbol, bindings}] =
          renumber(summary, unit.globals, globalNames);
    for (const auto &[input, summary] : function.memorySpecializations)
      if (const auto mapped = core::remapCallContext(input, map))
        memorySummaries[{symbol, *mapped}].join(
            renumber(summary, unit.globals, globalNames));
  }
}

const core::FunctionSummary *
ProgramDatabase::findCallable(std::string_view symbol) const {
  const auto it = callableSummaries.find(symbol);
  return it == callableSummaries.end() ? nullptr : &it->second;
}
const core::FunctionSummary *ProgramDatabase::findSpecialization(
    std::string_view symbol, const core::CallbackBindings &bindings) const {
  const auto it = contextSummaries.find({std::string(symbol), bindings});
  return it == contextSummaries.end() ? nullptr : &it->second;
}
const std::set<core::CallbackBindings> &
ProgramDatabase::requestsFor(std::string_view symbol) const {
  static const std::set<core::CallbackBindings> Empty;
  const auto it = callbackRequests.find(symbol);
  return it == callbackRequests.end() ? Empty : it->second;
}

void ProgramDatabase::clear() {
  memorySummaries.clear();
  memoryRequests.clear();
  functions.clear();
  callableSummaries.clear();
  contextSummaries.clear();
  callbackRequests.clear();
  callbackGlobals.clear();
  candidateSummaries.clear();
  globalNames = GlobalNames{};
  countFields.clear();
  sizedFields.clear();
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

const core::FunctionSummary *ProgramDatabase::findMemorySpecialization(
    std::string_view symbol, const core::CallContext &context) const {
  const auto it = memorySummaries.find({std::string(symbol), context});
  return it == memorySummaries.end() ? nullptr : &it->second;
}

const std::set<core::CallContext> &
ProgramDatabase::memoryRequestsFor(std::string_view symbol) const {
  static const std::set<core::CallContext> Empty;
  const auto it = memoryRequests.find(symbol);
  return it == memoryRequests.end() ? Empty : it->second;
}

std::optional<core::CallContext>
ProgramDatabase::importContext(const core::CallContext &input,
                               const ASTContext &context,
                               GlobalTable &table) const {
  return core::remapCallContext(input, [&](std::uint32_t id) {
    const auto *var = externalVariable(globalNames.nameOf(id), context);
    return var ? std::optional(table.idFor(*var)) : std::nullopt;
  });
}

std::optional<core::CallContext>
ProgramDatabase::exportContext(const core::CallContext &input,
                               const GlobalTable &table) const {
  return core::remapCallContext(input, [&](std::uint32_t id) {
    const auto *var = table.declFor(id);
    return var && var->isExternallyVisible() ? globalNames.find(var->getName())
                                             : std::nullopt;
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
  for (const auto &[root, graph] : summary.heap) {
    os << "    heap " << core::printSummaryPath(root, namer)
       << (graph.incomplete ? " incomplete{" : " complete{");
    bool firstField = true;
    for (const core::Store &field : graph.fields) {
      os << (firstField ? "" : ", ")
         << core::printSummaryPath(field.dest, namer) << " = "
         << core::printValueSource(field.value, namer);
      firstField = false;
    }
    os << "}\n";
  }
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
  // RFC 0012, *Sized fields*.
  for (const SizedFieldWitness &witness : sizedFields.witnesses) {
    os << "  sized-field '" << witness.field << "' by '" << witness.count
       << "' * " << witness.scale << '\n';
  }
  for (const std::string &field : sizedFields.unsizedFields)
    os << "  unsized-field '" << field << "'\n";
  for (const UnsizedPair &pair : sizedFields.unsizedPairs) {
    os << "  unsized-field '" << pair.field << "' by '" << pair.count << "'\n";
  }
}

} // namespace weavec::analysis
