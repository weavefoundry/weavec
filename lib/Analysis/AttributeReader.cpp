//===- AttributeReader.cpp - Declared pointer kinds (RFC 0030) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/AttributeReader.h"

#include "weavec/Analysis/Annotations.h"

#include "clang/AST/Attr.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CheckedArithmetic.h"

#include <array>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace weavec::analysis {

namespace {

/// `k * <path> + c`, or the constant `c`.
struct LinearTerm {
  std::optional<core::ExtentPath> path;
  std::int64_t scale = 1;
  std::int64_t offset = 0;
};

using PathOf =
    std::function<std::optional<core::ExtentPath>(const clang::ValueDecl &)>;
using NameResolver =
    std::function<std::optional<core::ExtentPath>(llvm::StringRef)>;

/// What one precedence level says about one position, over every
/// redeclaration.
struct LevelFacts {
  std::optional<core::PointerKind> shape;
  std::optional<core::Nullability> nullability;
  std::optional<std::uint32_t> extentFactor;
};

/// Everything the levels say about one position (a parameter, a result, a
/// field or a variable), and its resolution by precedence (§7.2).
class PositionFacts {
public:
  PositionFacts(KindTable &kindTable, std::string name)
      : table(kindTable), subject(std::move(name)) {}

  void addShape(KindLevel level, core::PointerKind shape,
                clang::SourceLocation at,
                std::optional<std::uint32_t> factor = std::nullopt) {
    shape.nullability = core::Nullability::Nullable;
    shape.source = core::KindSource::Declared;
    LevelFacts &facts = levels.at(static_cast<std::size_t>(level));
    if (!facts.shape) {
      facts.shape = std::move(shape);
      facts.extentFactor = factor;
      return;
    }
    if (facts.shape->sameShape(shape) && facts.extentFactor == factor)
      return;
    table.addProblem(KindProblem{
        .location = at,
        .message = "conflicting kinds for '" + subject +
                   "': " + facts.shape->toString() + " and " + shape.toString(),
    });
    // §7.2: the weaker kind is used.
    facts.shape = core::join(*facts.shape, shape);
    facts.shape->source = core::KindSource::Declared;
    facts.extentFactor = std::nullopt;
  }

  void addNullability(KindLevel level, core::Nullability nullability,
                      clang::SourceLocation at) {
    LevelFacts &facts = levels.at(static_cast<std::size_t>(level));
    if (!facts.nullability) {
      facts.nullability = nullability;
      return;
    }
    if (*facts.nullability == nullability)
      return;
    table.addProblem(KindProblem{
        .location = at,
        .message =
            "conflicting kinds for '" + subject + "': nonnull and nullable",
    });
    facts.nullability = core::Nullability::Nullable;
  }

  /// The entry, or none when no level said anything. With
  /// `libraryGoverns`, a `LibrarySpec` entry (level 3) outranks the
  /// system-header attributes (level 4), which are dropped.
  [[nodiscard]] std::optional<KindEntry> resolve(bool libraryGoverns) const {
    KindEntry entry;
    entry.kind = core::PointerKind::unknown(core::Nullability::Nullable,
                                            core::KindSource::Declared);
    for (std::size_t i = 0; i < KindLevelCount; ++i) {
      const auto level = static_cast<KindLevel>(i);
      if (libraryGoverns && level == KindLevel::SystemHeader)
        break;
      const LevelFacts &facts = levels.at(i);
      if (!entry.shapeLevel && facts.shape) {
        const core::Nullability kept = entry.kind.nullability;
        entry.kind = *facts.shape;
        entry.kind.nullability = kept;
        entry.shapeLevel = level;
        entry.extentFactor = facts.extentFactor;
      }
      if (!entry.nullabilityLevel && facts.nullability) {
        entry.kind.nullability = *facts.nullability;
        entry.nullabilityLevel = level;
      }
    }
    if (!entry.shapeLevel && !entry.nullabilityLevel)
      return std::nullopt;
    entry.kind.source = core::KindSource::Declared;
    entry.extentClass = core::extentClassOf(entry.kind);
    return entry;
  }

private:
  std::array<LevelFacts, KindLevelCount> levels{};
  KindTable &table;
  std::string subject;
};

/// Every function, field and variable declaration of the unit.
class DeclarationVisitor
    : public clang::RecursiveASTVisitor<DeclarationVisitor> {
public:
  DeclarationVisitor(AttributeReader &attributeReader, KindTable &kindTable)
      : reader(attributeReader), table(kindTable) {}

  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitFunctionDecl(clang::FunctionDecl *function) {
    const clang::FunctionDecl *canonical = function->getCanonicalDecl();
    if (!seen.insert(canonical).second)
      return true;
    // A declaration nothing calls or defines has no site that could read
    // its kinds.
    if (!function->isReferenced() && !function->isDefined())
      return true;
    reader.readFunction(*function, table);
    return true;
  }

  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitFieldDecl(clang::FieldDecl *field) {
    reader.readField(*field, table);
    return true;
  }

  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitVarDecl(clang::VarDecl *variable) {
    if (!llvm::isa<clang::ParmVarDecl>(variable))
      reader.readVariable(*variable, table);
    return true;
  }

private:
  AttributeReader &reader;
  KindTable &table;
  llvm::DenseSet<const clang::FunctionDecl *> seen;
};

} // namespace

