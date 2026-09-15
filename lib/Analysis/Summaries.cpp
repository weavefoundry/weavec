//===- Summaries.cpp - Function summaries for Clang declarations ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Summaries.h"

#include "InterfaceTypes.h"
#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/StringExtras.h"

// Defines `LazyGenerationalUpdatePtr::makeValue`, which `Redeclarable`
// walks (`getCanonicalDecl`, `redecls()`) instantiate here.
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"

#include "llvm/ADT/STLExtras.h"

#include <string>
#include <utility>

using namespace clang;

namespace weavec::analysis {

std::vector<const FieldDecl *>
SummaryStore::recursiveLinks(const RecordDecl &record) {
  const auto importedLinks = [&] {
    std::vector<const FieldDecl *> result;
    if (!context)
      return result;
    // Imported descriptors nominate fields without changing during one
    // database generation. Preserve every observed dependency on reuse;
    // replacing or mutating the database invalidates these candidates.
    const auto generation = database ? database->importGeneration() : nullptr;
    if (recursiveLinkGeneration != generation) {
      importedRecursiveLinkCache.clear();
      recursiveLinkGeneration = generation;
    }
    if (const auto found = importedRecursiveLinkCache.find(&record);
        found != importedRecursiveLinkCache.end()) {
      inheritDependencies(found->second.dependencies);
      if (stats)
        stats->add("recursive_link_import_hits");
      return found->second.fields;
    }
    if (stats)
      stats->add("recursive_link_import_misses");
    Dependencies dependencies;
    beginDependencies(dependencies);
    for (const auto *decl : context->getTranslationUnitDecl()->decls()) {
      const auto *fn = dyn_cast<FunctionDecl>(decl);
      if (!fn || fn->getDefinition() ||
          std::ranges::none_of(fn->parameters(), [&](const auto *param) {
            return param->getType()->isPointerType() &&
                   param->getType()->getPointeeType()->getAsRecordDecl() ==
                       &record;
          }))
        continue;
      const auto imported = lookup(*fn);
      if (!imported || imported->source != SummarySource::Program)
        continue;
      for (const auto &requirement : imported->summary->checked.requirements) {
        if (requirement.kind != core::CheckedRequirementKind::Container)
          continue;
        const auto shape = core::ContainerShape::decode(requirement.family);
        if (!shape)
          continue;
        // Imported predicates only nominate local fields. The current target
        // layout and all actual memory facts are checked when it is folded.
        for (const auto *field : record.fields())
          if (field->getType()->isPointerType() &&
              field->getType()->getPointeeType()->getAsRecordDecl() ==
                  &record &&
              shape->recursiveLink(field->getNameAsString()) &&
              std::ranges::find(result, field) == result.end())
            result.push_back(field);
      }
    }
    std::ranges::sort(result, {},
                      [](const auto *field) { return field->getFieldIndex(); });
    endDependencies();
    if (importedRecursiveLinkCache.size() < 256)
      importedRecursiveLinkCache.emplace(
          &record,
          ImportedRecursiveLinks{.fields = result,
                                 .dependencies = std::move(dependencies)});
    return result;
  };
  if (const auto found = recursiveLinkCache.find(&record);
      found != recursiveLinkCache.end())
    return found->second.empty() ? importedLinks() : found->second;
  std::set<const FieldDecl *> links;
  const auto fieldOf = [&](const Expr *expr) -> const FieldDecl * {
    const auto *member =
        expr ? dyn_cast<MemberExpr>(expr->IgnoreParenImpCasts()) : nullptr;
    const auto *field =
        member ? dyn_cast<FieldDecl>(member->getMemberDecl()) : nullptr;
    if (!field || field->getParent() != &record ||
        !field->getType()->isPointerType() ||
        field->getType()->getPointeeType()->getAsRecordDecl() != &record)
      return nullptr;
    return field;
  };
  if (context)
    for (const auto *decl : context->getTranslationUnitDecl()->decls()) {
      const auto *fn = dyn_cast<FunctionDecl>(decl);
      if (!fn || !fn->doesThisDeclarationHaveABody() ||
          std::ranges::none_of(fn->parameters(), [&](const auto *param) {
            const auto type = param->getType();
            return type->isPointerType() &&
                   type->getPointeeType()->getAsRecordDecl() == &record;
          }))
        continue;
      std::set<const FieldDecl *> children;
      std::set<const FieldDecl *> cursors;
      std::vector<const Stmt *> work{fn->getBody()};
      for (std::size_t i = 0; i < work.size() && work.size() <= 65536; ++i) {
        const auto *stmt = work[i];
        if (!stmt)
          continue;
        if (const auto *call = dyn_cast<CallExpr>(stmt))
          if (const auto *callee = call->getDirectCallee();
              callee && callee->getCanonicalDecl() == fn->getCanonicalDecl())
            for (const auto *argument : call->arguments())
              if (const auto *field = fieldOf(argument))
                children.insert(field);
        if (const auto *assignment = dyn_cast<BinaryOperator>(stmt);
            assignment && assignment->getOpcode() == BO_Assign &&
            isa<DeclRefExpr>(assignment->getLHS()->IgnoreParenImpCasts()))
          if (const auto *field = fieldOf(assignment->getRHS()))
            cursors.insert(field);
        if (const auto *decls = dyn_cast<DeclStmt>(stmt))
          for (const auto *local : decls->decls())
            if (const auto *var = dyn_cast<VarDecl>(local);
                var && var->hasInit())
              if (const auto *field = fieldOf(var->getInit()))
                cursors.insert(field);
        for (const auto *child : stmt->children())
          work.push_back(child);
      }
      if (work.size() > 65536)
        continue;
      if (!children.empty()) {
        links.insert(children.begin(), children.end());
        links.insert(cursors.begin(), cursors.end());
      }
    }
  std::vector<const FieldDecl *> result(links.begin(), links.end());
  std::ranges::sort(result, {},
                    [](const auto *field) { return field->getFieldIndex(); });
  if (recursiveLinkCache.size() < 256)
    recursiveLinkCache.emplace(&record, result);
  return result.empty() ? importedLinks() : result;
}

std::optional<core::BufferShape> SummaryStore::bufferShape(
    const RecordDecl &record,
    const std::function<std::optional<core::BufferShape>()> &discover) {
  if (const auto found = bufferShapeCache.find(&record);
      found != bufferShapeCache.end())
    return found->second;
  auto result = discover();
  // Cache saturation changes cost only; discovery still runs on a miss.
  if (bufferShapeCache.size() < 256)
    bufferShapeCache.emplace(&record, result);
  return result;
}

std::map<std::string, core::ContainerCondition>
SummaryStore::containerOwnership(
    const RecordDecl &record,
    const std::function<std::map<std::string, core::ContainerCondition>()>
        &discover) {
  if (const auto found = containerOwnershipCache.find(&record);
      found != containerOwnershipCache.end())
    return found->second;
  auto result = discover();
  if (containerOwnershipCache.size() < 256)
    containerOwnershipCache.emplace(&record, result);
  return result;
}

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

// RFC 0028: source-private state uses a declaration identity and a separate
// validated representation. Neither is a source-visible declaration.
std::optional<std::string> GlobalTable::portableName(std::uint32_t id) const {
  if (const auto found = storageProxies.find(id); found != storageProxies.end())
    return found->second;
  if (const auto found = portableNames.find(id); found != portableNames.end())
    return found->second;
  const auto *var = declFor(id);
  if (!var)
    return std::nullopt;
  if (var->isExternallyVisible())
    return var->getNameAsString();
  const auto description =
      describeInterfaceType(var->getType(), var->getASTContext());
  const auto name = privateStorageName(*var);
  std::optional<std::string> result;
  if (description && !name.empty()) {
    interfaces.emplace(name, *description);
    result = name;
  }
  portableNames.emplace(id, result);
  return result;
}

std::string GlobalTable::callbackName(std::uint32_t id) const {
  if (const auto name = portableName(id))
    return *name;
  const auto *var = declFor(id);
  return var ? privateStorageName(*var) : std::string{};
}

std::optional<std::uint32_t>
GlobalTable::importName(llvm::StringRef name, const ASTContext &context,
                        const core::InterfaceTypes &descriptions) {
  // Validate even a previously interned root against the current publication.
  const bool privateRoot = name.starts_with("@weavec-state:");
  const auto metadata = descriptions.find(name.str());
  if (privateRoot && metadata != descriptions.end() && !metadata->second)
    return std::nullopt;
  auto &names = importedNames[&context];
  if (const auto found = names.find(name.str()); found != names.end()) {
    if (privateRoot && storageProxies.contains(found->second) &&
        metadata == descriptions.end())
      return std::nullopt;
    if (privateRoot && metadata != descriptions.end()) {
      const auto prior = interfaces.find(name.str());
      if (prior == interfaces.end() || prior->second != metadata->second)
        return std::nullopt;
    }
    return found->second;
  }
  const auto remember = [&](const VarDecl &var) {
    const auto id = idFor(var);
    names.emplace(name.str(), id);
    return id;
  };
  if (!privateRoot) {
    for (const auto *decl : context.getTranslationUnitDecl()->lookup(
             DeclarationName(&context.Idents.get(name))))
      if (const auto *var = dyn_cast<VarDecl>(decl);
          var && var->hasGlobalStorage() && var->isExternallyVisible())
        return remember(*var);
    return std::nullopt;
  }
  // The table already contains any referenced local static, including those
  // inside function bodies. File-scope declarations may not yet be interned.
  for (const auto *var : decls)
    if (var && !storageProxies.contains(ids.lookup(var)) &&
        privateStorageName(*var) == name) {
      const auto id = idFor(*var);
      (void)portableName(id);
      if (metadata != descriptions.end() &&
          interfaces[name.str()] != metadata->second)
        return std::nullopt;
      return remember(*var);
    }
  for (const auto *decl : context.getTranslationUnitDecl()->decls())
    if (const auto *var = dyn_cast<VarDecl>(decl);
        var && var->hasGlobalStorage() && !var->isExternallyVisible() &&
        privateStorageName(*var) == name) {
      const auto id = idFor(*var);
      if (!portableName(id) || (metadata != descriptions.end() &&
                                interfaces[name.str()] != metadata->second))
        return std::nullopt;
      return remember(*var);
    }
  if (metadata == descriptions.end() || !metadata->second)
    return std::nullopt;
  auto &arena = context.getTranslationUnitDecl()->getASTContext();
  const auto type = materializeInterfaceType(*metadata->second, arena);
  if (type.isNull())
    return std::nullopt;
  auto *proxy = VarDecl::Create(arena, context.getTranslationUnitDecl(), {}, {},
                                &context.Idents.get("__weavec_private_storage"),
                                type, nullptr, SC_Extern);
  proxy->setImplicit();
  const auto id = remember(*proxy);
  storageProxies.emplace(id, name.str());
  interfaces.emplace(name.str(), metadata->second);
  return id;
}

// -- Annotations --------------------------------------------------------------

SummarySnapshot
SummaryStore::importSummary(const core::FunctionSummary &summary) {
  auto &imports = importedSummaries[database->importGeneration()][context];
  if (const auto found = imports.find(&summary); found != imports.end()) {
    if (stats)
      stats->add("program_import_hits");
    return found->second;
  }
  if (stats)
    stats->add("program_import_misses");
  return imports
      .emplace(&summary, publishSummary(database->importInto(summary, *context,
                                                             globalTable)))
      .first->second;
}

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

bool SignatureAnnotations::anySizedBy() const noexcept {
  return llvm::any_of(
      params, [](const AnnotationSet &s) { return !s.sizedBy.empty(); });
}

std::optional<SizedBy> sizedByOf(const FunctionDecl &function, unsigned param) {
  if (param >= function.getNumParams())
    return std::nullopt;
  const SignatureAnnotations annotations = collectAnnotations(function);
  if (param >= annotations.params.size() ||
      annotations.params[param].sizedBy.empty())
    return std::nullopt;
  const ParmVarDecl &pointer = *function.getParamDecl(param);
  if (!pointer.getType()->isPointerType())
    return std::nullopt;
  const ParmVarDecl *count = nullptr;
  for (const ParmVarDecl *candidate : function.parameters()) {
    if (candidate->getName() == annotations.params[param].sizedBy) {
      count = candidate;
      break;
    }
  }
  if (count == nullptr || !count->getType()->isIntegerType())
    return std::nullopt;
  std::int64_t unit = 1;
  const QualType pointee = pointer.getType()->getPointeeType();
  if (!pointee->isIncompleteType() && !pointee->isFunctionType()) {
    const CharUnits size = function.getASTContext().getTypeSizeInChars(pointee);
    if (!size.isZero())
      unit = size.getQuantity();
  }
  return SizedBy{.count = count, .unit = unit};
}

std::optional<SizedField> sizedFieldOf(const FieldDecl &field) {
  // RFC 0012, *Sized fields*, "Annotation": a pointer field naming a
  // sibling integer field of the same record.
  const AnnotationSet annotations = getAnnotations(field);
  if (annotations.sizedBy.empty() || !field.getType()->isPointerType())
    return std::nullopt;
  const RecordDecl *record = field.getParent();
  if (record == nullptr)
    return std::nullopt;
  const FieldDecl *count = nullptr;
  for (const FieldDecl *candidate : record->fields()) {
    if (candidate->getName() == annotations.sizedBy) {
      count = candidate;
      break;
    }
  }
  if (count == nullptr || count == &field || !count->getType()->isIntegerType())
    return std::nullopt;
  std::int64_t unit = 1;
  const QualType pointee = field.getType()->getPointeeType();
  if (!pointee->isIncompleteType() && !pointee->isFunctionType()) {
    const CharUnits size = field.getASTContext().getTypeSizeInChars(pointee);
    if (!size.isZero())
      unit = size.getQuantity();
  }
  return SizedField{.count = count, .unit = unit};
}

std::string fieldKeyOf(const FieldDecl &field, const ASTContext &context) {
  const RecordDecl *record = field.getParent();
  if (record == nullptr || field.getName().empty())
    return {};
  const core::PathElem step{.step = core::PathStep::Field,
                            .field = field.getNameAsString()};
  return countFieldKey(context.getCanonicalTagType(record), {step}, context);
}

void applySizedByAnnotations(core::FunctionSummary &summary,
                             const FunctionDecl &function) {
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const auto sized = sizedByOf(function, i);
    if (!sized)
      continue;
    summary.requiresExtent[i] = {core::ExtentRequirement{
        .need = core::PathAffine::ofPath(
            core::SummaryPath::param(sized->count->getFunctionScopeIndex()),
            sized->unit),
        .when = {}}};
  }
}

