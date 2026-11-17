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
// for the ledger (§12). RFC 0046: the unit's globals (§4), the calls that
// start threads the rules cannot follow, and the C functions that keep state
// of their own. Nothing here reaches the guard passes.
//
//===----------------------------------------------------------------------===//

#include "SafeCxxKinds.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "clang/AST/ASTLambda.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/CommandLine.h"

#include <set>
#include <tuple>

namespace weavec::frontend {

// RFC 0046 G6, G7: measures what the thread rules cost and change (`-mllvm
// -weavec-thread-rules=0`); not a user's switch.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static llvm::cl::opt<bool> threadRules("weavec-thread-rules",
                                       llvm::cl::init(true), llvm::cl::Hidden);

/// RFC 0045 §4: the symbols of `function` as the IR names them: the
/// mangled names without the target's label prefix (Darwin's `_`).
static void symbolsOf(const clang::FunctionDecl &function,
                      clang::ASTNameGenerator &names, llvm::StringSet<> &into) {
  const llvm::StringRef prefix =
      function.getASTContext().getTargetInfo().getUserLabelPrefix();
  auto add = [&](llvm::StringRef name) {
    name.consume_front(prefix);
    into.insert(name);
  };
  if (llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(
          function)) {
    for (const std::string &name : names.getAllManglings(&function))
      add(name);
  } else {
    add(names.getName(&function));
  }
}

namespace {

namespace rule {
constexpr llvm::StringLiteral RawOwner = "raw-owner";
constexpr llvm::StringLiteral IntToPointer = "int-to-pointer";
constexpr llvm::StringLiteral PointerCast = "pointer-cast";
constexpr llvm::StringLiteral PointerBytes = "pointer-bytes";
constexpr llvm::StringLiteral UnionPointer = "union-pointer";
constexpr llvm::StringLiteral Variadic = "variadic";
constexpr llvm::StringLiteral NoAsm = "no-asm";
constexpr llvm::StringLiteral UnsafeBlock = "unsafe-block";
constexpr llvm::StringLiteral BadAnnotation = "bad-annotation";
constexpr llvm::StringLiteral NotModeled = "not-modeled";
constexpr llvm::StringLiteral ThreadShare = "thread-share";
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
    body = function.getBody();
    for (const clang::ParmVarDecl *parameter : function.parameters())
      checkAnnotations(parameter->getType(), parameter->getLocation());
    // RFC 0046 §8: every declaration says the same.
    for (const clang::FunctionDecl *other : function.redecls())
      for (unsigned i = 0;
           i < other->getNumParams() && i < function.getNumParams(); ++i)
        if (hasAnnotation(annotationsOf(other->getParamDecl(i)->getType(), ast),
                          SendsAnnotation) !=
            hasAnnotation(
                annotationsOf(function.getParamDecl(i)->getType(), ast),
                SendsAnnotation)) {
          error(other->getParamDecl(i)->getLocation(), rule::BadAnnotation,
                "the declarations of '" + function.getNameAsString() +
                    "' disagree on WEAVEC_SENDS",
                "write WEAVEC_SENDS on the parameter in every declaration");
          break;
        }
    checkAnnotations(function.getReturnType(), function.getLocation());
    // A constructor's member initializers are its code too.
    if (const auto *constructor =
            llvm::dyn_cast<clang::CXXConstructorDecl>(&function))
      for (const clang::CXXCtorInitializer *initializer : constructor->inits())
        if (initializer->isWritten())
          TraverseStmt(initializer->getInit());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    TraverseStmt(const_cast<clang::Stmt *>(function.getBody()));
  }

  /// A member's default initializer, which every constructor that does not
  /// name the member runs.
  void run(const clang::FieldDecl &field) {
    body = nullptr;
    if (field.hasInClassInitializer())
      TraverseStmt(field.getInClassInitializer());
  }

  // RecursiveASTVisitor's names.
  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitCXXNewExpr(clang::CXXNewExpr *expression) {
    if (ownedAtOnce(*expression))
      return true;
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
    if (construct->getNumArgs() != 0)
      constView(construct->getType(), construct->getArg(0),
                construct->getExprLoc());
    if (construct->getNumArgs() == 0 || constructor->getNumParams() == 0 ||
        !isLibrary(*record))
      return true;
    const clang::Expr *first = construct->getArg(0)->IgnoreParenImpCasts();
    const clang::QualType given = first->getType();
    if (!given->isPointerType() || given->isFunctionPointerType() ||
        llvm::isa<clang::StringLiteral, clang::CXXNullPtrLiteralExpr>(first))
      return true;
    const auto written = static_cast<unsigned>(
        llvm::count_if(construct->arguments(), [](const clang::Expr *argument) {
          return !llvm::isa<clang::CXXDefaultArgExpr>(argument);
        }));
    const clang::QualType second =
        written > 1 ? construct->getArg(1)->getType() : clang::QualType();
    // A smart pointer made from a raw pointer (and perhaps a deleter); a
    // container or a string copies what it is given. RFC 0047 §4: but for a
    // block `new` makes in the same expression.
    if (smartPointer(*record) && !kinds.viewClass(record) &&
        !given->getPointeeType().isConstQualified() &&
        (written == 1 ||
         !(second->isPointerType() || second->isIntegerType())) &&
        constructor->getParamDecl(0)->getType()->isPointerType() &&
        !llvm::isa<clang::CXXNewExpr>(first))
      error(construct->getExprLoc(), rule::RawOwner,
            "a raw pointer is handed to '" + record->getNameAsString() +
                "' to own",
            "make the block with its owner: 'std::make_unique', "
            "'std::make_shared'");
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
            "use an exception or a return value", /*expanded=*/true);
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
    // RFC 0046 §9: an algorithm given a parallel execution policy runs its
    // callable on several threads at once.
    if (kinds.threadRules && isLibrary(*callee))
      for (const clang::Expr *given : call->arguments())
        if (CxxKinds::tableIs(given->getType(), "spawns")) {
          error(call->getExprLoc(), rule::NotModeled,
                "'" + callee->getNameAsString() +
                    "' with a parallel execution policy runs its callable on "
                    "several threads at once",
                "use the sequential form, or start threads that each own "
                "their part and collect the results");
          return true;
        }
    if (!callee->isExternC() && !callee->isInStdNamespace() &&
        !isLibrary(*callee))
      return true;
    const auto match = core::LibrarySpec::shipped().lookup(
        callee->getName().str(), call->getNumArgs());
    if (!match.has_value())
      return true;
    const core::LibraryEntry &row = *match->entry;
    if (kinds.threadRules && row.spawns) {
      error(call->getExprLoc(), rule::NotModeled,
            "'" + callee->getNameAsString() +
                "' starts a thread with a 'void *' the rules cannot follow",
            "start it with 'std::jthread', whose callable the rules check",
            /*expanded=*/true);
      return true;
    }
    if (kinds.threadRules && row.unshared) {
      error(call->getExprLoc(), rule::ThreadShare,
            "'" + callee->getNameAsString() +
                "' keeps state of its own that every thread shares",
            "use the reentrant form ('localtime_r', 'strtok_r', "
            "'strerror_r'), or '<random>' and '<chrono>'",
            /*expanded=*/true);
      return true;
    }
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

  bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *call) {
    if (call->getOperator() == clang::OO_Equal && call->getNumArgs() == 2)
      constView(call->getArg(0)->getType(), call->getArg(1),
                call->getOperatorLoc());
    return true;
  }

  /// §4: a global of the unit that safe code names, defined elsewhere.
  bool VisitDeclRefExpr(clang::DeclRefExpr *named) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(named->getDecl());
        variable != nullptr && kinds.threadRules &&
        variable->getDefinition() == nullptr &&
        !hasAnnotate(*variable, GuardedAnnotation))
      global(*variable, named->getLocation());
    return true;
  }

  bool VisitOMPExecutableDirective(clang::OMPExecutableDirective *directive) {
    return notModeled(directive->getBeginLoc(), "an OpenMP directive",
                      "start threads with 'std::jthread', which the rules "
                      "check");
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
          (to->isPointerType() || cast->isGLValue()) &&
          !byteView(from, to, cast->isGLValue()))
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
      if (llvm::isa<clang::CXXReinterpretCastExpr>(cast) &&
          (cast->getCastKind() != clang::CK_NoOp ||
           !byteView(from, to, cast->isGLValue())))
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

  /// RFC 0046 §2: a `std::shared_ptr<const T>` made from a
  /// `std::shared_ptr<T>` leaves a handle that can still change the object.
  void constView(clang::QualType to, const clang::Expr *from,
                 clang::SourceLocation at) {
    if (!kinds.threadRules)
      return;
    const auto pointee = [](clang::QualType type) {
      const auto *specialization =
          llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
              type.getNonReferenceType()->getAsCXXRecordDecl());
      return specialization == nullptr || !CxxKinds::shared(specialization) ||
                     specialization->getTemplateArgs().size() == 0 ||
                     specialization->getTemplateArgs()[0].getKind() !=
                         clang::TemplateArgument::Type
                 ? clang::QualType()
                 : specialization->getTemplateArgs()[0].getAsType();
    };
    const clang::QualType target = pointee(to);
    const clang::QualType source = pointee(from->getType());
    if (target.isNull() || source.isNull() || !target.isConstQualified() ||
        source.isConstQualified() || kinds.shareable(source) || fresh(from))
      return;
    error(at, rule::ThreadShare,
          "a 'std::shared_ptr<const " +
              source.getUnqualifiedType().getAsString(ast.getPrintingPolicy()) +
              ">' is made from one that can still change the object",
          "make it with 'std::make_shared<const T>(...)', so that no "
          "holder can write what other threads read");
  }

  /// Amendment: a handle no other handle shares: what a library function
  /// the table calls `fresh` returns (`std::make_shared<T>`), or a local
  /// made so, moved, and otherwise only dereferenced.
  [[nodiscard]] bool fresh(const clang::Expr *from) {
    from = from->IgnoreImplicit();
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(from)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      if (callee != nullptr && call->isCallToStdMove()) {
        const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(
            call->getArg(0)->IgnoreParenImpCasts());
        const auto *local =
            named == nullptr ? nullptr
                             : llvm::dyn_cast<clang::VarDecl>(named->getDecl());
        return local != nullptr && local->hasLocalStorage() &&
               !llvm::isa<clang::ParmVarDecl>(local) &&
               local->getInit() != nullptr && fresh(local->getInit()) &&
               onlyDereferenced(*local, *named);
      }
      return callee != nullptr && isLibrary(*callee) &&
             core::LibrarySpec::shipped().functionIs(
                 CxxKinds::qualifiedName(*callee), "fresh");
    }
    return false;
  }

  /// Every use of `local` but `moved` is `*local` or `local->`.
  [[nodiscard]] bool onlyDereferenced(const clang::VarDecl &local,
                                      const clang::DeclRefExpr &moved) {
    bool only = true;
    const auto visit = [&](const clang::Stmt *statement, const clang::Stmt *up,
                           const auto &self) -> void {
      if (statement == nullptr || !only)
        return;
      if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement);
          named != nullptr && named != &moved && named->getDecl() == &local) {
        const auto *operation =
            llvm::dyn_cast_or_null<clang::CXXOperatorCallExpr>(up);
        only = operation != nullptr &&
               (operation->getOperator() == clang::OO_Arrow ||
                operation->getOperator() == clang::OO_Star);
      }
      for (const clang::Stmt *child : statement->children())
        self(child,
             llvm::isa<clang::ImplicitCastExpr, clang::ParenExpr>(statement)
                 ? up
                 : statement,
             self);
    };
    visit(body, nullptr, visit);
    return only;
  }
  const clang::Stmt *body = nullptr;

public:
  /// §4: a variable with static storage that is not `thread_local` is
  /// immutable or of shareable type.
  void global(const clang::VarDecl &variable, clang::SourceLocation at) {
    const clang::VarDecl *canonical = variable.getCanonicalDecl();
    if (!variable.hasGlobalStorage() ||
        variable.getTLSKind() != clang::VarDecl::TLS_None ||
        isLibrary(variable) || variable.getType()->isDependentType() ||
        globals.contains(canonical))
      return;
    const clang::QualType type = variable.getType();
    if (kinds.shareable(type) ||
        (type.isConstQualified() && kinds.frozen(type)))
      return;
    // A `const` global whose value is a constant points only at constants
    // and at other globals: the unit's are checked in turn, and one defined
    // elsewhere is checked here, as if safe code named it.
    if (type.isConstQualified() && variable.hasConstantInitialization() &&
        !kinds.holdsMutable(type)) {
      const auto visit = [&](const clang::Stmt *statement,
                             const auto &self) -> void {
        if (statement == nullptr)
          return;
        if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement))
          if (const auto *other =
                  llvm::dyn_cast<clang::VarDecl>(named->getDecl());
              other != nullptr && other->getDefinition() == nullptr &&
              !hasAnnotate(*other, GuardedAnnotation))
            global(*other, at);
        for (const clang::Stmt *child : statement->children())
          self(child, self);
      };
      visit(variable.getAnyInitializer(), visit);
      return;
    }
    globals.insert(canonical);
    report(at, rule::ThreadShare,
           "every thread can reach the global '" + variable.getNameAsString() +
               "', and '" +
               type.getUnqualifiedType().getAsString(ast.getPrintingPolicy()) +
               "' is not safe to share",
           at,
           "make it 'thread_local' if each thread should have its own, "
           "'const' if it never changes, or a class that locks its members "
           "(or WEAVEC_GUARDED, which the audit lists); 'weavec-c++ "
           "--weavec-explain thread-share' shows each");
  }

private:
  llvm::DenseSet<const clang::VarDecl *> globals;

  void castError(clang::SourceLocation at, const std::string &message,
                 const std::string &fix) {
    error(at, rule::PointerCast, message, fix);
  }

  bool notModeled(clang::SourceLocation at, const std::string &what,
                  const std::string &fix) {
    error(at, rule::NotModeled, what + " is not followed by the rules", fix);
    return true;
  }

  /// A library smart pointer: an owner the table says `reset` resets
  /// (`std::unique_ptr`), or a shared one (`std::shared_ptr`).
  [[nodiscard]] static bool smartPointer(const clang::CXXRecordDecl &record) {
    return isLibrary(record) &&
           (CxxKinds::shared(&record) ||
            (record.hasAttr<clang::OwnerAttr>() &&
             core::LibrarySpec::shipped().classMember(
                 CxxKinds::qualifiedName(record), "resets", "reset")));
  }

  /// RFC 0047 §4: a block `new` makes that a smart pointer takes in the
  /// same expression (`std::unique_ptr<T>(new T(...))`, `p.reset(new T)`):
  /// no path leaves it without an owner.
  [[nodiscard]] bool ownedAtOnce(const clang::Expr &expression) const {
    const auto smart = [](const clang::FunctionDecl &taker) {
      const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&taker);
      return method != nullptr && smartPointer(*method->getParent()) &&
             taker.getNumParams() >= 1 &&
             taker.getParamDecl(0)->getType()->isPointerType();
    };
    // The smart pointer deletes as it was declared: `new T[n]` only into
    // `T[]`, `new T` only into `T`, and a derived object only through a base
    // whose destructor is virtual.
    const auto *made = llvm::dyn_cast<clang::CXXNewExpr>(&expression);
    const auto matches = [&](const clang::FunctionDecl &taker) {
      const auto *specialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
              llvm::cast<clang::CXXMethodDecl>(taker).getParent());
      if (made == nullptr || specialization == nullptr ||
          specialization->getTemplateArgs().size() == 0 ||
          specialization->getTemplateArgs()[0].getKind() !=
              clang::TemplateArgument::Type)
        return false;
      const clang::QualType element =
          specialization->getTemplateArgs()[0].getAsType();
      if (element->isArrayType() != made->isArray())
        return false;
      const clang::QualType pointee =
          taker.getParamDecl(0)->getType()->getPointeeType();
      if (clang::ASTContext::hasSameUnqualifiedType(pointee,
                                                    made->getAllocatedType()))
        return true;
      const clang::CXXRecordDecl *base = pointee->getAsCXXRecordDecl();
      const clang::CXXDestructorDecl *destructor =
          base == nullptr || !base->hasDefinition()
              ? nullptr
              : base->getDefinition()->getDestructor();
      return !made->isArray() && destructor != nullptr &&
             destructor->isVirtual();
    };
    const clang::Expr *current = &expression;
    for (unsigned depth = 0; depth < 6; ++depth) {
      const auto parents = ast.getParents(*current);
      if (parents.empty())
        return false;
      if (const auto *construct = parents[0].get<clang::CXXConstructExpr>())
        return construct->getNumArgs() >= 1 &&
               construct->getArg(0) == current &&
               smart(*construct->getConstructor()) &&
               matches(*construct->getConstructor());
      if (const auto *call = parents[0].get<clang::CXXMemberCallExpr>())
        return call->getMethodDecl() != nullptr &&
               call->getMethodDecl()->getName() == "reset" &&
               call->getNumArgs() >= 1 && call->getArg(0) == current &&
               smart(*call->getMethodDecl()) && matches(*call->getMethodDecl());
      const auto *next = parents[0].get<clang::Expr>();
      if (next == nullptr ||
          !llvm::isa<clang::ImplicitCastExpr, clang::ParenExpr>(next))
        return false;
      current = next;
    }
    return false;
  }

  /// RFC 0047 §3: a pointer to an object viewed as its bytes: `const char`,
  /// `const unsigned char` or `const std::byte`, and the same without
  /// `const` for a trivially copyable object that holds no pointer.
  [[nodiscard]] bool byteView(clang::QualType from, clang::QualType to,
                              bool glvalue) const {
    if (!glvalue) {
      if (!from->isPointerType() || !to->isPointerType())
        return false;
      from = from->getPointeeType();
      to = to->getPointeeType();
    } else if (to->isReferenceType()) {
      to = to.getNonReferenceType();
    }
    const clang::QualType canonical = to.getCanonicalType();
    if (!(canonical->isCharType() || canonical->isStdByteType()) ||
        from->isVoidType() || from->isFunctionType() ||
        from->isIncompleteType() ||
        (from.isConstQualified() && !to.isConstQualified()))
      return false;
    return to.isConstQualified() ||
           (from.isTriviallyCopyableType(ast) && !holdsPointer(from));
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

  void checkAnnotations(clang::QualType type, clang::SourceLocation at) {
    // RFC 0046 §8: WEAVEC_SENDS is for a callable whose type hides what it
    // captured.
    if (hasAnnotation(annotationsOf(type, ast), SendsAnnotation) &&
        !kinds.erased(type)) {
      error(at, rule::BadAnnotation,
            "WEAVEC_SENDS is on a parameter whose type is not a callable "
            "that hides what it captured",
            "put it on a 'std::function' or another erased callable; a "
            "template's callable needs none");
      return;
    }
    if (!type->isPointerType())
      return;
    // RFC 0047 §4 (D3): ownership is C's; a bound is both languages'.
    for (const TypeAnnotation &annotation : annotationsOf(type, ast))
      if (annotation.name == OwnedAnnotation ||
          annotation.name == DropsAnnotation) {
        error(at, rule::BadAnnotation,
              "'" + annotation.name.str() +
                  "' is Safe C's, and a safe C++ function does not use it",
              "an owner is a 'std::unique_ptr' or a container; a destructor "
              "releases what a class owns");
        return;
      }
  }
};

