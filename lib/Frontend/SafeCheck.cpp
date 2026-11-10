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
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"

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
} // namespace rule

/// Origins of a pointer value: the borrowed parameters it may derive from.
using Origins = llvm::SmallPtrSet<const clang::ParmVarDecl *, 2>;

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
    return annotation.name != EscapesAnnotation;
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

/// `char`, `signed char`, `unsigned char` or `void`.
static bool isByteType(clang::QualType type) {
  const clang::QualType canonical = type.getCanonicalType();
  return canonical->isVoidType() || canonical->isCharType();
}

namespace {

/// The parameters' and result's bounds a declaration gives (§2).
class Declarations {
public:
  Declarations(clang::ASTContext &ast, clang::DiagnosticsEngine &diagnostics,
               std::function<void(clang::SourceLocation, llvm::StringRef,
                                  llvm::StringRef, llvm::StringRef)>
                   report)
      : ast(ast), diagnostics(diagnostics), report(std::move(report)) {}

  PointerBounds boundsOf(const clang::FunctionDecl &function,
                         clang::QualType type, clang::SourceLocation at) {
    PointerBounds bounds;
    if (!type->isPointerType())
      return bounds;
    bounds.kind = PointerBounds::Kind::Single;
    const clang::QualType pointee = type->getPointeeType();
    for (const TypeAnnotation &annotation : annotationsOf(type, ast)) {
      const llvm::StringRef name = annotation.name;
      if (name == EscapesAnnotation) {
        bounds.escapes = true;
        continue;
      }
      if (name == NullTerminatedAnnotation) {
        bounds.kind = PointerBounds::Kind::NullTerminated;
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
      if (parameter < 0 ||
          !function.getParamDecl(static_cast<unsigned>(parameter))
               ->getType()
               ->isIntegerType()) {
        report(at, rule::BadAnnotation,
               "'" + annotation.argument +
                   "' in the bound names no integer parameter of '" +
                   function.getName().str() + "'",
               "name an integer parameter of the function, or a constant");
        bounds.kind = PointerBounds::Kind::Single;
        continue;
      }
      bounds.count = parameter;
    }
    return bounds;
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

private:
  clang::ASTContext &ast;
  [[maybe_unused]] clang::DiagnosticsEngine &diagnostics;
  std::function<void(clang::SourceLocation, llvm::StringRef, llvm::StringRef,
                     llvm::StringRef)>
      report;

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
    return true;
  }

  bool VisitUnaryOperator(clang::UnaryOperator *op) {
    if (op->isIncrementDecrementOp() &&
        op->getSubExpr()->getType()->isPointerType() &&
        !bounded(op->getSubExpr()))
      single(op->getExprLoc(), op->getSubExpr());
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
    }
    if (op->getOpcode() == clang::BO_Assign && !isLocalSlot(op->getLHS()))
      storesBorrowed(op->getRHS(), op->getExprLoc());
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
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr)
      return true;
    checkBuiltin(*call, *callee);
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
      if (bounds != nullptr && bounds->bounded() && !bounded(argument) &&
          !argument->isNullPointerConstant(
              ast, clang::Expr::NPC_ValueDependentIsNotNull))
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
                 "'" + (*origins.begin())->getName().str() +
                     "' is borrowed for this call and cannot be passed to "
                     "the escaping parameter '" +
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
    for (const clang::ParmVarDecl *origin : originsOf(value))
      if (!origin->hasAttr<clang::LifetimeBoundAttr>()) {
        report(value->getExprLoc(), rule::Escape,
               "'" + origin->getName().str() +
                   "' is borrowed for this call and cannot be returned",
               "mark the parameter WEAVEC_LIFETIMEBOUND (after its name), so "
               "that callers treat the result as borrowed from it");
        break;
      }
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *variable) {
    if (variable->hasInit() && !variable->hasLocalStorage())
      storesBorrowed(variable->getInit(), variable->getLocation());
    return true;
  }