core::FunctionSummary summaryFromAnnotations(const FunctionDecl &function) {
  core::FunctionSummary summary;
  const SignatureAnnotations annotations = collectAnnotations(function);
  applyAnnotations(summary, shapeOf(function), annotations.result,
                   annotations.params);
  applyNullnessAnnotations(summary, shapeOf(function), annotations.result,
                           annotations.params);
  if (annotations.anySizedBy())
    applySizedByAnnotations(summary, function);
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
    if (step.step == core::PathStep::Index && !step.field.empty())
      continue;
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
  const QualType object = followSteps(
      pointer,
      llvm::ArrayRef<core::PathElem>(path.steps.data(), path.steps.size())
          .take_front(1),
      *context);
  if (object.isNull())
    return std::nullopt;
  std::string key = countFieldKey(
      object,
      llvm::ArrayRef<core::PathElem>(path.steps.data(), path.steps.size())
          .drop_front(),
      *context);
  if (key.empty())
    return std::nullopt;
  return key;
}

void SummaryStore::addKnownCount(std::string key) {
  if (!key.empty() && knownCounts.insert(std::move(key)).second)
    invalidateDependency("@counts");
}

bool SummaryStore::isKnownCount(llvm::StringRef key) const {
  noteDependency("@counts");
  if (key.empty())
    return false;
  if (knownCounts.contains(key.str()))
    return true;
  return database != nullptr && database->isKnownCount(key);
}