/// RFC 0045 §5: the standard library's functions a safe function calls
/// whose preconditions the library's hardening checks: a function of
/// namespace `std` with a name the standard does not reserve, of a class
/// that is no view, none of whose parameters is a pointer, a view or a
/// callable; and the destruction, the copy and the move of what the
/// function holds.
class TrustedCalls final : public clang::RecursiveASTVisitor<TrustedCalls> {
public:
  TrustedCalls(CxxKinds &kinds, SafeUnit &unit, clang::ASTNameGenerator &names)
      : kinds(kinds), unit(unit), names(names) {}

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitCallExpr(clang::CallExpr *call) {
    if (const clang::FunctionDecl *callee = call->getDirectCallee())
      consider(*callee);
    return true;
  }
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *construct) {
    if (const clang::CXXConstructorDecl *constructor =
            construct->getConstructor())
      consider(*constructor);
    destroyed(construct->getType());
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *variable) {
    destroyed(variable->getType());
    return true;
  }
  bool VisitCXXBindTemporaryExpr(clang::CXXBindTemporaryExpr *temporary) {
    if (const clang::CXXDestructorDecl *destructor =
            temporary->getTemporary()->getDestructor())
      consider(*destructor);
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

  /// What an implicit member of a safe class does to a member or base of
  /// type `type`.
  void members(clang::QualType type) {
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || !record->hasDefinition())
      return;
    record = record->getDefinition();
    for (const clang::CXXConstructorDecl *constructor : record->ctors())
      if (constructor->isCopyOrMoveConstructor() ||
          constructor->isDefaultConstructor())
        consider(*constructor);
    for (const clang::CXXMethodDecl *method : record->methods())
      if (method->isCopyAssignmentOperator() ||
          method->isMoveAssignmentOperator())
        consider(*method);
    destroyed(type);
  }

private:
  CxxKinds &kinds;
  SafeUnit &unit;
  clang::ASTNameGenerator &names;

  void destroyed(clang::QualType type) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = type->getAsArrayTypeUnsafe())
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record != nullptr && record->hasDefinition())
      if (const clang::CXXDestructorDecl *destructor =
              record->getDefinition()->getDestructor())
        consider(*destructor);
  }

  [[nodiscard]] static bool reserved(const clang::NamedDecl &declaration) {
    const clang::IdentifierInfo *identifier = declaration.getIdentifier();
    if (identifier == nullptr)
      return false;
    const llvm::StringRef name = identifier->getName();
    return name.starts_with("__") ||
           (name.size() > 1 && name[0] == '_' && llvm::isUpper(name[1]));
  }

  [[nodiscard]] bool forbiddenParameter(clang::QualType type) {
    const clang::QualType object = type.getNonReferenceType();
    if (object->isPointerType() || object->isFunctionType() ||
        object->isMemberPointerType())
      return true;
    const clang::CXXRecordDecl *record = object->getAsCXXRecordDecl();
    return record != nullptr &&
           (record->isLambda() || kinds.viewClass(record) ||
            kinds.erased(object) || CxxKinds::pointerClass(object));
  }

  void consider(const clang::FunctionDecl &function) {
    if (!isLibrary(function) || function.isDeleted() ||
        function.isDependentContext() || reserved(function))
      return;
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    const clang::DeclContext *space =
        function.getDeclContext()->getEnclosingNamespaceContext();
    if (space == nullptr || !space->isStdNamespace())
      return;
    if (method != nullptr) {
      const clang::CXXRecordDecl *record = method->getParent();
      if (reserved(*record) || kinds.viewClass(record) ||
          record->hasAttr<clang::PointerAttr>() || record->isLambda())
        return;
      // A nested class of another (an iterator of a container) only if its
      // enclosing classes are not reserved either.
      for (const clang::DeclContext *outer = record->getParent();
           outer != nullptr && !outer->isNamespace();
           outer = outer->getParent())
        if (const auto *enclosing = llvm::dyn_cast<clang::CXXRecordDecl>(outer);
            enclosing != nullptr && reserved(*enclosing))
          return;
    }
    if (llvm::any_of(function.parameters(),
                     [this](const clang::ParmVarDecl *parameter) {
                       return forbiddenParameter(parameter->getType());
                     }))
      return;
    symbolsOf(function, names, unit.cxxTrusted);
  }
};

/// §1: the functions the unit defines in its own files, and the lambdas
/// and instantiations among them.
class CxxUnit final : public clang::RecursiveASTVisitor<CxxUnit> {
public:
  CxxUnit(clang::ASTContext &ast, bool unitSafe, SafeUnit &unit,
          const RuleReport &report, const BoundsRules &bounds)
      : ast(ast), unitSafe(unitSafe), unit(unit), report(report),
        bounds(bounds), kinds(ast), rules(ast, kinds, report), names(ast),
        trusted(kinds, unit, names) {
    kinds.threadRules = threadRules;
  }

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  [[nodiscard]] static bool shouldVisitTemplateInstantiations() { return true; }

  /// Nothing a system header declares is the unit's own: its functions are
  /// not checked and its classes not listed, so the walk does not enter
  /// it (the standard library and its instantiations are most of a unit).
  bool TraverseDecl(clang::Decl *declaration) {
    if (declaration != nullptr &&
        !llvm::isa<clang::TranslationUnitDecl>(declaration) &&
        isLibrary(*declaration))
      return true;
    return clang::RecursiveASTVisitor<CxxUnit>::TraverseDecl(declaration);
  }

