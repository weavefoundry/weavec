//===- SafeCheck.cpp - Safe C's rules and its bounds table ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0041 §3, §8. Every rule is decided inside one function from the
// declarations it can see. A rule rejects a construct; each error names the
// rule and comes with a note saying what to write instead, so that an agent
// can act on it. What the rules leave to run time (an index against a
// count) the guard passes check (GuardSafe.cpp).
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/SafeCheck.h"

#include "weavec/Core/LibrarySpec.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"

#include <optional>
#include <set>
#include <string>
#include <utility>

namespace weavec::frontend {

namespace {

/// The rules' identifiers (§3), printed with every error.
namespace rule {
constexpr llvm::StringLiteral IndexSingle = "index-single";
constexpr llvm::StringLiteral IntToPointer = "int-to-pointer";
constexpr llvm::StringLiteral PointerCast = "pointer-cast";
constexpr llvm::StringLiteral PointerBytes = "pointer-bytes";
constexpr llvm::StringLiteral UnionPointer = "union-pointer";
constexpr llvm::StringLiteral Escape = "escape";
constexpr llvm::StringLiteral CallArgs = "call-args";
constexpr llvm::StringLiteral Variadic = "variadic";
constexpr llvm::StringLiteral NoAsm = "no-asm";
constexpr llvm::StringLiteral UnsafeBlock = "unsafe-block";
constexpr llvm::StringLiteral BadAnnotation = "bad-annotation";
constexpr llvm::StringLiteral Cxx = "cxx";
// RFC 0042 §3: typed units only.
constexpr llvm::StringLiteral CountedPair = "counted-pair";
constexpr llvm::StringLiteral StringIndex = "string-index";
constexpr llvm::StringLiteral StringWrite = "string-write";
constexpr llvm::StringLiteral FieldAddress = "field-address";
} // namespace rule

/// Origins of a value: the borrowed parameters it may derive from, the
/// `WEAVEC_BORROWED` fields it was loaded from, and the local aggregates
/// holding borrowed pointers it is or points into.
using Origins = llvm::SmallPtrSet<const clang::NamedDecl *, 2>;

/// One annotation of a pointer type.
struct TypeAnnotation {
  llvm::StringRef name;
  std::string argument;
};

} // namespace

/// The annotations on `type`'s own pointer level, through its sugar
/// (typedefs, attributes), and Clang's `counted_by` family on a field.
static std::vector<TypeAnnotation> annotationsOf(clang::QualType type,
                                                 const clang::ASTContext &ast) {
  std::vector<TypeAnnotation> found;
  clang::QualType current = type;
  for (unsigned depth = 0; depth < 16; ++depth) {
    if (const auto *attributed =
            llvm::dyn_cast<clang::AttributedType>(current.getTypePtr())) {
      if (const auto *annotate =
              llvm::dyn_cast_or_null<clang::AnnotateTypeAttr>(
                  attributed->getAttr());
          annotate != nullptr &&
          annotate->getAnnotation().starts_with("weavec.")) {
        TypeAnnotation annotation{.name = annotate->getAnnotation(),
                                  .argument = {}};
        if (annotate->args_size() != 0)
          if (const auto *text = llvm::dyn_cast<clang::StringLiteral>(
                  (*annotate->args_begin())->IgnoreParenCasts()))
            annotation.argument = text->getString().str();
        found.push_back(std::move(annotation));
      }
    }
    if (const auto *counted =
            llvm::dyn_cast<clang::CountAttributedType>(current.getTypePtr())) {
      std::string field;
      if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(
              counted->getCountExpr()->IgnoreParenImpCasts()))
        field = reference->getDecl()->getName().str();
      llvm::StringRef name = CountedByAnnotation;
      if (counted->isCountInBytes())
        name =
            counted->isOrNull() ? SizedByOrNullAnnotation : SizedByAnnotation;
      else if (counted->isOrNull())
        name = CountedByOrNullAnnotation;
      found.push_back({.name = name, .argument = field});
    }
    const clang::QualType next = current.getSingleStepDesugaredType(ast);
    if (next == current)
      break;
    current = next;
  }
  return found;
}

static bool hasAnnotation(llvm::ArrayRef<TypeAnnotation> annotations,
                          llvm::StringRef name) {
  return llvm::any_of(annotations, [&](const TypeAnnotation &annotation) {
    return annotation.name == name;
  });
}

/// Whether the annotations give the pointer bounds of more than one object.
static bool boundedBy(llvm::ArrayRef<TypeAnnotation> annotations) {
  return llvm::any_of(annotations, [](const TypeAnnotation &annotation) {
    return annotation.name != EscapesAnnotation &&
           annotation.name != BorrowedAnnotation;
  });
}

static bool hasAnnotateAttr(const clang::Decl &declaration,
                            llvm::StringRef name) {
  return llvm::any_of(declaration.specific_attrs<clang::AnnotateAttr>(),
                      [&](const clang::AnnotateAttr *attribute) {
                        return attribute->getAnnotation() == name;
                      });
}

/// Whether memory of `type` holds a pointer (a function pointer too).
static bool holdsPointers(clang::QualType type,
                          llvm::SmallPtrSetImpl<const clang::Type *> &seen) {
  const clang::Type *canonical = type.getCanonicalType().getTypePtr();
  if (!seen.insert(canonical).second)
    return false;
  if (canonical->isAnyPointerType() || canonical->isBlockPointerType() ||
      canonical->isMemberPointerType())
    return true;
  if (const auto *array = llvm::dyn_cast<clang::ArrayType>(canonical))
    return holdsPointers(array->getElementType(), seen);
  if (const auto *atomic = llvm::dyn_cast<clang::AtomicType>(canonical))
    return holdsPointers(atomic->getValueType(), seen);
  if (const auto *record = canonical->getAsRecordDecl()) {
    const clang::RecordDecl *definition = record->getDefinition();
    if (definition == nullptr)
      return false;
    return llvm::any_of(definition->fields(), [&](const clang::FieldDecl *f) {
      return holdsPointers(f->getType(), seen);
    });
  }
  return false;
}

static bool holdsPointers(clang::QualType type) {
  llvm::SmallPtrSet<const clang::Type *, 8> seen;
  return holdsPointers(type, seen);
}

/// Whether a field of `type` (or of a struct or array in it) is
/// `WEAVEC_BORROWED`.
static bool holdsBorrowed(clang::QualType type, const clang::ASTContext &ast,
                          unsigned depth = 0) {
  const clang::Type *canonical = type.getCanonicalType().getTypePtr();
  if (depth > 8)
    return false;
  if (const auto *array = llvm::dyn_cast<clang::ArrayType>(canonical))
    return holdsBorrowed(array->getElementType(), ast, depth + 1);
  const clang::RecordDecl *record = canonical->getAsRecordDecl();
  if (record == nullptr || record->getDefinition() == nullptr)
    return false;
  return llvm::any_of(
      record->getDefinition()->fields(), [&](const clang::FieldDecl *field) {
        return hasAnnotation(annotationsOf(field->getType(), ast),
                             BorrowedAnnotation) ||
               holdsBorrowed(field->getType(), ast, depth + 1);
      });
}

/// Whether `a` and `b` are one type but for qualifiers, at every level of
/// pointer: copying a `const char *` into a `char *` forges nothing.
static bool sameShape(clang::QualType a, clang::QualType b) {
  a = a.getCanonicalType().getUnqualifiedType();
  b = b.getCanonicalType().getUnqualifiedType();
  if (a->isPointerType() && b->isPointerType())
    return sameShape(a->getPointeeType(), b->getPointeeType());
  if (const auto *x = llvm::dyn_cast<clang::ConstantArrayType>(a.getTypePtr()))
    if (const auto *y =
            llvm::dyn_cast<clang::ConstantArrayType>(b.getTypePtr()))
      return x->getSize() == y->getSize() &&
             sameShape(x->getElementType(), y->getElementType());
  return a == b;
}

/// `char`, `signed char`, `unsigned char` or `void`.
static bool isByteType(clang::QualType type) {
  const clang::QualType canonical = type.getCanonicalType();
  return canonical->isVoidType() || canonical->isCharType();
}

/// RFC 0042 §3: a struct initializer whose count is a constant gives its
/// counted pointer at least that many elements (Clang may copy a constant
/// initializer as a whole, not field by field).
template <typename Report>
static void checkCountedInit(clang::ASTContext &ast,
                             const clang::InitListExpr &list,
                             const clang::RecordDecl &record, Report &report) {
  llvm::SmallVector<const clang::FieldDecl *, 8> fields(record.fields());
  for (unsigned i = 0; i < fields.size() && i < list.getNumInits(); ++i) {
    const clang::FieldDecl *field = fields[i];
    if (!field->getType()->isPointerType() || record.isUnion())
      continue;
    for (const TypeAnnotation &annotation :
         annotationsOf(field->getType(), ast)) {
      const bool sized = annotation.name == SizedByAnnotation ||
                         annotation.name == SizedByOrNullAnnotation;
      if (!sized && annotation.name != CountedByAnnotation &&
          annotation.name != CountedByOrNullAnnotation)
        continue;
      std::optional<std::uint64_t> count;
      unsigned long long literal = 0;
      if (!llvm::StringRef(annotation.argument).getAsInteger(0, literal))
        count = literal;
      for (unsigned j = 0; j < fields.size(); ++j)
        if (fields[j]->getName() == annotation.argument) {
          clang::Expr::EvalResult value;
          if (j < list.getNumInits() &&
              list.getInit(j)->EvaluateAsInt(value, ast))
            count = value.Val.getInt().getExtValue();
          else if (j >= list.getNumInits())
            count = 0;
        }
      const clang::Expr *init = list.getInit(i);
      clang::Expr::EvalResult pointer;
      if (!count.has_value() ||
          init->isNullPointerConstant(
              ast, clang::Expr::NPC_ValueDependentIsNotNull) ||
          !init->EvaluateAsRValue(pointer, ast) || !pointer.Val.isLValue() ||
          pointer.Val.isNullPointer())
        continue;
      const clang::APValue::LValueBase base = pointer.Val.getLValueBase();
      clang::QualType object;
      if (const auto *declaration = base.dyn_cast<const clang::ValueDecl *>())
        object = declaration->getType();
      else if (const auto *expression = base.dyn_cast<const clang::Expr *>())
        object = expression->getType();
      if (object.isNull() || object->isIncompleteType() ||
          object->isFunctionType())
        continue;
      const std::int64_t size = ast.getTypeSizeInChars(object).getQuantity();
      const std::int64_t offset = pointer.Val.getLValueOffset().getQuantity();
      const clang::QualType pointee = field->getType()->getPointeeType();
      const std::uint64_t element =
          sized || pointee->isIncompleteType()
              ? 1
              : static_cast<std::uint64_t>(
                    ast.getTypeSizeInChars(pointee).getQuantity());
      if (offset >= 0 && offset <= size &&
          *count * element <= static_cast<std::uint64_t>(size - offset))
        continue;
      report(init->getExprLoc(), rule::CountedPair,
             "'" + field->getName().str() +
                 "' is initialized with fewer "
                 "bytes than '" +
                 annotation.argument + "' gives it",
             "initialize '" + annotation.argument +
                 "' with the number of elements '" + field->getName().str() +
                 "' points to");
    }
  }
}

namespace {

/// The parameters' and result's bounds a declaration gives (§2).
class Declarations {
public:
  Declarations(clang::ASTContext &ast, clang::DiagnosticsEngine &diagnostics,
               const clang::Preprocessor *preprocessor,
               std::function<void(clang::SourceLocation, llvm::StringRef,
                                  llvm::StringRef, llvm::StringRef)>
                   report,
               bool typed)
      : ast(ast), diagnostics(diagnostics), preprocessor(preprocessor),
        report(std::move(report)), typed(typed) {}