// -- Sized fields (RFC 0012)
// ----------------------------------------------------

void SummaryStore::addSizedWitness(
    std::string field, std::string count, std::int64_t scale,
    std::optional<core::IntegerType> productType) {
  const bool changed =
      sizedFields.witnesses
          .insert(SizedFieldWitness{.field = std::move(field),
                                    .count = std::move(count),
                                    .scale = scale,
                                    .productType = productType})
          .second;
  if (changed && unitSizedFactsInForce)
    invalidateDependency("@sized");
}

void SummaryStore::refuteSizedField(std::string field) {
  if (sizedFields.unsizedFields.insert(std::move(field)).second &&
      unitSizedFactsInForce)
    invalidateDependency("@sized");
}

void SummaryStore::refuteSizedPair(std::string field, std::string count) {
  if (sizedFields.unsizedPairs
          .insert(
              UnsizedPair{.field = std::move(field), .count = std::move(count)})
          .second &&
      unitSizedFactsInForce)
    invalidateDependency("@sized");
}

const SizedFieldFacts &SummaryStore::sizedFieldFacts() const noexcept {
  return sizedFields;
}

void SummaryStore::noteSizedFieldLoad(std::string key) {
  sizedLoads.insert(std::move(key));
}

const std::set<std::string> &SummaryStore::sizedFieldLoads() const noexcept {
  return sizedLoads;
}

