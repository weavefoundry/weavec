//===- Summaries.cpp - Function summaries for Clang declarations ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Summaries.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ProgramDatabase.h"

// Defines `LazyGenerationalUpdatePtr::makeValue`, which `Redeclarable`
// walks (`getCanonicalDecl`, `redecls()`) instantiate here.
#include "clang/AST/ASTContext.h"

#include "llvm/ADT/STLExtras.h"

#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

// -- GlobalTable --------------------------------------------------------------

std::uint32_t GlobalTable::idFor(const VarDecl &var) {
  const VarDecl *canonical = var.getCanonicalDecl();
  const auto [it, inserted] =
      ids.try_emplace(canonical, static_cast<std::uint32_t>(decls.size()));
  if (inserted)
    decls.push_back(canonical);
  return it->second;
}

const VarDecl *GlobalTable::declFor(std::uint32_t id) const noexcept {
  return id < decls.size() ? decls[id] : nullptr;
}

llvm::StringRef GlobalTable::nameOf(std::uint32_t id) const {
  const VarDecl *decl = declFor(id);
  return decl == nullptr ? llvm::StringRef("<global>") : decl->getName();
}

// -- Annotations --------------------------------------------------------------

bool SignatureAnnotations::anyOwnership() const noexcept {
  return result.ownership() || llvm::any_of(params, [](const AnnotationSet &s) {
           return s.ownership();
         });
}

SignatureAnnotations collectAnnotations(const FunctionDecl &function) {
  SignatureAnnotations collected;
  collected.params.resize(function.getNumParams());
  for (const FunctionDecl *redecl : function.redecls()) {
    const AnnotationSet onFunction = getAnnotations(*redecl);
    collected.result.merge(onFunction);
    collected.unsafe = collected.unsafe || onFunction.unsafe;
    for (unsigned i = 0;
         i < redecl->getNumParams() && i < collected.params.size(); ++i)
      collected.params[i].merge(getAnnotations(*redecl->getParamDecl(i)));
  }
  return collected;
}

bool hasOwnershipAnnotations(const FunctionDecl &function) {
  return collectAnnotations(function).anyOwnership();
}

namespace {

/// The shape of a signature the annotation rules need: which parameters and
/// whether the result are pointers. Shared by function declarations and
/// function-pointer types.
struct SignatureShape {
  std::vector<bool> pointerParams;
  bool pointerResult = false;
};

} // namespace

static SignatureShape shapeOf(const FunctionDecl &function) {
  SignatureShape shape;
  shape.pointerParams.reserve(function.getNumParams());
  for (const ParmVarDecl *param : function.parameters())
    shape.pointerParams.push_back(param->getType()->isPointerType());
  shape.pointerResult = function.getReturnType()->isPointerType();
  return shape;
}

static SignatureShape shapeOf(const FunctionProtoType &type) {
  SignatureShape shape;
  shape.pointerParams.reserve(type.getNumParams());
  for (const QualType param : type.getParamTypes())
    shape.pointerParams.push_back(param->isPointerType());
  shape.pointerResult = type.getReturnType()->isPointerType();
  return shape;
}

/// Replaces the inferred facts about every annotated root with what the
/// annotation says (RFC 0003: annotations are authoritative per root; RFC
/// 0004: `WEAVEC_RAW` on a parameter records nothing, on a result records
/// `raw`).
static void applyAnnotations(core::FunctionSummary &summary,
                             const SignatureShape &shape,
                             const AnnotationSet &result,
                             const std::vector<AnnotationSet> &params) {
  const auto eraseRoot = [&summary](unsigned index, bool includeStores) {
    for (auto it = summary.effects.begin(); it != summary.effects.end();) {
      if (it->first.isParam() && it->first.index == index)
        it = summary.effects.erase(it);
      else
        ++it;
    }
    if (!includeStores)
      return;
    for (auto it = summary.stores.begin(); it != summary.stores.end();) {
      if (it->dest.isParam() && it->dest.index == index)
        it = summary.stores.erase(it);
      else
        ++it;
    }
  };

  for (unsigned i = 0; i < params.size() && i < shape.pointerParams.size();
       ++i) {
    if (!shape.pointerParams[i])
      continue;
    const AnnotationSet &set = params[i];
    const core::SummaryPath root = core::SummaryPath::param(i);
    if (set.owned) {
      // Whatever happens to a consumed object is the callee's business. The
      // body's release family survives so `xfree(fopen(...))` is reported.
      const std::string family = summary.effectOf(root).family;
      eraseRoot(i, /*includeStores=*/true);
      summary.addEffect(root,
                        core::PlaceEffect{.moved = true, .family = family});
    } else if (set.mutBorrowed) {
      eraseRoot(i, /*includeStores=*/false);
      summary.addEffect(root.deref(), core::PlaceEffect{.written = true});
    } else if (set.borrowed) {
      eraseRoot(i, /*includeStores=*/false);
      summary.addEffect(root.deref(), core::PlaceEffect{.read = true});
    } else if (set.raw) {
      // The callee promised to uphold the caller's invariants itself; the
      // caller's pointer is untouched (RFC 0004, *Raw pointers*).
      eraseRoot(i, /*includeStores=*/true);
    }
  }

  if (!shape.pointerResult)
    return;
  if (result.owned) {
    const std::string family = summary.freshReturnFamily();
    summary.returns.clear();
    summary.addReturn(core::ValueSource::fresh(family));
  } else if (result.borrowed || result.mutBorrowed) {
    // The signature promises a borrow: a fresh allocation or a raw value
    // the body may return is a reported mismatch (or an assertion inside an
    // unsafe region), and callers must trust the annotation.
    summary.eraseFreshReturns();
    summary.returns.erase(core::ValueSource::raw());
    if (summary.returns.empty())
      summary.addReturn(core::ValueSource::unknown());
  } else if (result.raw) {
    summary.returns.clear();
    summary.addReturn(core::ValueSource::raw());
  }
}