  PointerBounds boundsOf(const clang::FunctionDecl &function,
                         clang::QualType type, clang::SourceLocation at) {
    PointerBounds bounds;
    if (!type->isPointerType())
      return bounds;
    bounds.kind = PointerBounds::Kind::Single;
    const clang::QualType pointee = type->getPointeeType();
    // RFC 0042 §5: a typed unit's table gives a single pointer's size.
    if (typed)
      bounds.pointeeSize = sizeOf(pointee);
    for (const TypeAnnotation &annotation : annotationsOf(type, ast)) {
      const llvm::StringRef name = annotation.name;
      if (name == EscapesAnnotation) {
        bounds.escapes = true;
        continue;
      }
      if (name == NullTerminatedAnnotation ||
          name == NullTerminatedOrNullAnnotation) {
        bounds.kind = PointerBounds::Kind::NullTerminated;
        bounds.orNull = name == NullTerminatedOrNullAnnotation;
        continue;
      }
      const bool sized =
          name == SizedByAnnotation || name == SizedByOrNullAnnotation;
      bounds.orNull =
          name == CountedByOrNullAnnotation || name == SizedByOrNullAnnotation;
      const int parameter = parameterNamed(function, annotation.argument);
      if (name == EndedByAnnotation) {
        if (parameter < 0 ||
            !function.getParamDecl(static_cast<unsigned>(parameter))
                 ->getType()
                 ->isPointerType()) {
          report(at, rule::BadAnnotation,
                 "'" + annotation.argument +
                     "' in WEAVEC_ENDED_BY names no pointer parameter of '" +
                     function.getName().str() + "'",
                 "name another pointer parameter of the function");
          continue;
        }
        bounds.kind = PointerBounds::Kind::Ended;
        bounds.end = parameter;
        continue;
      }
      bounds.kind = PointerBounds::Kind::Counted;
      if (!sized) {
        if (pointee->isIncompleteType() || pointee->isFunctionType()) {
          report(at, rule::BadAnnotation,
                 "WEAVEC_COUNTED_BY needs a pointer to a complete type",
                 "use WEAVEC_SIZED_BY(n) to give the bound in bytes");
          continue;
        }
        bounds.elementSize = static_cast<std::uint64_t>(
            ast.getTypeSizeInChars(pointee).getQuantity());
      }
      unsigned long long constant = 0;
      if (!llvm::StringRef(annotation.argument).getAsInteger(0, constant)) {
        bounds.constant = constant;
        continue;
      }
      if (parameter < 0) {
        if (ambiguousMacro(annotation.argument)) {
          reportAmbiguous(at, annotation.argument);
          bounds.kind = PointerBounds::Kind::Single;
          continue;
        }
        if (const auto value = macroConstant(annotation.argument)) {
          bounds.constant = *value;
          continue;
        }
      }
      if (parameter < 0 ||
          !function.getParamDecl(static_cast<unsigned>(parameter))
               ->getType()
               ->isIntegerType()) {
        report(at, rule::BadAnnotation,
               "'" + annotation.argument +
                   "' in the bound names no integer parameter of '" +
                   function.getName().str() + "'",
               "name an integer parameter of the function, an integer "
               "literal, or a macro that expands to one (an expression is "
               "not read)");
        bounds.kind = PointerBounds::Kind::Single;
        continue;
      }
      bounds.count = parameter;
      bounds.countSigned =
          function.getParamDecl(static_cast<unsigned>(parameter))
              ->getType()
              ->isSignedIntegerType();
    }
    return bounds;
  }

  /// A bound that names an object-like macro whose expansion is an integer
  /// literal, perhaps in parentheses (`#define SIZE 21`).
  [[nodiscard]] std::optional<std::uint64_t>
  macroConstant(llvm::StringRef name) const {
    if (ambiguousMacro(name))
      return std::nullopt;
    const clang::IdentifierInfo *identifier = identifierOf(name);
    const clang::MacroInfo *macro =
        identifier == nullptr ? nullptr
                              : preprocessor->getMacroInfo(identifier);
    return macro == nullptr ? std::nullopt : literalOf(*macro);
  }

  /// Whether `name` is a macro the unit undefines, or defines to more than
  /// one value: the rules run at the end of the unit, where the value may
  /// not be the one the bound was written with.
  [[nodiscard]] bool ambiguousMacro(llvm::StringRef name) const {
    const clang::IdentifierInfo *identifier = identifierOf(name);
    if (identifier == nullptr)
      return false;
    std::optional<std::optional<std::uint64_t>> first;
    for (const clang::MacroDirective *directive =
             preprocessor->getLocalMacroDirectiveHistory(identifier);
         directive != nullptr; directive = directive->getPrevious()) {
      if (directive->getKind() == clang::MacroDirective::MD_Undefine)
        return true;
      const auto *definition =
          llvm::dyn_cast<clang::DefMacroDirective>(directive);
      if (definition == nullptr)
        continue;
      const auto value = literalOf(*definition->getInfo());
      if (first.has_value() && *first != value)
        return true;
      first = value;
    }
    return false;
  }

  void reportAmbiguous(clang::SourceLocation at, llvm::StringRef name) {
    report(at, rule::BadAnnotation,
           "the macro '" + name.str() +
               "' in the bound is undefined or redefined in this unit, so "
               "its value where the bound is written is not known",
           "give the bound as an integer literal, or as a macro defined once");
  }

  /// A struct's bounds name its own fields, a literal or a macro that
  /// expands to one (their counts are not used at run time, amendment 1,
  /// but a bound that names nothing is a mistake).
  void checkRecord(const clang::RecordDecl &record) {
    for (const clang::FieldDecl *field : record.fields())
      for (const TypeAnnotation &annotation :
           annotationsOf(field->getType(), ast)) {
        const llvm::StringRef name = annotation.name;
        if (name == EscapesAnnotation || name == BorrowedAnnotation ||
            name == NullTerminatedAnnotation ||
            name == NullTerminatedOrNullAnnotation)
          continue;
        unsigned long long literal = 0;
        if (!llvm::StringRef(annotation.argument).getAsInteger(0, literal))
          continue;
        const bool named =
            namesField(record, annotation.argument, name == EndedByAnnotation);
        if (!named && ambiguousMacro(annotation.argument)) {
          reportAmbiguous(field->getLocation(), annotation.argument);
          continue;
        }
        if (!named && !macroConstant(annotation.argument).has_value())
          report(field->getLocation(), rule::BadAnnotation,
                 "'" + annotation.argument + "' in the bound of field '" +
                     field->getName().str() + "' names no field of the struct",
                 "name an integer field of the same struct (a pointer "
                 "field for WEAVEC_ENDED_BY), an integer literal, or a "
                 "macro that expands to one");
      }
  }

  FunctionBounds tableOf(const clang::FunctionDecl &function) {
    FunctionBounds table;
    for (const clang::ParmVarDecl *parameter : function.parameters()) {
      PointerBounds bounds =
          boundsOf(function, parameter->getType(), parameter->getLocation());
      bounds.lifetimeBound = parameter->hasAttr<clang::LifetimeBoundAttr>();
      table.parameters.push_back(bounds);
    }
    table.result =
        boundsOf(function, function.getReturnType(), function.getLocation());
    return table;
  }

  /// The bytes of a complete object type; 0 for `void`, an incomplete or a
  /// variably modified type, or a function.
  [[nodiscard]] std::uint64_t sizeOf(clang::QualType type) const {
    if (type->isIncompleteType() || type->isFunctionType() ||
        type->isVoidType() || type->isVariablyModifiedType() ||
        type->isDependentType())
      return 0;
    return static_cast<std::uint64_t>(
        ast.getTypeSizeInChars(type).getQuantity());
  }

  /// RFC 0042 §5: gives each pointer field of `record` (a typed unit's) the
  /// implicit `weavec.field` annotation, and each field that counts one a
  /// `weavec.count`. A bound that names something else than a sibling
  /// integer field or a constant, and `WEAVEC_ENDED_BY`, give none: the
  /// field's loads keep their guards.
  void annotateFields(clang::RecordDecl &record) {
    if (record.isInvalidDecl() || record.isDependentType() ||
        record.getDefinition() != &record)
      return;
    const clang::ASTRecordLayout &layout = ast.getASTRecordLayout(&record);
    auto offsetOf = [&](const clang::FieldDecl &field) {
      return static_cast<std::int64_t>(
          ast
              .toCharUnitsFromBits(static_cast<std::int64_t>(
                  layout.getFieldOffset(field.getFieldIndex())))
              .getQuantity());
    };
    for (clang::FieldDecl *field : record.fields()) {
      if (field->isBitField() || field->isInvalidDecl())
        continue;
      clang::QualType type = field->getType();
      FieldBounds bounds;
      if (const auto *array = ast.getAsConstantArrayType(type)) {
        type = array->getElementType();
        bounds.flags |= FieldBounds::Array;
      }
      if (!type->isPointerType())
        continue;
      bounds.elementSize =
          static_cast<std::int64_t>(sizeOf(type->getPointeeType()));
      clang::FieldDecl *count = nullptr;
      bool known = true;
      for (const TypeAnnotation &annotation : annotationsOf(type, ast)) {
        const llvm::StringRef name = annotation.name;
        if (name == EscapesAnnotation || name == BorrowedAnnotation)
          continue;
        if (name == NullTerminatedAnnotation ||
            name == NullTerminatedOrNullAnnotation) {
          bounds.kind = FieldBounds::Kind::NullTerminated;
          if (name == NullTerminatedOrNullAnnotation)
            bounds.flags |= FieldBounds::OrNull;
          continue;
        }
        if (name == EndedByAnnotation ||
            (bounds.flags & FieldBounds::Array) != 0) {
          known = false;
          continue;
        }
        bounds.kind = FieldBounds::Kind::Counted;
        if (name == SizedByAnnotation || name == SizedByOrNullAnnotation)
          bounds.elementSize = 1;
        if (name == CountedByOrNullAnnotation ||
            name == SizedByOrNullAnnotation)
          bounds.flags |= FieldBounds::OrNull;
        unsigned long long literal = 0;
        if (!llvm::StringRef(annotation.argument).getAsInteger(0, literal)) {
          bounds.countOffset = static_cast<std::int64_t>(literal);
          continue;
        }
        if (const auto value = macroConstant(annotation.argument)) {
          bounds.countOffset = static_cast<std::int64_t>(*value);
          continue;
        }
        count = nullptr;
        for (clang::FieldDecl *other : record.fields())
          if (other->getName() == annotation.argument && !other->isBitField() &&
              other->getType()->isIntegerType())
            count = other;
        if (count == nullptr) {
          known = false;
          continue;
        }
        bounds.countOffset = offsetOf(*count) - offsetOf(*field);
        bounds.countWidth = static_cast<std::int64_t>(sizeOf(count->getType()));
        if (count->getType()->isSignedIntegerType())
          bounds.flags |= FieldBounds::CountSigned;
      }
      if (!known || (bounds.kind == FieldBounds::Kind::Counted &&
                     bounds.elementSize == 0))
        continue;
      // A pointee completed later: its size is the end of the unit's.
      if (bounds.kind == FieldBounds::Kind::Single && bounds.elementSize == 0 &&
          type->getPointeeType()->isIncompleteType() &&
          type->getPointeeType()->isRecordType()) {
        laterPointees.push_back(type->getPointeeType());
        bounds.pointee = static_cast<std::int64_t>(laterPointees.size());
      }
      addAnnotation(*field, FieldAnnotation,
                    {static_cast<std::int64_t>(bounds.kind), bounds.elementSize,
                     bounds.countOffset, bounds.countWidth,
                     static_cast<std::int64_t>(bounds.flags), bounds.pointee});
      if (count != nullptr)
        addAnnotation(*count, CountAnnotation,
                      {offsetOf(*field) - offsetOf(*count), bounds.elementSize,
                       static_cast<std::int64_t>(bounds.flags)});
    }
  }