/// The linear form of an extent expression over sibling parameters or
/// fields: constants, one path, `+`, `-` a constant, `*` a constant.
static std::optional<LinearTerm> linearTermOf(const clang::Expr *expr,
                                              const clang::ASTContext &context,
                                              const PathOf &pathOf,
                                              unsigned depth = 0) {
  if (expr == nullptr || depth > 8)
    return std::nullopt;
  expr = expr->IgnoreParenImpCasts();
  if (const auto value = expr->getIntegerConstantExpr(context)) {
    if (const auto fits = value->tryExtValue())
      return LinearTerm{.path = std::nullopt, .scale = 1, .offset = *fits};
    return std::nullopt;
  }
  const clang::ValueDecl *named = nullptr;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr))
    named = ref->getDecl();
  else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr))
    named = member->getMemberDecl();
  if (named != nullptr) {
    if (auto path = pathOf(*named))
      return LinearTerm{.path = std::move(path), .scale = 1, .offset = 0};
    return std::nullopt;
  }
  const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr);
  if (binary == nullptr)
    return std::nullopt;
  const auto lhs = linearTermOf(binary->getLHS(), context, pathOf, depth + 1);
  const auto rhs = linearTermOf(binary->getRHS(), context, pathOf, depth + 1);
  if (!lhs || !rhs)
    return std::nullopt;
  switch (binary->getOpcode()) {
  case clang::BO_Add: {
    if (lhs->path && rhs->path)
      return std::nullopt;
    const auto offset = llvm::checkedAdd(lhs->offset, rhs->offset);
    if (!offset)
      return std::nullopt;
    const LinearTerm &withPath = lhs->path ? *lhs : *rhs;
    return LinearTerm{
        .path = withPath.path, .scale = withPath.scale, .offset = *offset};
  }
  case clang::BO_Sub: {
    if (rhs->path)
      return std::nullopt;
    const auto offset = llvm::checkedSub(lhs->offset, rhs->offset);
    if (!offset)
      return std::nullopt;
    return LinearTerm{
        .path = lhs->path, .scale = lhs->scale, .offset = *offset};
  }
  case clang::BO_Mul: {
    if (lhs->path && rhs->path)
      return std::nullopt;
    const LinearTerm &variable = lhs->path ? *lhs : *rhs;
    const std::int64_t factor = lhs->path ? rhs->offset : lhs->offset;
    const auto scale = llvm::checkedMul(variable.scale, factor);
    const auto offset = llvm::checkedMul(variable.offset, factor);
    if (!scale || !offset)
      return std::nullopt;
    return LinearTerm{
        .path = variable.path, .scale = *scale, .offset = *offset};
  }
  default:
    return std::nullopt;
  }
}

static core::ExtentTerm toExtentTerm(const LinearTerm &term) {
  if (!term.path)
    return core::ExtentTerm::constant(term.offset);
  return core::ExtentTerm::of(*term.path, term.scale, term.offset);
}

/// `void` and character pointees count bytes (§7.2, `WEAVEC_SIZED_BY`).
static bool countsBytes(clang::QualType pointer) {
  if (!pointer->isPointerType())
    return false;
  const clang::QualType pointee = pointer->getPointeeType();
  return pointee->isVoidType() || pointee->isCharType();
}

