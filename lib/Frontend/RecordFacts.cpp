//===- RecordFacts.cpp - A unit's interface facts (RFC 0030) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/RecordFacts.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/AttributeReader.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Analysis/SlotCollector.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"

#include <array>
#include <map>
#include <string>
#include <utility>

namespace weavec::frontend::record {

namespace {

/// Top-level declarations of the unit, by name.
struct UnitDecls {
  std::map<std::string, const clang::FunctionDecl *, std::less<>> functions;
  std::map<std::string, const clang::VarDecl *, std::less<>> variables;
};

} // namespace

static UnitDecls unitDecls(const clang::ASTContext &context) {
  UnitDecls decls;
  for (const clang::Decl *decl : context.getTranslationUnitDecl()->decls()) {
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (function->getIdentifier() == nullptr)
        continue;
      const clang::FunctionDecl *&known =
          decls.functions[function->getNameAsString()];
      // The definition, when there is one, else the first declaration.
      if (known == nullptr || function->doesThisDeclarationHaveABody())
        known = function;
    } else if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
               variable != nullptr && variable->getIdentifier() != nullptr) {
      decls.variables.try_emplace(variable->getNameAsString(), variable);
    }
  }
  return decls;
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

static bool isShapeBeyondSingle(core::PointerShape shape) {
  return shape == core::PointerShape::Counted ||
         shape == core::PointerShape::Sized ||
         shape == core::PointerShape::EndedBy ||
         shape == core::PointerShape::NulTerminated;
}

/// §13.2 step 5: the bytes one element of `pointee` takes, or 0 when it has
/// no known size (`void`, an incomplete or dependent type).
static std::uint64_t elementBytes(clang::QualType pointee,
                                  const clang::ASTContext &context) {
  if (pointee.isNull() || pointee->isIncompleteType() ||
      pointee->isDependentType() || pointee->isFunctionType())
    return 0;
  const clang::CharUnits size = context.getTypeSizeInChars(pointee);
  return size.isPositive() ? static_cast<std::uint64_t>(size.getQuantity()) : 0;
}