  /// The sizes of the pointees `annotateFields` left to the end of the
  /// unit.
  [[nodiscard]] std::vector<std::uint64_t> laterSizes() const {
    std::vector<std::uint64_t> sizes;
    sizes.reserve(laterPointees.size());
    for (const clang::QualType type : laterPointees)
      sizes.push_back(sizeOf(type));
    return sizes;
  }

private:
  std::vector<clang::QualType> laterPointees;
  clang::ASTContext &ast;
  [[maybe_unused]] clang::DiagnosticsEngine &diagnostics;
  const clang::Preprocessor *preprocessor;
  std::function<void(clang::SourceLocation, llvm::StringRef, llvm::StringRef,
                     llvm::StringRef)>
      report;
  bool typed;

  /// An implicit `annotate(name, values...)`, its arguments constants of
  /// type `long long`, as Sema would have made them.
  void addAnnotation(clang::Decl &declaration, llvm::StringRef name,
                     llvm::ArrayRef<std::int64_t> values) {
    llvm::SmallVector<clang::Expr *, 5> arguments;
    for (const std::int64_t value : values) {
      const llvm::APSInt integer(
          llvm::APInt(64, static_cast<std::uint64_t>(value), /*isSigned=*/true),
          /*isUnsigned=*/false);
      auto *literal =
          clang::IntegerLiteral::Create(ast, integer, ast.LongLongTy, {});
      arguments.push_back(
          clang::ConstantExpr::Create(ast, literal, clang::APValue(integer)));
    }
    declaration.addAttr(clang::AnnotateAttr::CreateImplicit(
        ast, name, arguments.data(), static_cast<unsigned>(arguments.size())));
  }

  [[nodiscard]] const clang::IdentifierInfo *
  identifierOf(llvm::StringRef name) const {
    if (preprocessor == nullptr || name.empty())
      return nullptr;
    return preprocessor->getIdentifierInfo(name);
  }

  /// Exactly one integer literal, perhaps in parentheses: an expression
  /// (`(2*30+1)`) is not read, so a bound is never a part of one.
  static std::optional<std::uint64_t> literalOf(const clang::MacroInfo &macro) {
    if (macro.isFunctionLike())
      return std::nullopt;
    std::optional<std::uint64_t> found;
    for (const clang::Token &token : macro.tokens()) {
      if (token.isOneOf(clang::tok::l_paren, clang::tok::r_paren))
        continue;
      if (!token.is(clang::tok::numeric_constant) || found.has_value())
        return std::nullopt;
      llvm::StringRef spelled(token.getLiteralData(), token.getLength());
      spelled = spelled.rtrim("uUlL");
      unsigned long long value = 0;
      if (spelled.getAsInteger(0, value))
        return std::nullopt;
      found = value;
    }
    return found;
  }

  /// Whether `record`, or an anonymous struct or union in it, has a field
  /// `name` of the kind a bound needs (a pointer for WEAVEC_ENDED_BY).
  static bool namesField(const clang::RecordDecl &record, llvm::StringRef name,
                         bool pointer) {
    return llvm::any_of(record.fields(), [&](const clang::FieldDecl *field) {
      if (field->isAnonymousStructOrUnion())
        if (const clang::RecordDecl *inner =
                field->getType()->getAsRecordDecl())
          return namesField(*inner, name, pointer);
      return field->getName() == name &&
             (pointer ? field->getType()->isPointerType()
                      : field->getType()->isIntegerType());
    });
  }

  static int parameterNamed(const clang::FunctionDecl &function,
                            llvm::StringRef name) {
    for (unsigned i = 0; i < function.getNumParams(); ++i)
      if (function.getParamDecl(i)->getName() == name)
        return static_cast<int>(i);
    return -1;
  }
};

/// The rules for one safe function's body.
class FunctionCheck final : public clang::RecursiveASTVisitor<FunctionCheck> {
public:
  using Report = std::function<void(clang::SourceLocation, llvm::StringRef,
                                    llvm::StringRef, llvm::StringRef)>;

  FunctionCheck(clang::ASTContext &ast, const clang::FunctionDecl &function,
                const SafeUnit &unit, Report report)
      : ast(ast), function(function), unit(unit), report(std::move(report)) {}

  void run() {
    inferLocals();
    TraverseStmt(function.getBody());
  }

  // RecursiveASTVisitor's names.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *subscript) {
    const clang::Expr *base = subscript->getBase();
    if (base->getType()->isPointerType() && !isZero(subscript->getIdx()) &&
        !bounded(base))
      single(subscript->getExprLoc(), base);
    if (unit.typed && base->getType()->isPointerType() &&
        nullTerminated(base) && !forwardConstant(subscript->getIdx(), false))
      stringIndex(subscript->getExprLoc(), base);
    return true;
  }

  bool VisitUnaryOperator(clang::UnaryOperator *op) {
    if (op->isIncrementDecrementOp() &&
        op->getSubExpr()->getType()->isPointerType() &&
        !bounded(op->getSubExpr()))
      single(op->getExprLoc(), op->getSubExpr());
    if (!unit.typed)
      return true;
    if (op->isDecrementOp() && op->getSubExpr()->getType()->isPointerType() &&
        nullTerminated(op->getSubExpr()))
      stringIndex(op->getExprLoc(), op->getSubExpr());
    // A count stepped up or down alone is checked at run time (amendment
    // 2: growth against the shadow).
    if (op->isIncrementDecrementOp())
      writesString(*op->getSubExpr(), op->getExprLoc());
    return true;
  }