  bool VisitFunctionDecl(clang::FunctionDecl *function) {
    if (function->isDependentContext() || function->isImplicit() ||
        isLibrary(*function))
      return true;
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
    if (method != nullptr && method->getParent()->isLambda()) {
      if (function->doesThisDeclarationHaveABody())
        nameAs(*function, isSafe(*function));
      return true;
    }
    // As a class's bounds, a function's matter to safe code only.
    if (isSafe(*function))
      agree(*function);
    if (!function->doesThisDeclarationHaveABody() || function->isDefaulted()) {
      table(*function, false, unit.fast);
      return true;
    }
    const bool safe = isSafe(*function);
    const std::size_t index = define(*function, safe);
    usedBounds = false;
    if (!safe && unit.survey && unit.definitions[index].tier == "guarded") {
      bounds.survey(&unit.definitions[index].survey);
      survey(*function);
      bounds.survey(nullptr);
    }
    const bool settled = !safe || check(*function);
    if (!settled)
      unit.definitions[index].tier = "unsettled";
    nameAs(*function, safe && settled);
    // RFC 0048 §2: in a fast unit every safe function is checked against
    // what it holds, and every other function of the unit with a pointer
    // states what it hands safe code.
    table(*function, safe && settled && (usedBounds || unit.fast),
          !safe && unit.fast);
    return true;
  }