/// §13.1 `imports[].calls[].evidence[].exact`: the argument is a whole
/// object at offset 0, so `bytes` is its size and not a lower bound. A
/// complete array, a string literal, or the address of a complete object.
static bool isExactExtent(const clang::Expr &argument) {
  const clang::Expr *expr = argument.IgnoreParens();
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
    if (cast->getCastKind() == clang::CK_ArrayToPointerDecay) {
      const clang::Expr *array = cast->getSubExpr()->IgnoreParens();
      if (llvm::isa<clang::StringLiteral>(array))
        return true;
      const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(array);
      const auto *variable =
          ref != nullptr ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl())
                         : nullptr;
      return variable != nullptr && variable->getType()->isConstantArrayType();
    }
    if (cast->getCastKind() != clang::CK_NoOp &&
        cast->getCastKind() != clang::CK_BitCast)
      return false;
    expr = cast->getSubExpr()->IgnoreParens();
  }
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr);
  if (unary == nullptr || unary->getOpcode() != clang::UO_AddrOf)
    return false;
  const clang::Expr *object = unary->getSubExpr()->IgnoreParens();
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(object);
  const auto *variable =
      ref != nullptr ? llvm::dyn_cast<clang::VarDecl>(ref->getDecl()) : nullptr;
  return variable != nullptr && !variable->getType()->isIncompleteType() &&
         !variable->getType()->isArrayType();
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
    if (entry->reliesOnSingle)
      facts.reliesOnSingle.push_back(i);
    if (entry->enforcement != analysis::RequirementEnforcement::CallerContract)
      continue;
    for (const analysis::MustAccessRequirement &requirement :
         entry->mustAccess) {
      if (!isShapeBeyondSingle(requirement.kind.shape))
        continue;
      facts.requirements.push_back(ExportedRequirement{
          .param = i,
          .kind = requirement.kind.toString(),
          .guard = requirement.guard
                       ? std::optional(requirement.guard->toString())
                       : std::nullopt,
          .element = elementBytes(param->getType()->getPointeeType(),
                                  function.getASTContext())});
    }
  }
  if (function.getReturnType()->isPointerType()) {
    if (const analysis::KindEntry *entry = kinds.result(function))
      facts.result = spellKind(entry->kind);
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

/// The declaration the link step blames: the first one, in source order,
/// with a WeaveC annotation on it or its parameters, else the first one.
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

/// §7.3 slot kinds of the fields of structs declared in user headers, and of
/// the external pointer variables: what other units may store into.
static std::vector<SlotKindRow> slotKinds(const analysis::KindTable &kinds,
                                          const clang::ASTContext &context,
                                          const std::string &unit) {
  const clang::SourceManager &sm = context.getSourceManager();
  const auto locations = [&](const analysis::KindEntry &entry) {
    std::vector<core::SourceLocation> stores;
    for (const analysis::KindDemotion &demotion : entry.demotedBy) {
      if (demotion.store == nullptr)
        continue;
      core::SourceLocation location =
          analysis::toCoreLocation(sm, demotion.store->getBeginLoc());
      location.opaque = 0;
      stores.push_back(std::move(location));
    }
    return stores;
  };
  std::map<std::string, SlotKindRow> rows;
  for (const auto &[field, entry] : kinds.fieldEntries()) {
    const clang::RecordDecl *record = field->getParent();
    const clang::SourceLocation at = sm.getExpansionLoc(record->getLocation());
    if (!field->getType()->isPointerType() || sm.isInMainFile(at) ||
        sm.isInSystemHeader(at) || record->getIdentifier() == nullptr)
      continue;
    const std::string slot =
        core::SlotKey::field(
            analysis::SlotCollector::recordSlotKey(*record, context, unit),
            field->getNameAsString())
            .toString();
    rows[slot] = SlotKindRow{.slot = slot,
                             .kind = entry.kind.toString(),
                             .demotedBy = locations(entry)};
  }
  for (const auto &[variable, entry] : kinds.variableEntries()) {
    if (!variable->getType()->isPointerType() ||
        !variable->hasGlobalStorage() || !variable->isExternallyVisible() ||
        variable->isStaticLocal() ||
        sm.isInSystemHeader(sm.getExpansionLoc(variable->getLocation())))
      continue;
    const std::string slot =
        core::SlotKey::global(variable->getNameAsString()).toString();
    rows[slot] = SlotKindRow{.slot = slot,
                             .kind = entry.kind.toString(),
                             .demotedBy = locations(entry)};
  }
  std::vector<SlotKindRow> sorted;
  sorted.reserve(rows.size());
  for (auto &[slot, row] : rows)
    sorted.push_back(std::move(row));
  return sorted;
}

/// Every call of an import, by the Call site the unit's ledger gives it.
static void collectCalls(const analysis::SiteIndex &sites,
                         const analysis::KindInferenceResult &inferred,
                         InterfaceFacts &facts) {
  for (const analysis::SiteIndex::FunctionSites &function : sites.functions()) {
    if (function.decl == nullptr)
      continue;
    const std::string caller = function.decl->getNameAsString();
    const clang::ASTContext &context = function.decl->getASTContext();
    for (const analysis::SiteInfo &site : function.sites) {
      if (site.kind != core::SiteKind::Call ||
          site.boundary != core::Boundary::Call || site.callee == nullptr ||
          site.callee->getIdentifier() == nullptr)
        continue;
      const auto import = facts.imports.find(site.callee->getNameAsString());
      const auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(site.stmt);
      if (import == facts.imports.end() || call == nullptr)
        continue;
      ImportCall entry{.function = caller, .site = site.id.ordinal, .args = {}};
      const unsigned params = site.callee->getNumParams();
      const unsigned count = site.callee->hasPrototype()
                                 ? std::min(params, call->getNumArgs())
                                 : call->getNumArgs();
      for (unsigned i = 0; i < count; ++i) {
        const clang::Expr *argument = call->getArg(i);
        ArgumentEvidence evidence;
        if (argument->getType()->isPointerType()) {
          entry.args.emplace_back(inferred.argumentIsSingleValid(*call, i));
          const analysis::KindInferenceResult::ArgumentWidth width =
              inferred.argumentWidth(*call, i);
          evidence.bytes = width.bytes;
          evidence.null = width.null;
          evidence.exact = !width.null && width.bytes.has_value() &&
                           isExactExtent(*argument);
        } else {
          entry.args.emplace_back();
          if (argument->getType()->isIntegerType())
            if (const auto value = argument->getIntegerConstantExpr(context))
              if (const std::optional<std::int64_t> number =
                      value->tryExtValue())
                evidence.value = number;
        }
        entry.evidence.push_back(evidence);
      }
      import->second.calls.push_back(std::move(entry));
    }
  }
}

/// §11: the heap allocator function the unit defines, if any.
static std::optional<std::string> definedAllocator(const UnitDecls &decls) {
  static constexpr std::array<llvm::StringLiteral, 4> Allocators{
      "malloc", "calloc", "realloc", "free"};
  for (const llvm::StringLiteral name : Allocators) {
    const auto it = decls.functions.find(name);
    if (it != decls.functions.end() &&
        it->second->doesThisDeclarationHaveABody() &&
        it->second->isExternallyVisible())
      return name.str();
  }
  return std::nullopt;
}

InterfaceFacts collectInterfaceFacts(clang::ASTContext &context,
                                     const FactsInput &input) {
  const core::LibrarySpec &library =
      input.library != nullptr ? *input.library : core::LibrarySpec::shipped();
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
  const analysis::KindInferenceResult &inferred = unitKinds->inferred;
  const UnitDecls decls = unitDecls(context);

  InterfaceFacts facts;
  facts.slots = slotFacts(slots);
  // RFC 0030 §9.4, §13.1 `boundaries`: what the boundary invariants found,
  // for the program-wide propagation of §13.2 step 5.
  facts.boundaries = input.exports.boundaries;
  for (const auto &[name, exported] : input.exports.functions) {
    const auto it = decls.functions.find(name);
    if (it != decls.functions.end() && it->second->hasBody())
      facts.functions.emplace(
          name, functionInterface(*it->second->getDefinition(), kinds, sm));
  }
  for (std::uint32_t id = 0; id < input.exports.globals.size(); ++id) {
    const std::string name = input.exports.globals.nameOf(id).str();
    const auto it = decls.variables.find(name);
    if (it == decls.variables.end())
      continue;
    GlobalInterface global{
        .typeKey = it->second->getType().getCanonicalType().getAsString(),
        .kind = {}};
    if (it->second->getType()->isPointerType())
      if (const analysis::KindEntry *entry = kinds.variable(*it->second))
        global.kind = spellKind(entry->kind);
    facts.globals.emplace(name, std::move(global));
  }
  for (const std::string &name : input.exports.imports) {
    const auto it = decls.functions.find(name);
    if (it == decls.functions.end())
      continue;
    ImportInterface import{.declared = declaredInterface(*it->second, kinds),
                           .location = std::nullopt,
                           .calls = {}};
    core::SourceLocation location = analysis::toCoreLocation(
        sm, blamedDeclaration(*it->second, sm)->getLocation());
    location.opaque = 0;
    import.location = std::move(location);
    facts.imports.emplace(name, std::move(import));
  }
  if (input.sites != nullptr)
    collectCalls(*input.sites, inferred, facts);
  facts.slotKinds = slotKinds(kinds, context, slots.unit());
  facts.allocator = definedAllocator(decls);
  facts.loweredAllocations = input.loweredAllocations;
  return facts;
}

InterfaceFacts collectSlotFacts(clang::ASTContext &context,
                                const core::LibrarySpec *library) {
  InterfaceFacts facts;
  facts.slots = slotFacts(
      analysis::SlotCollector(
          context, library != nullptr ? *library : core::LibrarySpec::shipped())
          .collect());
  return facts;
}

Payload payloadOf(const UnitResult &result) {
  Payload payload;
  payload.exports = result.exports;
  if (result.interface)
    payload.facts = *result.interface;
  if (result.ledger && !result.ledger->ledger.units.empty()) {
    const core::UnitLedger &unit = result.ledger->ledger.units.front();
    payload.sites = siteRows(unit);
    payload.a5 = unit.a5;
  }
  payload.reported = result.reported;
  return payload;
}

} // namespace weavec::frontend::record