void SummaryStore::setUnitSizedFactsInForce(bool inForce) noexcept {
  if (unitSizedFactsInForce != inForce)
    invalidateDependency("@sized");
  unitSizedFactsInForce = inForce;
}

std::optional<std::pair<std::string, std::int64_t>>
SummaryStore::confirmedSizedBy(std::string_view field) const {
  const auto witness = confirmedSizedWitness(field);
  return witness ? std::optional(std::pair{witness->count, witness->scale})
                 : std::nullopt;
}
std::optional<SizedFieldWitness>
SummaryStore::confirmedSizedWitness(std::string_view field) const {
  noteDependency("@sized");
  if (field.empty())
    return std::nullopt;
  const SizedFieldFacts *program =
      database != nullptr ? &database->sizedFieldFacts() : nullptr;
  if (!unitSizedFactsInForce)
    return program != nullptr ? program->confirmedWitness(field) : std::nullopt;
  if (program == nullptr || program->empty())
    return sizedFields.confirmedWitness(field);
  SizedFieldFacts both = sizedFields;
  both.merge(*program);
  return both.confirmedWitness(field);
}

bool SummaryStore::noteInvalidSizedField(const FieldDecl &field) {
  return invalidSizedFields.insert(field.getCanonicalDecl()).second;
}

