//===- InterfaceFacts.cpp - A unit's interface facts ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/InterfaceFacts.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SlotCollector.h"

#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace weavec::frontend {

std::string spellKind(const core::PointerKind &kind) {
  return std::string(core::toString(kind.source)) + " " + kind.toString();
}

std::optional<core::PointerKind> parseKind(std::string_view text) {
  const std::size_t space = text.find(' ');
  if (space == std::string_view::npos)
    return std::nullopt;
  const std::optional<core::KindSource> source =
      core::parseKindSource(text.substr(0, space));
  if (!source)
    return std::nullopt;
  return core::PointerKind::parse(text.substr(space + 1), *source);
}

bool DeclaredInterface::empty() const noexcept {
  return !result && !ownership &&
         std::ranges::all_of(params, [](const DeclaredParam &param) {
           return !param.kind && !param.ownership;
         });
}

/// The unit's top-level functions by name: the definition, when there is
/// one, else the first declaration.
static std::map<std::string, const clang::FunctionDecl *, std::less<>>
unitFunctions(const clang::ASTContext &context) {
  std::map<std::string, const clang::FunctionDecl *, std::less<>> functions;
  for (const clang::Decl *decl : context.getTranslationUnitDecl()->decls()) {
    const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (function == nullptr || function->getIdentifier() == nullptr)
      continue;
    const clang::FunctionDecl *&known = functions[function->getNameAsString()];
    if (known == nullptr || function->doesThisDeclarationHaveABody())
      known = function;
  }
  return functions;
}

static bool isPointer(clang::QualType type) {
  return type->isPointerType() || type->isArrayType();
}

/// A kind some declaration outside the system headers states (§7.2 levels
/// 1 and 2): the declared shape, or `unknown` with a declared `nonnull`.
static std::optional<std::string>
declaredKind(const analysis::KindEntry *entry) {
  if (entry == nullptr)
    return std::nullopt;
  const auto userLevel = [](const std::optional<analysis::KindLevel> &level) {
    return level && (*level == analysis::KindLevel::Annotation ||
                     *level == analysis::KindLevel::Ecosystem);
  };
  const bool shape = entry->hasDeclaredShape() && userLevel(entry->shapeLevel);
  const bool nonnull =
      entry->declaresNonnull() && userLevel(entry->nullabilityLevel);
  if (!shape && !nonnull)
    return std::nullopt;
  core::PointerKind kind = shape ? entry->kind : core::PointerKind::unknown();
  kind.nullability =
      nonnull ? core::Nullability::Nonnull : core::Nullability::Nullable;
  return kind.toString();
}

/// The ownership annotation `set` states, spelled as the macro.
static std::optional<std::string>
ownershipOf(const analysis::AnnotationSet &set) {
  if (set.owned)
    return "WEAVEC_OWNED";
  if (set.borrowed)
    return "WEAVEC_BORROWED";
  if (set.mutBorrowed)
    return "WEAVEC_MUT";
  if (set.raw)
    return "WEAVEC_RAW";
  if (set.retains)
    return "WEAVEC_RETAINS";
  if (set.releases)
    return "WEAVEC_RELEASES";
  return std::nullopt;
}

static FunctionInterface functionInterface(const clang::FunctionDecl &function,
                                           const analysis::KindTable &kinds,
                                           const clang::SourceManager &sm) {
  FunctionInterface facts;
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const clang::ParmVarDecl *param = function.getParamDecl(i);
    const analysis::KindEntry *entry = kinds.param(function, i);
    if (!isPointer(param->getType()) || entry == nullptr) {
      facts.params.emplace_back();
      continue;
    }
    facts.params.emplace_back(spellKind(entry->kind));
  }
  facts.location = analysis::toCoreLocation(sm, function.getLocation());
  facts.location->opaque = 0;
  return facts;
}

