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

/// What the name in a `WEAVEC_*` extent annotation resolves to (§7.2): a
/// sibling parameter or field, in any position.
struct Sibling {
  core::ExtentPath path;
  bool integer = false;
  bool pointer = false;
};
using SiblingResolver = std::function<std::optional<Sibling>(llvm::StringRef)>;

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
  /// `declared` names the declaration in RFC 0008's contradiction message
  /// (the function, for its result); it defaults to `name`.
  PositionFacts(KindTable &kindTable, std::string name,
                std::string declared = {})
      : table(kindTable), subject(std::move(name)),
        declaredName(declared.empty() ? subject : std::move(declared)) {}

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
    // RFC 0008's wording for the two WeaveC annotations on one declaration.
    table.addProblem(KindProblem{
        .location = at,
        .message = level == KindLevel::Annotation
                       ? "'" + declaredName +
                             "' is declared both WEAVEC_NULLABLE and "
                             "WEAVEC_NONNULL"
                       : "conflicting kinds for '" + subject +
                             "': nonnull and nullable",
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
  std::string declaredName;
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

/// The macro that spells an extent annotation, for messages.
static llvm::StringRef macroOf(Annotation annotation) {
  switch (annotation) {
  case Annotation::SizedBy:
    return "WEAVEC_SIZED_BY";
  case Annotation::CountedBy:
    return "WEAVEC_COUNTED_BY";
  case Annotation::EndedBy:
    return "WEAVEC_ENDED_BY";
  default:
    return "WEAVEC_STRING";
  }
}

/// The name an extent annotation carries (empty for `weavec.string`).
static llvm::StringRef nameOf(Annotation annotation, llvm::StringRef text) {
  switch (annotation) {
  case Annotation::SizedBy:
    return text.drop_front(spelling::SizedByPrefix.size());
  case Annotation::CountedBy:
    return text.drop_front(spelling::CountedByPrefix.size());
  case Annotation::EndedBy:
    return text.drop_front(spelling::EndedByPrefix.size());
  default:
    return {};
  }
}

namespace {
/// Where an annotation is written, for the §7.2 "applies to" column.
enum class Position : std::uint8_t { Parameter, Field, Result, Variable };
} // namespace

/// WEAVEC_* annotations on one declaration (level 1): nullability, and the
/// extent annotations (§7.2), whose names `resolve` looks up among the
/// siblings. A result takes only `WEAVEC_STRING`; a variable takes only
/// nullability.
static void readAnnotations(const clang::Decl &decl, clang::QualType type,
                            const SiblingResolver &resolve,
                            PositionFacts &facts, KindTable &table,
                            Position position) {
  const AnnotationSet annotations = getAnnotations(decl);
  const clang::SourceLocation at = decl.getLocation();
  const auto *named = llvm::dyn_cast<clang::NamedDecl>(&decl);
  const std::string subject =
      position == Position::Result
          ? "the result of '" +
                (named != nullptr ? named->getNameAsString() : std::string()) +
                "'"
          : "'" +
                (named != nullptr ? named->getNameAsString() : std::string()) +
                "'";
  const auto problem = [&](std::string message) {
    table.addProblem(
        KindProblem{.location = at, .message = std::move(message)});
  };
  // §6.3: `WEAVEC_REQUIRE_SAFE` goes before a function.
  if (annotations.requireSafe && position != Position::Result)
    problem("WEAVEC_REQUIRE_SAFE on " + subject + ", which is not a function");
  if (annotations.nonNull)
    facts.addNullability(KindLevel::Annotation, core::Nullability::Nonnull, at);
  if (annotations.nullable)
    facts.addNullability(KindLevel::Annotation, core::Nullability::Nullable,
                         at);
  if (!annotations.extent())
    return;
  // Every extent annotation written on this declaration. An inherited one
  // was read on the declaration that wrote it, where its name resolves
  // against that declaration's parameters (§7.2: by name).
  std::vector<std::pair<Annotation, std::string>> written;
  for (const auto *attr : decl.specific_attrs<clang::AnnotateAttr>()) {
    if (attr->isInherited())
      continue;
    const llvm::StringRef text = attr->getAnnotation();
    const auto parsed = parseAnnotation(text);
    if (parsed != Annotation::SizedBy && parsed != Annotation::CountedBy &&
        parsed != Annotation::EndedBy && parsed != Annotation::String)
      continue;
    std::pair entry{*parsed, nameOf(*parsed, text).str()};
    if (!llvm::is_contained(written, entry))
      written.push_back(std::move(entry));
  }
  for (const auto &[annotation, name] : written) {
    std::string spelled = macroOf(annotation).str();
    if (!name.empty())
      spelled += "(" + name + ")";
    if (!type->isPointerType()) {
      std::string message = subject;
      message += " is declared ";
      message += spelled;
      message += " but is not a pointer";
      problem(std::move(message));
      continue;
    }
    if (position == Position::Variable ||
        (position == Position::Result && annotation != Annotation::String)) {
      std::string message = subject;
      message += " is declared ";
      message += spelled;
      message += annotation == Annotation::String
                     ? " but only parameters, fields and results take it"
                     : " but only parameters and fields take it";
      problem(std::move(message));
      continue;
    }
    if (annotation == Annotation::String) {
      facts.addShape(KindLevel::Annotation, core::PointerKind::nulTerminated(),
                     at);
      continue;
    }
    const std::optional<Sibling> sibling = resolve(name);
    if (!sibling) {
      problem("'" + name + "' in " + macroOf(annotation).str() +
              " does not name a parameter or field");
      continue;
    }
    if (annotation == Annotation::EndedBy) {
      if (!sibling->pointer) {
        problem("'" + name +
                "' in WEAVEC_ENDED_BY is not a pointer parameter or field");
        continue;
      }
      facts.addShape(KindLevel::Annotation,
                     core::PointerKind::endedBy(sibling->path), at);
      continue;
    }
    if (!sibling->integer) {
      problem("'" + name + "' in " + macroOf(annotation).str() +
              " is not an integer parameter or field");
      continue;
    }
    // §7.2: the two macros are synonyms, counting elements, and bytes for
    // `void` and character pointees.
    const core::ExtentTerm extent = core::ExtentTerm::of(sibling->path);
    facts.addShape(KindLevel::Annotation,
                   countsBytes(type) ? core::PointerKind::sized(extent)
                                     : core::PointerKind::counted(extent),
                   at);
  }
}

/// The sibling parameter of `function` named `wanted`, in any position.
static std::optional<Sibling>
siblingParameter(const clang::FunctionDecl &function, llvm::StringRef wanted) {
  for (unsigned i = 0; i < function.getNumParams(); ++i) {
    const clang::ParmVarDecl *param = function.getParamDecl(i);
    if (param->getName() != wanted || wanted.empty())
      continue;
    const clang::QualType type = param->getType();
    return Sibling{.path = core::ExtentPath::ofParam(i),
                   .integer = type->isIntegerType(),
                   .pointer = type->isPointerType()};
  }
  return std::nullopt;
}

/// §7.2: `T p[N]` without `static` is no requirement; it yields a fix-it
/// suggestion that inserts `static` after the `[`.
static void suggestStatic(const clang::ParmVarDecl &param,
                          const clang::ConstantArrayType &array,
                          const clang::ASTContext &context, KindTable &table) {
  const clang::TypeSourceInfo *info = param.getTypeSourceInfo();
  if (info == nullptr || param.getName().empty())
    return;
  const auto loc = info->getTypeLoc().getAsAdjusted<clang::ArrayTypeLoc>();
  if (loc.isNull() || !loc.getLBracketLoc().isFileID())
    return;
  const std::string count = std::to_string(array.getSize().getZExtValue());
  const std::string name = param.getNameAsString();
  table.addSuggestion(KindSuggestion{
      .location = loc.getLBracketLoc().getLocWithOffset(1),
      .message = "'" + name + "' is declared with " + count +
                 " elements, which C does not require of callers; declare "
                 "it '" +
                 name + "[static " + count + "]' to require them",
      .insert = "static ",
  });
  (void)context;
}

/// `T p[static N]` and VLA parameters `T p[n]` (level 2 or 4). With
/// `suggest`, a constant bound without `static` yields a suggestion.
static void readArrayParameter(const clang::ParmVarDecl &param,
                               const clang::ASTContext &context,
                               const PathOf &pathOf, KindLevel level,
                               PositionFacts &facts, KindTable &table,
                               bool suggest) {
  const clang::ArrayType *array =
      context.getAsArrayType(param.getOriginalType());
  if (array == nullptr)
    return;
  const bool isStatic =
      array->getSizeModifier() == clang::ArraySizeModifier::Static;
  std::optional<LinearTerm> count;
  if (const auto *constant = llvm::dyn_cast<clang::ConstantArrayType>(array)) {
    // §7.2: a constant bound without `static` is no requirement, because C
    // gives it none and code commonly passes fewer elements.
    if (!isStatic) {
      if (suggest && level != KindLevel::SystemHeader)
        suggestStatic(param, *constant, context, table);
      return;
    }
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

/// §7.2: `malloc` and the `ownership_*` attributes of every declaration of
/// `function`: level 4 when all of them sit in system headers.
static OwnershipContract ownershipContract(const clang::FunctionDecl &function,
                                           const clang::SourceManager &sm) {
  OwnershipContract contract;
  bool outsideSystem = false;
  for (const clang::FunctionDecl *redecl : function.redecls()) {
    const bool system = sm.isInSystemHeader(redecl->getLocation());
    if (redecl->hasAttr<clang::RestrictAttr>() &&
        redecl->getReturnType()->isPointerType()) {
      contract.freshResult =
          contract.freshResult.value_or(std::string(core::HeapFamily));
      outsideSystem = outsideSystem || !system;
    }
    for (const auto *attr : redecl->specific_attrs<clang::OwnershipAttr>()) {
      outsideSystem = outsideSystem || !system;
      const std::string family = attr->getModule() != nullptr
                                     ? attr->getModule()->getName().str()
                                     : std::string();
      if (attr->getOwnKind() == clang::OwnershipAttr::Returns) {
        contract.freshResult = family;
        continue;
      }
      for (const clang::ParamIdx index : attr->args()) {
        if (!index.isValid())
          continue;
        OwnershipContract::Argument argument{
            .index = index.getASTIndex(),
            .family = family,
            .retains = attr->getOwnKind() == clang::OwnershipAttr::Holds};
        if (!llvm::is_contained(contract.arguments, argument))
          contract.arguments.push_back(std::move(argument));
      }
    }
  }
  contract.level =
      outsideSystem ? KindLevel::Ecosystem : KindLevel::SystemHeader;
  return contract;
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
  PositionFacts result(table, "result of '" + name + "'", name);
  // §7.2 suggestions go on the definition, or on the first declaration.
  const clang::FunctionDecl *suggestOn = canonical.getDefinition();
  if (suggestOn == nullptr)
    suggestOn = &canonical;

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
    const SiblingResolver byName =
        [redecl](llvm::StringRef wanted) -> std::optional<Sibling> {
      return siblingParameter(*redecl, wanted);
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
      readAnnotations(param, type, byName, facts, table, Position::Parameter);
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
      readArrayParameter(param, context, paramPath, level, facts, table,
                         /*suggest=*/redecl == suggestOn);
    }

    // The result: nullability, and `WEAVEC_STRING` (§7.2). The function's
    // own name is no sibling of its result.
    const AnnotationSet onFunction = getAnnotations(*redecl);
    const clang::SourceLocation at = redecl->getLocation();
    readAnnotations(
        *redecl, redecl->getReturnType(),
        [](llvm::StringRef) -> std::optional<Sibling> { return std::nullopt; },
        result, table, Position::Result);
    // §6.3: `WEAVEC_REQUIRE_SAFE` holds the function to `checked`.
    if (onFunction.requireSafe)
      table.setRequireSafe(canonical);
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
  // A `LibrarySpec` row (level 3) outranks system-header contracts.
  if (auto contract = ownershipContract(canonical, sm);
      !contract.empty() &&
      (!libraryGoverns || contract.level != KindLevel::SystemHeader))
    table.setOwnership(canonical, std::move(contract));
}

void AttributeReader::readField(const clang::FieldDecl &field,
                                KindTable &table) {
  const clang::QualType type = field.getType();
  if (!type->isPointerType() && !type->isArrayType()) {
    // Only malformed annotations to report: an extent on a non-pointer.
    if (const AnnotationSet set = getAnnotations(field);
        set.extent() || set.requireSafe) {
      PositionFacts ignored(table, field.getNameAsString());
      readAnnotations(
          field, type,
          [](llvm::StringRef) -> std::optional<Sibling> {
            return std::nullopt;
          },
          ignored, table, Position::Field);
    }
    return;
  }
  const clang::RecordDecl *record = field.getParent();
  const SiblingResolver byName =
      [record, &field](llvm::StringRef wanted) -> std::optional<Sibling> {
    if (record == nullptr || wanted.empty())
      return std::nullopt;
    for (const clang::FieldDecl *sibling : record->fields()) {
      if (sibling == &field || sibling->getName() != wanted)
        continue;
      const clang::QualType type = sibling->getType();
      return Sibling{.path = core::ExtentPath::ofField(wanted.str()),
                     .integer = type->isIntegerType(),
                     .pointer = type->isPointerType()};
    }
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
  readAnnotations(field, type, byName, facts, table, Position::Field);
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
  const AnnotationSet annotations = getAnnotations(variable);
  if (!type->isPointerType() && !annotations.extent() &&
      !annotations.requireSafe)
    return;
  PositionFacts facts(table, variable.getNameAsString());
  readAnnotations(
      variable, type,
      [](llvm::StringRef) -> std::optional<Sibling> { return std::nullopt; },
      facts, table, Position::Variable);
  if (!type->isPointerType())
    return;
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