  /// RFC 0046 §4: a global, a static member or a `static` local the unit
  /// defines, in safe code.
  bool VisitVarDecl(clang::VarDecl *variable) {
    if (!kinds.threadRules || !variable->hasGlobalStorage() ||
        llvm::isa<clang::ParmVarDecl>(variable) ||
        hasAnnotate(*variable, GuardedAnnotation) ||
        variable->isThisDeclarationADefinition() ==
            clang::VarDecl::DeclarationOnly ||
        variable->getDeclContext()->isDependentContext() ||
        isLibrary(*variable))
      return true;
    if (safeGlobal(*variable)) {
      rules.global(*variable, variable->getLocation());
      return true;
    }
    // RFC 0047 §7: a global of code that is not safe yet, as if it were.
    if (!unit.survey)
      return true;
    std::map<std::string, unsigned> counts;
    bounds.survey(&counts);
    CxxRules(ast, kinds, report).global(*variable, variable->getLocation());
    bounds.survey(nullptr);
    if (counts.empty())
      return true;
    const clang::PresumedLoc at =
        ast.getSourceManager().getPresumedLoc(variable->getLocation());
    unit.surveyedGlobals.push_back(
        {.name = variable->getQualifiedNameAsString(),
         .file = at.isValid() ? at.getFilename() : "",
         .line = at.isValid() ? at.getLine() : 0,
         .survey = std::move(counts)});
    return true;
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl *record) {
    if (record->isThisDeclarationADefinition() && !record->isLambda() &&
        !record->isDependentContext() && !isLibrary(*record) &&
        listed.insert(record->getCanonicalDecl()).second) {
      listFields(*record);
      // A class's bounds matter to safe code only: a guarded unit keeps
      // compiling whatever its classes say, as a C unit does.
      if (isSafe(*record)) {
        bounds.checkRecord(*record);
        aliasedOwns(*record);
        for (const clang::FieldDecl *field : record->fields())
          rules.run(*field);
      }
      implicitMembers(*record);
    }
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  clang::ASTContext &ast;
  bool unitSafe;
  SafeUnit &unit;
  const RuleReport &report;
  const BoundsRules &bounds;
  CxxKinds kinds;
  CxxRules rules;
  /// RFC 0047 §1.2: the function `check` just checked uses bounds.
  bool usedBounds = false;
  std::set<std::pair<std::string, unsigned>> defined;
  llvm::DenseSet<const clang::CXXRecordDecl *> listed;
  clang::ASTNameGenerator names;
  TrustedCalls trusted;

  /// RFC 0045 §4: the symbols `function` is emitted under, as safe code or
  /// as the unit's other code.
  void nameAs(const clang::FunctionDecl &function, bool safe) {
    if (!unit.fast)
      return;
    symbolsOf(function, names, safe ? unit.cxxSafe : unit.cxxOther);
  }

  /// The members the compiler writes for a class of the unit, which run as
  /// its own code: safe for a safe class.
  void implicitMembers(const clang::CXXRecordDecl &record) {
    if (!unit.fast)
      return;
    const bool safe = isSafe(record);
    for (const clang::CXXMethodDecl *method : record.methods())
      if ((method->isImplicit() || method->isDefaulted()) &&
          method->isDefined() && !method->isDeleted())
        nameAs(*method, safe);
    if (!safe)
      return;
    for (const clang::FieldDecl *field : record.fields())
      trusted.members(field->getType());
    for (const clang::CXXBaseSpecifier &base : record.bases())
      trusted.members(base.getType());
  }

  /// RFC 0047 §5.2 (K): an aliased class owns no storage that changes type
  /// in place, but a string's characters.
  void aliasedOwns(const clang::CXXRecordDecl &record) {
    if (!CxxKinds::aliased(&record))
      return;
    const auto refuse = [&](clang::SourceLocation at, const std::string &what) {
      report(at, "owner-path",
             "'" + record.getNameAsString() + "' is WEAVEC_ALIASED and owns " +
                 what + ", whose contents change type in place",
             at,
             "an aliased class holds no 'std::variant', 'std::any' or "
             "'std::function'; change '" +
                 record.getNameAsString() +
                 "' from what owns it instead, and drop WEAVEC_ALIASED");
    };
    for (const clang::FieldDecl *field : record.fields())
      if (kinds.switchesAny(field->getType())) {
        refuse(field->getLocation(), "'" + field->getNameAsString() + "'");
        return;
      }
    for (const clang::CXXBaseSpecifier &base : record.bases())
      if (!CxxKinds::aliased(base.getType()->getAsCXXRecordDecl()) &&
          kinds.switchesAny(base.getType())) {
        refuse(base.getBeginLoc(),
               "its base '" + base.getType().getAsString() + "'");
        return;
      }
  }

  /// RFC 0047 §1.1: every declaration of a function, and every function
  /// that overrides a virtual one, gives the same bounds.
  void agree(const clang::FunctionDecl &function) {
    const auto differs = [&](const clang::FunctionDecl &other) {
      return !bounds.tableOf(function).sameBounds(bounds.tableOf(other));
    };
    if (const clang::FunctionDecl *previous = function.getPreviousDecl();
        previous != nullptr && !isLibrary(*previous) && differs(*previous))
      report(function.getLocation(), "bad-annotation",
             "this declaration of '" + function.getNameAsString() +
                 "' gives its pointers other bounds than an earlier one",
             previous->getLocation(),
             "write the same bounds on every declaration of '" +
                 function.getNameAsString() + "', as here");
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function))
      for (const clang::CXXMethodDecl *base : method->overridden_methods())
        if (differs(*base)) {
          report(function.getLocation(), "bad-annotation",
                 "'" + function.getNameAsString() + "' overrides '" +
                     base->getQualifiedNameAsString() + "' with other bounds",
                 base->getLocation(),
                 "give an override the bounds of the function it overrides, "
                 "as here");
          break;
        }
  }

  /// RFC 0047 §1.4: whether a declaration of `function` gives a pointer a
  /// bound.
  [[nodiscard]] bool declaresBounds(const clang::FunctionDecl &function) const {
    const FunctionBounds table = bounds.tableOf(function);
    return table.result.bounded() ||
           llvm::any_of(table.parameters, [](const PointerBounds &parameter) {
             return parameter.bounded();
           });
  }

  /// RFC 0048 §7: whether a function takes or returns a pointer, whose
  /// bounds a fast unit's safe callers believe of its result.
  [[nodiscard]] static bool handsPointers(const clang::FunctionDecl &function) {
    return function.getReturnType()->isPointerType() ||
           llvm::any_of(function.parameters(),
                        [](const clang::ParmVarDecl *parameter) {
                          return parameter->getType()->isPointerType();
                        });
  }

  /// RFC 0047 §1.4: the bounds the guard passes read, by the symbols the
  /// function is emitted under: of each function the unit declares with a
  /// bound, and of each safe function that uses bounds, which the passes
  /// check. A member function's object is its first parameter there.
  void table(const clang::FunctionDecl &function, bool safe, bool other) {
    if (!safe && !declaresBounds(function) &&
        !(other && handsPointers(function)))
      return;
    FunctionBounds entry = bounds.tableOf(function);
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
        method != nullptr && method->isInstance()) {
      const auto shift = [](PointerBounds &given) {
        if (given.count >= 0)
          ++given.count;
        if (given.end >= 0)
          ++given.end;
      };
      for (PointerBounds &parameter : entry.parameters)
        shift(parameter);
      shift(entry.result);
      // RFC 0048 §4: `this`, which WEAVEC_ESCAPES after the parameter list
      // says the function keeps.
      entry.parameters.insert(
          entry.parameters.begin(),
          PointerBounds{.escapes =
                            hasAnnotation(annotationsOf(method->getType(), ast),
                                          EscapesAnnotation)});
    }
    // RFC 0047 amendments 3 and 17, in a checked unit: a pointer without a
    // bound is checked by its guards, not at the entry or the return, and a
    // string is not scanned at the entry, the return or a call. RFC 0048
    // §2, §3: a fast unit believes both, since an end is not a plain
    // pointer there (`call-args`) and a string is checked where it is
    // stepped.
    const auto guardedOnly = [](PointerBounds &given) {
      given.pointeeSize = 0;
      if (given.kind == PointerBounds::Kind::NullTerminated)
        given.kind = PointerBounds::Kind::None;
    };
    if (!unit.fast) {
      for (PointerBounds &parameter : entry.parameters)
        guardedOnly(parameter);
      guardedOnly(entry.result);
    }
    entry.safe = safe;
    llvm::StringSet<> symbols;
    symbolsOf(function, names, symbols);
    for (const auto &symbol : symbols) {
      const auto [found, inserted] =
          unit.functions.try_emplace(symbol.getKey(), entry);
      if (!inserted)
        found->second.safe = found->second.safe || safe;
    }
  }

  /// §1: what the function or the classes around it say, else the unit.
  [[nodiscard]] bool isSafe(const clang::FunctionDecl &function) const {
    if (hasAnnotate(function, UnsafeAnnotation) ||
        function.hasAttr<clang::NoSanitizeAttr>() ||
        function.hasAttr<clang::DisableSanitizerInstrumentationAttr>())
      return false;
    return isSafe(static_cast<const clang::DeclContext &>(function));
  }

  /// The same for a class's own code (its members' default initializers).
  [[nodiscard]] bool isSafe(const clang::DeclContext &inner) const {
    clang::SourceLocation at;
    for (const clang::DeclContext *context = &inner; context != nullptr;
         context = context->getParent()) {
      const auto *declaration = llvm::dyn_cast<clang::Decl>(context);
      if (declaration == nullptr ||
          !llvm::isa<clang::FunctionDecl, clang::CXXRecordDecl>(declaration))
        continue;
      if (at.isInvalid())
        at = declaration->getLocation();
      if (hasAnnotate(*declaration, SafeAnnotation))
        return true;
      if (hasAnnotate(*declaration, GuardedAnnotation))
        return false;
    }
    // RFC 0047 §6: a file that says WEAVEC_SAFE_FILE.
    return unitSafe || inSafeFile(unit, ast.getSourceManager(), at);
  }

  /// A variable's declarations are safe code: its function's or class's,
  /// or, at namespace scope, the unit's or its file's (RFC 0047 §6).
  [[nodiscard]] bool safeGlobal(const clang::VarDecl &variable) const {
    const clang::DeclContext *context = variable.getDeclContext();
    if (context->isFunctionOrMethod() || context->isRecord())
      return isSafe(*context);
    return unitSafe ||
           inSafeFile(unit, ast.getSourceManager(), variable.getLocation());
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
                                .owns = false,
                                .starts = 0,
                                .locks = false,
                                .sends = false,
                                .survey = {}});
    threadFacts(function, unit.definitions.back());
    return unit.definitions.size() - 1;
  }

  /// RFC 0046 §12: the thread starts a function makes, whether it uses a
  /// lock or a guard object, and whether a parameter is WEAVEC_SENDS: what
  /// the audit lists of functions that are not safe.
  void threadFacts(const clang::FunctionDecl &function,
                   SafeUnit::Definition &definition) {
    definition.sends = llvm::any_of(
        function.parameters(), [&](const clang::ParmVarDecl *parameter) {
          return hasAnnotation(annotationsOf(parameter->getType(), ast),
                               SendsAnnotation);
        });
    const auto visit = [&](const clang::Stmt *statement,
                           const auto &self) -> void {
      if (statement == nullptr)
        return;
      if (const auto *expression = llvm::dyn_cast<clang::Expr>(statement)) {
        definition.locks |= CxxKinds::lockClass(expression->getType()) ||
                            CxxKinds::guardClass(expression->getType());
        if (kinds.startOf(*expression))
          ++definition.starts;
      }
      // A C function that starts a thread (`pthread_create`).
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(statement))
        if (const clang::FunctionDecl *callee = call->getDirectCallee();
            callee != nullptr && callee->isExternC() &&
            callee->getIdentifier() != nullptr)
          if (const auto row = core::LibrarySpec::shipped().lookup(
                  callee->getName().str(), call->getNumArgs());
              row.has_value() && row->entry->spawns)
            ++definition.starts;
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(function.getBody(), visit);
    if (const auto *constructor =
            llvm::dyn_cast<clang::CXXConstructorDecl>(&function))
      for (const clang::CXXCtorInitializer *initializer : constructor->inits())
        if (initializer->isWritten())
          visit(initializer->getInit(), visit);
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
    keptAddresses(ast, function, true, &report);
    usedBounds = bounds.check(function) || declaresBounds(function);
    if (kinds.threadRules)
      threadGlobals(function);
    if (unit.fast)
      // The visitor takes what it walks as changeable; it changes nothing.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      trusted.TraverseDecl(const_cast<clang::FunctionDecl *>(&function));
    bool settled = checkCxxFlow(ast, kinds, function, *body, nullptr, report);
    std::vector<const clang::LambdaExpr *> lambdas;
    collectLambdas(body, lambdas);
    for (const clang::LambdaExpr *lambda : lambdas) {
      if (const clang::FunctionTemplateDecl *generic =
              lambda->getDependentCallOperator()) {
        for (const clang::FunctionDecl *instance : generic->specializations())
          if (instance->hasBody())
            settled &= checkCxxFlow(ast, kinds, *instance, *instance->getBody(),
                                    lambda, report);
      } else if (const clang::CXXMethodDecl *call = lambda->getCallOperator();
                 call != nullptr && call->hasBody()) {
        settled &=
            checkCxxFlow(ast, kinds, *call, *call->getBody(), lambda, report);
      }
    }
    return settled;
  }

  /// RFC 0047 §7: the rules over a function that is not safe, whose errors
  /// the checker counts; nothing else of the unit changes.
  void survey(const clang::FunctionDecl &function) {
    const clang::Stmt *body = function.getBody();
    if (body == nullptr || llvm::isa<clang::CoroutineBodyStmt>(body))
      return;
    CxxRules fresh(ast, kinds, report);
    fresh.run(function);
    keptAddresses(ast, function, true, &report);
    (void)bounds.check(function);
    (void)checkCxxFlow(ast, kinds, function, *body, nullptr, report);
    std::vector<const clang::LambdaExpr *> lambdas;
    collectLambdas(body, lambdas);
    for (const clang::LambdaExpr *lambda : lambdas)
      if (const clang::CXXMethodDecl *call = lambda->getCallOperator();
          call != nullptr && call->hasBody() &&
          lambda->getDependentCallOperator() == nullptr)
        (void)checkCxxFlow(ast, kinds, *call, *call->getBody(), lambda, report);
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
    // RFC 0046 §12: the class, and the members its lock guards.
    const clang::PresumedLoc where =
        ast.getSourceManager().getPresumedLoc(record.getLocation());
    const clang::QualType self = ast.getCanonicalTagType(&record);
    unit.fields.push_back({.record = record.getQualifiedNameAsString(),
                           .field = "",
                           .kind = "class",
                           .file = where.isValid() ? where.getFilename() : "",
                           .line = where.isValid() ? where.getLine() : 0,
                           .sendable = kinds.sendable(self),
                           .shareable = kinds.shareable(self)});
    // RFC 0047 §8.
    if (CxxKinds::aliased(&record))
      unit.fields.push_back({.record = record.getQualifiedNameAsString(),
                             .field = "",
                             .kind = "aliased",
                             .file = where.isValid() ? where.getFilename() : "",
                             .line = where.isValid() ? where.getLine() : 0,
                             .sendable = false,
                             .shareable = false});
    const LockedClass &locked = kinds.lockedClass(record);
    for (const clang::FieldDecl *field : record.fields()) {
      const char *kind = nullptr;
      if (field == locked.lock)
        kind = "lock";
      else if (locked.locked.contains(field))
        kind = "locked";
      else if (locked.threads.contains(field))
        kind = "thread";
      if (kind == nullptr)
        continue;
      const clang::PresumedLoc at =
          ast.getSourceManager().getPresumedLoc(field->getLocation());
      unit.fields.push_back({.record = record.getQualifiedNameAsString(),
                             .field = field->getNameAsString(),
                             .kind = kind,
                             .file = at.isValid() ? at.getFilename() : "",
                             .line = at.isValid() ? at.getLine() : 0,
                             .sendable = false,
                             .shareable = false});
    }
    for (const clang::FieldDecl *field : record.fields()) {
      clang::QualType type = field->getType();
      while (const clang::ArrayType *array = ast.getAsArrayType(type))
        type = array->getElementType();
      const clang::CXXRecordDecl *of = type->getAsCXXRecordDecl();
      const char *kind = nullptr;
      if (type->isFunctionPointerType())
        continue;
      const auto annotations = annotationsOf(field->getType(), ast);
      if (type->isReferenceType() || kinds.viewClass(of) ||
          (type->isPointerType() &&
           hasAnnotation(annotations, BorrowedAnnotation)))
        kind = "borrowed";
      else if (type->isPointerType() &&
               llvm::any_of(annotations, [](const TypeAnnotation &each) {
                 return each.name != EscapesAnnotation;
               }))
        // RFC 0047 §8: a reference with a bound.
        kind = "counted";
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
                             .line = at.isValid() ? at.getLine() : 0,
                             .sendable = false,
                             .shareable = false});
    }
  }

  /// RFC 0046 §12: the globals of code that is not safe that a safe
  /// function names, which it trusts to be used as safe code would.
  void threadGlobals(const clang::FunctionDecl &function) {
    llvm::DenseSet<const clang::VarDecl *> seen;
    llvm::DenseSet<const clang::VarDecl *> followed;
    const auto visit = [&](const clang::Stmt *statement,
                           const auto &self) -> void {
      if (statement == nullptr)
        return;
      if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement))
        if (const auto *variable =
                llvm::dyn_cast<clang::VarDecl>(named->getDecl());
            variable != nullptr && variable->hasGlobalStorage() &&
            variable->getTLSKind() == clang::VarDecl::TLS_None &&
            !isLibrary(*variable) &&
            (hasAnnotate(*variable, GuardedAnnotation) ||
             (variable->getDefinition() != nullptr &&
              !safeGlobal(*variable->getDefinition()))) &&
            !(variable->getType().isConstQualified() &&
              kinds.frozen(variable->getType())) &&
            !kinds.shareable(variable->getType()) &&
            seen.insert(variable->getCanonicalDecl()).second) {
          const clang::PresumedLoc at =
              ast.getSourceManager().getPresumedLoc(named->getLocation());
          unit.fields.push_back({.record = function.getQualifiedNameAsString(),
                                 .field = variable->getNameAsString(),
                                 .kind = "thread-global",
                                 .file = at.isValid() ? at.getFilename() : "",
                                 .line = at.isValid() ? at.getLine() : 0,
                                 .sendable = false,
                                 .shareable = false});
        }
      // What a `const` global points at is reached through it.
      if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement))
        if (const auto *variable =
                llvm::dyn_cast<clang::VarDecl>(named->getDecl());
            variable != nullptr && variable->hasGlobalStorage() &&
            variable->getType().isConstQualified() &&
            followed.insert(variable->getCanonicalDecl()).second)
          self(variable->getAnyInitializer(), self);
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(function.getBody(), visit);
  }
};

} // namespace