static KindLevel ecosystemLevel(const clang::Decl &decl,
                                const clang::SourceManager &sm) {
  return sm.isInSystemHeader(decl.getLocation()) ? KindLevel::SystemHeader
                                                 : KindLevel::Ecosystem;
}

/// `_Nonnull` / `_Nullable` on a written type.
static std::optional<core::Nullability> typeNullability(clang::QualType type) {
  const auto nullability = type->getNullability();
  if (!nullability)
    return std::nullopt;
  switch (*nullability) {
  case clang::NullabilityKind::NonNull:
    return core::Nullability::Nonnull;
  case clang::NullabilityKind::Nullable:
  case clang::NullabilityKind::NullableResult:
    return core::Nullability::Nullable;
  case clang::NullabilityKind::Unspecified:
    return std::nullopt;
  }
  return std::nullopt;
}

/// `counted_by` and friends on a written type: the shape over `pathOf`, and
/// the nullability they imply.
static std::optional<std::pair<core::PointerKind, core::Nullability>>
countAttributedKind(clang::QualType type, const clang::ASTContext &context,
                    const PathOf &pathOf) {
  const auto *counted = type->getAs<clang::CountAttributedType>();
  if (counted == nullptr)
    return std::nullopt;
  const auto term = linearTermOf(counted->getCountExpr(), context, pathOf);
  if (!term)
    return std::nullopt;
  const core::ExtentTerm extent = toExtentTerm(*term);
  core::PointerKind kind = counted->isCountInBytes()
                               ? core::PointerKind::sized(extent)
                               : core::PointerKind::counted(extent);
  return std::pair{std::move(kind), counted->isOrNull()
                                        ? core::Nullability::Nullable
                                        : core::Nullability::Nonnull};
}

/// WEAVEC_* annotations on one declaration (level 1): nullability, and a
/// `WEAVEC_SIZED_BY` name resolved by `resolve`.
static void readAnnotations(const clang::Decl &decl, clang::QualType type,
                            const NameResolver &resolve, PositionFacts &facts,
                            KindTable &table) {
  const AnnotationSet annotations = getAnnotations(decl);
  const clang::SourceLocation at = decl.getLocation();
  if (annotations.nonNull)
    facts.addNullability(KindLevel::Annotation, core::Nullability::Nonnull, at);
  if (annotations.nullable)
    facts.addNullability(KindLevel::Annotation, core::Nullability::Nullable,
                         at);
  if (annotations.sizedBy.empty() || !type->isPointerType())
    return;
  // Every `weavec.sized_by.<n>` on the declaration: Clang copies a previous
  // declaration's annotations onto a redeclared parameter, so two different
  // names can meet here, which `AnnotationSet` keeps only one of.
  std::vector<std::string> names;
  for (const auto *attr : decl.specific_attrs<clang::AnnotateAttr>()) {
    const llvm::StringRef text = attr->getAnnotation();
    if (parseAnnotation(text) != Annotation::SizedBy)
      continue;
    std::string name = text.drop_front(spelling::SizedByPrefix.size()).str();
    if (!llvm::is_contained(names, name))
      names.push_back(std::move(name));
  }
  for (const std::string &name : names) {
    const auto path = resolve(name);
    if (!path) {
      table.addProblem(KindProblem{
          .location = at,
          .message = "'" + name +
                     "' in WEAVEC_SIZED_BY does not name a parameter or field",
      });
      continue;
    }
    const core::ExtentTerm extent = core::ExtentTerm::of(*path);
    facts.addShape(KindLevel::Annotation,
                   countsBytes(type) ? core::PointerKind::sized(extent)
                                     : core::PointerKind::counted(extent),
                   at);
  }
}