  bool VisitBinaryOperator(clang::BinaryOperator *op) {
    if (op->isAdditiveOp() || op->getOpcode() == clang::BO_AddAssign ||
        op->getOpcode() == clang::BO_SubAssign) {
      const clang::Expr *pointer = nullptr;
      if (op->getLHS()->getType()->isPointerType() &&
          op->getRHS()->getType()->isIntegerType())
        pointer = op->getLHS();
      else if (op->getRHS()->getType()->isPointerType() &&
               op->getLHS()->getType()->isIntegerType())
        pointer = op->getRHS();
      const clang::Expr *offset =
          pointer == op->getLHS() ? op->getRHS() : op->getLHS();
      if (pointer != nullptr && !isZero(offset) && !bounded(pointer))
        single(op->getExprLoc(), pointer);
      if (unit.typed && pointer != nullptr && nullTerminated(pointer) &&
          !forwardConstant(offset, op->getOpcode() == clang::BO_Sub ||
                                       op->getOpcode() == clang::BO_SubAssign))
        stringIndex(op->getExprLoc(), pointer);
    }
    if (op->getOpcode() == clang::BO_Assign && !isLocalSlot(op->getLHS()) &&
        !isBorrowedSlot(op->getLHS()))
      storesBorrowed(op->getRHS(), op->getExprLoc());
    if (unit.typed && op->isAssignmentOp()) {
      writesString(*op->getLHS(), op->getExprLoc());
      const bool decrease =
          op->getOpcode() == clang::BO_SubAssign ||
          op->getOpcode() == clang::BO_AddAssign ||
          (op->getOpcode() == clang::BO_Assign && isZero(op->getRHS()));
      assignsCount(*op->getLHS(), *op, decrease);
      if (op->getOpcode() == clang::BO_Assign && !isLocalSlot(op->getLHS()))
        leavesWithFieldAddress(*op->getRHS());
      if (op->getOpcode() == clang::BO_Assign)
        if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
                op->getLHS()->IgnoreParenImpCasts()))
          singleIntoBound(*op->getRHS(), member->getMemberDecl()->getType(),
                          "field '" + member->getMemberDecl()->getName().str() +
                              "'");
    }
    return true;
  }

  bool VisitCastExpr(clang::CastExpr *cast) {
    switch (cast->getCastKind()) {
    case clang::CK_IntegralToPointer:
      report(cast->getExprLoc(), rule::IntToPointer,
             "an integer cannot become a pointer in a safe function",
             "keep the pointer itself rather than its address as an "
             "integer, or convert it in a WEAVEC_GUARDED helper function");
      break;
    case clang::CK_BitCast:
      if (cast->getType()->isPointerType() &&
          cast->getSubExpr()->getType()->isPointerType())
        checkPointerCast(*cast);
      break;
    case clang::CK_LValueBitCast:
    case clang::CK_LValueToRValueBitCast:
      if (holdsPointers(cast->getType()))
        report(cast->getExprLoc(), rule::PointerCast,
               "reinterpreting bytes as a type that holds pointers could "
               "forge a pointer",
               "copy the value as its own type, or reinterpret it in a "
               "WEAVEC_GUARDED helper function");
      break;
    default:
      break;
    }
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *member) {
    const clang::QualType base =
        member->isArrow() ? member->getBase()->getType()->getPointeeType()
                          : member->getBase()->getType();
    const clang::RecordDecl *record = base->getAsRecordDecl();
    if (record == nullptr || !record->isUnion() ||
        !holdsPointers(member->getType()) || isAssignedTo(*member))
      return true;
    report(member->getMemberLoc(), rule::UnionPointer,
           "reading union member '" + member->getMemberDecl()->getName().str() +
               "' may reinterpret another member's bytes as a pointer",
           "keep a tag beside the union and one struct per variant, or "
           "read the member in a WEAVEC_GUARDED helper function");
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *call) {
    if (unit.typed)
      for (const clang::Expr *argument : call->arguments())
        leavesWithFieldAddress(*argument);
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr)
      return true;
    checkBuiltin(*call, *callee);
    if (unit.typed)
      for (unsigned i = 0; i < call->getNumArgs() && i < callee->getNumParams();
           ++i)
        passesString(*call->getArg(i), *callee->getParamDecl(i), *callee);
    const auto *prototype =
        callee->getType()->getAs<clang::FunctionProtoType>();
    if (prototype != nullptr && prototype->isVariadic() &&
        !core::LibrarySpec::shipped()
             .lookup(callee->getName().str(), call->getNumArgs())
             .has_value())
      report(call->getExprLoc(), rule::Variadic,
             "'" + callee->getName().str() +
                 "' is variadic and the library table does not describe it",
             "call a variant that takes its arguments by type, or call it "
             "from a WEAVEC_GUARDED helper function");
    const FunctionBounds *table = unit.find(callee->getName());
    for (unsigned i = 0; i < call->getNumArgs() && i < callee->getNumParams();
         ++i) {
      const clang::Expr *argument = call->getArg(i);
      const clang::ParmVarDecl *parameter = callee->getParamDecl(i);
      const PointerBounds *bounds =
          table != nullptr && i < table->parameters.size()
              ? &table->parameters[i]
              : nullptr;
      const bool null = argument->isNullPointerConstant(
                            ast, clang::Expr::NPC_ValueDependentIsNotNull) !=
                        clang::Expr::NPCK_NotNull;
      if (bounds != nullptr && bounds->bounded() && null && !bounds->orNull &&
          bounds->kind == PointerBounds::Kind::NullTerminated)
        report(argument->getExprLoc(), rule::CallArgs,
               "null passed for '" + parameter->getName().str() + "' of '" +
                   callee->getName().str() + "', which points to a string",
               "declare the parameter WEAVEC_NULL_TERMINATED_OR_NULL if it "
               "may be null, or pass a string");
      if (bounds != nullptr && bounds->bounded() && !bounded(argument) && !null)
        report(argument->getExprLoc(), rule::CallArgs,
               "the argument for '" + parameter->getName().str() + "' of '" +
                   callee->getName().str() +
                   "' must have bounds, and this pointer points to one object",
               "pass an array, an allocation, or a pointer whose declaration "
               "has a bound (WEAVEC_COUNTED_BY, WEAVEC_SIZED_BY, "
               "WEAVEC_ENDED_BY, WEAVEC_NULL_TERMINATED)");
      if (bounds != nullptr && bounds->escapes) {
        const Origins origins = originsOf(argument);
        if (!origins.empty())
          report(argument->getExprLoc(), rule::Escape,
                 borrowed(**origins.begin()) +
                     " and cannot be passed to the escaping parameter '" +
                     parameter->getName().str() + "'",
                 "mark '" + (*origins.begin())->getName().str() +
                     "' WEAVEC_ESCAPES too, or pass a copy of what it "
                     "points to");
      }
    }
    return true;
  }

  bool VisitReturnStmt(clang::ReturnStmt *statement) {
    const clang::Expr *value = statement->getRetValue();
    if (value == nullptr)
      return true;
    if (unit.typed) {
      leavesWithFieldAddress(*value);
      singleIntoBound(*value, function.getReturnType(), "the result");
    }
    for (const clang::NamedDecl *origin : originsOf(value))
      if (!origin->hasAttr<clang::LifetimeBoundAttr>() ||
          !llvm::isa<clang::ParmVarDecl>(origin)) {
        report(value->getExprLoc(), rule::Escape,
               borrowed(*origin) + " and cannot be returned",
               llvm::isa<clang::ParmVarDecl>(origin)
                   ? "mark the parameter WEAVEC_LIFETIMEBOUND (after its "
                     "name), so that callers treat the result as borrowed "
                     "from it"
                   : "return an offset or a copy instead: a pointer from a "
                     "borrowed field or a local holding one cannot leave "
                     "the call");
        break;
      }
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *variable) {
    if (variable->hasInit() && !variable->hasLocalStorage())
      storesBorrowed(variable->getInit(), variable->getLocation());
    if (unit.typed && variable->hasInit() && !variable->hasLocalStorage())
      leavesWithFieldAddress(*variable->getInit());
    return true;
  }

  bool VisitInitListExpr(clang::InitListExpr *visited) {
    // A local aggregate's WEAVEC_BORROWED fields may take borrowed pointers.
    const clang::InitListExpr *list =
        visited->isSyntacticForm() && visited->getSemanticForm() != nullptr
            ? visited->getSemanticForm()
            : visited;
    const clang::RecordDecl *record = list->getType()->getAsRecordDecl();
    const bool local = initializesLocal(*list) || initializesLocal(*visited);
    if (unit.typed && record != nullptr && list == visited)
      checkCountedInit(ast, *list, *record, report);
    unsigned index = 0;
    llvm::SmallVector<const clang::FieldDecl *, 8> fields;
    if (record != nullptr && !record->isUnion())
      fields.append(record->field_begin(), record->field_end());
    for (const clang::Expr *element : list->inits()) {
      const clang::FieldDecl *field =
          index < fields.size() ? fields[index] : nullptr;
      ++index;
      if (unit.typed && element != nullptr)
        leavesWithFieldAddress(*element);
      if (element == nullptr || !(element->getType()->isPointerType() ||
                                  holdsBorrowed(element->getType(), ast)))
        continue;
      if (local && field != nullptr &&
          hasAnnotation(annotationsOf(field->getType(), ast),
                        BorrowedAnnotation))
        continue;
      storesBorrowed(element, element->getExprLoc());
    }
    return true;
  }

  bool VisitVAArgExpr(clang::VAArgExpr *argument) {
    if (holdsPointers(argument->getType()))
      report(argument->getExprLoc(), rule::Variadic,
             "a variadic argument read as a type that holds pointers could "
             "forge a pointer",
             "take the arguments by type, or read them in a WEAVEC_GUARDED "
             "helper function");
    return true;
  }

  bool VisitGCCAsmStmt(clang::GCCAsmStmt *statement) {
    // An empty template is a compiler barrier: it touches no memory itself.
    if (statement->getNumOutputs() == 0 && statement->getNumInputs() == 0 &&
        statement->getAsmString().empty())
      return true;
    forbidden(statement->getAsmLoc(), "inline assembly");
    return true;
  }
  bool VisitMSAsmStmt(clang::MSAsmStmt *statement) {
    forbidden(statement->getAsmLoc(), "inline assembly");
    return true;
  }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *statement) {
    forbidden(statement->getGotoLoc(), "a computed goto");
    return true;
  }
  bool VisitAddrLabelExpr(clang::AddrLabelExpr *label) {
    forbidden(label->getExprLoc(), "a label's address");
    return true;
  }

  bool VisitAttributedStmt(clang::AttributedStmt *statement) {
    for (const clang::Attr *attribute : statement->getAttrs())
      if (const auto *annotate = llvm::dyn_cast<clang::AnnotateAttr>(attribute);
          annotate != nullptr && annotate->getAnnotation() == UnsafeAnnotation)
        report(statement->getBeginLoc(), rule::UnsafeBlock,
               "a WEAVEC_UNSAFE block cannot be inside a safe function",
               "move the block into a WEAVEC_GUARDED or WEAVEC_UNSAFE "
               "function that the safe one calls");
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  clang::ASTContext &ast;
  const clang::FunctionDecl &function;
  const SafeUnit &unit;
  Report report;
  /// Locals that may hold a pointer to more than one object.
  llvm::DenseMap<const clang::VarDecl *, bool> localBounded;
  llvm::DenseMap<const clang::VarDecl *, Origins> localOrigins;
  /// RFC 0042: locals that may hold a pointer into a string, or the
  /// address of a pointer field; variables assigned after their
  /// initialization; variables whose string `strlen` measures.
  llvm::DenseSet<const clang::VarDecl *> localStrings;
  llvm::DenseSet<const clang::VarDecl *> localFieldAddresses;
  llvm::DenseSet<const clang::VarDecl *> reassigned;
  llvm::DenseSet<const clang::VarDecl *> assignedOnce;
  llvm::DenseSet<const clang::VarDecl *> measuredStrings;

  /// What a local pointer variable is assigned: its initializer and every
  /// `=`; a local whose address is taken may be written through it.
  void inferLocals() {
    llvm::DenseMap<const clang::VarDecl *, std::vector<const clang::Expr *>>
        assigned;
    llvm::DenseSet<const clang::VarDecl *> addressed;
    collectAssignments(function.getBody(), assigned, addressed);
    for (auto &[variable, values] : assigned)
      localBounded[variable] = true;
    for (const clang::VarDecl *variable : addressed)
      localBounded[variable] =
          boundedBy(annotationsOf(variable->getType(), ast));
    if (unit.typed)
      collectTyped(function.getBody());
    for (bool changed = true; changed;) {
      changed = false;
      for (auto &[variable, values] : assigned) {
        if (unit.typed)
          for (const clang::Expr *value : values) {
            if (!localStrings.contains(variable) && nullTerminated(value)) {
              localStrings.insert(variable);
              changed = true;
            }
            if (!localFieldAddresses.contains(variable) &&
                carriesFieldAddress(*value)) {
              localFieldAddresses.insert(variable);
              changed = true;
            }
          }
        if (addressed.contains(variable))
          continue;
        bool isBounded = true;
        Origins origins = localOrigins[variable];
        const std::size_t before = origins.size();
        for (const clang::Expr *value : values) {
          isBounded = isBounded && bounded(value);
          for (const clang::NamedDecl *origin : originsOf(value))
            origins.insert(origin);
        }
        if (isBounded != localBounded[variable] || origins.size() != before) {
          localBounded[variable] = isBounded;
          localOrigins[variable] = std::move(origins);
          changed = true;
        }
      }
    }
  }

  static void collectAssignments(
      const clang::Stmt *statement,
      llvm::DenseMap<const clang::VarDecl *, std::vector<const clang::Expr *>>
          &assigned,
      llvm::DenseSet<const clang::VarDecl *> &addressed) {
    if (statement == nullptr)
      return;
    if (const auto *declaration = llvm::dyn_cast<clang::DeclStmt>(statement))
      for (const clang::Decl *decl : declaration->decls())
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
            variable != nullptr && isLocalPointer(*variable)) {
          auto &values = assigned[variable];
          if (variable->hasInit())
            values.push_back(variable->getInit());
        }
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(statement);
        op != nullptr && op->getOpcode() == clang::BO_Assign)
      if (const clang::VarDecl *variable = localPointerOf(op->getLHS()))
        assigned[variable].push_back(op->getRHS());
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(statement);
        op != nullptr && op->getOpcode() == clang::UO_AddrOf)
      if (const clang::VarDecl *variable = localPointerOf(op->getSubExpr()))
        addressed.insert(variable);
    for (const clang::Stmt *child : statement->children())
      collectAssignments(child, assigned, addressed);
  }

  static bool isLocalPointer(const clang::VarDecl &variable) {
    return variable.hasLocalStorage() &&
           !llvm::isa<clang::ParmVarDecl>(variable) &&
           variable.getType()->isPointerType();
  }

  static const clang::VarDecl *localPointerOf(const clang::Expr *expression) {
    const auto *reference =
        llvm::dyn_cast<clang::DeclRefExpr>(expression->IgnoreParenImpCasts());
    if (reference == nullptr)
      return nullptr;
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
    return variable != nullptr && isLocalPointer(*variable) ? variable
                                                            : nullptr;
  }

  /// An lvalue that is a local variable or parameter itself (not memory a
  /// pointer reaches): storing a borrowed pointer there keeps it in the
  /// frame.
  static bool isLocalSlot(const clang::Expr *lvalue) {
    const auto *reference =
        llvm::dyn_cast<clang::DeclRefExpr>(lvalue->IgnoreParenImpCasts());
    if (reference == nullptr)
      return false;
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
    return variable != nullptr && variable->hasLocalStorage() &&
           variable->getType()->isPointerType();
  }

  /// Whether `expression` (a pointer) may point to more than one object:
  /// its bounds are known (§2).
  bool bounded(const clang::Expr *expression) const {
    const clang::Expr *e = expression->IgnoreParens();
    if (e->isNullPointerConstant(ast, clang::Expr::NPC_ValueDependentIsNotNull))
      return true;
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_ArrayToPointerDecay)
        return true;
      if (cast->getCastKind() == clang::CK_LValueToRValue ||
          cast->getCastKind() == clang::CK_BitCast ||
          cast->getCastKind() == clang::CK_NoOp)
        return bounded(cast->getSubExpr());
      return false;
    }
    if (llvm::isa<clang::StringLiteral, clang::CompoundLiteralExpr>(e))
      return true;
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      const auto *variable =
          llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
      if (variable == nullptr)
        return false;
      if (const auto *parameter = llvm::dyn_cast<clang::ParmVarDecl>(variable))
        return boundedBy(annotationsOf(parameter->getType(), ast));
      if (const auto found = localBounded.find(variable);
          found != localBounded.end())
        return found->second;
      return boundedBy(annotationsOf(variable->getType(), ast));
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
      return boundedBy(annotationsOf(member->getMemberDecl()->getType(), ast));
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      if (callee == nullptr)
        return false;
      if (callee->hasAttr<clang::AllocSizeAttr>())
        return true;
      return boundedBy(annotationsOf(callee->getReturnType(), ast));
    }
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      if (op->getOpcode() == clang::UO_AddrOf)
        return true;
      if (op->isIncrementDecrementOp())
        return bounded(op->getSubExpr());
      if (op->getOpcode() == clang::UO_Deref)
        return boundedBy(annotationsOf(op->getType(), ast));
      return false;
    }
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
      return boundedBy(annotationsOf(subscript->getType(), ast));
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (op->isAdditiveOp() || op->isCompoundAssignmentOp())
        return bounded(op->getLHS()->getType()->isPointerType() ? op->getLHS()
                                                                : op->getRHS());
      if (op->getOpcode() == clang::BO_Assign ||
          op->getOpcode() == clang::BO_Comma)
        return bounded(op->getRHS());
      return false;
    }
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(e))
      return bounded(conditional->getTrueExpr()) &&
             bounded(conditional->getFalseExpr());
    return false;
  }

  /// The borrowed parameters `expression` may derive from (§5).
  Origins originsOf(const clang::Expr *expression) const {
    Origins origins;
    addOrigins(expression, origins);
    return origins;
  }

  void addOrigins(const clang::Expr *expression, Origins &origins) const {
    const clang::Expr *e = expression->IgnoreParens();
    // An integer (`c.offset`, `end - s`) carries no pointer.
    if (!e->getType()->isPointerType() && !e->getType()->isArrayType() &&
        !holdsBorrowed(e->getType(), ast))
      return;
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      if (cast->getCastKind() != clang::CK_ArrayToPointerDecay)
        addOrigins(cast->getSubExpr(), origins);
      return;
    }
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      if (const auto *parameter =
              llvm::dyn_cast<clang::ParmVarDecl>(reference->getDecl())) {
        if (parameter->getType()->isPointerType() &&
            !parameter->getType()->isFunctionPointerType() &&
            !hasAnnotation(annotationsOf(parameter->getType(), ast),
                           EscapesAnnotation))
          origins.insert(parameter);
        return;
      }
      if (const auto *variable =
              llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) {
        if (const auto found = localOrigins.find(variable);
            found != localOrigins.end())
          origins.insert(found->second.begin(), found->second.end());
        // A copy of a local aggregate copies its borrowed pointers.
        if (variable->hasLocalStorage() &&
            holdsBorrowed(variable->getType(), ast))
          origins.insert(variable);
      }
      return;
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
      if (hasAnnotation(annotationsOf(member->getMemberDecl()->getType(), ast),
                        BorrowedAnnotation)) {
        // Borrowed for as long as the struct it was loaded from: the
        // origins of the struct's pointer or local, or the field itself
        // when those are not known.
        Origins base;
        if (member->isArrow())
          addOrigins(member->getBase(), base);
        else
          addAddressOrigins(member->getBase(), base);
        if (base.empty())
          origins.insert(member->getMemberDecl());
        else
          origins.insert(base.begin(), base.end());
      } else if (!member->isArrow()) {
        addOrigins(member->getBase(), origins);
      }
      return;
    }
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      if (op->getOpcode() == clang::UO_AddrOf)
        addAddressOrigins(op->getSubExpr(), origins);
      else if (op->isIncrementDecrementOp())
        addOrigins(op->getSubExpr(), origins);
      return;
    }
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      // `end - s` is an integer: it carries no pointer.
      if (!e->getType()->isPointerType())
        return;
      if (op->isAdditiveOp() || op->isCompoundAssignmentOp()) {
        if (op->getLHS()->getType()->isPointerType())
          addOrigins(op->getLHS(), origins);
        if (op->getRHS()->getType()->isPointerType())
          addOrigins(op->getRHS(), origins);
      } else if (op->getOpcode() == clang::BO_Assign ||
                 op->getOpcode() == clang::BO_Comma) {
        addOrigins(op->getRHS(), origins);
      }
      return;
    }
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(e)) {
      addOrigins(conditional->getTrueExpr(), origins);
      addOrigins(conditional->getFalseExpr(), origins);
      return;
    }
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      if (callee == nullptr)
        return;
      for (unsigned i = 0; i < call->getNumArgs() && i < callee->getNumParams();
           ++i)
        if (callee->getParamDecl(i)->hasAttr<clang::LifetimeBoundAttr>())
          addOrigins(call->getArg(i), origins);
    }
  }

  /// `&base->field`, `&base[i]`: the origins of `base`.
  void addAddressOrigins(const clang::Expr *lvalue, Origins &origins) const {
    const clang::Expr *e = lvalue->IgnoreParens();
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
      if (member->isArrow())
        addOrigins(member->getBase(), origins);
      else
        addAddressOrigins(member->getBase(), origins);
      return;
    }
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
      addOrigins(subscript->getBase(), origins);
      return;
    }
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e);
        op != nullptr && op->getOpcode() == clang::UO_Deref)
      addOrigins(op->getSubExpr(), origins);
    // `&local`, where the local holds borrowed pointers.
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(e))
      if (const auto *variable =
              llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
          variable != nullptr && variable->hasLocalStorage() &&
          holdsBorrowed(variable->getType(), ast))
        origins.insert(variable);
  }

  /// How an error names an origin.
  static std::string borrowed(const clang::NamedDecl &origin) {
    if (llvm::isa<clang::FieldDecl>(origin))
      return "'" + origin.getName().str() +
             "' was loaded from a WEAVEC_BORROWED field, so it is borrowed,";
    if (!llvm::isa<clang::ParmVarDecl>(origin))
      return "'" + origin.getName().str() +
             "' holds borrowed pointers (WEAVEC_BORROWED fields),";
    return "'" + origin.getName().str() + "' is borrowed for this call";
  }

  /// `local.field` (through `.` only) where the field is WEAVEC_BORROWED: a
  /// borrowed pointer may be stored there, since the local dies with the
  /// frame and its address cannot leave it.
  bool isBorrowedSlot(const clang::Expr *lvalue) const {
    const auto *member =
        llvm::dyn_cast<clang::MemberExpr>(lvalue->IgnoreParenImpCasts());
    if (member == nullptr ||
        !hasAnnotation(annotationsOf(member->getMemberDecl()->getType(), ast),
                       BorrowedAnnotation))
      return false;
    const clang::Expr *base = member;
    while (const auto *inner =
               llvm::dyn_cast<clang::MemberExpr>(base->IgnoreParenImpCasts())) {
      if (inner->isArrow())
        return false;
      base = inner->getBase();
    }
    const auto *reference =
        llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts());
    const auto *variable =
        reference == nullptr
            ? nullptr
            : llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
    return variable != nullptr && variable->hasLocalStorage();
  }

  /// Whether `list` initializes a local variable (directly or nested).
  bool initializesLocal(const clang::Expr &list) const {
    const clang::Expr *current = &list;
    for (unsigned depth = 0; depth < 8; ++depth) {
      const auto parents = ast.getParents(*current);
      if (parents.empty())
        return false;
      if (const auto *variable = parents[0].get<clang::VarDecl>())
        return variable->hasLocalStorage();
      const auto *next = parents[0].get<clang::InitListExpr>();
      if (next == nullptr)
        return false;
      current = next;
    }
    return false;
  }

  //===--------------------------------------------------------------------===//
  // RFC 0042 §3: typed functions' rules
  //===--------------------------------------------------------------------===//

  /// Variables assigned after their initialization, and those a `strlen`
  /// call measures.
  void collectTyped(const clang::Stmt *statement) {
    if (statement == nullptr)
      return;
    auto variableOf = [](const clang::Expr *e) -> const clang::VarDecl * {
      const auto *reference =
          llvm::dyn_cast<clang::DeclRefExpr>(e->IgnoreParenCasts());
      return reference == nullptr
                 ? nullptr
                 : llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
    };
    // A variable declared without a value may be assigned once.
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(statement);
        op != nullptr && op->isAssignmentOp())
      if (const clang::VarDecl *variable = variableOf(op->getLHS());
          variable != nullptr &&
          (op->getOpcode() != clang::BO_Assign || variable->hasInit() ||
           llvm::isa<clang::ParmVarDecl>(variable) ||
           !assignedOnce.insert(variable).second))
        reassigned.insert(variable);
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(statement);
        op != nullptr &&
        (op->isIncrementDecrementOp() || op->getOpcode() == clang::UO_AddrOf))
      if (const clang::VarDecl *variable = variableOf(op->getSubExpr()))
        reassigned.insert(variable);
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(statement)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      if (callee != nullptr && call->getNumArgs() == 1 &&
          (callee->getBuiltinID() == clang::Builtin::BIstrlen ||
           callee->getBuiltinID() == clang::Builtin::BI__builtin_strlen))
        if (const clang::VarDecl *variable = variableOf(call->getArg(0)))
          measuredStrings.insert(variable);
    }
    for (const clang::Stmt *child : statement->children())
      collectTyped(child);
  }

  static bool isStringType(clang::QualType type, const clang::ASTContext &ast) {
    const auto annotations = annotationsOf(type, ast);
    return hasAnnotation(annotations, NullTerminatedAnnotation) ||
           hasAnnotation(annotations, NullTerminatedOrNullAnnotation);
  }

  /// Whether `expression` (a pointer) may point into a string through a
  /// null-terminated declaration (§3, `string-index`, `string-write`).
  bool nullTerminated(const clang::Expr *expression) const {
    const clang::Expr *e = expression->IgnoreParens();
    if (e->getType()->isPointerType() && isStringType(e->getType(), ast))
      return true;
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_LValueToRValue ||
          cast->getCastKind() == clang::CK_BitCast ||
          cast->getCastKind() == clang::CK_NoOp)
        return nullTerminated(cast->getSubExpr());
      return false;
    }
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      const auto *variable =
          llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
      return variable != nullptr && (isStringType(variable->getType(), ast) ||
                                     localStrings.contains(variable));
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
      return isStringType(member->getMemberDecl()->getType(), ast);
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      return callee != nullptr && isStringType(callee->getReturnType(), ast);
    }
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e))
      return op->isIncrementDecrementOp() && nullTerminated(op->getSubExpr());
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (!e->getType()->isPointerType())
        return false;
      if (op->isAdditiveOp() || op->isCompoundAssignmentOp())
        return nullTerminated(op->getLHS()->getType()->isPointerType()
                                  ? op->getLHS()
                                  : op->getRHS());
      if (op->getOpcode() == clang::BO_Assign ||
          op->getOpcode() == clang::BO_Comma)
        return nullTerminated(op->getRHS());
      return false;
    }
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(e))
      return nullTerminated(conditional->getTrueExpr()) ||
             nullTerminated(conditional->getFalseExpr());
    return false;
  }

  /// An integer constant that moves a string pointer forward (or not at
  /// all): `negated` for a subtraction.
  bool forwardConstant(const clang::Expr *offset, bool negated) const {
    clang::Expr::EvalResult value;
    if (!offset->EvaluateAsInt(value, ast))
      return false;
    const llvm::APSInt &integer = value.Val.getInt();
    return negated ? integer.isZero() : !integer.isNegative();
  }

  /// `string-index`, unless `strlen` measured the string (a variable never
  /// assigned after its initialization): its bounds are then known.
  void stringIndex(clang::SourceLocation at, const clang::Expr *pointer) {
    if (ast.getSourceManager().isInSystemMacro(at))
      return;
    const auto *reference =
        llvm::dyn_cast<clang::DeclRefExpr>(pointer->IgnoreParenImpCasts());
    const auto *variable =
        reference == nullptr
            ? nullptr
            : llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
    if (variable != nullptr && measuredStrings.contains(variable) &&
        !reassigned.contains(variable))
      return;
    report(at, rule::StringIndex,
           "'" + text(pointer) +
               "' points into a string and is indexed or moved by a "
               "variable or backward; a string is read at its current byte "
               "and stepped forward by constants",
           "take the buffer with its size ('const char "
           "*WEAVEC_COUNTED_BY(n) s, size_t n'), or, to only read it, "
           "measure it first: 'size_t n = strlen(s);' with 's' not "
           "assigned again (one assignment is fine if its declaration has "
           "no value)");
  }

  /// `string-write`: a store through a pointer into a string.
  void writesString(const clang::Expr &lvalue, clang::SourceLocation at) {
    const clang::Expr *e = lvalue.IgnoreParens();
    const clang::Expr *base = nullptr;
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e);
        op != nullptr && op->getOpcode() == clang::UO_Deref)
      base = op->getSubExpr();
    else if (const auto *subscript =
                 llvm::dyn_cast<clang::ArraySubscriptExpr>(e))
      base = subscript->getBase();
    if (base == nullptr || !base->getType()->isPointerType() ||
        !nullTerminated(base))
      return;
    report(at, rule::StringWrite,
           "store through '" + text(base) +
               "', which points into a string: typed code only reads a "
               "string",
           "take the buffer and its size to write it: "
           "'char *WEAVEC_COUNTED_BY(cap) s, size_t cap'");
  }

  /// RFC 0042 `call-args` for typed functions: a pointer to one object
  /// stored where a declaration bounds it, or returned as a bounded result.
  void singleIntoBound(const clang::Expr &value, clang::QualType place,
                       const std::string &what) {
    if (!place->isPointerType() || !boundedBy(annotationsOf(place, ast)) ||
        value.isNullPointerConstant(ast,
                                    clang::Expr::NPC_ValueDependentIsNotNull) ||
        bounded(&value))
      return;
    report(value.getExprLoc(), rule::CallArgs,
           "'" + text(&value) + "' points to one object, and " + what +
               " has a bound",
           "give its declaration the same bound (WEAVEC_COUNTED_BY, "
           "WEAVEC_SIZED_BY, WEAVEC_NULL_TERMINATED), or pass an array or "
           "an allocation");
  }

  /// `string-write`: a string passed where the callee may write it.
  void passesString(const clang::Expr &argument,
                    const clang::ParmVarDecl &parameter,
                    const clang::FunctionDecl &callee) {
    const clang::QualType type = parameter.getType();
    // Releasing a string writes nothing a reader could see.
    const unsigned builtin = callee.getBuiltinID();
    if (!type->isPointerType() || type->getPointeeType().isConstQualified() ||
        isStringType(type, ast) || !nullTerminated(&argument) ||
        builtin == clang::Builtin::BIfree ||
        builtin == clang::Builtin::BIrealloc)
      return;
    const std::string name =
        parameter.getName().empty()
            ? "parameter " +
                  std::to_string(parameter.getFunctionScopeIndex() + 1) +
                  " of '" + callee.getName().str() + "'"
            : "'" + parameter.getName().str() + "' of '" +
                  callee.getName().str() + "'";
    report(argument.getExprLoc(), rule::StringWrite,
           "'" + text(&argument) + "' points into a string, and " + name +
               " may write through it: typed code only reads a string",
           "pass a counted buffer, or declare what the parameter points to "
           "'const' ('const char *')");
  }

  /// The pointer fields of `count`'s struct whose bound names it.
  llvm::SmallVector<const clang::FieldDecl *, 2>
  countedBy(const clang::FieldDecl &count) const {
    llvm::SmallVector<const clang::FieldDecl *, 2> found;
    for (const clang::FieldDecl *field : count.getParent()->fields()) {
      if (!field->getType()->isPointerType())
        continue;
      for (const TypeAnnotation &annotation :
           annotationsOf(field->getType(), ast))
        if ((annotation.name == CountedByAnnotation ||
             annotation.name == CountedByOrNullAnnotation ||
             annotation.name == SizedByAnnotation ||
             annotation.name == SizedByOrNullAnnotation) &&
            annotation.argument == count.getName())
          found.push_back(field);
    }
    return found;
  }

  static bool containsCall(const clang::Stmt *statement) {
    if (statement == nullptr)
      return false;
    if (llvm::isa<clang::CallExpr>(statement))
      return true;
    return llvm::any_of(statement->children(), containsCall);
  }

  /// Whether `a` and `b` name the same struct: a variable, or a member or
  /// dereference of the same.
  static bool sameBase(const clang::Expr *a, const clang::Expr *b) {
    a = a->IgnoreParenImpCasts();
    b = b->IgnoreParenImpCasts();
    if (const auto *x = llvm::dyn_cast<clang::DeclRefExpr>(a))
      if (const auto *y = llvm::dyn_cast<clang::DeclRefExpr>(b))
        return x->getDecl() == y->getDecl();
    if (const auto *x = llvm::dyn_cast<clang::MemberExpr>(a))
      if (const auto *y = llvm::dyn_cast<clang::MemberExpr>(b))
        return x->getMemberDecl() == y->getMemberDecl() &&
               x->isArrow() == y->isArrow() &&
               sameBase(x->getBase(), y->getBase());
    if (const auto *x = llvm::dyn_cast<clang::UnaryOperator>(a))
      if (const auto *y = llvm::dyn_cast<clang::UnaryOperator>(b))
        return x->getOpcode() == clang::UO_Deref &&
               y->getOpcode() == clang::UO_Deref &&
               sameBase(x->getSubExpr(), y->getSubExpr());
    return false;
  }

  /// Whether `statement` assigns one of `pointers` of the struct `base`.
  static bool assignsPointer(const clang::Stmt *statement,
                             llvm::ArrayRef<const clang::FieldDecl *> pointers,
                             const clang::Expr *base) {
    if (statement != nullptr)
      statement = statement->stripLabelLikeStatements();
    const auto *op = llvm::dyn_cast_or_null<clang::BinaryOperator>(statement);
    if (op == nullptr || op->getOpcode() != clang::BO_Assign)
      return false;
    const auto *member =
        llvm::dyn_cast<clang::MemberExpr>(op->getLHS()->IgnoreParenImpCasts());
    return member != nullptr &&
           llvm::is_contained(pointers, member->getMemberDecl()) &&
           member->isArrow() == llvm::isa<clang::PointerType>(
                                    base->getType().getCanonicalType()) &&
           sameBase(member->getBase(), base);
  }

  /// `counted-pair`: a count is assigned in the statement next to its
  /// pointer's, the second of the two calling nothing, or stepped by `++`,
  /// `--`, `+=`, `-=` or set to 0 alone (checked at run time).
  void assignsCount(const clang::Expr &lvalue, const clang::Expr &assignment,
                    bool decrease) {
    const auto *member =
        llvm::dyn_cast<clang::MemberExpr>(lvalue.IgnoreParenImpCasts());
    const auto *field =
        member == nullptr
            ? nullptr
            : llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr || decrease)
      return;
    const auto pointers = countedBy(*field);
    if (pointers.empty())
      return;
    const std::string count = field->getName().str();
    const std::string pointer = pointers.front()->getName().str();
    // The statement, under its `case` or label if it has one.
    const clang::Stmt *statement = &assignment;
    const clang::CompoundStmt *block = nullptr;
    for (unsigned depth = 0; depth < 8 && block == nullptr; ++depth) {
      const auto parents = ast.getParents(*statement);
      if (parents.empty())
        break;
      block = parents[0].get<clang::CompoundStmt>();
      const auto *label = parents[0].get<clang::Stmt>();
      if (block == nullptr &&
          !llvm::isa_and_nonnull<clang::SwitchCase, clang::LabelStmt>(label))
        break;
      if (block == nullptr)
        statement = label;
    }
    if (block != nullptr) {
      const clang::Stmt *previous = nullptr;
      for (const auto *it = block->body_begin(); it != block->body_end();
           ++it) {
        if (*it != statement) {
          previous = *it;
          continue;
        }
        const clang::Stmt *next =
            std::next(it) == block->body_end() ? nullptr : *std::next(it);
        const bool before =
            assignsPointer(previous, pointers, member->getBase());
        const bool after = assignsPointer(next, pointers, member->getBase());
        if (next != nullptr)
          next = next->stripLabelLikeStatements();
        if ((before && !containsCall(&assignment)) ||
            (after && !containsCall(next)))
          return;
        if (before || after) {
          report(assignment.getExprLoc(), rule::CountedPair,
                 (llvm::Twine("'") + count + "' and '" + pointer +
                  "' are assigned with a call between them")
                     .str(),
                 (llvm::Twine("compute the new values into locals first, "
                              "then assign '") +
                  pointer + "' and '" + count + "' in two adjacent statements")
                     .str());
          return;
        }
        break;
      }
    }
    report(assignment.getExprLoc(), rule::CountedPair,
           "'" + count + "' bounds '" + pointer +
               "' and is assigned without it",
           "assign '" + pointer + "' in the next statement, or '" + pointer +
               " = " + pointer + "' if only the count shrinks");
  }

  /// Whether `expression` is or holds the address of a pointer field (or of
  /// an element of an array of them).
  bool carriesFieldAddress(const clang::Expr &expression) const {
    const clang::Expr *e = expression.IgnoreParens();
    auto pointerMember = [](const clang::Expr *lvalue, bool array) {
      const auto *member =
          llvm::dyn_cast<clang::MemberExpr>(lvalue->IgnoreParens());
      if (member == nullptr)
        return false;
      const clang::QualType type = member->getType();
      if (array)
        return type->isArrayType() &&
               type->getAsArrayTypeUnsafe()->getElementType()->isPointerType();
      return type->isPointerType();
    };
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_ArrayToPointerDecay)
        return pointerMember(cast->getSubExpr(), true);
      if (cast->getCastKind() == clang::CK_LValueToRValue ||
          cast->getCastKind() == clang::CK_BitCast ||
          cast->getCastKind() == clang::CK_NoOp)
        return carriesFieldAddress(*cast->getSubExpr());
      return false;
    }
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      if (op->getOpcode() != clang::UO_AddrOf)
        return false;
      const clang::Expr *lvalue = op->getSubExpr()->IgnoreParens();
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(lvalue))
        return carriesFieldAddress(*subscript->getBase());
      return pointerMember(lvalue, false);
    }
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (op->isAdditiveOp() && e->getType()->isPointerType())
        return carriesFieldAddress(op->getLHS()->getType()->isPointerType()
                                       ? *op->getLHS()
                                       : *op->getRHS());
      if (op->getOpcode() == clang::BO_Assign ||
          op->getOpcode() == clang::BO_Comma)
        return carriesFieldAddress(*op->getRHS());
      return false;
    }
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(e))
      return carriesFieldAddress(*conditional->getTrueExpr()) ||
             carriesFieldAddress(*conditional->getFalseExpr());
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(e))
      if (const auto *variable =
              llvm::dyn_cast<clang::VarDecl>(reference->getDecl()))
        return localFieldAddresses.contains(variable);
    return false;
  }

  /// `field-address`: the address of a pointer field stays in the function,
  /// where each access through it is known to be the field's.
  void leavesWithFieldAddress(const clang::Expr &expression) {
    if (!carriesFieldAddress(expression))
      return;
    report(expression.getExprLoc(), rule::FieldAddress,
           "the address of a pointer field cannot leave a typed function",
           "pass the struct that holds the field, or read the field into a "
           "local, pass that, and assign the field back");
  }

  /// Whether `size` copies or fills whole objects of `type`: a multiple of
  /// its size, or `n * sizeof(T)` for a T of that size.
  bool wholeObjects(const clang::Expr *size, clang::QualType type) const {
    if (type->isIncompleteType())
      return false;
    const std::int64_t bytes = ast.getTypeSizeInChars(type).getQuantity();
    clang::Expr::EvalResult value;
    if (size->EvaluateAsInt(value, ast))
      return bytes != 0 && value.Val.getInt().getExtValue() % bytes == 0;
    const clang::Expr *e = size->IgnoreParenImpCasts();
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e);
        op != nullptr && op->getOpcode() == clang::BO_Mul)
      return wholeObjects(op->getLHS(), type) ||
             wholeObjects(op->getRHS(), type);
    return false;
  }

  /// RFC 0042 §3, `pointer-cast` tightened for typed functions: memory
  /// that holds a pointer is viewed only as itself, `void` or `const` bytes,
  /// and a `void *` (which may hide such memory) becomes another type only
  /// where an allocation made it. A view larger than its object needs no
  /// rule: its accesses are compared with the object's bounds.
  void checkTypedCast(const clang::CastExpr &cast, clang::QualType from,
                      clang::QualType to) {
    if (to->isVoidType() || (isByteType(to) && to.isConstQualified()))
      return;
    const std::string names =
        "'" + from.getAsString() + " *' to '" + to.getAsString() + " *'";
    if (holdsPointers(from)) {
      if (!isByteType(to))
        report(cast.getExprLoc(), rule::PointerCast,
               "converting " + names + ": '" + from.getAsString() +
                   "' holds a pointer",
               "convert only between types without pointers, or in a "
               "WEAVEC_GUARDED helper function");
      return;
    }
    if (isAllocation(*cast.getSubExpr()) ||
        !(from->isVoidType() || from->isIncompleteType()))
      return;
    report(cast.getExprLoc(), rule::PointerCast,
           "converting " + names +
               " in a typed function: a 'void *' may hide memory that holds "
               "pointers",
           "convert an allocation's result where it is made, take a '" +
               to.getAsString() +
               " *' parameter, or convert in a WEAVEC_GUARDED helper");
  }

  void storesBorrowed(const clang::Expr *value, clang::SourceLocation at) {
    const Origins origins = originsOf(value);
    if (origins.empty())
      return;
    const bool field = llvm::isa<clang::FieldDecl>(**origins.begin());
    report(at, rule::Escape,
           borrowed(**origins.begin()) +
               " and cannot be stored where it outlives the call",
           field ? "keep an offset beside the borrowed field and advance the "
                   "offset, not the pointer, through a pointer to the struct"
                 : "mark the parameter WEAVEC_ESCAPES (after its '*'; callers "
                   "then cannot pass a stack address), store a copy of what "
                   "it points to, or, to carry it in a local struct, make "
                   "the field WEAVEC_BORROWED");
  }

  void single(clang::SourceLocation at, const clang::Expr *pointer) {
    // A system header's macro (glibc's ctype through __ctype_b_loc) is not
    // the program's to annotate; its access keeps its guard.
    if (ast.getSourceManager().isInSystemMacro(at))
      return;
    report(at, rule::IndexSingle,
           "'" + text(pointer) +
               "' points to one object and cannot be indexed or offset",
           "give its declaration a bound: 'T *WEAVEC_COUNTED_BY(n) p' (or "
           "WEAVEC_SIZED_BY, WEAVEC_ENDED_BY, WEAVEC_NULL_TERMINATED), or "
           "index an array or an allocation");
  }

  void forbidden(clang::SourceLocation at, llvm::StringRef what) {
    report(at, rule::NoAsm, (what + " cannot be in a safe function").str(),
           "move it into a WEAVEC_GUARDED helper function");
  }

  std::string text(const clang::Expr *expression) const {
    const clang::SourceManager &sources = ast.getSourceManager();
    const llvm::StringRef spelled = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(expression->getSourceRange()),
        sources, ast.getLangOpts());
    if (!spelled.empty() && spelled.size() <= 48)
      return spelled.str();
    // A variable or field by its name, however a macro spelled it.
    const clang::Expr *bare = expression->IgnoreParenImpCasts();
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(bare))
      return reference->getDecl()->getName().str();
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(bare))
      return member->getMemberDecl()->getName().str();
    return "this pointer";
  }

  /// An integer constant 0: indexing a single pointer by it is `*p`.
  bool isZero(const clang::Expr *index) const {
    clang::Expr::EvalResult value;
    return index->EvaluateAsInt(value, ast) && value.Val.getInt().isZero();
  }

  /// Whether `member` is the left side of `=` (written, not read).
  bool isAssignedTo(const clang::Expr &member) const {
    for (const clang::DynTypedNode &parent : ast.getParents(member)) {
      const auto *op = parent.get<clang::BinaryOperator>();
      if (op != nullptr && op->getOpcode() == clang::BO_Assign &&
          op->getLHS()->IgnoreParens() == &member)
        return true;
    }
    return false;
  }

  /// The call `expression` is directly an argument of (through parentheses
  /// and implicit conversions), and the argument's index.
  std::pair<const clang::CallExpr *, unsigned>
  argumentOf(const clang::Expr &expression) const {
    const clang::Expr *current = &expression;
    for (unsigned depth = 0; depth < 8; ++depth) {
      const auto parents = ast.getParents(*current);
      if (parents.empty())
        return {nullptr, 0};
      if (const auto *call = parents[0].get<clang::CallExpr>()) {
        for (unsigned i = 0; i < call->getNumArgs(); ++i)
          if (call->getArg(i) == current)
            return {call, i};
        return {nullptr, 0};
      }
      const auto *next = parents[0].get<clang::Expr>();
      if (next == nullptr || !(llvm::isa<clang::ParenExpr>(next) ||
                               llvm::isa<clang::ImplicitCastExpr>(next)))
        return {nullptr, 0};
      current = next;
    }
    return {nullptr, 0};
  }

  void checkPointerCast(const clang::CastExpr &cast) {
    const clang::QualType from = cast.getSubExpr()->getType()->getPointeeType();
    const clang::QualType to = cast.getType()->getPointeeType();
    if (clang::ASTContext::hasSameUnqualifiedType(from, to) ||
        cast.getSubExpr()->isNullPointerConstant(
            ast, clang::Expr::NPC_ValueDependentIsNotNull))
      return;
    if (from->isFunctionType() || to->isFunctionType()) {
      report(cast.getExprLoc(), rule::PointerCast,
             "a function pointer cannot convert to another type in a safe "
             "function",
             "declare the function with the type it is called through");
      return;
    }
    if (holdsPointers(to)) {
      if (from->isVoidType() && isAllocation(*cast.getSubExpr()))
        return;
      report(cast.getExprLoc(), rule::PointerCast,
             "converting '" + from.getAsString() + " *' to '" +
                 to.getAsString() +
                 " *' would read memory as a type that holds pointers",
             "convert only between types without pointers, from an "
             "allocation's result, or in a WEAVEC_GUARDED helper function");
      return;
    }
    if (unit.typed)
      checkTypedCast(cast, from, to);
    if (isByteType(to) && !to.isConstQualified() && holdsPointers(from) &&
        !byteViewAllowed(cast, from))
      report(cast.getExprLoc(), rule::PointerBytes,
             "writing '" + from.getAsString() +
                 "' as bytes could forge the pointers it holds",
             "copy it as its own type ('*d = *s'), memcpy between two '" +
                 from.getAsString() +
                 "' objects, memset it to 0, or pass it to a function that "
                 "is not safe");
  }

  static bool isAllocation(const clang::Expr &expression) {
    const auto *call =
        llvm::dyn_cast<clang::CallExpr>(expression.IgnoreParenImpCasts());
    return call != nullptr && call->getDirectCallee() != nullptr &&
           call->getDirectCallee()->hasAttr<clang::AllocSizeAttr>();
  }

  /// A byte view of `from`, which holds pointers, may be made only as an
  /// argument: to a function that is not safe (its own code is guarded and
  /// keeps the object's pointers as it found them, S-A2), to a release, to
  /// a library call that reads it, or to a copy whose other side has the
  /// same type, or a fill with zero.
  bool byteViewAllowed(const clang::CastExpr &cast,
                       clang::QualType from) const {
    const auto [call, index] = argumentOf(cast);
    if (call == nullptr)
      return false;
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr)
      return false;
    const unsigned builtin = callee->getBuiltinID();
    // RFC 0042: in a typed function, whole objects only, so that no
    // pointer is half written.
    // The size: memcpy's, memmove's and memset's third argument (their
    // fortified forms add the object's size after it), bzero's second.
    const bool whole =
        !unit.typed ||
        wholeObjects(call->getArg(std::min(2U, call->getNumArgs() - 1)), from);
    if (builtin == clang::Builtin::BImemset ||
        builtin == clang::Builtin::BI__builtin_memset ||
        builtin == clang::Builtin::BI__builtin___memset_chk) {
      clang::Expr::EvalResult value;
      return call->getNumArgs() > 1 &&
             call->getArg(1)->EvaluateAsInt(value, ast) &&
             value.Val.getInt().isZero() &&
             (!unit.typed || wholeObjects(call->getArg(2), from));
    }
    if (builtin == clang::Builtin::BIbzero ||
        builtin == clang::Builtin::BI__builtin_bzero)
      return whole;
    const auto match = core::LibrarySpec::shipped().lookup(
        callee->getName().str(), call->getNumArgs());
    if (!match.has_value())
      return !unit.isSafe(callee->getName()) &&
             !hasAnnotateAttr(*callee, SafeAnnotation);
    const int row = match->rowArgument(index);
    if (row < 0 || static_cast<std::size_t>(row) >= match->entry->params.size())
      return false;
    const core::LibraryParam &parameter =
        match->entry->params[static_cast<std::size_t>(row)];
    if (parameter.effect != core::LibraryParam::Effect::Borrow ||
        parameter.access == core::LibraryParam::Access::None ||
        parameter.access == core::LibraryParam::Access::Read)
      return true;
    // A write: a copy from another argument of the same type.
    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
      if (i == index)
        continue;
      const int other = match->rowArgument(i);
      if (other < 0 ||
          static_cast<std::size_t>(other) >= match->entry->params.size() ||
          match->entry->params[static_cast<std::size_t>(other)].access !=
              core::LibraryParam::Access::Read)
        continue;
      const clang::Expr *source = call->getArg(i)->IgnoreParenCasts();
      if (source->getType()->isPointerType() &&
          sameShape(source->getType()->getPointeeType(), from))
        return whole;
    }
    return false;
  }

  void checkBuiltin(const clang::CallExpr &call,
                    const clang::FunctionDecl &callee) {
    if (callee.hasAttr<clang::ReturnsTwiceAttr>()) {
      forbidden(call.getExprLoc(), "'" + callee.getName().str() + "'");
      return;
    }
    switch (callee.getBuiltinID()) {
    case clang::Builtin::BIlongjmp:
    case clang::Builtin::BI_longjmp:
    case clang::Builtin::BIsiglongjmp:
    case clang::Builtin::BI__builtin_longjmp:
    case clang::Builtin::BI__builtin_setjmp:
    case clang::Builtin::BI__builtin_frame_address:
    case clang::Builtin::BI__builtin_return_address:
      forbidden(call.getExprLoc(), "'" + callee.getName().str() + "'");
      break;
    case clang::Builtin::BI__builtin_va_start:
    case clang::Builtin::BI__builtin_c23_va_start:
      report(call.getExprLoc(), rule::Variadic,
             "a safe function cannot read its variadic arguments",
             "take the arguments by type (an array with its count), or "
             "read them in a WEAVEC_GUARDED helper function");
      break;
    default:
      break;
    }
  }
};

