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
/// whether the result are pointers, and which parameters have the result's
/// type (the returning-ref shape, RFC 0010). Shared by function declarations
/// and function-pointer types.
struct SignatureShape {
  std::vector<bool> pointerParams;
  std::vector<bool> resultTyped;
  bool pointerResult = false;
};

} // namespace

static SignatureShape shapeOf(llvm::ArrayRef<QualType> params,
                              QualType result) {
  SignatureShape shape;
  shape.pointerParams.reserve(params.size());
  shape.resultTyped.reserve(params.size());
  for (const QualType param : params) {
    shape.pointerParams.push_back(param->isPointerType());
    shape.resultTyped.push_back(
        param->isPointerType() &&
        param.getCanonicalType().getUnqualifiedType() ==
            result.getCanonicalType().getUnqualifiedType());
  }
  shape.pointerResult = result->isPointerType();
  return shape;
}

static SignatureShape shapeOf(const FunctionDecl &function) {
  std::vector<QualType> params;
  params.reserve(function.getNumParams());
  for (const ParmVarDecl *param : function.parameters())
    params.push_back(param->getType());
  return shapeOf(params, function.getReturnType());
}

static SignatureShape shapeOf(const FunctionProtoType &type) {
  return shapeOf(type.getParamTypes(), type.getReturnType());
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
      // body's release family survives so `xfree(fopen(...))` is reported;
      // `WEAVEC_OWNED_BY(f)` names it outright (RFC 0010).
      const std::string family =
          set.family.empty() ? summary.effectOf(root).family : set.family;
      eraseRoot(i, /*includeStores=*/true);
      summary.addEffect(root,
                        core::PlaceEffect{.moved = true, .family = family});
    } else if (set.releases) {
      // RFC 0010, *Annotations*: one share of the argument's object is
      // released; the caller's name is dead, other shares live on. The
      // count field is unknown, so the object path itself stands for it.
      const std::string family = summary.effectOf(root).family;
      eraseRoot(i, /*includeStores=*/true);
      summary.addEffect(
          root,
          core::PlaceEffect{.freed = true, .share = true, .family = family});
      summary.counts.insert(root.deref());
    } else if (set.retains) {
      // The callee takes a reference: the caller's place gains a share.
      eraseRoot(i, /*includeStores=*/false);
      summary.addEffect(root.deref(), core::PlaceEffect{.written = true});
      summary.increments.insert(root.deref());
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
  // RFC 0010, *Annotations*: a declaration with no body that returns the
  // type of its one `WEAVEC_RETAINS` parameter and says nothing about the
  // result is the returning-ref shape (`g_object_ref`): the result is a copy
  // of that argument, so the caller's copy carries the share away.
  if (!result.ownership() && summary.returns.empty()) {
    std::optional<unsigned> retained;
    for (unsigned i = 0; i < params.size() && i < shape.resultTyped.size();
         ++i) {
      if (!params[i].retains || !shape.resultTyped[i])
        continue;
      retained = retained ? std::optional<unsigned>() : std::optional(i);
      if (!retained)
        break;
    }
    if (retained)
      summary.addReturn(
          core::ValueSource::copy(core::SummaryPath::param(*retained)));
  }
  if (result.owned) {
    const std::string family =
        result.family.empty() ? summary.freshReturnFamily() : result.family;
    summary.returns.clear();
    summary.addReturn(core::ValueSource::fresh(family));
  } else if (result.borrowed || result.mutBorrowed) {
    // The signature promises a borrow: a fresh allocation or a raw value
    // the body may return is a reported mismatch (or an assertion inside an
    // unsafe region), and callers must trust the annotation.
    summary.eraseFreshReturns();
    summary.eraseReturns(core::ValueSource::Kind::Raw);
    if (summary.returns.empty())
      summary.addReturn(core::ValueSource::unknown());
  } else if (result.raw) {
    summary.returns.clear();
    summary.addReturn(core::ValueSource::raw());
  }
}