namespace {

/// RFC 0048 §4: what an address is an address in.
struct Origin {
  enum class Kind : std::uint8_t { Local, Parameter, This, Temporary };
  Kind kind = Kind::Local;
  const clang::VarDecl *variable = nullptr;
  bool operator<(const Origin &other) const {
    return std::tie(kind, variable) < std::tie(other.kind, other.variable);
  }
};

/// RFC 0048 §4, §5.1 (keptAddresses).
class Kept final : public clang::RecursiveASTVisitor<Kept> {
public:
  Kept(clang::ASTContext &ast, const clang::FunctionDecl &function, bool safe,
       const RuleReport *report)
      : ast(ast), function(function), safe(safe), report(report) {}

  void run() {
    // Every value a local pointer is given, before any is followed.
    localValues(function.getBody(), assigned);
    if (const auto *constructor =
            llvm::dyn_cast<clang::CXXConstructorDecl>(&function))
      for (const clang::CXXCtorInitializer *initializer :
           constructor->inits()) {
        // A member or base whose constructor keeps its object keeps this
        // object's address (a default initializer is the constructor's).
        const clang::Expr *value = initial(*initializer);
        const clang::SourceLocation at = initializer->isWritten()
                                             ? initializer->getSourceLocation()
                                             : constructor->getLocation();
        if (const clang::CXXConstructorDecl *made = constructed(value);
            made != nullptr && keepsMade(made))
          kept({.kind = Origin::Kind::This}, at,
               "'" + made->getNameAsString() + "'");
        if (!initializer->isWritten() && value == initializer->getInit())
          continue;
        if (initializer->isAnyMemberInitializer() &&
            initializer->getAnyMember()->getType()->isPointerType() &&
            !borrowed(initializer->getAnyMember()) && safe)
          keep(valueOrigins(value), at,
               "'" + initializer->getAnyMember()->getNameAsString() + "'");
        // A member or base constructed from an argument that it keeps.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        TraverseStmt(const_cast<clang::Expr *>(value));
      }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    TraverseStmt(const_cast<clang::Stmt *>(function.getBody()));
  }

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitVarDecl(clang::VarDecl *variable) {
    if (!variable->hasLocalStorage() || llvm::isa<clang::ParmVarDecl>(variable))
      return true;
    if (keptType(variable->getType(), 0) ||
        keepsMade(constructed(variable->getInit())))
      place(*variable);
    return true;
  }