class SafeChecker final : public clang::ASTConsumer {
public:
  SafeChecker(clang::CompilerInstance &compiler, std::shared_ptr<SafeUnit> out,
              bool unitSafe)
      : compiler(compiler), out(std::move(out)), unitSafe(unitSafe) {}

  /// RFC 0042 §5: a typed unit's struct is annotated when its definition
  /// is complete, before the code of any function that uses it is made.
  void HandleTagDeclDefinition(clang::TagDecl *tag) override {
    auto *record = llvm::dyn_cast<clang::RecordDecl>(tag);
    if (!out->typed || record == nullptr ||
        tag->getASTContext().getLangOpts().CPlusPlus ||
        tag->getASTContext().getSourceManager().isInSystemHeader(
            tag->getLocation()))
      return;
    declarationsFor(tag->getASTContext()).annotateFields(*record);
  }

  void HandleTranslationUnit(clang::ASTContext &ast) override {
    if (ast.getLangOpts().CPlusPlus) {
      auto report = [this](clang::SourceLocation at, llvm::StringRef rule,
                           llvm::StringRef message, llvm::StringRef fix) {
        this->report(at, rule, message, fix);
      };
      rejectCxx(ast.getTranslationUnitDecl(), report);
      return;
    }
    Declarations &declarations = declarationsFor(ast);
    out->pointeeSizes = declarations.laterSizes();
    std::vector<const clang::FunctionDecl *> safe;
    std::vector<const clang::RecordDecl *> records;
    collect(ast.getTranslationUnitDecl(), ast, declarations, safe, records);
    // A struct's bounds matter to safe code only: a guarded unit keeps
    // compiling whatever its structs say (Clang checks its own counted_by).
    if (!safe.empty())
      for (const clang::RecordDecl *record : records)
        declarations.checkRecord(*record);
    for (const clang::FunctionDecl *function : safe)
      out->functions[function->getName()].safe = true;
    if (out->typed)
      checkGlobalInits(ast.getTranslationUnitDecl(), ast);
    for (const clang::FunctionDecl *function : safe)
      FunctionCheck(ast, *function, *out,
                    [this](clang::SourceLocation at, llvm::StringRef rule,
                           llvm::StringRef message, llvm::StringRef fix) {
                      report(at, rule, message, fix);
                    })
          .run();
  }

private:
  clang::CompilerInstance &compiler;
  std::shared_ptr<SafeUnit> out;
  bool unitSafe;
  std::set<std::pair<clang::SourceLocation::UIntTy, std::string>> reported;
  std::unique_ptr<Declarations> unitDeclarations;