core::FunctionSummary summaryFromAnnotations(const FunctionDecl &function) {
  core::FunctionSummary summary;
  const SignatureAnnotations annotations = collectAnnotations(function);
  applyAnnotations(summary, shapeOf(function), annotations.result,
                   annotations.params);
  return summary;
}

// -- Indirect callees ---------------------------------------------------------

const FunctionProtoType *indirectCalleeType(const CallExpr &call) {
  QualType type = call.getCallee()->getType();
  if (const auto *pointer = type->getAs<PointerType>())
    type = pointer->getPointeeType();
  return type->getAs<FunctionProtoType>();
}

const Decl *indirectCalleeDecl(const CallExpr &call) {
  const Expr *callee = call.getCallee();
  while (callee != nullptr) {
    callee = callee->IgnoreParenCasts();
    if (const auto *unary = dyn_cast<UnaryOperator>(callee);
        unary != nullptr && unary->getOpcode() == UO_Deref) {
      callee = unary->getSubExpr();
      continue;
    }
    if (const auto *subscript = dyn_cast<ArraySubscriptExpr>(callee)) {
      callee = subscript->getBase();
      continue;
    }
    if (const auto *ref = dyn_cast<DeclRefExpr>(callee))
      return isa<FunctionDecl>(ref->getDecl()) ? nullptr : ref->getDecl();
    if (const auto *member = dyn_cast<MemberExpr>(callee))
      return member->getMemberDecl();
    return nullptr;
  }
  return nullptr;
}

// -- SummaryStore -------------------------------------------------------------

bool SummaryStore::setInferred(const FunctionDecl &function,
                               core::FunctionSummary summary) {
  const FunctionDecl *canonical = key(function);
  merged.erase(canonical);
  mergedSource.erase(canonical);
  auto [it, inserted] = inferred.try_emplace(canonical, std::move(summary));
  if (inserted) {
    mergedIndirect.clear();
    return true;
  }
  if (it->second == summary)
    return false;
  it->second = std::move(summary);
  // Any indirect join may have included this function.
  mergedIndirect.clear();
  return true;
}

const core::FunctionSummary *
SummaryStore::inferredFor(const FunctionDecl &function) const {
  const auto it = inferred.find(key(function));
  return it == inferred.end() ? nullptr : &it->second;
}

std::optional<core::FunctionSummary>
SummaryStore::programSummaryFor(const FunctionDecl &callee) {
  if (database == nullptr || context == nullptr ||
      !callee.isExternallyVisible() || callee.getIdentifier() == nullptr)
    return std::nullopt;
  const core::FunctionSummary *exported = database->find(callee.getName());
  if (exported == nullptr)
    return std::nullopt;
  return database->importInto(*exported, *context, globalTable);
}

std::optional<ResolvedSummary>
SummaryStore::lookup(const FunctionDecl &callee) {
  const FunctionDecl *canonical = key(callee);
  if (const auto it = merged.find(canonical); it != merged.end())
    return ResolvedSummary{.summary = &it->second,
                           .source = mergedSource.at(canonical)};

  const SignatureAnnotations annotations = collectAnnotations(callee);
  const core::FunctionSummary *inferredBody = inferredFor(callee);
  // RFC 0005: a body in another unit of the program, below this unit's own
  // inference and above the library table.
  const std::optional<core::FunctionSummary> programBody =
      inferredBody == nullptr ? programSummaryFor(callee) : std::nullopt;
  // `WEAVEC_UNSAFE` on a declaration with no analysed body is an explicit
  // opt-out: the user asked for the empty summary rather than a warning. A
  // `WEAVEC_UNSAFE` definition is analysed like any other (RFC 0004, *Unsafe
  // regions*) and its inferred summary is used once it exists.
  const bool haveBody = inferredBody != nullptr || programBody.has_value();
  const bool annotated =
      annotations.anyOwnership() || (annotations.unsafe && !haveBody);

  if (!haveBody && !annotated) {
    if (const core::FunctionSummary *builtin = builtinSummary(callee))
      return ResolvedSummary{.summary = builtin,
                             .source = SummarySource::Builtin};
    return std::nullopt;
  }

  core::FunctionSummary result;
  SummarySource source = SummarySource::Program;
  if (inferredBody != nullptr) {
    result = *inferredBody;
    source = SummarySource::Inferred;
  } else if (programBody) {
    result = *programBody;
  }
  if (annotated) {
    applyAnnotations(result, shapeOf(callee), annotations.result,
                     annotations.params);
    source = SummarySource::Annotation;
  }
  const auto it = merged.try_emplace(canonical, std::move(result)).first;
  mergedSource[canonical] = source;
  return ResolvedSummary{.summary = &it->second, .source = source};
}