  bool VisitInitListExpr(clang::InitListExpr *list) {
    for (const clang::Expr *element : list->inits())
      if (element != nullptr && element->getType()->isPointerType())
        storesBorrowed(element, element->getExprLoc());
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
    for (bool changed = true; changed;) {
      changed = false;
      for (auto &[variable, values] : assigned) {
        if (addressed.contains(variable))
          continue;
        bool isBounded = true;
        Origins origins = localOrigins[variable];
        const std::size_t before = origins.size();
        for (const clang::Expr *value : values) {
          isBounded = isBounded && bounded(value);
          for (const clang::ParmVarDecl *origin : originsOf(value))
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
              llvm::dyn_cast<clang::VarDecl>(reference->getDecl()))
        if (const auto found = localOrigins.find(variable);
            found != localOrigins.end())
          origins.insert(found->second.begin(), found->second.end());
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
  }

  void storesBorrowed(const clang::Expr *value, clang::SourceLocation at) {
    const Origins origins = originsOf(value);
    if (origins.empty())
      return;
    const std::string name = (*origins.begin())->getName().str();
    report(at, rule::Escape,
           "'" + name +
               "' is borrowed for this call and cannot be stored where it "
               "outlives the call",
           "mark the parameter WEAVEC_ESCAPES (after its '*'; callers then "
           "cannot pass a stack address), or store a copy of what it points "
           "to");
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
    llvm::StringRef spelled = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(expression->getSourceRange()),
        sources, ast.getLangOpts());
    if (spelled.empty() || spelled.size() > 48)
      return "this pointer";
    return spelled.str();
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
    if (clang::ASTContext::hasSameUnqualifiedType(from, to))
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
    if (builtin == clang::Builtin::BImemset ||
        builtin == clang::Builtin::BI__builtin_memset ||
        builtin == clang::Builtin::BI__builtin___memset_chk) {
      clang::Expr::EvalResult value;
      return call->getNumArgs() > 1 &&
             call->getArg(1)->EvaluateAsInt(value, ast) &&
             value.Val.getInt().isZero();
    }
    if (builtin == clang::Builtin::BIbzero ||
        builtin == clang::Builtin::BI__builtin_bzero)
      return true;
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
      const clang::Expr *source = call->getArg(i)->IgnoreParenImpCasts();
      if (source->getType()->isPointerType() &&
          clang::ASTContext::hasSameUnqualifiedType(
              source->getType()->getPointeeType(), from))
        return true;
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

  void HandleTranslationUnit(clang::ASTContext &ast) override {
    clang::DiagnosticsEngine &diagnostics = compiler.getDiagnostics();
    const unsigned errorId = diagnostics.getCustomDiagID(
        clang::DiagnosticsEngine::Error, "%0 [weavec-safe:%1]");
    const unsigned noteId =
        diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Note, "%0");
    std::set<std::pair<clang::SourceLocation::UIntTy, std::string>> reported;
    auto report = [&](clang::SourceLocation at, llvm::StringRef rule,
                      llvm::StringRef message, llvm::StringRef fix) {
      if (!reported.emplace(at.getRawEncoding(), rule.str()).second)
        return;
      diagnostics.Report(at, errorId) << message << rule;
      diagnostics.Report(at, noteId) << fix;
    };
    if (ast.getLangOpts().CPlusPlus) {
      rejectCxx(ast.getTranslationUnitDecl(), report);
      return;
    }
    Declarations declarations(ast, diagnostics, report);
    std::vector<const clang::FunctionDecl *> safe;
    collect(ast.getTranslationUnitDecl(), ast, declarations, safe);
    for (const clang::FunctionDecl *function : safe)
      out->functions[function->getName()].safe = true;
    for (const clang::FunctionDecl *function : safe)
      FunctionCheck(ast, *function, *out, report).run();
  }

private:
  clang::CompilerInstance &compiler;
  std::shared_ptr<SafeUnit> out;
  bool unitSafe;

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
               std::vector<const clang::FunctionDecl *> &safe) {
    for (const clang::Decl *declaration : context->decls()) {
      if (const auto *inner =
              llvm::dyn_cast<clang::LinkageSpecDecl>(declaration)) {
        collect(inner, ast, declarations, safe);
        continue;
      }
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
      if (function == nullptr || function->getName().empty())
        continue;
      const FunctionBounds table = declarations.tableOf(*function);
      const bool annotated =
          table.result.bounded() ||
          llvm::any_of(table.parameters, [](const PointerBounds &p) {
            return p.bounded() || p.escapes || p.lifetimeBound;
          });
      if (annotated) {
        FunctionBounds &entry = out->functions[function->getName()];
        const bool wasSafe = entry.safe;
        entry = table;
        entry.safe = wasSafe;
      }
      if (function->doesThisDeclarationHaveABody() &&
          isSafe(*function, ast.getSourceManager())) {
        if (!annotated)
          out->functions[function->getName()] = table;
        safe.push_back(function);
      }
    }
  }
};

} // namespace

std::unique_ptr<clang::ASTConsumer>
createSafeChecker(clang::CompilerInstance &compiler,
                  std::shared_ptr<SafeUnit> out, bool unitSafe) {
  return std::make_unique<SafeChecker>(compiler, std::move(out), unitSafe);
}

} // namespace weavec::frontend
