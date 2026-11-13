//===- SafeCxx.cpp - Safe C++'s units and its expression rules ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0044. The safe functions of a C++ unit (§1): every function defined
// outside system headers under `-fweavec-safe`, each instantiation of a
// template defined there, and what `WEAVEC_SAFE` and `WEAVEC_GUARDED` say on
// a function or a class. Each is checked against the rules of single
// expressions here (§4: no raw owner, no pointer arithmetic, no cast that
// forges a pointer) and against the flow rules of SafeCxxFlow.cpp, its
// lambdas with it. The unit's functions and its classes' members are listed
// for the ledger (§12). Nothing here reaches the guard passes: a safe C++
// function is compiled as a guarded one.
//
//===----------------------------------------------------------------------===//

#include "SafeCxxKinds.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "clang/AST/ASTLambda.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/Builtins.h"

#include "llvm/ADT/DenseSet.h"

#include <set>

namespace weavec::frontend {

namespace {

namespace rule {
constexpr llvm::StringLiteral RawOwner = "raw-owner";
constexpr llvm::StringLiteral IndexSingle = "index-single";
constexpr llvm::StringLiteral IntToPointer = "int-to-pointer";
constexpr llvm::StringLiteral PointerCast = "pointer-cast";
constexpr llvm::StringLiteral PointerBytes = "pointer-bytes";
constexpr llvm::StringLiteral UnionPointer = "union-pointer";
constexpr llvm::StringLiteral Variadic = "variadic";
constexpr llvm::StringLiteral NoAsm = "no-asm";
constexpr llvm::StringLiteral UnsafeBlock = "unsafe-block";
constexpr llvm::StringLiteral BadAnnotation = "bad-annotation";
constexpr llvm::StringLiteral NotModeled = "not-modeled";
} // namespace rule

constexpr llvm::StringLiteral Helper = "; or keep this function WEAVEC_GUARDED";

/// §4: the rules of single expressions, over one safe function's body and
/// the lambdas in it.
class CxxRules final : public clang::RecursiveASTVisitor<CxxRules> {
public:
  CxxRules(clang::ASTContext &ast, CxxKinds &kinds, const RuleReport &report)
      : ast(ast), kinds(kinds), report(report),
        sources(ast.getSourceManager()) {}

  void run(const clang::FunctionDecl &function) {
    programArguments = function.isMain();
    for (const clang::ParmVarDecl *parameter : function.parameters())
      checkAnnotations(parameter->getType(), parameter->getLocation());
    checkAnnotations(function.getReturnType(), function.getLocation());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    TraverseStmt(const_cast<clang::Stmt *>(function.getBody()));
  }

  // RecursiveASTVisitor's names.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitCXXNewExpr(clang::CXXNewExpr *expression) {
    error(expression->getBeginLoc(), rule::RawOwner,
          "'new' makes a block no object owns",
          expression->isArray()
              ? "a 'std::vector' or 'std::make_unique<T[]>(n)' owns an array"
              : "'std::make_unique<T>(...)' returns its owner");
    return true;
  }

  bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *expression) {
    error(expression->getBeginLoc(), rule::RawOwner,
          "'delete' releases a block by hand",
          "let the object that owns the block release it: a "
          "'std::unique_ptr', a container, a member");
    return true;
  }

  bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *call) {
    const clang::CXXMethodDecl *method = call->getMethodDecl();
    if (method == nullptr)
      return true;
    if (llvm::isa<clang::CXXDestructorDecl>(method)) {
      error(call->getExprLoc(), rule::RawOwner,
            "a destructor is called by hand",
            "an object is destroyed when its owner is: 'reset()' a "
            "'std::optional' or a 'std::unique_ptr' instead");
      return true;
    }
    // A member function of a library owner that is not const, has no const
    // twin and returns a raw pointer hands its block away (`release()`).
    const clang::QualType result = method->getReturnType();
    if (CxxKinds::libraryOwner(method->getParent()) && !method->isConst() &&
        !method->isStatic() && result->isPointerType() &&
        !result->isFunctionPointerType() && !kinds.accessor(*method))
      error(call->getExprLoc(), rule::RawOwner,
            "'" + method->getNameAsString() +
                "' takes a block away from the object that owns it",
            "move the owner itself ('std::move(p)') to where the block "
            "should go");
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *construct) {
    const clang::CXXConstructorDecl *constructor = construct->getConstructor();
    const clang::CXXRecordDecl *record = constructor->getParent();
    if (construct->getNumArgs() == 0 || constructor->getNumParams() == 0 ||
        !isLibrary(*record))
      return true;
    const clang::Expr *first = construct->getArg(0)->IgnoreParenImpCasts();
    const clang::QualType given = first->getType();
    if (!given->isPointerType() || given->isFunctionPointerType() ||
        argument(first) ||
        llvm::isa<clang::StringLiteral, clang::CXXNullPtrLiteralExpr>(first))
      return true;
    const auto written = static_cast<unsigned>(
        llvm::count_if(construct->arguments(), [](const clang::Expr *argument) {
          return !llvm::isa<clang::CXXDefaultArgExpr>(argument);
        }));
    const clang::QualType second =
        written > 1 ? construct->getArg(1)->getType() : clang::QualType();
    if ((record->hasAttr<clang::OwnerAttr>() || CxxKinds::shared(record)) &&
        !kinds.viewClass(record)) {
      // A smart pointer made from a raw pointer (and perhaps a deleter).
      if (!given->getPointeeType().isConstQualified() &&
          (written == 1 ||
           !(second->isPointerType() || second->isIntegerType())) &&
          constructor->getParamDecl(0)->getType()->isPointerType())
        error(construct->getExprLoc(), rule::RawOwner,
              "a raw pointer is handed to '" + record->getNameAsString() +
                  "' to own",
              "make the block with its owner: 'std::make_unique', "
              "'std::make_shared'");
    } else if (record->hasAttr<clang::PointerAttr>() && written > 1 &&
               (second->isIntegerType() || second->isPointerType())) {
      // A span or a string view made from a pointer and a count the
      // library cannot check.
      error(construct->getExprLoc(), rule::IndexSingle,
            "a '" + record->getNameAsString() +
                "' is made from a pointer and a size nothing checks",
            "make it from the container or the array itself, and narrow it "
            "with its own functions ('first', 'subspan', 'substr')");
    }
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *call) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr || callee->getIdentifier() == nullptr)
      return true;
    if (callee->hasAttr<clang::ReturnsTwiceAttr>()) {
      error(call->getExprLoc(), rule::NotModeled,
            "'" + callee->getNameAsString() +
                "' returns twice, which the rules do not follow",
            "use an exception or a return value");
      return true;
    }
    switch (callee->getBuiltinID()) {
    case clang::Builtin::BI__builtin_launder:
      error(call->getExprLoc(), rule::PointerCast,
            "'std::launder' makes a pointer to an object the rules did "
            "not see created",
            "use the object through the name or the owner that made it");
      return true;
    case clang::Builtin::BI__builtin_va_start:
    case clang::Builtin::BI__builtin_c23_va_start:
      error(call->getExprLoc(), rule::Variadic,
            "a safe function cannot read its variadic arguments",
            "take a parameter pack, a 'std::initializer_list' or a "
            "'std::span'",
            /*expanded=*/true);
      return true;
    default:
      break;
    }
    if (!callee->isExternC() && !callee->isInStdNamespace() &&
        !isLibrary(*callee))
      return true;
    const auto match = core::LibrarySpec::shipped().lookup(
        callee->getName().str(), call->getNumArgs());
    if (!match.has_value())
      return true;
    const core::LibraryEntry &row = *match->entry;
    const bool releases =
        llvm::any_of(row.params, [](const core::LibraryParam &parameter) {
          return parameter.effect != core::LibraryParam::Effect::Borrow;
        });
    if (row.owned || releases) {
      error(call->getExprLoc(), rule::RawOwner,
            "'" + callee->getNameAsString() +
                (releases ? "' releases a block by hand"
                          : "' returns a block no object owns"),
            "use a container, a 'std::string' or 'std::make_unique'; wrap a "
            "C library's allocation in a WEAVEC_GUARDED class that owns it");
      return true;
    }
    // The bytes of an object that is not trivially copyable.
    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
      const int index = match->rowArgument(i);
      const core::LibraryParam *parameter =
          index < 0 ? nullptr : row.param(static_cast<unsigned>(index));
      if (parameter == nullptr ||
          parameter->type != core::LibraryParam::Type::Pointer ||
          !parameter->bytes.has_value())
        continue;
      const clang::QualType given =
          call->getArg(i)->IgnoreParenImpCasts()->getType();
      const clang::QualType target =
          given->isPointerType() ? given->getPointeeType() : clang::QualType();
      if (target.isNull() || !target->isRecordType() ||
          target.isTriviallyCopyableType(ast))
        continue;
      error(
          call->getArg(i)->getExprLoc(), rule::PointerBytes,
          "'" + callee->getNameAsString() +
              "' reads or writes the bytes "
              "of a '" +
              target.getUnqualifiedType().getAsString(ast.getPrintingPolicy()) +
              "', which is not trivially copyable",
          "copy, assign or compare the object itself");
      break;
    }
    return true;
  }

  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *subscript) {
    const clang::Expr *base = subscript->getBase()->IgnoreParens();
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(base);
        cast != nullptr &&
        cast->getCastKind() == clang::CK_ArrayToPointerDecay) {
      indexedArrays.insert(cast);
      return true;
    }
    if (base->getType()->isPointerType() && !libraryIterator(base->getType()) &&
        !argument(base))
      error(subscript->getRBracketLoc(), rule::IndexSingle,
            "a pointer is indexed, and it points to one object",
            "index a 'std::span', a 'std::array' or a container, whose "
            "bounds are checked");
    return true;
  }

  bool VisitBinaryOperator(clang::BinaryOperator *binary) {
    const bool arithmetic =
        (binary->isAdditiveOp() || binary->getOpcode() == clang::BO_AddAssign ||
         binary->getOpcode() == clang::BO_SubAssign);
    const clang::QualType left = binary->getLHS()->getType();
    const clang::QualType right = binary->getRHS()->getType();
    // The difference of two pointers forges nothing.
    if (arithmetic && (left->isPointerType() != right->isPointerType()) &&
        !libraryIterator(left->isPointerType() ? left : right) &&
        !argument(left->isPointerType() ? binary->getLHS() : binary->getRHS()))
      pointerArithmetic(binary->getOperatorLoc());
    return true;
  }

  bool VisitUnaryOperator(clang::UnaryOperator *unary) {
    const clang::QualType type = unary->getSubExpr()->getType();
    if (unary->isIncrementDecrementOp() && type->isPointerType() &&
        !libraryIterator(type) && !argument(unary->getSubExpr()))
      pointerArithmetic(unary->getOperatorLoc());
    return true;
  }

  bool VisitCastExpr(clang::CastExpr *cast) {
    const clang::QualType to = cast->getType();
    const clang::QualType from = cast->getSubExpr()->getType();
    switch (cast->getCastKind()) {
    case clang::CK_IntegralToPointer:
      error(cast->getExprLoc(), rule::IntToPointer,
            "an integer cannot become a pointer in a safe function",
            "keep the pointer itself rather than its address as an integer");
      return true;
    case clang::CK_ArrayToPointerDecay:
      // A string literal, and an array of constant characters, is read as
      // a string.
      if (!indexedArrays.contains(cast) &&
          !llvm::isa<clang::StringLiteral, clang::PredefinedExpr>(
              cast->getSubExpr()->IgnoreParens()) &&
          !(to->getPointeeType().isConstQualified() &&
            to->getPointeeType()->isAnyCharacterType()) &&
          // Where `va_list` is an array (x86-64), naming one decays it;
          // `variadic` is the rule for it.
          !ast.hasSameType(cast->getSubExpr()->getType(),
                           ast.getBuiltinVaListType()))
        error(cast->getExprLoc(), rule::IndexSingle,
              "an array becomes a pointer that has lost its size",
              "pass a 'std::span' of it, or make it a 'std::array' and "
              "pass that");
      return true;
    case clang::CK_BaseToDerived:
      if (llvm::isa<clang::ExplicitCastExpr>(cast))
        castError(cast->getExprLoc(),
                  "a base is converted to a derived class without a check",
                  "'dynamic_cast' checks the object's type; a 'std::variant' "
                  "names the alternatives");
      return true;
    case clang::CK_BitCast:
    case clang::CK_LValueBitCast:
      if (llvm::isa<clang::ExplicitCastExpr>(cast) &&
          (to->isPointerType() || cast->isGLValue()))
        castError(cast->getExprLoc(),
                  "a pointer is converted to a pointer to another type",
                  "use the object through its own type; 'std::variant' and "
                  "'dynamic_cast' are the checked conversions");
      return true;
    case clang::CK_NoOp:
      if (llvm::isa<clang::CXXConstCastExpr>(cast) ||
          (llvm::isa<clang::CStyleCastExpr, clang::CXXFunctionalCastExpr>(
               cast) &&
           (to->isPointerType() || to->isReferenceType() ||
            cast->isGLValue()) &&
           dropsConst(from, to, cast->isGLValue())))
        castError(cast->getExprLoc(), "a cast removes 'const'",
                  "take the object as non-'const' where it is changed");
      return true;
    default:
      if (llvm::isa<clang::CXXReinterpretCastExpr>(cast))
        castError(cast->getExprLoc(),
                  "'reinterpret_cast' reads an object as another type",
                  "use the object through its own type, or 'std::bit_cast' for "
                  "values that hold no pointer");
      return true;
    }
  }

  bool VisitBuiltinBitCastExpr(clang::BuiltinBitCastExpr *cast) {
    if (holdsPointer(cast->getType()) ||
        holdsPointer(cast->getSubExpr()->getType()))
      castError(cast->getExprLoc(), "'std::bit_cast' makes or reads a pointer",
                "convert values that hold no pointer");
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *member) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr || !field->getParent()->isUnion())
      return true;
    for (const clang::FieldDecl *other : field->getParent()->fields())
      if (holdsPointer(other->getType()) ||
          kinds.holdsOwners(other->getType()) || kinds.view(other->getType())) {
        error(member->getMemberLoc(), rule::UnionPointer,
              "union member '" + field->getNameAsString() +
                  "' shares its bytes with '" + other->getNameAsString() +
                  "', which holds a pointer or an owner",
              "'std::variant' keeps which alternative is live and checks "
              "it");
        break;
      }
    return true;
  }

  bool VisitVAArgExpr(clang::VAArgExpr *expression) {
    error(expression->getBeginLoc(), rule::Variadic,
          "a safe function cannot read its variadic arguments",
          "take a parameter pack, a 'std::initializer_list' or a "
          "'std::span'",
          /*expanded=*/true);
    return true;
  }

  bool VisitGCCAsmStmt(clang::GCCAsmStmt *statement) {
    error(statement->getBeginLoc(), rule::NoAsm,
          "inline assembly cannot be in a safe function",
          "move it into a WEAVEC_GUARDED helper function");
    return true;
  }

  bool VisitGotoStmt(clang::GotoStmt *statement) {
    return notModeled(statement->getBeginLoc(), "'goto'",
                      "use a loop, a function or a scope that ends");
  }
  bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *statement) {
    return notModeled(statement->getBeginLoc(), "a computed 'goto'",
                      "use a 'switch' or a table of functions");
  }
  bool VisitStmtExpr(clang::StmtExpr *expression) {
    return notModeled(expression->getBeginLoc(), "a statement expression",
                      "use a lambda called in place");
  }

  bool VisitAttributedStmt(clang::AttributedStmt *statement) {
    for (const clang::Attr *attribute : statement->getAttrs())
      if (const auto *annotate = llvm::dyn_cast<clang::AnnotateAttr>(attribute);
          annotate != nullptr && annotate->getAnnotation() == UnsafeAnnotation)
        error(statement->getBeginLoc(), rule::UnsafeBlock,
              "a WEAVEC_UNSAFE block cannot be inside a safe function",
              "move the block into a WEAVEC_GUARDED or WEAVEC_UNSAFE "
              "function that the safe one calls",
              /*expanded=*/true);
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  clang::ASTContext &ast;
  CxxKinds &kinds;
  const RuleReport &report;
  const clang::SourceManager &sources;
  /// Decays that are the base of a subscript: the array is indexed in place.
  llvm::DenseSet<const clang::Expr *> indexedArrays;
  /// The function is `main`, whose `argv` is the one array a program is
  /// handed as a pointer and a count.
  bool programArguments = false;

  [[nodiscard]] bool argument(const clang::Expr *expression) const {
    const auto *named =
        llvm::dyn_cast<clang::DeclRefExpr>(expression->IgnoreParenImpCasts());
    return programArguments && named != nullptr &&
           llvm::isa<clang::ParmVarDecl>(named->getDecl());
  }

  /// `expanded`: the construct is a library macro's own (`va_start`,
  /// WEAVEC_UNSAFE), reported where the function wrote the macro.
  void error(clang::SourceLocation at, llvm::StringRef id,
             const std::string &message, const std::string &fix,
             bool expanded = false) {
    if (expanded)
      at = sources.getExpansionLoc(at);
    // What a system header's macro expands to is otherwise the library's.
    if (at.isInvalid() ||
        sources.isInSystemHeader(sources.getSpellingLoc(at)) ||
        (!expanded && sources.isInSystemMacro(at)))
      return;
    report(at, id, message, at, fix + Helper.str());
  }

  void pointerArithmetic(clang::SourceLocation at) {
    error(at, rule::IndexSingle,
          "arithmetic on a pointer, which points to one object",
          "walk a 'std::span' or a container with its iterators or an "
          "index");
  }

  void castError(clang::SourceLocation at, const std::string &message,
                 const std::string &fix) {
    error(at, rule::PointerCast, message, fix);
  }

  bool notModeled(clang::SourceLocation at, const std::string &what,
                  const std::string &fix) {
    error(at, rule::NotModeled, what + " is not followed by the rules", fix);
    return true;
  }

  [[nodiscard]] static bool dropsConst(clang::QualType from, clang::QualType to,
                                       bool glvalue) {
    if (glvalue)
      return from.isConstQualified() && !to.isConstQualified();
    if (!from->isPointerType() || !to->isPointerType())
      return false;
    return from->getPointeeType().isConstQualified() &&
           !to->getPointeeType().isConstQualified();
  }

  [[nodiscard]] bool holdsPointer(clang::QualType type) const {
    type = type.getCanonicalType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    if (type->isAnyPointerType() || type->isMemberPointerType() ||
        type->isReferenceType())
      return true;
    const clang::RecordDecl *record = type->getAsRecordDecl();
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr)
      return false;
    return llvm::any_of(record->fields(),
                        [this](const clang::FieldDecl *field) {
                          return holdsPointer(field->getType());
                        });
  }

  /// A pointer type spelled as a member typedef of a library class (a
  /// container's `iterator` that happens to be a pointer): arithmetic on
  /// it is the iterator's.
  [[nodiscard]] bool libraryIterator(clang::QualType type) const {
    for (unsigned depth = 0; depth < 16 && !type.isNull(); ++depth) {
      if (const auto *alias =
              llvm::dyn_cast<clang::TypedefType>(type.getTypePtr())) {
        const clang::TypedefNameDecl *declaration = alias->getDecl();
        if (isLibrary(*declaration) &&
            llvm::isa<clang::CXXRecordDecl>(declaration->getDeclContext()))
          return true;
      }
      const clang::QualType next = type.getSingleStepDesugaredType(ast);
      if (next == type)
        break;
      type = next;
    }
    return false;
  }

  void checkAnnotations(clang::QualType type, clang::SourceLocation at) {
    if (!type->isPointerType())
      return;
    for (const TypeAnnotation &annotation : annotationsOf(type, ast))
      if (annotation.name != EscapesAnnotation &&
          annotation.name != BorrowedAnnotation) {
        error(at, rule::BadAnnotation,
              "'" + annotation.name.str() +
                  "' is one of Safe C's annotations, which a safe C++ "
                  "function does not use",
              "a pointer with a count is a 'std::span'; an owner is a "
              "'std::unique_ptr' or a container");
        return;
      }
  }
};