const std::set<std::string> &SummaryStore::knownCountKeys() const noexcept {
  return knownCounts;
}

bool SummaryStore::setInferred(const FunctionDecl &function,
                               core::FunctionSummary summary, bool widen,
                               bool verifiedInduction) {
  const FunctionDecl *canonical = key(function);
  merged.erase(canonical);
  mergedSource.erase(canonical);
  // RFC 0010: the count fields this function releases through are known
  // counts for every function of the unit (and, exported, of the program).
  for (const core::SummaryPath &count : summary.counts) {
    if (auto countKey = countKeyOf(function, count))
      addKnownCount(std::move(*countKey));
  }
  const auto previous = inferred.find(canonical);
  const auto globalStores = [](const core::FunctionSummary &value) {
    std::vector<core::Store> result;
    for (const auto &store : value.stores)
      if (store.dest.isGlobal())
        result.push_back(store);
    return result;
  };
  const bool globalsChanged =
      previous == inferred.end()
          ? !globalStores(summary).empty()
          : globalStores(*previous->second) != globalStores(summary);
  if (previous == inferred.end()) {
    inferred.emplace(canonical, publishSummary(std::move(summary)));
    mergedIndirect.clear();
    if (globalsChanged) {
      callbackGlobalCache.reset();
      invalidateDependency("@callback-globals");
    }
    invalidateDependency(callableSymbol(function));
    if (stats)
      stats->add("summary_changes");
    return true;
  }
  if (widen) {
    // RFC 0017: recursive approximation must retain earlier possibilities.
    // Replacement can cycle as numeric and temporal guards project together.
    auto joined = *previous->second;
    joined.join(summary);
    // RFC 0027: these outputs were verified against private proper-child
    // induction hypotheses. They do not come from the optimistic SCC seed.
    // Continue widening ordinary may-effects and retaining every obligation.
    if (verifiedInduction && summary.checked.complete()) {
      joined.checked.establishes = summary.checked.establishes;
      joined.checked.discardUnrepresentedContainerOutputs();
    }
    summary = std::move(joined);
  }
  if (*previous->second == summary)
    return false;
  previous->second = publishSummary(std::move(summary));
  if (globalsChanged) {
    callbackGlobalCache.reset();
    invalidateDependency("@callback-globals");
  }
  invalidateDependency(callableSymbol(function));
  if (stats)
    stats->add("summary_changes");
  // Any indirect join may have included this function.
  mergedIndirect.clear();
  return true;
}