/// `T p[static N]` and VLA parameters `T p[n]` (level 2 or 4).
static void readArrayParameter(const clang::ParmVarDecl &param,
                               const clang::ASTContext &context,
                               const PathOf &pathOf, KindLevel level,
                               PositionFacts &facts) {
  const clang::ArrayType *array =
      context.getAsArrayType(param.getOriginalType());
  if (array == nullptr)
    return;
  const bool isStatic =
      array->getSizeModifier() == clang::ArraySizeModifier::Static;
  std::optional<LinearTerm> count;
  if (const auto *constant = llvm::dyn_cast<clang::ConstantArrayType>(array)) {
    // §7.2: a constant bound without `static` is no requirement.
    if (!isStatic)
      return;
    if (const auto value = constant->getSize().tryZExtValue();
        value && *value <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
      count = LinearTerm{.path = std::nullopt,
                         .scale = 1,
                         .offset = static_cast<std::int64_t>(*value)};
  } else if (const auto *variable =
                 llvm::dyn_cast<clang::VariableArrayType>(array)) {
    if (array->getSizeModifier() == clang::ArraySizeModifier::Star)
      return;
    count = linearTermOf(variable->getSizeExpr(), context, pathOf);
  }
  const clang::SourceLocation at = param.getLocation();
  if (count)
    facts.addShape(level, core::PointerKind::counted(toExtentTerm(*count)), at);
  if (isStatic)
    facts.addNullability(level, core::Nullability::Nonnull, at);
}

void AttributeReader::readFunction(const clang::FunctionDecl &function,
                                   KindTable &table) {
  const clang::SourceManager &sm = context.getSourceManager();
  const clang::FunctionDecl &canonical = *function.getCanonicalDecl();
  const bool libraryGoverns =
      governingLibraryEntry(canonical, library).has_value();
  if (libraryGoverns)
    table.setGovernedByLibrary(canonical);
  const unsigned count = canonical.getNumParams();
  const std::string name = canonical.getNameAsString();

  std::vector<PositionFacts> params;
  params.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    std::string paramName = canonical.getParamDecl(i)->getNameAsString();
    for (const clang::FunctionDecl *redecl : canonical.redecls())
      if (paramName.empty() && i < redecl->getNumParams())
        paramName = redecl->getParamDecl(i)->getNameAsString();
    params.emplace_back(table, paramName.empty()
                                   ? "parameter " + std::to_string(i + 1) +
                                         " of '" + name + "'"
                                   : paramName);
  }
  PositionFacts result(table, "result of '" + name + "'");

  for (const clang::FunctionDecl *redecl : canonical.redecls()) {
    const KindLevel level = ecosystemLevel(*redecl, sm);
    const PathOf paramPath =
        [redecl](
            const clang::ValueDecl &decl) -> std::optional<core::ExtentPath> {
      const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(&decl);
      if (param == nullptr || !param->getType()->isIntegerType())
        return std::nullopt;
      for (unsigned i = 0; i < redecl->getNumParams(); ++i)
        if (redecl->getParamDecl(i) == param)
          return core::ExtentPath::ofParam(i);
      return std::nullopt;
    };
    const auto byName =
        [redecl](llvm::StringRef wanted) -> std::optional<core::ExtentPath> {
      for (unsigned i = 0; i < redecl->getNumParams(); ++i) {
        const clang::ParmVarDecl *param = redecl->getParamDecl(i);
        if (param->getName() == wanted && param->getType()->isIntegerType())
          return core::ExtentPath::ofParam(i);
      }
      return std::nullopt;
    };

    // Function-level `nonnull` (all pointer parameters, or the listed ones).
    std::vector<bool> nonnullParams(count, false);
    for (const auto *attr : redecl->specific_attrs<clang::NonNullAttr>()) {
      if (attr->args_size() == 0) {
        nonnullParams.assign(count, true);
        continue;
      }
      for (const clang::ParamIdx index : attr->args())
        if (index.isValid() && index.getASTIndex() < count)
          nonnullParams[index.getASTIndex()] = true;
    }

    const unsigned declared = std::min(count, redecl->getNumParams());
    for (unsigned i = 0; i < declared; ++i) {
      const clang::ParmVarDecl &param = *redecl->getParamDecl(i);
      PositionFacts &facts = params[i];
      const clang::QualType type = param.getType();
      readAnnotations(param, type, byName, facts, table);
      const clang::SourceLocation at = param.getLocation();
      if (type->isPointerType() &&
          (nonnullParams[i] || param.hasAttr<clang::NonNullAttr>()))
        facts.addNullability(level, core::Nullability::Nonnull, at);
      if (const auto nullability = typeNullability(type))
        facts.addNullability(level, *nullability, at);
      if (auto counted = countAttributedKind(type, context, paramPath)) {
        facts.addShape(level, std::move(counted->first), at);
        facts.addNullability(level, counted->second, at);
      }
      readArrayParameter(param, context, paramPath, level, facts);
    }

    // The result.
    const AnnotationSet onFunction = getAnnotations(*redecl);
    const clang::SourceLocation at = redecl->getLocation();
    if (onFunction.nonNull)
      result.addNullability(KindLevel::Annotation, core::Nullability::Nonnull,
                            at);
    if (onFunction.nullable)
      result.addNullability(KindLevel::Annotation, core::Nullability::Nullable,
                            at);
    if (redecl->getReturnType()->isPointerType()) {
      if (redecl->hasAttr<clang::ReturnsNonNullAttr>())
        result.addNullability(level, core::Nullability::Nonnull, at);
      if (const auto nullability = typeNullability(redecl->getReturnType()))
        result.addNullability(level, *nullability, at);
      if (const auto *allocSize = redecl->getAttr<clang::AllocSizeAttr>()) {
        const clang::ParamIdx size = allocSize->getElemSizeParam();
        const clang::ParamIdx elements = allocSize->getNumElemsParam();
        if (size.isValid() && size.getASTIndex() < count) {
          std::optional<std::uint32_t> factor;
          if (elements.isValid() && elements.getASTIndex() < count)
            factor = elements.getASTIndex();
          result.addShape(level,
                          core::PointerKind::sized(core::ExtentTerm::of(
                              core::ExtentPath::ofParam(size.getASTIndex()))),
                          at, factor);
        }
      }
    }
  }

  for (unsigned i = 0; i < count; ++i)
    if (auto entry = params[i].resolve(libraryGoverns))
      table.setParam(canonical, i, std::move(*entry));
  if (auto entry = result.resolve(libraryGoverns))
    table.setResult(canonical, std::move(*entry));
}