void SummaryStore::addAddressTaken(const FunctionDecl &function) {
  const FunctionDecl *canonical = key(function);
  if (addressTakenSet.insert(canonical).second) {
    addressTaken.push_back(canonical);
    mergedIndirect.clear();
  }
}

bool SummaryStore::isAddressTaken(const FunctionDecl &function) const {
  return addressTakenSet.contains(key(function));
}

std::vector<const FunctionDecl *>
SummaryStore::candidatesFor(const CallExpr &call) const {
  std::vector<const FunctionDecl *> result;
  const FunctionProtoType *type = indirectCalleeType(call);
  if (type == nullptr)
    return result;
  const QualType wanted = QualType(type, 0).getCanonicalType();
  for (const FunctionDecl *function : addressTaken) {
    if (function->getType().getCanonicalType() == wanted)
      result.push_back(function);
  }
  return result;
}

std::optional<ResolvedSummary>
SummaryStore::lookupIndirect(const CallExpr &call) {
  const FunctionProtoType *type = indirectCalleeType(call);
  if (type == nullptr)
    return std::nullopt;

  const Decl *declaration = indirectCalleeDecl(call);
  FunctionTypeAnnotations annotations;
  if (declaration != nullptr)
    annotations = collectFunctionTypeAnnotations(*declaration);
  const bool annotated = annotations.anyOwnership();

  const std::pair<const Type *, const Decl *> cacheKey{
      QualType(type, 0).getCanonicalType().getTypePtr(),
      annotated ? declaration : nullptr};
  if (const auto it = mergedIndirect.find(cacheKey);
      it != mergedIndirect.end()) {
    return ResolvedSummary{.summary = &it->second,
                           .source = annotated ? SummarySource::Annotation
                                               : SummarySource::Inferred};
  }

  core::FunctionSummary joined;
  bool anyCandidate = false;
  bool anyLocal = false;
  for (const FunctionDecl *candidate : candidatesFor(call)) {
    const auto resolved = lookup(*candidate);
    if (!resolved)
      continue;
    joined.join(*resolved->summary);
    anyCandidate = true;
    anyLocal = true;
  }
  // RFC 0005: address-taken functions of the type anywhere in the program.
  if (database != nullptr && context != nullptr) {
    const std::string typeKey = functionTypeKey(QualType(type, 0), *context);
    if (const core::FunctionSummary *program =
            typeKey.empty() ? nullptr : database->candidates(typeKey)) {
      joined.join(database->importInto(*program, *context, globalTable));
      anyCandidate = true;
    }
  }
  if (!annotated && !anyCandidate)
    return std::nullopt;

  SummarySource source =
      anyLocal ? SummarySource::Inferred : SummarySource::Program;
  if (annotated) {
    applyAnnotations(joined, shapeOf(*type), annotations.result,
                     annotations.params);
    source = SummarySource::Annotation;
  }
  const auto it = mergedIndirect.try_emplace(cacheKey, std::move(joined)).first;
  return ResolvedSummary{.summary = &it->second, .source = source};
}

std::vector<std::string> SummaryStore::unknownCalleeNames() const {
  std::vector<std::string> names;
  for (const FunctionDecl *callee : unknownCallees) {
    if (callee->getIdentifier() != nullptr && callee->isExternallyVisible())
      names.push_back(callee->getNameAsString());
  }
  llvm::sort(names);
  return names;
}

std::vector<std::string> SummaryStore::unknownIndirectTypeKeys() const {
  std::vector<std::string> keys;
  if (context == nullptr)
    return keys;
  for (const Type *type : unknownIndirect) {
    std::string key = functionTypeKey(QualType(type, 0), *context);
    if (!key.empty())
      keys.push_back(std::move(key));
  }
  llvm::sort(keys);
  return keys;
}

bool SummaryStore::noteUnknownCallee(const FunctionDecl &callee) {
  return unknownCallees.insert(key(callee)).second;
}

bool SummaryStore::noteUnknownIndirect(const CallExpr &call) {
  const FunctionProtoType *type = indirectCalleeType(call);
  if (type == nullptr)
    return false;
  return unknownIndirect
      .insert(QualType(type, 0).getCanonicalType().getTypePtr())
      .second;
}

} // namespace weavec::analysis