const core::FunctionSummary *
SummaryStore::inferredFor(const FunctionDecl &function) const {
  const auto it = inferred.find(key(function));
  return it == inferred.end() ? nullptr : it->second.get();
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

void SummaryStore::applyContract(const FunctionDecl &function,
                                 core::FunctionSummary &summary) {
  const auto annotations = collectAnnotations(function);
  if (annotations.anyOwnership())
    applyAnnotations(summary, shapeOf(function), annotations.result,
                     annotations.params);
  applyNullnessAnnotations(summary, shapeOf(function), annotations.result,
                           annotations.params);
  applySizedByAnnotations(summary, function);
}

std::optional<ResolvedSummary>
SummaryStore::lookup(const FunctionDecl &callee) {
  noteDependency(callableSymbol(callee));
  const FunctionDecl *canonical = key(callee);
  if (const auto it = merged.find(canonical); it != merged.end())
    return ResolvedSummary{.summary = it->second,
                           .source = mergedSource.at(canonical)};

  const SignatureAnnotations annotations = collectAnnotations(callee);
  const core::FunctionSummary *inferredBody = inferredFor(callee);
  // RFC 0005: a body in another unit of the program, below this unit's own
  // inference and above the library table.
  std::optional<core::FunctionSummary> programBody =
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
  // (RFC 0008, *Annotation surface*); they layer on the table's entry. So
  // does `WEAVEC_SIZED_BY` (RFC 0011).
  const bool nullness = annotations.anyNullness();
  const bool sized = annotations.anySizedBy();

  if (inferredBody && !annotated && !nullness && !sized) {
    // An unadjusted body is already an immutable published contract.
    const auto snapshot = inferred.at(canonical);
    merged.emplace(canonical, snapshot);
    mergedSource[canonical] = SummarySource::Inferred;
    return ResolvedSummary{.summary = snapshot,
                           .source = SummarySource::Inferred};
  }

  if (!haveBody && !annotated) {
    const core::FunctionSummary *builtin = builtinSummary(callee);
    if (builtin == nullptr)
      return std::nullopt;
    if (!nullness && !sized)
      // The library table has process lifetime; this alias needs no control
      // block.
      return ResolvedSummary{.summary =
                                 SummarySnapshot{SummarySnapshot{}, builtin},
                             .source = SummarySource::Builtin};
    core::FunctionSummary adjusted = *builtin;
    applyNullnessAnnotations(adjusted, shapeOf(callee), annotations.result,
                             annotations.params);
    if (sized)
      applySizedByAnnotations(adjusted, callee);
    const auto it =
        merged.try_emplace(canonical, publishSummary(std::move(adjusted)))
            .first;
    mergedSource[canonical] = SummarySource::Builtin;
    return ResolvedSummary{.summary = it->second,
                           .source = SummarySource::Builtin};
  }

  core::FunctionSummary result;
  SummarySource source = SummarySource::Program;
  if (inferredBody != nullptr) {
    result = *inferredBody;
    source = SummarySource::Inferred;
  } else if (programBody) {
    result = std::move(*programBody);
  }
  if (annotated) {
    applyAnnotations(result, shapeOf(callee), annotations.result,
                     annotations.params);
    source = SummarySource::Annotation;
  }
  if (nullness)
    applyNullnessAnnotations(result, shapeOf(callee), annotations.result,
                             annotations.params);
  if (sized)
    applySizedByAnnotations(result, callee);
  const auto it =
      merged.try_emplace(canonical, publishSummary(std::move(result))).first;
  mergedSource[canonical] = source;
  return ResolvedSummary{.summary = it->second, .source = source};
}

void SummaryStore::addAddressTaken(const FunctionDecl &function) {
  registerCallable(function);
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
    return ResolvedSummary{.summary = it->second,
                           .source = annotated ? SummarySource::Annotation
                                               : SummarySource::Inferred};
  }

  if (!annotated)
    return std::nullopt;
  core::FunctionSummary joined;
  SummarySource source = SummarySource::Annotation;
  if (annotated) {
    applyAnnotations(joined, shapeOf(*type), annotations.result,
                     annotations.params);
    source = SummarySource::Annotation;
  }
  const auto it =
      mergedIndirect.try_emplace(cacheKey, publishSummary(std::move(joined)))
          .first;
  return ResolvedSummary{.summary = it->second, .source = source};
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