  bool VisitBinaryOperator(clang::BinaryOperator *op) {
    if (op->getOpcode() != clang::BO_Assign || !op->getType()->isPointerType())
      return true;
    const clang::Expr *left = op->getLHS()->IgnoreParenImpCasts();
    const auto *member = llvm::dyn_cast<clang::MemberExpr>(left);
    if (localPointer(left) != nullptr ||
        (member != nullptr && borrowed(member->getMemberDecl())))
      return true;
    if (safe &&
        (llvm::isa<clang::MemberExpr, clang::ArraySubscriptExpr>(left) ||
         llvm::isa<clang::UnaryOperator>(left) ||
         llvm::isa<clang::DeclRefExpr>(left)))
      keep(valueOrigins(op->getRHS()), op->getOperatorLoc(),
           "'" + spelled(left) + "'");
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *call) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr)
      return true;
    const bool member = llvm::isa<clang::CXXOperatorCallExpr>(call) &&
                        llvm::isa<clang::CXXMethodDecl>(callee);
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(callee);
        method != nullptr && method->isInstance()) {
      const clang::Expr *object = nullptr;
      if (member)
        object = call->getArg(0);
      else if (const auto *named =
                   llvm::dyn_cast<clang::CXXMemberCallExpr>(call))
        object = named->getImplicitObjectArgument();
      if (object != nullptr && keepsThis(*method))
        keep(object->getType()->isPointerType() ? valueOrigins(object)
                                                : objectOrigins(object),
             call->getExprLoc(), "'" + method->getNameAsString() + "'");
    }
    for (unsigned i = member ? 1 : 0, p = 0;
         i < call->getNumArgs() && p < callee->getNumParams(); ++i, ++p)
      argument(*callee, *callee->getParamDecl(p), *call->getArg(i),
               call->getExprLoc());
    return true;
  }

  /// A temporary whose constructor keeps it, or of an aliased class.
  bool VisitMaterializeTemporaryExpr(clang::MaterializeTemporaryExpr *made) {
    const clang::CXXConstructorDecl *constructor =
        constructed(made->getSubExpr());
    if (constructor == nullptr ||
        !(keepsMade(constructor) ||
          (safe && CxxKinds::aliased(constructor->getParent()))))
      return true;
    if (temporary(*made))
      kept({.kind = Origin::Kind::Temporary}, made->getExprLoc(),
           "its own constructor");
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *construct) {
    const clang::CXXConstructorDecl *constructor = construct->getConstructor();
    for (unsigned i = 0;
         constructor != nullptr && i < construct->getNumArgs() &&
         i < constructor->getNumParams();
         ++i)
      argument(*constructor, *constructor->getParamDecl(i),
               *construct->getArg(i), construct->getExprLoc());
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  clang::ASTContext &ast;
  const clang::FunctionDecl &function;
  bool safe;
  const RuleReport *report;
  LocalValues assigned;
  std::set<std::pair<Origin, unsigned>> reported;

  /// §4: a view's member (a `WEAVEC_BORROWED` pointer) holds a borrow the
  /// rules follow (X3, X4); storing in it keeps nothing.
  bool borrowed(const clang::ValueDecl *member) const {
    return member != nullptr &&
           hasAnnotation(annotationsOf(member->getType(), ast),
                         BorrowedAnnotation);
  }

  std::string spelled(const clang::Expr *expression) const {
    const llvm::StringRef text = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(expression->getSourceRange()),
        ast.getSourceManager(), ast.getLangOpts());
    return text.empty() || text.size() > 40 ? std::string("memory")
                                            : text.str();
  }

  /// RFC 0047 §5, RFC 0048 §4: a class whose objects other objects keep
  /// pointers to: aliased, or of a constructor that keeps its object.
  bool keptType(clang::QualType type, unsigned depth) const {
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > 8 || !record->hasDefinition())
      return false;
    record = record->getDefinition();
    if (CxxKinds::aliased(record) ||
        (!isLibrary(*record) &&
         llvm::any_of(record->ctors(),
                      [&](const auto *each) { return keepsMade(each); })))
      return true;
    return llvm::any_of(record->fields(),
                        [&](const clang::FieldDecl *field) {
                          return !field->getType()->isReferenceType() &&
                                 keptType(field->getType(), depth + 1);
                        }) ||
           llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return keptType(base.getType(), depth + 1);
                        });
  }

  /// A template's parameter whose type is the template's own (`T t`, given
  /// a pointer by an instantiation): a value the template may keep, as a
  /// container keeps its elements.
  static bool generic(const clang::ParmVarDecl &parameter) {
    const auto *owner =
        llvm::dyn_cast<clang::FunctionDecl>(parameter.getDeclContext());
    // (A library template's iterators and pointers are its algorithms'.)
    if (owner == nullptr || isLibrary(*owner))
      return false;
    const clang::FunctionDecl *pattern =
        owner->getTemplateInstantiationPattern();
    const unsigned index = parameter.getFunctionScopeIndex();
    if (pattern == nullptr || index >= pattern->getNumParams())
      return false;
    const clang::QualType written =
        pattern->getParamDecl(index)->getType().getNonReferenceType();
    return written->isDependentType() && !written->isPointerType();
  }

  /// Whether a parameter keeps what it is given: WEAVEC_ESCAPES, or a
  /// pointer or reference to an aliased class.
  bool keeps(clang::QualType type) const {
    return hasAnnotation(annotationsOf(type, ast), EscapesAnnotation) ||
           CxxKinds::aliased(
               type->getPointeeType().isNull()
                   ? nullptr
                   : type->getPointeeType()->getAsCXXRecordDecl());
  }

  /// The constructor an initialization runs, if it runs one.
  static const clang::CXXConstructorDecl *
  constructed(const clang::Expr *initialization) {
    if (initialization == nullptr)
      return nullptr;
    const clang::Expr *made = initialization->IgnoreImplicit();
    // (`T(x)`, which constructs as `T t(x)` does.)
    if (const auto *cast = llvm::dyn_cast<clang::CXXFunctionalCastExpr>(made))
      made = cast->getSubExpr()->IgnoreImplicit();
    const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(made);
    return construct == nullptr ? nullptr : construct->getConstructor();
  }

  /// What an initializer runs: a member's default initializer, if it is it.
  static const clang::Expr *initial(const clang::CXXCtorInitializer &each) {
    const auto *byDefault =
        llvm::dyn_cast<clang::CXXDefaultInitExpr>(each.getInit());
    return byDefault != nullptr ? byDefault->getExpr() : each.getInit();
  }

  /// Whether a temporary is no local (one a local reference extends too).
  static bool temporary(const clang::MaterializeTemporaryExpr &made) {
    const auto *extended =
        llvm::dyn_cast_or_null<clang::VarDecl>(made.getExtendingDecl());
    return extended == nullptr || extended->hasLocalStorage();
  }

  /// Whether a constructor keeps the object it makes: it says
  /// WEAVEC_ESCAPES, or its definition here makes a member or base that is
  /// kept, or is implicit and points a member into the object (a safe
  /// written one that does not say so is an error where it is made).
  bool keepsMade(const clang::CXXConstructorDecl *constructor,
                 unsigned depth = 0) const {
    const clang::FunctionDecl *definition = nullptr;
    if (constructor == nullptr || depth > 8)
      return false;
    if (hasAnnotation(annotationsOf(constructor->getType(), ast),
                      EscapesAnnotation))
      return true;
    return constructor->isDefined(definition) &&
           llvm::any_of(
               llvm::cast<clang::CXXConstructorDecl>(definition)->inits(),
               [&](const clang::CXXCtorInitializer *each) {
                 const clang::FieldDecl *member = each->getAnyMember();
                 return keepsMade(constructed(initial(*each)), depth + 1) ||
                        (definition->isImplicit() && member != nullptr &&
                         member->getType()->isPointerType() &&
                         !borrowed(member) &&
                         valueOrigins(initial(*each))
                             .contains({.kind = Origin::Kind::This}));
               });
  }

  /// Whether a member function keeps its object: WEAVEC_ESCAPES after its
  /// parameter list, or an aliased class's.
  bool keepsThis(const clang::CXXMethodDecl &method) const {
    return CxxKinds::aliased(method.getParent()) ||
           hasAnnotation(annotationsOf(method.getType(), ast),
                         EscapesAnnotation);
  }

  /// What a library container of pointers keeps of a pointer argument.
  static bool storesPointers(const clang::FunctionDecl &callee) {
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&callee);
    const auto *specialization =
        method == nullptr
            ? nullptr
            : llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
                  method->getParent());
    if (specialization == nullptr || !isLibrary(*specialization))
      return false;
    return llvm::any_of(specialization->getTemplateArgs().asArray(),
                        [](const clang::TemplateArgument &argument) {
                          return argument.getKind() ==
                                     clang::TemplateArgument::Type &&
                                 argument.getAsType()->isPointerType();
                        });
  }

  void argument(const clang::FunctionDecl &callee,
                const clang::ParmVarDecl &parameter,
                const clang::Expr &argument, clang::SourceLocation at) {
    const clang::QualType type = parameter.getType();
    const std::string what = "'" + callee.getNameAsString() + "'";
    const bool held =
        isLibrary(callee) && storesPointers(callee) &&
        argument.IgnoreParenImpCasts()->getType()->isPointerType();
    if (type->isReferenceType() && keeps(type))
      keep(objectOrigins(&argument), at, what);
    else if ((type->isPointerType() && (keeps(type) || generic(parameter))) ||
             held)
      keep(valueOrigins(&argument), at, what);
  }

  /// Where a pointer value points: into a local, a parameter's referent,
  /// `this`; nothing for a pointer loaded from memory or returned.
  std::set<Origin> valueOrigins(const clang::Expr *expression,
                                unsigned depth = 0) const {
    std::set<Origin> found;
    if (expression == nullptr || depth > 12)
      return found;
    const clang::Expr *e = expression->IgnoreParenCasts();
    if (const auto *made = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e))
      e = made->getSubExpr()->IgnoreParenCasts();
    if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(e))
      e = cleanups->getSubExpr()->IgnoreParenCasts();
    if (e->getType()->isArrayType()) {
      found = objectOrigins(e, depth + 1);
    } else if (llvm::isa<clang::CXXThisExpr>(e)) {
      found.insert({.kind = Origin::Kind::This});
    } else if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e);
               op != nullptr && op->getOpcode() == clang::UO_AddrOf) {
      found = objectOrigins(op->getSubExpr(), depth + 1);
    } else if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e);
               call != nullptr && call->getNumArgs() == 1 &&
               (call->getBuiltinCallee() == clang::Builtin::BIaddressof ||
                call->getBuiltinCallee() == clang::Builtin::BI__addressof ||
                call->getBuiltinCallee() ==
                    clang::Builtin::BI__builtin_addressof)) {
      found = objectOrigins(call->getArg(0), depth + 1);
    } else if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      if (const auto *parameter =
              llvm::dyn_cast<clang::ParmVarDecl>(named->getDecl());
          parameter != nullptr && parameter->getType()->isPointerType()) {
        found.insert({.kind = Origin::Kind::Parameter, .variable = parameter});
      } else if (const clang::VarDecl *variable = localPointer(named)) {
        const auto values = assigned.find(variable);
        if (values != assigned.end())
          for (const clang::Expr *value : values->second)
            found.merge(valueOrigins(value, depth + 1));
      }
    } else if (const auto *conditional =
                   llvm::dyn_cast<clang::AbstractConditionalOperator>(e)) {
      found = valueOrigins(conditional->getTrueExpr(), depth + 1);
      found.merge(valueOrigins(conditional->getFalseExpr(), depth + 1));
    } else if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (binary->getOpcode() == clang::BO_Comma)
        found = valueOrigins(binary->getRHS(), depth + 1);
      else if (binary->isAdditiveOp())
        found = valueOrigins(binary->getLHS()->getType()->isPointerType()
                                 ? binary->getLHS()
                                 : binary->getRHS(),
                             depth + 1);
    }
    return found;
  }

  /// What an lvalue is a part of.
  std::set<Origin> objectOrigins(const clang::Expr *expression,
                                 unsigned depth = 0) const {
    std::set<Origin> found;
    if (expression == nullptr || depth > 12)
      return found;
    // (IgnoreParenImpCasts would look through a temporary too.)
    const clang::Expr *e = expression->IgnoreParens();
    while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
      e = cast->getSubExpr()->IgnoreParens();
    if (const auto *made = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(e)) {
      if (temporary(*made))
        found.insert({.kind = Origin::Kind::Temporary});
      return found;
    }
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      const auto *variable = llvm::dyn_cast<clang::VarDecl>(named->getDecl());
      if (variable == nullptr)
        return found;
      if (variable->getType()->isReferenceType()) {
        if (llvm::isa<clang::ParmVarDecl>(variable))
          found.insert({.kind = Origin::Kind::Parameter, .variable = variable});
        else if (variable->hasLocalStorage() && variable->hasInit())
          found = objectOrigins(variable->getInit(), depth + 1);
      } else if (variable->hasLocalStorage() ||
                 variable->getTLSKind() != clang::VarDecl::TLS_None) {
        found.insert({.kind = Origin::Kind::Local, .variable = variable});
      }
      return found;
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
      if (member->getMemberDecl()->getType()->isReferenceType())
        return found;
      return member->isArrow() ? valueOrigins(member->getBase(), depth + 1)
                               : objectOrigins(member->getBase(), depth + 1);
    }
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e);
        op != nullptr && op->getOpcode() == clang::UO_Deref)
      return valueOrigins(op->getSubExpr(), depth + 1);
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
      const clang::Expr *base = subscript->getBase()->IgnoreParenImpCasts();
      return base->getType()->isArrayType() ? objectOrigins(base, depth + 1)
                                            : valueOrigins(base, depth + 1);
    }
    if (llvm::isa<clang::CXXThisExpr>(e))
      found.insert({.kind = Origin::Kind::This});
    return found;
  }

  void keep(const std::set<Origin> &origins, clang::SourceLocation at,
            const std::string &where) {
    for (const Origin &origin : origins)
      kept(origin, at, where);
  }

  void place(const clang::VarDecl &variable) {
    for (const auto *annotate : variable.specific_attrs<clang::AnnotateAttr>())
      if (annotate->getAnnotation().starts_with(PlacedAnnotation))
        return;
    const std::string text =
        PlacedAnnotation.str() + " " +
        variable.getType().getCanonicalType().getUnqualifiedType().getAsString(
            ast.getPrintingPolicy());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto &mutableVariable = const_cast<clang::VarDecl &>(variable);
    mutableVariable.addAttr(
        clang::AnnotateAttr::CreateImplicit(ast, text, nullptr, 0));
    // A local that is returned by value is copied out of its block.
    if (variable.isNRVOVariable())
      mutableVariable.setNRVOVariable(false);
  }

  void kept(const Origin &origin, clang::SourceLocation at,
            const std::string &where) {
    using Kind = Origin::Kind;
    const auto *parameter =
        llvm::dyn_cast_or_null<clang::ParmVarDecl>(origin.variable);
    if (origin.kind == Kind::Local && parameter == nullptr &&
        origin.variable->getTLSKind() == clang::VarDecl::TLS_None) {
      place(*origin.variable);
      return;
    }
    if (report == nullptr || !safe)
      return;
    const unsigned line = ast.getSourceManager().getPresumedLineNumber(at);
    if (!reported.insert({origin, line}).second)
      return;
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    std::string message;
    std::string note;
    switch (origin.kind) {
    case Kind::Parameter:
      if (keeps(parameter->getType()) || generic(*parameter))
        return;
      message = "the address of '" + parameter->getNameAsString() +
                "' is kept by " + where + ", and '" +
                function.getNameAsString() + "' only borrows it";
      note = "declare the parameter WEAVEC_ESCAPES (after its '*' or '&') "
             "if '" +
             function.getNameAsString() +
             "' keeps it; a caller's local then lives in typed memory";
      break;
    case Kind::This:
      if (method == nullptr || keepsThis(*method) ||
          method->getParent()->isLambda())
        return;
      message = "'this' is kept by " + where + ", and '" +
                function.getNameAsString() + "' only borrows it";
      note = "write WEAVEC_ESCAPES after the parameter list of '" +
             function.getNameAsString() + "', or declare '" +
             method->getParent()->getNameAsString() + "' WEAVEC_ALIASED";
      break;
    case Kind::Temporary:
      message = "a temporary is kept by " + where;
      note = "keep a local, which then lives in typed memory, or an object "
             "of the heap";
      break;
    case Kind::Local:
      message = parameter != nullptr
                    ? "the address of '" + parameter->getNameAsString() +
                          "', a parameter taken by value, is kept by " + where
                    : "'" + origin.variable->getNameAsString() +
                          "' is thread_local, and its address is kept by " +
                          where;
      note = parameter != nullptr
                 ? "take it by reference and declare it WEAVEC_ESCAPES, or "
                   "move it into what keeps it"
                 : "keep an object of the heap or a global instead";
      break;
    }
    (*report)(at, "escape", message, at, note);
  }
};

} // namespace