void AttributeReader::readField(const clang::FieldDecl &field,
                                KindTable &table) {
  const clang::QualType type = field.getType();
  if (!type->isPointerType() && !type->isArrayType())
    return;
  const clang::RecordDecl *record = field.getParent();
  const auto byName =
      [record,
       &field](llvm::StringRef wanted) -> std::optional<core::ExtentPath> {
    if (record == nullptr)
      return std::nullopt;
    for (const clang::FieldDecl *sibling : record->fields())
      if (sibling != &field && sibling->getName() == wanted &&
          sibling->getType()->isIntegerType())
        return core::ExtentPath::ofField(wanted.str());
    return std::nullopt;
  };
  const PathOf fieldPath =
      [&field](
          const clang::ValueDecl &decl) -> std::optional<core::ExtentPath> {
    const auto *sibling = llvm::dyn_cast<clang::FieldDecl>(&decl);
    if (sibling == nullptr || sibling == &field ||
        !sibling->getType()->isIntegerType())
      return std::nullopt;
    return core::ExtentPath::ofField(sibling->getNameAsString());
  };
  PositionFacts facts(table, field.getNameAsString());
  readAnnotations(field, type, byName, facts, table);
  const KindLevel level = ecosystemLevel(field, context.getSourceManager());
  const clang::SourceLocation at = field.getLocation();
  if (auto counted = countAttributedKind(type, context, fieldPath)) {
    facts.addShape(level, std::move(counted->first), at);
    // A counted flexible array member has no nullability.
    if (type->isPointerType())
      facts.addNullability(level, counted->second, at);
  }
  if (type->isPointerType())
    if (const auto nullability = typeNullability(type))
      facts.addNullability(level, *nullability, at);
  if (auto entry = facts.resolve(/*libraryGoverns=*/false))
    table.setField(field, std::move(*entry));
}

void AttributeReader::readVariable(const clang::VarDecl &variable,
                                   KindTable &table) {
  const clang::QualType type = variable.getType();
  if (!type->isPointerType())
    return;
  PositionFacts facts(table, variable.getNameAsString());
  readAnnotations(
      variable, type,
      [](llvm::StringRef) -> std::optional<core::ExtentPath> {
        return std::nullopt;
      },
      facts, table);
  if (const auto nullability = typeNullability(type))
    facts.addNullability(ecosystemLevel(variable, context.getSourceManager()),
                         *nullability, variable.getLocation());
  if (auto entry = facts.resolve(/*libraryGoverns=*/false))
    table.setVariable(variable, std::move(*entry));
}

KindTable AttributeReader::read() {
  KindTable table;
  DeclarationVisitor visitor(*this, table);
  visitor.TraverseDecl(context.getTranslationUnitDecl());
  return table;
}

} // namespace weavec::analysis