/// RFC 0008, *Annotation surface*: `WEAVEC_NONNULL` on a parameter is a
/// requirement on callers, `WEAVEC_NULLABLE` lifts one the body implied; on
/// the result they add or remove the `null` alternative. Neither changes
/// ownership, so they layer on whatever else the summary says.
static void applyNullnessAnnotations(core::FunctionSummary &summary,
                                     const SignatureShape &shape,
                                     const AnnotationSet &result,
                                     const std::vector<AnnotationSet> &params) {
  for (unsigned i = 0; i < params.size() && i < shape.pointerParams.size();
       ++i) {
    if (!shape.pointerParams[i])
      continue;
    if (params[i].nonNull)
      summary.requiresNonNull.insert(i);
    else if (params[i].nullable)
      summary.requiresNonNull.erase(i);
  }
  if (!shape.pointerResult)
    return;
  if (result.nonNull) {
    summary.eraseReturns(core::ValueSource::Kind::Null);
    if (summary.returns.empty())
      summary.addReturn(core::ValueSource::unknown());
  } else if (result.nullable) {
    summary.addReturn(core::ValueSource::null());
  }
}

bool SignatureAnnotations::anyNullness() const noexcept {
  return result.nullness() || llvm::any_of(params, [](const AnnotationSet &s) {
           return s.nullness();
         });
}