/// §1: the functions the unit defines in its own files, and the lambdas
/// and instantiations among them.
class CxxUnit final : public clang::RecursiveASTVisitor<CxxUnit> {
public:
  CxxUnit(clang::ASTContext &ast, bool unitSafe, SafeUnit &unit,
          const RuleReport &report)
      : ast(ast), unitSafe(unitSafe), unit(unit), report(report), kinds(ast),
        rules(ast, kinds, report) {}

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  [[nodiscard]] static bool shouldVisitTemplateInstantiations() { return true; }

  bool VisitFunctionDecl(clang::FunctionDecl *function) {
    if (!function->doesThisDeclarationHaveABody() ||
        function->isDependentContext() || function->isDefaulted() ||
        function->isImplicit() || isLibrary(*function))
      return true;
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
        method != nullptr && method->getParent()->isLambda())
      return true;
    const bool safe = isSafe(*function);
    const std::size_t index = define(*function, safe);
    if (safe && !check(*function))
      unit.definitions[index].tier = "unsettled";
    return true;
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl *record) {
    if (record->isThisDeclarationADefinition() && !record->isLambda() &&
        !record->isDependentContext() && !isLibrary(*record) &&
        listed.insert(record->getCanonicalDecl()).second)
      listFields(*record);
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  clang::ASTContext &ast;
  bool unitSafe;
  SafeUnit &unit;
  const RuleReport &report;
  CxxKinds kinds;
  CxxRules rules;
  std::set<std::pair<std::string, unsigned>> defined;
  llvm::DenseSet<const clang::CXXRecordDecl *> listed;

  /// §1: what the function or the classes around it say, else the unit.
  [[nodiscard]] bool isSafe(const clang::FunctionDecl &function) const {
    if (hasAnnotate(function, UnsafeAnnotation) ||
        function.hasAttr<clang::NoSanitizeAttr>() ||
        function.hasAttr<clang::DisableSanitizerInstrumentationAttr>())
      return false;
    for (const clang::DeclContext *context = &function; context != nullptr;
         context = context->getParent()) {
      const auto *declaration = llvm::dyn_cast<clang::Decl>(context);
      if (declaration == nullptr ||
          !llvm::isa<clang::FunctionDecl, clang::CXXRecordDecl>(declaration))
        continue;
      if (hasAnnotate(*declaration, SafeAnnotation))
        return true;
      if (hasAnnotate(*declaration, GuardedAnnotation))
        return false;
    }
    return unitSafe;
  }

  [[nodiscard]] static bool unsafe(const clang::FunctionDecl &function) {
    return hasAnnotate(function, UnsafeAnnotation) ||
           function.hasAttr<clang::NoSanitizeAttr>() ||
           function.hasAttr<clang::DisableSanitizerInstrumentationAttr>();
  }

  /// §12: the function's tier, for the ledger. Its index in the list.
  std::size_t define(const clang::FunctionDecl &function, bool safe) {
    const clang::PresumedLoc at =
        ast.getSourceManager().getPresumedLoc(function.getLocation());
    std::string name = function.getQualifiedNameAsString();
    const unsigned line = at.isValid() ? at.getLine() : 0;
    if (!defined.emplace(name, line).second)
      for (std::size_t i = 0; i < unit.definitions.size(); ++i)
        if (unit.definitions[i].name == name &&
            unit.definitions[i].line == line)
          return i;
    const char *tier = "guarded";
    if (safe)
      tier = "safe";
    else if (unsafe(function))
      tier = "unsafe";
    unit.definitions.push_back({.name = std::move(name),
                                .tier = tier,
                                .file = at.isValid() ? at.getFilename() : "",
                                .line = line,
                                .owns = false});
    return unit.definitions.size() - 1;
  }

  /// The rules for one function and the lambdas written in it. Whether
  /// every dataflow settled.
  bool check(const clang::FunctionDecl &function) {
    const clang::Stmt *body = function.getBody();
    if (body == nullptr)
      return true;
    if (llvm::isa<clang::CoroutineBodyStmt>(body)) {
      report(function.getLocation(), rule::NotModeled,
             "a coroutine is not checked", function.getLocation(),
             "a borrow kept across a suspension is not followed; keep this "
             "function WEAVEC_GUARDED");
      return true;
    }
    rules.run(function);
    bool settled = checkCxxFlow(ast, kinds, function, *body, report);
    std::vector<const clang::LambdaExpr *> lambdas;
    collectLambdas(body, lambdas);
    for (const clang::LambdaExpr *lambda : lambdas) {
      if (const clang::FunctionTemplateDecl *generic =
              lambda->getDependentCallOperator()) {
        for (const clang::FunctionDecl *instance : generic->specializations())
          if (instance->hasBody())
            settled &= checkCxxFlow(ast, kinds, *instance, *instance->getBody(),
                                    report);
      } else if (const clang::CXXMethodDecl *call = lambda->getCallOperator();
                 call != nullptr && call->hasBody()) {
        settled &= checkCxxFlow(ast, kinds, *call, *call->getBody(), report);
      }
    }
    return settled;
  }

  static void collectLambdas(const clang::Stmt *statement,
                             std::vector<const clang::LambdaExpr *> &lambdas) {
    if (statement == nullptr)
      return;
    if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(statement)) {
      lambdas.push_back(lambda);
      collectLambdas(lambda->getBody(), lambdas);
    }
    for (const clang::Stmt *child : statement->children())
      collectLambdas(child, lambdas);
  }

  [[nodiscard]] static bool sharedConst(clang::QualType type) {
    const auto *specialization =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            type->getAsCXXRecordDecl());
    if (specialization == nullptr ||
        specialization->getTemplateArgs().size() == 0)
      return false;
    const clang::TemplateArgument &first = specialization->getTemplateArgs()[0];
    return first.getKind() == clang::TemplateArgument::Type &&
           first.getAsType().isConstQualified();
  }

  /// §12: each member of the unit's classes that owns, borrows, refers,
  /// shares or erases.
  void listFields(const clang::CXXRecordDecl &record) {
    for (const clang::FieldDecl *field : record.fields()) {
      clang::QualType type = field->getType();
      while (const clang::ArrayType *array = ast.getAsArrayType(type))
        type = array->getElementType();
      const clang::CXXRecordDecl *of = type->getAsCXXRecordDecl();
      const char *kind = nullptr;
      if (type->isFunctionPointerType())
        continue;
      if (type->isReferenceType() || kinds.viewClass(of) ||
          (type->isPointerType() &&
           hasAnnotation(annotationsOf(field->getType(), ast),
                         BorrowedAnnotation)))
        kind = "borrowed";
      else if (type->isPointerType())
        kind = "reference";
      else if (kinds.erased(type))
        kind = "erased";
      else if (CxxKinds::shared(of))
        // A shared object that is const is changed by no holder.
        kind = sharedConst(type) ? "owner" : "shared";
      else if (kinds.holdsOwners(type))
        kind = "owner";
      if (kind == nullptr)
        continue;
      const clang::PresumedLoc at =
          ast.getSourceManager().getPresumedLoc(field->getLocation());
      unit.fields.push_back({.record = record.getQualifiedNameAsString(),
                             .field = field->getNameAsString(),
                             .kind = kind,
                             .file = at.isValid() ? at.getFilename() : "",
                             .line = at.isValid() ? at.getLine() : 0});
    }
  }
};

} // namespace

void checkCxx(clang::ASTContext &ast, bool unitSafe, SafeUnit &unit,
              const RuleReport &report) {
  CxxUnit(ast, unitSafe, unit, report)
      .TraverseDecl(ast.getTranslationUnitDecl());
}

} // namespace weavec::frontend