static DeclaredInterface declaredInterface(const clang::FunctionDecl &function,
                                           const analysis::KindTable &kinds) {
  DeclaredInterface declared;
  const clang::FunctionDecl *latest = function.getMostRecentDecl();
  for (unsigned i = 0; i < latest->getNumParams(); ++i) {
    DeclaredParam param;
    analysis::AnnotationSet annotations;
    for (const clang::FunctionDecl *redecl : function.redecls()) {
      if (i >= redecl->getNumParams())
        continue;
      const clang::ParmVarDecl *decl = redecl->getParamDecl(i);
      annotations.merge(analysis::getAnnotations(*decl));
      if (param.name.empty() && decl->getIdentifier() != nullptr)
        param.name = decl->getNameAsString();
    }
    if (isPointer(latest->getParamDecl(i)->getType()))
      param.kind = declaredKind(kinds.param(function, i));
    param.ownership = ownershipOf(annotations);
    declared.params.push_back(std::move(param));
  }
  analysis::AnnotationSet result;
  for (const clang::FunctionDecl *redecl : function.redecls())
    result.merge(analysis::getAnnotations(*redecl));
  if (function.getReturnType()->isPointerType()) {
    declared.result = declaredKind(kinds.result(function));
    declared.ownership = ownershipOf(result);
  }
  return declared;
}

/// The declaration a contradiction is reported at: the first one, in source
/// order, with a WeaveC annotation on it or its parameters, else the first
/// one.
static const clang::FunctionDecl *
blamedDeclaration(const clang::FunctionDecl &f,
                  const clang::SourceManager &sm) {
  const clang::FunctionDecl *blamed = nullptr;
  for (const clang::FunctionDecl *redecl : f.redecls()) {
    bool annotated = analysis::getAnnotations(*redecl).any();
    for (const clang::ParmVarDecl *param : redecl->parameters())
      annotated = annotated || analysis::getAnnotations(*param).any();
    if (annotated && (blamed == nullptr ||
                      sm.isBeforeInTranslationUnit(redecl->getLocation(),
                                                   blamed->getLocation())))
      blamed = redecl;
  }
  return blamed != nullptr ? blamed : f.getFirstDecl();
}

/// The unit's function-pointer slots (§9.3), with local slots eliminated.
static SlotFacts slotFacts(const analysis::SlotCollection &slots) {
  SlotFacts facts;
  facts.rows = slots.exported().rows();
  facts.unit = slots.unit();
  const core::SlotRules &rules = slots.rules();
  facts.defined = rules.defined;
  facts.exported = rules.exported;
  facts.confinedRecords = rules.confinedRecords;
  facts.escapedStatics = rules.escapedStatics;
  return facts;
}

InterfaceFacts collectInterfaceFacts(clang::ASTContext &context,
                                     const FactsInput &input) {
  const core::LibrarySpec &library = core::LibrarySpec::shipped();
  const clang::SourceManager &sm = context.getSourceManager();
  // The analysis's own kinds when it passed them (RFC 0030 §1 step 2).
  std::shared_ptr<const analysis::UnitKinds> built;
  const analysis::UnitKinds *unitKinds = input.kinds;
  if (unitKinds == nullptr) {
    built = analysis::UnitKinds::build(context, library);
    unitKinds = built.get();
  }
  const analysis::KindTable &kinds = unitKinds->table;
  const analysis::SlotCollection &slots = unitKinds->slots;
  const auto functions = unitFunctions(context);

  InterfaceFacts facts;
  facts.slots = slotFacts(slots);
  // RFC 0030 §9.4: what the boundary invariants found, for the
  // program-wide propagation.
  facts.boundaries = input.exports.boundaries;
  for (const auto &[name, exported] : input.exports.functions) {
    const auto it = functions.find(name);
    if (it != functions.end() && it->second->hasBody())
      facts.functions.emplace(
          name, functionInterface(*it->second->getDefinition(), kinds, sm));
  }
  for (const std::string &name : input.exports.imports) {
    const auto it = functions.find(name);
    if (it == functions.end())
      continue;
    ImportInterface import{.declared = declaredInterface(*it->second, kinds),
                           .location = std::nullopt};
    core::SourceLocation location = analysis::toCoreLocation(
        sm, blamedDeclaration(*it->second, sm)->getLocation());
    location.opaque = 0;
    import.location = std::move(location);
    facts.imports.emplace(name, std::move(import));
  }
  return facts;
}

InterfaceFacts collectSlotFacts(clang::ASTContext &context) {
  InterfaceFacts facts;
  facts.slots = slotFacts(
      analysis::SlotCollector(context, core::LibrarySpec::shipped()).collect());
  return facts;
}

} // namespace weavec::frontend