  /// RFC 0042 §2: a typed unit's globals are initialized by constants
  /// whose counts the checker compares with their pointers' objects.
  void checkGlobalInits(const clang::DeclContext *context,
                        clang::ASTContext &ast) {
    auto reportTo = [this](clang::SourceLocation at, llvm::StringRef rule,
                           llvm::StringRef message, llvm::StringRef fix) {
      report(at, rule, message, fix);
    };
    std::function<void(const clang::Stmt *)> walk =
        [&](const clang::Stmt *statement) {
          if (statement == nullptr)
            return;
          if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(statement))
            if (const clang::RecordDecl *record =
                    list->getType()->getAsRecordDecl())
              checkCountedInit(ast, *list, *record, reportTo);
          for (const clang::Stmt *child : statement->children())
            walk(child);
        };
    for (const clang::Decl *declaration : context->decls()) {
      if (const auto *inner =
              llvm::dyn_cast<clang::LinkageSpecDecl>(declaration))
        checkGlobalInits(inner, ast);
      const auto *variable = llvm::dyn_cast<clang::VarDecl>(declaration);
      if (variable != nullptr && variable->hasGlobalStorage() &&
          variable->hasInit() &&
          !ast.getSourceManager().isInSystemHeader(variable->getLocation()))
        walk(variable->getInit());
    }
  }

  /// An error that names its rule, with the note that says what to write.
  void report(clang::SourceLocation at, llvm::StringRef rule,
              llvm::StringRef message, llvm::StringRef fix) {
    // A macro may expand an argument twice (a fortified memcpy): once per
    // place in the file.
    const clang::SourceLocation file =
        at.isValid() ? compiler.getSourceManager().getFileLoc(at) : at;
    if (!reported.emplace(file.getRawEncoding(), rule.str()).second)
      return;
    clang::DiagnosticsEngine &diagnostics = compiler.getDiagnostics();
    diagnostics.Report(
        at, diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "%0 [weavec-safe:%1]"))
        << message << rule;
    diagnostics.Report(
        at, diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Note, "%0"))
        << fix;
  }

  Declarations &declarationsFor(clang::ASTContext &ast) {
    if (!unitDeclarations)
      unitDeclarations = std::make_unique<Declarations>(
          ast, compiler.getDiagnostics(),
          compiler.hasPreprocessor() ? &compiler.getPreprocessor() : nullptr,
          [this](clang::SourceLocation at, llvm::StringRef rule,
                 llvm::StringRef message,
                 llvm::StringRef fix) { report(at, rule, message, fix); },
          out->typed);
    return *unitDeclarations;
  }

  /// RFC 0041 §12: a C++ unit has no safe functions; a unit compiled with
  /// -fweavec-safe or a function marked WEAVEC_SAFE is an error.
  template <typename Report>
  void rejectCxx(const clang::DeclContext *context, Report &report) {
    for (const clang::Decl *declaration : context->decls()) {
      if (llvm::isa<clang::NamespaceDecl, clang::LinkageSpecDecl,
                    clang::CXXRecordDecl>(declaration)) {
        rejectCxx(llvm::cast<clang::DeclContext>(declaration), report);
        continue;
      }
      const auto *function = declaration->getAsFunction();
      if (function != nullptr && function->doesThisDeclarationHaveABody() &&
          (hasAnnotateAttr(*function, SafeAnnotation) ||
           (unitSafe &&
            !function->getASTContext().getSourceManager().isInSystemHeader(
                function->getLocation()))))
        report(function->getLocation(), rule::Cxx,
               "safe functions are C only (RFC 0041 §12)",
               "compile the function as C, or drop WEAVEC_SAFE");
    }
  }

  bool isSafe(const clang::FunctionDecl &function,
              const clang::SourceManager &sources) const {
    if (hasAnnotateAttr(function, SafeAnnotation))
      return true;
    if (!unitSafe || hasAnnotateAttr(function, GuardedAnnotation) ||
        hasAnnotateAttr(function, UnsafeAnnotation) ||
        function.hasAttr<clang::NoSanitizeAttr>() ||
        function.hasAttr<clang::DisableSanitizerInstrumentationAttr>())
      return false;
    return !sources.isInSystemHeader(function.getLocation());
  }

  void collect(const clang::DeclContext *context, clang::ASTContext &ast,
               Declarations &declarations,
               std::vector<const clang::FunctionDecl *> &safe,
               std::vector<const clang::RecordDecl *> &records) {
    for (const clang::Decl *declaration : context->decls()) {
      if (const auto *inner =
              llvm::dyn_cast<clang::LinkageSpecDecl>(declaration)) {
        collect(inner, ast, declarations, safe, records);
        continue;
      }
      if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(declaration);
          record != nullptr && record->isThisDeclarationADefinition() &&
          !ast.getSourceManager().isInSystemHeader(record->getLocation()))
        records.push_back(record);
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
      if (function == nullptr || function->getName().empty())
        continue;
      const FunctionBounds table = declarations.tableOf(*function);
      const bool annotated =
          table.result.bounded() ||
          llvm::any_of(table.parameters, [](const PointerBounds &p) {
            return p.bounded() || p.escapes || p.lifetimeBound;
          });
      const bool definesSafe = function->doesThisDeclarationHaveABody() &&
                               isSafe(*function, ast.getSourceManager());
      // RFC 0042 §6, §5: a typed unit's table knows every function it
      // declares (single results' sizes), and which are typed elsewhere.
      if (out->typed && !annotated && !definesSafe &&
          !out->functions.contains(function->getName()))
        out->functions[function->getName()] = table;

      const auto earlier = annotatedAt.find(function->getName());
      if (earlier != annotatedAt.end()) {
        // Every declaration with bounds, and a safe definition, says what
        // the first said: the table, the entry's checks and the body's
        // rules would otherwise each read a different one.
        if ((annotated || definesSafe) &&
            !out->functions[function->getName()].sameBounds(table))
          reportMismatch(*function, earlier->second);
      } else if (annotated) {
        FunctionBounds &entry = out->functions[function->getName()];
        const bool wasSafe = entry.safe;
        entry = table;
        entry.safe = wasSafe;
        annotatedAt[function->getName()] = function->getLocation();
      } else if (definesSafe) {
        out->functions[function->getName()] = table;
      }
      if (definesSafe)
        safe.push_back(function);
      if (function->doesThisDeclarationHaveABody() &&
          !ast.getSourceManager().isInSystemHeader(function->getLocation()))
        define(*function, definesSafe, ast.getSourceManager());
      if (out->typed && !function->doesThisDeclarationHaveABody() &&
          hasAnnotateAttr(*function, SafeAnnotation))
        out->functions[function->getName()].declaredTyped = true;
    }
  }

  /// RFC 0042 §10: the function's tier, for the ledger.
  void define(const clang::FunctionDecl &function, bool safeFunction,
              const clang::SourceManager &sources) {
    const char *tier = "guarded";
    if (safeFunction)
      tier = out->typed ? "typed" : "safe";
    else if (hasAnnotateAttr(function, UnsafeAnnotation) ||
             function.hasAttr<clang::NoSanitizeAttr>() ||
             function.hasAttr<clang::DisableSanitizerInstrumentationAttr>())
      tier = "unsafe";
    const clang::PresumedLoc at =
        sources.getPresumedLoc(function.getLocation());
    out->definitions.push_back({.name = function.getName().str(),
                                .tier = tier,
                                .file = at.isValid() ? at.getFilename() : "",
                                .line = at.isValid() ? at.getLine() : 0});
  }

  void reportMismatch(const clang::FunctionDecl &function,
                      clang::SourceLocation earlier) {
    clang::DiagnosticsEngine &diagnostics = compiler.getDiagnostics();
    const std::string name = function.getName().str();
    diagnostics.Report(
        function.getLocation(),
        diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                    "%0 [weavec-safe:%1]"))
        << "this declaration of '" + name +
               "' gives its pointers other bounds than an earlier one"
        << rule::BadAnnotation;
    diagnostics.Report(earlier, diagnostics.getCustomDiagID(
                                    clang::DiagnosticsEngine::Note, "%0"))
        << "write the same bounds (WEAVEC_COUNTED_BY, WEAVEC_ESCAPES, "
           "WEAVEC_LIFETIMEBOUND and the rest) on every declaration of '" +
               name + "', as here";
  }

  /// Where each function's first declaration with bounds is.
  llvm::StringMap<clang::SourceLocation> annotatedAt;
};

} // namespace

std::unique_ptr<clang::ASTConsumer>
createSafeChecker(clang::CompilerInstance &compiler,
                  std::shared_ptr<SafeUnit> out, bool unitSafe) {
  return std::make_unique<SafeChecker>(compiler, std::move(out), unitSafe);
}

} // namespace weavec::frontend