const clang::VarDecl *localPointer(const clang::Expr *expression) {
  const auto *named =
      llvm::dyn_cast<clang::DeclRefExpr>(expression->IgnoreParenImpCasts());
  const auto *variable = named != nullptr
                             ? llvm::dyn_cast<clang::VarDecl>(named->getDecl())
                             : nullptr;
  return variable != nullptr && variable->hasLocalStorage() &&
                 variable->getType()->isPointerType()
             ? variable
             : nullptr;
}

void localValues(const clang::Stmt *statement, LocalValues &values,
                 llvm::DenseSet<const clang::VarDecl *> *stepped) {
  if (statement == nullptr)
    return;
  const auto *op = llvm::dyn_cast<clang::BinaryOperator>(statement);
  const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(statement);
  if (op != nullptr && op->getOpcode() == clang::BO_Assign)
    if (const clang::VarDecl *variable = localPointer(op->getLHS()))
      values[variable].push_back(op->getRHS());
  if (stepped != nullptr &&
      ((unary != nullptr && unary->isIncrementDecrementOp()) ||
       (op != nullptr && (op->getOpcode() == clang::BO_AddAssign ||
                          op->getOpcode() == clang::BO_SubAssign))))
    if (const clang::VarDecl *variable =
            localPointer(unary != nullptr ? unary->getSubExpr() : op->getLHS()))
      stepped->insert(variable);
  if (const auto *declaration = llvm::dyn_cast<clang::DeclStmt>(statement))
    for (const clang::Decl *each : declaration->decls())
      if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(each);
          variable != nullptr && variable->hasInit() &&
          variable->hasLocalStorage() && variable->getType()->isPointerType())
        values[variable].push_back(variable->getInit());
  for (const clang::Stmt *child : statement->children())
    localValues(child, values, stepped);
}

void keptAddresses(clang::ASTContext &ast, const clang::FunctionDecl &function,
                   bool safe, const RuleReport *report) {
  if (!function.hasBody() || function.isDependentContext())
    return;
  Kept(ast, function, safe, report).run();
}

void checkCxx(clang::ASTContext &ast, bool unitSafe, SafeUnit &unit,
              const RuleReport &report, const BoundsRules &bounds) {
  CxxUnit(ast, unitSafe, unit, report, bounds)
      .TraverseDecl(ast.getTranslationUnitDecl());
}

} // namespace weavec::frontend