core::FunctionSummary summaryFromAnnotations(const FunctionDecl &function) {
  core::FunctionSummary summary;
  const SignatureAnnotations annotations = collectAnnotations(function);
  applyAnnotations(summary, shapeOf(function), annotations.result,
                   annotations.params);
  applyNullnessAnnotations(summary, shapeOf(function), annotations.result,
                           annotations.params);
  // A declared `noreturn` is the strongest statement there is about the
  // exit; the inferred bit agrees with it (RFC 0009, *Inferred `noreturn`*).
  if (function.isNoReturn())
    summary.neverReturns = true;
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

// -- Count fields (RFC 0010) --------------------------------------------------

/// The record type a summary path's dereference steps land in, following
/// `steps` from `type`: `struct obj *` with `*` is `struct obj`; `.base`
/// then names the field's type. Null when a step does not fit the type.
static QualType followSteps(QualType type, llvm::ArrayRef<core::PathElem> steps,
                            const ASTContext &context) {
  for (const core::PathElem &step : steps) {
    if (type.isNull())
      return {};
    type = type.getCanonicalType();
    switch (step.step) {
    case core::PathStep::Deref:
    case core::PathStep::Index:
      if (const auto *pointer = type->getAs<PointerType>())
        type = pointer->getPointeeType();
      else if (const auto *array = context.getAsArrayType(type))
        type = array->getElementType();
      else
        return {};
      break;
    case core::PathStep::Field: {
      const RecordDecl *record = type->getAsRecordDecl();
      if (record == nullptr)
        return {};
      const FieldDecl *found = nullptr;
      for (const FieldDecl *field : record->fields()) {
        if (field->getName() == step.field) {
          found = field;
          break;
        }
      }
      if (found == nullptr)
        return {};
      type = found->getType();
      break;
    }
    }
  }
  return type;
}

std::string countFieldKey(QualType object,
                          llvm::ArrayRef<core::PathElem> fields,
                          const ASTContext &context) {
  std::string key = recordTypeKey(object, context);
  if (key.empty())
    return {};
  // Every step is a field of a record reached without another dereference;
  // the last one is the count itself (or none: the object stands for it).
  for (const core::PathElem &step : fields) {
    if (step.step != core::PathStep::Field)
      return {};
  }
  if (followSteps(object.getCanonicalType(), fields, context).isNull())
    return {};
  for (const core::PathElem &step : fields) {
    key += '.';
    key += step.field;
  }
  return key;
}

std::optional<std::string>
SummaryStore::countKeyOf(const FunctionDecl &function,
                         const core::SummaryPath &path) const {
  if (context == nullptr || path.steps.empty() ||
      path.steps.front().step != core::PathStep::Deref)
    return std::nullopt;
  QualType pointer;
  if (path.isParam()) {
    if (path.index >= function.getNumParams())
      return std::nullopt;
    pointer = function.getParamDecl(path.index)->getType();
  } else if (path.isGlobal()) {
    const VarDecl *global = globalTable.declFor(path.index);
    if (global == nullptr)
      return std::nullopt;
    pointer = global->getType();
  } else {
    return std::nullopt;
  }
  const QualType object =
      followSteps(pointer, llvm::ArrayRef(path.steps).take_front(1), *context);
  if (object.isNull())
    return std::nullopt;
  std::string key =
      countFieldKey(object, llvm::ArrayRef(path.steps).drop_front(), *context);
  if (key.empty())
    return std::nullopt;
  return key;
}

void SummaryStore::addKnownCount(std::string key) {
  if (!key.empty())
    knownCounts.insert(std::move(key));
}

bool SummaryStore::isKnownCount(llvm::StringRef key) const {
  if (key.empty())
    return false;
  if (knownCounts.contains(key.str()))
    return true;
  return database != nullptr && database->isKnownCount(key);
}

const std::set<std::string> &SummaryStore::knownCountKeys() const noexcept {
  return knownCounts;
}

bool SummaryStore::setInferred(const FunctionDecl &function,
                               core::FunctionSummary summary) {
  const FunctionDecl *canonical = key(function);
  merged.erase(canonical);
  mergedSource.erase(canonical);
  // RFC 0010: the count fields this function releases through are known
  // counts for every function of the unit (and, exported, of the program).
  for (const core::SummaryPath &count : summary.counts) {
    if (auto countKey = countKeyOf(function, count))
      knownCounts.insert(std::move(*countKey));
  }
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
  // Nullness annotations say nothing about ownership: alone they neither
  // make an unknown callee checked nor change where its summary comes from
  // (RFC 0008, *Annotation surface*); they layer on the table's entry.
  const bool nullness = annotations.anyNullness();

  if (!haveBody && !annotated) {
    const core::FunctionSummary *builtin = builtinSummary(callee);
    if (builtin == nullptr)
      return std::nullopt;
    if (!nullness)
      return ResolvedSummary{.summary = builtin,
                             .source = SummarySource::Builtin};
    core::FunctionSummary adjusted = *builtin;
    applyNullnessAnnotations(adjusted, shapeOf(callee), annotations.result,
                             annotations.params);
    const auto it = merged.try_emplace(canonical, std::move(adjusted)).first;
    mergedSource[canonical] = SummarySource::Builtin;
    return ResolvedSummary{.summary = &it->second,
                           .source = SummarySource::Builtin};
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
  if (nullness)
    applyNullnessAnnotations(result, shapeOf(callee), annotations.result,
                             annotations.params);
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
  // RFC 0009: the call never returns only if no candidate does. The join
  // cannot tell a candidate that does nothing from the empty summary it
  // starts from, so the bit is settled here.
  bool anyReturns = false;
  for (const FunctionDecl *candidate : candidatesFor(call)) {
    const auto resolved = lookup(*candidate);
    if (!resolved)
      continue;
    joined.join(*resolved->summary);
    anyReturns = anyReturns || !resolved->summary->neverReturns;
    anyCandidate = true;
    anyLocal = true;
  }
  // RFC 0005: address-taken functions of the type anywhere in the program.
  if (database != nullptr && context != nullptr) {
    const std::string typeKey = functionTypeKey(QualType(type, 0), *context);
    if (const core::FunctionSummary *program =
            typeKey.empty() ? nullptr : database->candidates(typeKey)) {
      joined.join(database->importInto(*program, *context, globalTable));
      anyReturns = anyReturns || !program->neverReturns;
      anyCandidate = true;
    }
  }
  if (!annotated && !anyCandidate)
    return std::nullopt;
  if (anyReturns)
    joined.neverReturns = false;

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
