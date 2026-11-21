//===- SafeFix.cpp - The fixes of the rules' errors -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0049 §1–§5. A rule that reports an error says what it wants of a
// declaration (a bound, that a parameter is kept, that a global is
// `const`); the planner turns the want into fixes. A certain fix states
// what the function with the error already does. A likely fix states a
// fact the uses of the declaration agree on, each use read inside the one
// function it is in (§3.2): a library call that pairs the pointer with a
// count (E1), a comparison that bounds its index (E2) or the pointer
// itself (E3), a byte tested against zero (E4), constant indices (E5), a
// bound that flows in or out (E6), a result that is always one parameter
// (E7). Uses that disagree make a choice, which is printed and never
// written. A fix is source text and nothing else: what it inserts is an
// annotation the rules then check as one a person wrote.
//
//===----------------------------------------------------------------------===//

#include "SafeCheckImpl.h"
#include "SafeCxxKinds.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace weavec::frontend {

namespace {

/// A fact a fix states (§3.2): a bound and what counts or ends it, or one
/// of the other annotations.
struct Fact {
  /// `counted_by`, `sized_by`, `ended_by`, `null_terminated`,
  /// `null_terminated_or_null`, `escapes`, `borrowed`, `const`,
  /// `lifetimebound`.
  std::string kind;
  /// What counts or ends it: a name or a constant.
  std::string name;
  /// The name is the function's parameter of this index (renamed in each
  /// declaration of the function).
  int parameter = -1;

  [[nodiscard]] std::string str() const {
    return name.empty() ? kind : kind + "(" + name + ")";
  }
  bool operator<(const Fact &other) const { return str() < other.str(); }
};

} // namespace

/// The macro of a fact, with its argument.
static std::string macroOf(const Fact &fact, const std::string &name) {
  std::string macro = "WEAVEC_" + llvm::StringRef(fact.kind).upper();
  if (fact.kind == "const")
    return "const";
  return fact.name.empty() ? macro : macro + "(" + name + ")";
}

namespace {

/// What one use of a declaration says, and where.
struct Clue {
  Fact fact;
  std::string why;
  /// Weak evidence (set from a literal) counts only without strong.
  bool weak = false;
  /// What the allocation behind it has: a comparison or a call bounds what
  /// is read, which may be less, so this alone counts when there is any.
  bool exact = false;
};

/// A fix as planned: the data, and the note's place and insertions.
struct Planned {
  RuleFix fix;
  clang::SourceLocation at;
  std::vector<std::pair<clang::SourceLocation, std::string>> insertions;
  /// RFC 0051 §4.1: the characters a removal takes out.
  // A designated initializer may leave it out.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::vector<clang::CharSourceRange> removals = {};
  /// What the note says to write.
  std::string what;
};

} // namespace

struct FixPlanner::Impl {
  clang::ASTContext &ast;
  const clang::SourceManager &sources;
  const clang::Preprocessor *preprocessor;
  struct Pending {
    clang::SourceLocation at;
    Want want;
  };
  std::optional<Pending> pending;

  Impl(clang::ASTContext &ast, const clang::Preprocessor *preprocessor)
      : ast(ast), sources(ast.getSourceManager()), preprocessor(preprocessor) {}

  //===--------------------------------------------------------------------===//
  // Declarations, their keys and their uses
  //===--------------------------------------------------------------------===//

  /// The declaration a fix is written on: a template's for an
  /// instantiation's, the first of a function's.
  static const clang::Decl *pattern(const clang::Decl *declaration) {
    if (declaration == nullptr)
      return nullptr;
    if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(declaration)) {
      const auto *record =
          llvm::dyn_cast<clang::CXXRecordDecl>(field->getParent());
      const clang::CXXRecordDecl *from =
          record == nullptr ? nullptr
                            : record->getTemplateInstantiationPattern();
      if (from != nullptr && from != record)
        for (const clang::FieldDecl *each : from->fields())
          if (each->getName() == field->getName())
            return each;
      return field;
    }
    if (const auto *parameter =
            llvm::dyn_cast<clang::ParmVarDecl>(declaration)) {
      const auto *function =
          llvm::dyn_cast<clang::FunctionDecl>(parameter->getDeclContext());
      if (function == nullptr)
        return parameter;
      const auto *first = llvm::cast<clang::FunctionDecl>(pattern(function));
      const unsigned index = parameter->getFunctionScopeIndex();
      return index < first->getNumParams() ? first->getParamDecl(index)
                                           : parameter;
    }
    if (const auto *function =
            llvm::dyn_cast<clang::FunctionDecl>(declaration)) {
      const clang::FunctionDecl *from =
          function->getTemplateInstantiationPattern();
      return (from != nullptr ? from : function)->getFirstDecl();
    }
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(declaration)) {
      const clang::VarDecl *from = variable->getTemplateInstantiationPattern();
      return (from != nullptr ? from : variable)->getFirstDecl();
    }
    if (const auto *record = llvm::dyn_cast<clang::CXXRecordDecl>(declaration))
      if (const clang::CXXRecordDecl *from =
              record->getTemplateInstantiationPattern())
        return from;
    return declaration;
  }

  /// The absolute path of a location's file.
  std::string path(clang::SourceLocation at) const {
    const clang::FileID file = sources.getFileID(sources.getExpansionLoc(at));
    const clang::OptionalFileEntryRef entry =
        sources.getFileEntryRefForID(file);
    if (!entry)
      return {};
    llvm::StringRef real = entry->getFileEntry().tryGetRealPathName();
    llvm::SmallString<256> name(real.empty() ? entry->getName() : real);
    std::ignore = llvm::sys::fs::make_absolute(name);
    llvm::sys::path::remove_dots(name, /*remove_dot_dot=*/true);
    return name.str().str();
  }

  std::string where(clang::SourceLocation at) const {
    at = sources.getExpansionLoc(at);
    return path(at) + ":" + std::to_string(sources.getSpellingLineNumber(at)) +
           ":" + std::to_string(sources.getSpellingColumnNumber(at));
  }

  /// A declaration's key, which edits do not move: its file and its
  /// qualified name; a function's with its type (overloads), a
  /// parameter's with its position, a static local's with its function's.
  std::string key(const clang::Decl *declaration) const {
    declaration = pattern(declaration);
    const auto named = [&](const clang::NamedDecl *each) {
      std::string name = each->getQualifiedNameAsString();
      if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(each))
        name += "|" + function->getType().getCanonicalType().getAsString(
                          ast.getPrintingPolicy());
      return name;
    };
    std::string name;
    if (const auto *parameter =
            llvm::dyn_cast<clang::ParmVarDecl>(declaration)) {
      const auto *function =
          llvm::dyn_cast<clang::FunctionDecl>(parameter->getDeclContext());
      name = (function == nullptr ? std::string() : named(function)) + "#" +
             std::to_string(parameter->getFunctionScopeIndex());
    } else if (const auto *variable =
                   llvm::dyn_cast<clang::VarDecl>(declaration);
               variable != nullptr && variable->isStaticLocal()) {
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(
          variable->getParentFunctionOrMethod());
      name = (function == nullptr ? std::string() : named(function)) +
             "::" + variable->getName().str();
    } else if (const auto *each =
                   llvm::dyn_cast<clang::NamedDecl>(declaration)) {
      name = named(each);
    }
    return path(declaration->getLocation()) + "|" + name;
  }

  /// A name a note gives a declaration: `Class::member`, `f`'s `p`.
  static std::string nameOf(const clang::Decl *declaration) {
    const auto *named = llvm::dyn_cast<clang::NamedDecl>(declaration);
    if (named == nullptr)
      return "";
    if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(named))
      return field->getParent()->getName().str() +
             "::" + field->getName().str();
    return named->getNameAsString();
  }

  struct Use {
    const clang::FunctionDecl *function;
    const clang::Expr *expression;
  };
  bool indexed = false;
  llvm::DenseMap<const clang::Decl *, std::vector<Use>> uses;
  std::vector<const clang::FunctionDecl *> definitions;
  llvm::DenseMap<const clang::FunctionDecl *, std::unique_ptr<clang::ParentMap>>
      parentMaps;
  /// A constructor's initializers, by their expression.
  llvm::DenseMap<const clang::Expr *, const clang::CXXCtorInitializer *>
      initializers;

  /// Every use of a parameter, a member or a global in the unit's
  /// functions (instantiations included), by the declaration a fix names.
  void index() {
    if (indexed)
      return;
    indexed = true;
    struct Visitor final : clang::RecursiveASTVisitor<Visitor> {
      Impl *impl = nullptr;
      // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
      [[nodiscard]] static bool shouldVisitTemplateInstantiations() {
        return true;
      }
      // Nothing a system header declares is the program's to annotate.
      bool TraverseDecl(clang::Decl *declaration) {
        if (declaration != nullptr &&
            !llvm::isa<clang::TranslationUnitDecl>(declaration) &&
            isLibrary(*declaration))
          return true;
        return clang::RecursiveASTVisitor<Visitor>::TraverseDecl(declaration);
      }
      // The uses are collected from each body below; the walk goes no
      // further into it.
      static bool TraverseStmt(clang::Stmt * /*statement*/) { return true; }
      bool VisitFunctionDecl(clang::FunctionDecl *function) {
        if (!function->doesThisDeclarationHaveABody() ||
            function->isDependentContext() || isLibrary(*function))
          return true;
        impl->definitions.push_back(function);
        const auto add = [&](const clang::Stmt *statement, const auto &self) {
          if (statement == nullptr)
            return;
          const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement);
          if (named != nullptr && llvm::isa<clang::VarDecl>(named->getDecl()))
            impl->uses[pattern(named->getDecl())].push_back(
                {.function = function, .expression = named});
          const auto *member = llvm::dyn_cast<clang::MemberExpr>(statement);
          if (member != nullptr &&
              llvm::isa<clang::FieldDecl>(member->getMemberDecl()))
            impl->uses[pattern(member->getMemberDecl())].push_back(
                {.function = function, .expression = member});
          for (const clang::Stmt *child : statement->children())
            self(child, self);
        };
        if (const auto *constructor =
                llvm::dyn_cast<clang::CXXConstructorDecl>(function))
          for (const clang::CXXCtorInitializer *each : constructor->inits())
            if (each->isWritten()) {
              impl->initializers[each->getInit()] = each;
              add(each->getInit(), add);
            }
        add(function->getBody(), add);
        return true;
      }
      // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
    } visitor;
    visitor.impl = this;
    visitor.TraverseDecl(ast.getTranslationUnitDecl());
  }

  clang::ParentMap &parents(const clang::FunctionDecl *function) {
    std::unique_ptr<clang::ParentMap> &map = parentMaps[function];
    if (!map) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      map = std::make_unique<clang::ParentMap>(
          const_cast<clang::Stmt *>(function->getBody()));
      if (const auto *constructor =
              llvm::dyn_cast<clang::CXXConstructorDecl>(function))
        for (const clang::CXXCtorInitializer *each : constructor->inits())
          if (each->isWritten())
            map->addStmt(each->getInit());
    }
    return *map;
  }

  /// The expression around `expression` that is not a cast or parentheses,
  /// and whether a cast on the way tests the pointer against null.
  const clang::Stmt *climb(const clang::FunctionDecl *function,
                           const clang::Expr *&expression, bool *tested) {
    const clang::ParentMap &map = parents(function);
    const clang::Stmt *up = map.getParent(expression);
    while (up != nullptr && (llvm::isa<clang::ParenExpr>(up) ||
                             llvm::isa<clang::ImplicitCastExpr>(up))) {
      if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(up);
          cast != nullptr && tested != nullptr &&
          cast->getCastKind() == clang::CK_PointerToBoolean)
        *tested = true;
      expression = llvm::cast<clang::Expr>(up);
      up = map.getParent(up);
    }
    return up;
  }

  //===--------------------------------------------------------------------===//
  // What a declaration is wanted of, from an expression
  //===--------------------------------------------------------------------===//

  /// The declaration a pointer's value comes from: a parameter, a member, a
  /// global, a function's result; a local's, through what it is given.
  const clang::Decl *origin(const clang::Expr *expression,
                            const clang::FunctionDecl *function,
                            unsigned depth = 0) {
    if (expression == nullptr || depth > 6)
      return nullptr;
    const clang::Expr *e = expression->IgnoreParenImpCasts();
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e);
        op != nullptr && op->isIncrementDecrementOp())
      return origin(op->getSubExpr(), function, depth + 1);
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (op->isAdditiveOp() || op->isCompoundAssignmentOp())
        return origin(op->getLHS()->getType()->isPointerType() ? op->getLHS()
                                                               : op->getRHS(),
                      function, depth + 1);
      if (op->getOpcode() == clang::BO_Assign)
        return origin(op->getLHS(), function, depth + 1);
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
      return llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      return callee != nullptr && !isLibrary(*callee) ? callee : nullptr;
    }
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(e);
    const auto *variable =
        named == nullptr ? nullptr
                         : llvm::dyn_cast<clang::VarDecl>(named->getDecl());
    if (variable == nullptr)
      return nullptr;
    if (llvm::isa<clang::ParmVarDecl>(variable) || !variable->hasLocalStorage())
      return variable;
    if (function == nullptr)
      return nullptr;
    LocalValues values;
    localValues(function->getBody(), values);
    const clang::Decl *found = nullptr;
    for (const clang::Expr *value : values[variable]) {
      const clang::Decl *each = origin(value, function, depth + 1);
      if (each == nullptr || (found != nullptr && found != each))
        return nullptr;
      found = each;
    }
    return found;
  }

  //===--------------------------------------------------------------------===//
  // Evidence (§3.2)
  //===--------------------------------------------------------------------===//

  /// Whether `count` can be written in `target`'s declaration (a name or a
  /// constant): beside a member, a member of the same object as `use`;
  /// beside a parameter or a result, a parameter of the function (or, for
  /// a member function's result, a member of its object).
  std::optional<Fact> admissible(const clang::Expr *count,
                                 const clang::Expr *use,
                                 const clang::Decl *target,
                                 const clang::FunctionDecl *function,
                                 llvm::StringRef kind) const {
    if (count == nullptr)
      return std::nullopt;
    const clang::Expr *e = count->IgnoreParenImpCasts();
    if (const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(e))
      e = cast->getSubExpr()->IgnoreParenImpCasts();
    const bool pointer = kind == "ended_by";
    if (!pointer && !e->isValueDependent() &&
        llvm::isa<clang::IntegerLiteral>(e)) {
      clang::Expr::EvalResult result;
      if (e->EvaluateAsInt(result, ast) && !result.Val.getInt().isNegative() &&
          !global(target))
        return Fact{.kind = kind.str(),
                    .name = std::to_string(result.Val.getInt().getExtValue()),
                    .parameter = -1};
      return std::nullopt;
    }
    if (pointer != e->getType()->isPointerType())
      return std::nullopt;
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      const auto *parameter =
          llvm::dyn_cast<clang::ParmVarDecl>(named->getDecl());
      const auto *used =
          llvm::dyn_cast<clang::DeclRefExpr>(use->IgnoreParenImpCasts());
      if (parameter == nullptr || parameter->getDeclContext() != function ||
          (used != nullptr && used->getDecl() == parameter) ||
          pattern(parameter) == target || parameter->getName().empty())
        return std::nullopt;
      const bool besideParameter = llvm::isa<clang::ParmVarDecl>(target);
      const bool besideResult = llvm::isa<clang::FunctionDecl>(target);
      if (!besideParameter && !besideResult)
        return std::nullopt;
      return Fact{.kind = kind.str(),
                  .name = parameter->getName().str(),
                  .parameter =
                      static_cast<int>(parameter->getFunctionScopeIndex())};
    }
    const auto *member = llvm::dyn_cast<clang::MemberExpr>(e);
    const auto *field =
        member == nullptr
            ? nullptr
            : llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr)
      return std::nullopt;
    if (const auto *targetField = llvm::dyn_cast<clang::FieldDecl>(target)) {
      const auto *used =
          llvm::dyn_cast<clang::MemberExpr>(use->IgnoreParenImpCasts());
      if (used == nullptr || pattern(field) == target ||
          pattern(field)->getDeclContext() !=
              pattern(targetField)->getDeclContext() ||
          !sameObject(used->getBase(), member->getBase()))
        return std::nullopt;
      return Fact{
          .kind = kind.str(), .name = field->getName().str(), .parameter = -1};
    }
    // A member function's result counted by a member of its object.
    if (llvm::isa<clang::FunctionDecl>(target) &&
        llvm::isa<clang::CXXThisExpr>(member->getBase()->IgnoreParenImpCasts()))
      return Fact{
          .kind = kind.str(), .name = field->getName().str(), .parameter = -1};
    return std::nullopt;
  }

  /// A global (or static) variable: what counts it is a constant alone.
  static bool global(const clang::Decl *target) {
    return llvm::isa<clang::VarDecl>(target) &&
           !llvm::isa<clang::ParmVarDecl>(target);
  }

  /// Whether two member accesses' bases are one object: `this`, or one
  /// variable.
  static bool sameObject(const clang::Expr *a, const clang::Expr *b) {
    a = a->IgnoreParenImpCasts();
    b = b->IgnoreParenImpCasts();
    if (llvm::isa<clang::CXXThisExpr>(a) && llvm::isa<clang::CXXThisExpr>(b))
      return true;
    const auto *left = llvm::dyn_cast<clang::DeclRefExpr>(a);
    const auto *right = llvm::dyn_cast<clang::DeclRefExpr>(b);
    return left != nullptr && right != nullptr &&
           left->getDecl() == right->getDecl();
  }

  /// The bound a declaration states, as a fact.
  std::optional<Fact> stated(const clang::QualType type) const {
    for (const TypeAnnotation &annotation : annotationsOf(type, ast)) {
      llvm::StringRef kind = annotation.name;
      if (!kind.consume_front("weavec."))
        continue;
      if (kind == "counted_by" || kind == "sized_by" || kind == "ended_by" ||
          kind == "null_terminated" || kind == "null_terminated_or_null" ||
          kind == "counted_by_or_null")
        return Fact{
            .kind = kind.str(), .name = annotation.argument, .parameter = -1};
    }
    return std::nullopt;
  }

  /// The argument of a call for a callee's parameter named `name`.
  static const clang::Expr *
  argumentNamed(const clang::FunctionDecl &callee,
                llvm::ArrayRef<const clang::Expr *> arguments,
                llvm::StringRef name) {
    for (unsigned i = 0; i < callee.getNumParams() && i < arguments.size(); ++i)
      if (callee.getParamDecl(i)->getName() == name)
        return arguments[i];
    return nullptr;
  }

  /// E1, E4, E6: a use that is argument `index` of a call.
  void argument(const clang::FunctionDecl &callee,
                llvm::ArrayRef<const clang::Expr *> arguments, unsigned index,
                const clang::Expr *use, const clang::Decl *target,
                const clang::FunctionDecl *function, clang::SourceLocation at,
                std::vector<Clue> &clues) {
    const std::string line =
        " at line " + std::to_string(sources.getExpansionLineNumber(at));
    const std::string calls = "'" + callee.getNameAsString() + "'";
    const clang::QualType pointee = use->getType()->isPointerType()
                                        ? use->getType()->getPointeeType()
                                        : clang::QualType();
    const bool bytes = !pointee.isNull() && !pointee->isIncompleteType() &&
                       ast.getTypeSizeInChars(pointee).getQuantity() == 1;
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&callee);
    if (isLibrary(callee) && method != nullptr) {
      // RFC 0047 §1.3: a library class's counted arguments.
      const std::string member = llvm::isa<clang::CXXConstructorDecl>(method)
                                     ? method->getParent()->getNameAsString()
                                     : method->getNameAsString();
      for (const core::ArgumentBound &bound :
           core::LibrarySpec::shipped().argumentBounds(
               CxxKinds::qualifiedName(*method->getParent()), member))
        if (bound.pointer == index && bound.count < arguments.size())
          if (auto fact =
                  admissible(arguments[bound.count], use, target, function,
                             bound.ended ? "ended_by" : "counted_by"))
            clues.push_back(
                {.fact = *fact,
                 .why = llvm::formatv("'{0}' is its {1} in the call to {2}{3}",
                                      fact->name, bound.ended ? "end" : "count",
                                      calls, line)
                            .str(),
                 .weak = false});
      return;
    }
    if (isLibrary(callee) || callee.isExternC()) {
      const auto match = core::LibrarySpec::shipped().lookup(
          callee.getName().str(), static_cast<unsigned>(arguments.size()));
      if (!match || match->entry == nullptr ||
          index >= match->entry->params.size())
        return;
      const core::LibraryParam &param = match->entry->params[index];
      // A fortified call by the name the program wrote.
      const std::string row = "'" + match->entry->name + "'";
      if (param.string && !pointee.isNull() && pointee->isAnyCharacterType())
        clues.push_back(
            {.fact = {.kind = "null_terminated", .name = "", .parameter = -1},
             .why = "it is passed to " + row + line,
             .weak = false});
      if (param.bytes && param.bytes->kind == core::LibTerm::Kind::Argument &&
          param.bytes->arg < arguments.size())
        if (auto fact = admissible(arguments[param.bytes->arg], use, target,
                                   function, bytes ? "counted_by" : "sized_by"))
          clues.push_back({.fact = *fact,
                           .why = "'" + fact->name +
                                  "' is its count in the "
                                  "call to " +
                                  row + line,
                           .weak = false});
      return;
    }
    // A project function's parameter that states a bound.
    if (index >= callee.getNumParams())
      return;
    const std::optional<Fact> given =
        stated(callee.getParamDecl(index)->getType());
    if (!given)
      return;
    if (given->name.empty()) {
      clues.push_back(
          {.fact = {.kind = given->kind, .name = "", .parameter = -1},
           .why = "it is passed for '" +
                  callee.getParamDecl(index)->getName().str() + "' of " +
                  calls + line,
           .weak = false});
      return;
    }
    if (auto fact = admissible(argumentNamed(callee, arguments, given->name),
                               use, target, function, given->kind))
      clues.push_back({.fact = *fact,
                       .why = "'" + fact->name +
                              "' is its count in the call "
                              "to " +
                              calls + line,
                       .weak = false});
  }

  /// E2: the comparisons that guard `inside` (an index of the pointer,
  /// the variable `index`) in its function.
  void guarded(const clang::Stmt *inside, const clang::VarDecl *index,
               const clang::Expr *use, const clang::Decl *target,
               const clang::FunctionDecl *function, std::vector<Clue> &clues) {
    const clang::ParentMap &map = parents(function);
    const auto compare = [&](const clang::Expr *condition, const auto &self) {
      if (condition == nullptr)
        return;
      const auto *op = llvm::dyn_cast<clang::BinaryOperator>(
          condition->IgnoreParenImpCasts());
      if (op == nullptr)
        return;
      if (op->getOpcode() == clang::BO_LAnd) {
        self(op->getLHS(), self);
        self(op->getRHS(), self);
        return;
      }
      const auto names = [&](const clang::Expr *side) {
        const auto *named =
            llvm::dyn_cast<clang::DeclRefExpr>(side->IgnoreParenImpCasts());
        return named != nullptr && named->getDecl() == index;
      };
      const clang::Expr *bound = nullptr;
      if ((op->getOpcode() == clang::BO_LT ||
           op->getOpcode() == clang::BO_NE) &&
          names(op->getLHS()))
        bound = op->getRHS();
      else if ((op->getOpcode() == clang::BO_GT ||
                op->getOpcode() == clang::BO_NE) &&
               names(op->getRHS()))
        bound = op->getLHS();
      if (bound != nullptr)
        if (auto fact = admissible(bound, use, target, function, "counted_by"))
          clues.push_back(
              {.fact = *fact,
               .why = "'" + fact->name + "' bounds its index at line " +
                      std::to_string(
                          sources.getExpansionLineNumber(op->getExprLoc())),
               .weak = false});
    };
    for (const clang::Stmt *child = inside, *up = map.getParent(inside);
         up != nullptr; child = up, up = map.getParent(up)) {
      if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(up);
          loop != nullptr && child == loop->getBody())
        compare(loop->getCond(), compare);
      else if (const auto *whileLoop = llvm::dyn_cast<clang::WhileStmt>(up);
               whileLoop != nullptr && child == whileLoop->getBody())
        compare(whileLoop->getCond(), compare);
      else if (const auto *branch = llvm::dyn_cast<clang::IfStmt>(up);
               branch != nullptr && child == branch->getThen())
        compare(branch->getCond(), compare);
      else if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(up);
               op != nullptr && op->getOpcode() == clang::BO_LAnd &&
               child == op->getRHS())
        compare(op->getLHS(), compare);
      else if (const auto *choice =
                   llvm::dyn_cast<clang::ConditionalOperator>(up);
               choice != nullptr && child == choice->getTrueExpr())
        compare(choice->getCond(), compare);
    }
  }

  /// Whether `function` steps `variable` forward (`++`, `+=`) and never
  /// back.
  static bool forward(const clang::FunctionDecl *function,
                      const clang::ValueDecl *variable) {
    bool ahead = false;
    bool back = false;
    const auto visit = [&](const clang::Stmt *statement, const auto &self) {
      if (statement == nullptr)
        return;
      const clang::Expr *stepped = nullptr;
      bool up = false;
      if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(statement);
          op != nullptr && op->isIncrementDecrementOp()) {
        stepped = op->getSubExpr();
        up = op->isIncrementOp();
      } else if (const auto *assign =
                     llvm::dyn_cast<clang::BinaryOperator>(statement);
                 assign != nullptr &&
                 (assign->getOpcode() == clang::BO_AddAssign ||
                  assign->getOpcode() == clang::BO_SubAssign)) {
        stepped = assign->getLHS();
        up = assign->getOpcode() == clang::BO_AddAssign;
      }
      if (const auto *named = stepped == nullptr
                                  ? nullptr
                                  : llvm::dyn_cast<clang::DeclRefExpr>(
                                        stepped->IgnoreParenImpCasts());
          named != nullptr && named->getDecl() == variable)
        (up ? ahead : back) = true;
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(function->getBody(), visit);
    return ahead && !back;
  }

  /// Whether `condition` is (a conjunct of) a loop's condition.
  bool loops(const clang::FunctionDecl *function,
             const clang::Stmt *condition) {
    const clang::ParentMap &map = parents(function);
    const clang::Stmt *child = condition;
    for (const clang::Stmt *up = map.getParent(child); up != nullptr;
         child = up, up = map.getParent(up)) {
      if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(up))
        return child == loop->getCond();
      if (const auto *loop = llvm::dyn_cast<clang::WhileStmt>(up))
        return child == loop->getCond();
      if (const auto *loop = llvm::dyn_cast<clang::DoStmt>(up))
        return child == loop->getCond();
      const auto *op = llvm::dyn_cast<clang::BinaryOperator>(up);
      if (!llvm::isa<clang::ParenExpr, clang::ImplicitCastExpr>(up) &&
          (op == nullptr || op->getOpcode() != clang::BO_LAnd))
        return false;
    }
    return false;
  }

  /// E4: whether a byte read through the pointer is tested against zero.
  bool testsZero(const clang::FunctionDecl *function, const clang::Expr *read) {
    if (!read->getType()->isAnyCharacterType())
      return false;
    const clang::Stmt *up = climb(function, read, nullptr);
    if (const auto *op = llvm::dyn_cast_or_null<clang::BinaryOperator>(up);
        op != nullptr && op->isEqualityOp()) {
      const clang::Expr *other =
          op->getLHS()->IgnoreParenImpCasts() == read->IgnoreParenImpCasts()
              ? op->getRHS()
              : op->getLHS();
      clang::Expr::EvalResult result;
      return !other->isValueDependent() && other->EvaluateAsInt(result, ast) &&
             result.Val.getInt() == 0;
    }
    if (const auto *op = llvm::dyn_cast_or_null<clang::UnaryOperator>(up))
      return op->getOpcode() == clang::UO_LNot;
    // `while (*p)`: read as a condition.
    return llvm::isa_and_nonnull<clang::WhileStmt, clang::IfStmt,
                                 clang::ForStmt, clang::DoStmt>(up) ||
           (llvm::isa_and_nonnull<clang::BinaryOperator>(up) &&
            llvm::cast<clang::BinaryOperator>(up)->isLogicalOp());
  }

  /// The value given to member `count` of `object`'s object in `function`
  /// (its initializer, or an assignment).
  static const clang::Expr *givenTo(const clang::FunctionDecl *function,
                                    llvm::StringRef count,
                                    const clang::Expr *object) {
    // (Null: the function's own object.)
    const bool own = object == nullptr || llvm::isa<clang::CXXThisExpr>(
                                              object->IgnoreParenImpCasts());
    if (const auto *constructor =
            llvm::dyn_cast<clang::CXXConstructorDecl>(function))
      for (const clang::CXXCtorInitializer *each : constructor->inits())
        if (each->isWritten() && each->getMember() != nullptr &&
            each->getMember()->getName() == count && own)
          return each->getInit();
    const clang::Expr *found = nullptr;
    const auto visit = [&](const clang::Stmt *statement, const auto &self) {
      if (statement == nullptr || found != nullptr)
        return;
      if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(statement);
          op != nullptr && op->getOpcode() == clang::BO_Assign)
        if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
                op->getLHS()->IgnoreParenImpCasts());
            member != nullptr && member->getMemberDecl()->getName() == count &&
            (own ? llvm::isa<clang::CXXThisExpr>(
                       member->getBase()->IgnoreParenImpCasts())
                 : sameObject(member->getBase(), object)))
          found = op->getRHS();
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(function->getBody(), visit);
    return found;
  }

  /// E6: a use stored where a bound is declared (`place`), whose count is
  /// given beside it in the same function.
  void storedInto(const clang::ValueDecl *place, const clang::Expr *object,
                  const clang::Expr *use, const clang::Decl *target,
                  const clang::FunctionDecl *function, clang::SourceLocation at,
                  std::vector<Clue> &clues) {
    const std::optional<Fact> given = stated(place->getType());
    if (!given)
      return;
    const std::string line =
        " at line " + std::to_string(sources.getExpansionLineNumber(at));
    if (given->name.empty()) {
      clues.push_back(
          {.fact = *given,
           .why = "it is stored in '" + place->getName().str() + "'" + line,
           .weak = false});
      return;
    }
    if (const clang::Expr *count = object == nullptr
                                       ? nullptr
                                       : givenTo(function, given->name, object))
      if (auto fact = admissible(count, use, target, function, given->kind))
        clues.push_back({.fact = *fact,
                         .why = "'" + given->name + "' is set from '" +
                                fact->name + "' beside it" + line,
                         .weak = false});
  }

  /// E6: a member set from a value that states a bound: the member of the
  /// same object set from what counts that value.
  void setFrom(const clang::Expr *value, const clang::Expr *use,
               const clang::Decl *target, const clang::FunctionDecl *function,
               clang::SourceLocation at, std::vector<Clue> &clues) {
    const clang::Expr *e = value->IgnoreParenImpCasts();
    const std::string line =
        " at line " + std::to_string(sources.getExpansionLineNumber(at));
    if (llvm::isa<clang::StringLiteral>(e)) {
      clues.push_back(
          {.fact = {.kind = "null_terminated", .name = "", .parameter = -1},
           .why = "it is set from a string" + line,
           .weak = true});
      return;
    }
    const auto *used =
        llvm::dyn_cast<clang::MemberExpr>(use->IgnoreParenImpCasts());
    // `new T[n]` beside a member set from `n`.
    if (const auto *made = llvm::dyn_cast<clang::CXXNewExpr>(e);
        made != nullptr && made->isArray() && made->getArraySize() &&
        used != nullptr && llvm::isa<clang::FieldDecl>(target)) {
      const auto *size = llvm::dyn_cast<clang::DeclRefExpr>(
          (*made->getArraySize())->IgnoreParenImpCasts());
      for (const clang::FieldDecl *field :
           llvm::cast<clang::FieldDecl>(target)->getParent()->fields()) {
        const clang::Expr *count =
            size == nullptr
                ? nullptr
                : givenTo(function, field->getName(), used->getBase());
        const auto *counted = count == nullptr
                                  ? nullptr
                                  : llvm::dyn_cast<clang::DeclRefExpr>(
                                        count->IgnoreParenImpCasts());
        if (counted != nullptr && counted->getDecl() == size->getDecl())
          clues.push_back({.fact = {.kind = "counted_by",
                                    .name = field->getName().str(),
                                    .parameter = -1},
                           .why = "it is allocated with '" +
                                  size->getDecl()->getName().str() +
                                  "' elements and '" + field->getName().str() +
                                  "' set from it" + line,
                           .weak = false});
      }
      return;
    }
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(e);
    const auto *parameter =
        named == nullptr ? nullptr
                         : llvm::dyn_cast<clang::ParmVarDecl>(named->getDecl());
    if (parameter == nullptr || used == nullptr ||
        !llvm::isa<clang::FieldDecl>(target))
      return;
    const std::optional<Fact> given = stated(parameter->getType());
    if (!given)
      return;
    if (given->name.empty()) {
      clues.push_back(
          {.fact = *given,
           .why = "it is set from '" + parameter->getName().str() + "'" + line,
           .weak = false});
      return;
    }
    // The member set from the parameter that counts the value.
    const auto *record = llvm::cast<clang::FieldDecl>(target)->getParent();
    for (const clang::FieldDecl *field : record->fields()) {
      const clang::Expr *count =
          givenTo(function, field->getName(), used->getBase());
      const auto *counted = count == nullptr
                                ? nullptr
                                : llvm::dyn_cast<clang::DeclRefExpr>(
                                      count->IgnoreParenImpCasts());
      if (counted != nullptr && counted->getDecl()->getName() == given->name)
        clues.push_back({.fact = {.kind = given->kind,
                                  .name = field->getName().str(),
                                  .parameter = -1},
                         .why = "it is set from '" +
                                parameter->getName().str() + "' and '" +
                                field->getName().str() + "' from '" +
                                given->name + "'" + line,
                         .weak = false});
    }
  }

  /// What the uses of `target` say of its bound (§3.2), and whether one
  /// tests it against null.
  struct Evidence {
    std::vector<Clue> clues;
    bool null = false;
    /// E5: the highest constant index, and whether anything else indexes
    /// or steps it.
    std::int64_t highest = -1;
    bool other = false;
    /// A use writes through it, or hands it where it may be written: it is
    /// no string (safe code only reads one).
    bool written = false;
  };

  /// Whether `read` (a byte through the pointer) is written: assigned,
  /// stepped, or its address taken.
  bool writes(const clang::FunctionDecl *function, const clang::Expr *read) {
    const clang::Stmt *up = parents(function).getParentIgnoreParens(read);
    if (const auto *op = llvm::dyn_cast_or_null<clang::BinaryOperator>(up))
      return op->isAssignmentOp() && op->getLHS()->IgnoreParens() == read;
    if (const auto *op = llvm::dyn_cast_or_null<clang::UnaryOperator>(up))
      return op->isIncrementDecrementOp() ||
             op->getOpcode() == clang::UO_AddrOf;
    return false;
  }

  void use(const clang::Expr *expression, const clang::Decl *target,
           const clang::FunctionDecl *function, bool pure, Evidence &found) {
    const clang::Expr *top = expression;
    bool tested = false;
    const clang::Stmt *up = climb(function, top, &tested);
    found.null = found.null || tested;
    const clang::SourceLocation at = expression->getExprLoc();
    if (up == nullptr) {
      // A constructor's initializer: what is set beside it counts it.
      const auto initializer = initializers.find(top);
      if (initializer != initializers.end() &&
          initializer->second->getMember() != nullptr)
        if (auto given = stated(initializer->second->getMember()->getType());
            given && !given->name.empty())
          if (const auto *constructor =
                  llvm::dyn_cast<clang::CXXConstructorDecl>(function))
            for (const clang::CXXCtorInitializer *each : constructor->inits())
              if (each->isWritten() && each->getMember() != nullptr &&
                  each->getMember()->getName() == given->name)
                if (auto fact = admissible(each->getInit(), expression, target,
                                           function, given->kind))
                  found.clues.push_back({.fact = *fact,
                                         .why = "'" + given->name +
                                                "' is set from '" + fact->name +
                                                "' beside it",
                                         .weak = false});
      return;
    }
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(up)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      const bool object = llvm::isa<clang::CXXOperatorCallExpr>(call) &&
                          llvm::isa_and_nonnull<clang::CXXMethodDecl>(callee);
      llvm::SmallVector<const clang::Expr *, 6> arguments;
      for (unsigned i = object ? 1 : 0; i < call->getNumArgs(); ++i)
        arguments.push_back(call->getArg(i));
      for (unsigned i = 0; callee != nullptr && i < arguments.size(); ++i)
        if (arguments[i] == top)
          argument(*callee, arguments, i, expression, target, function, at,
                   found.clues);
      return;
    }
    if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(up)) {
      llvm::SmallVector<const clang::Expr *, 6> arguments(
          construct->arguments());
      for (unsigned i = 0; i < arguments.size(); ++i)
        if (arguments[i] == top && construct->getConstructor() != nullptr)
          argument(*construct->getConstructor(), arguments, i, expression,
                   target, function, at, found.clues);
      return;
    }
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(up);
        subscript != nullptr && subscript->getBase() == top) {
      clang::Expr::EvalResult result;
      const clang::Expr *index = subscript->getIdx();
      if (!index->isValueDependent() && index->EvaluateAsInt(result, ast)) {
        found.highest =
            std::max(found.highest, result.Val.getInt().getExtValue());
      } else {
        found.other = true;
        if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(
                index->IgnoreParenImpCasts());
            pure && named != nullptr)
          if (const auto *variable =
                  llvm::dyn_cast<clang::VarDecl>(named->getDecl()))
            guarded(subscript, variable, expression, target, function,
                    found.clues);
      }
      found.written = found.written || writes(function, subscript);
      if (pure && testsZero(function, subscript))
        found.clues.push_back(
            {.fact = {.kind = "null_terminated", .name = "", .parameter = -1},
             .why = "a byte of it is tested against zero at line " +
                    std::to_string(sources.getExpansionLineNumber(at)),
             .weak = false});
      return;
    }
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(up)) {
      if (op->getOpcode() == clang::UO_Deref) {
        found.highest = std::max<std::int64_t>(found.highest, 0);
        found.written = found.written || writes(function, op);
        if (testsZero(function, op))
          found.clues.push_back(
              {.fact = {.kind = "null_terminated", .name = "", .parameter = -1},
               .why = "its byte is tested against zero at line " +
                      std::to_string(sources.getExpansionLineNumber(at)),
               .weak = false});
      } else if (op->isIncrementDecrementOp()) {
        found.other = true;
      } else if (op->getOpcode() == clang::UO_LNot) {
        found.null = true;
      }
      return;
    }
    const auto *op = llvm::dyn_cast<clang::BinaryOperator>(up);
    if (op == nullptr) {
      if (llvm::isa<clang::ReturnStmt>(up) &&
          function->getReturnType()->isPointerType())
        if (auto given = stated(function->getReturnType());
            given && given->name.empty())
          found.clues.push_back({.fact = *given,
                                 .why = "it is returned as '" +
                                        function->getNameAsString() +
                                        "''s result",
                                 .weak = false});
      return;
    }
    const clang::Expr *other =
        op->getLHS() == top ? op->getRHS() : op->getLHS();
    if (op->isAdditiveOp() || op->isCompoundAssignmentOp()) {
      found.other = true;
      if (const auto *named =
              llvm::dyn_cast<clang::DeclRefExpr>(other->IgnoreParenImpCasts());
          pure && named != nullptr && op->getOpcode() == clang::BO_Add)
        if (const auto *variable =
                llvm::dyn_cast<clang::VarDecl>(named->getDecl()))
          guarded(op, variable, expression, target, function, found.clues);
      return;
    }
    if (op->isComparisonOp()) {
      if (other->isNullPointerConstant(
              ast, clang::Expr::NPC_ValueDependentIsNotNull)) {
        found.null = true;
        return;
      }
      // E3: `p < e`, or `p != e` as a loop's condition; `p == q` is an
      // identity, not an end.
      const bool before = op->getLHS() == top;
      // Only a loop's condition: `if (end < begin) return` checks the
      // arguments, and `while (end != begin) *--end` walks backward.
      const auto *named =
          llvm::dyn_cast<clang::DeclRefExpr>(top->IgnoreParenImpCasts());
      if (loops(function, op) &&
          ((op->getOpcode() == clang::BO_NE && named != nullptr &&
            forward(function, named->getDecl())) ||
           (before && op->getOpcode() == clang::BO_LT) ||
           (!before && op->getOpcode() == clang::BO_GT)))
        if (auto fact =
                admissible(other, expression, target, function, "ended_by"))
          found.clues.push_back(
              {.fact = *fact,
               .why = "it is compared with '" + fact->name + "' at line " +
                      std::to_string(sources.getExpansionLineNumber(at)),
               .weak = false});
      return;
    }
    if (op->getOpcode() != clang::BO_Assign)
      return;
    if (op->getRHS() == top) {
      const clang::Expr *left = op->getLHS()->IgnoreParenImpCasts();
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(left))
        storedInto(member->getMemberDecl(), member->getBase(), expression,
                   target, function, at, found.clues);
      return;
    }
    if (other->isNullPointerConstant(ast,
                                     clang::Expr::NPC_ValueDependentIsNotNull))
      found.null = true;
    else
      setFrom(other, expression, target, function, at, found.clues);
  }

  /// The locals of `function` that copy (`pure`) or step a use of `target`.
  static llvm::DenseMap<const clang::VarDecl *, bool>
  aliases(const clang::FunctionDecl *function, const clang::Decl *target) {
    LocalValues values;
    llvm::DenseSet<const clang::VarDecl *> stepped;
    localValues(function->getBody(), values, &stepped);
    llvm::DenseMap<const clang::VarDecl *, bool> found;
    for (bool changed = true; changed;) {
      changed = false;
      for (const auto &[variable, given] : values) {
        if (found.contains(variable) || given.empty())
          continue;
        bool all = true;
        bool pure = !stepped.contains(variable);
        for (const clang::Expr *value : given) {
          const clang::Expr *e = value->IgnoreParenImpCasts();
          while (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e)) {
            if (!op->isAdditiveOp())
              break;
            pure = false;
            e = (op->getLHS()->getType()->isPointerType() ? op->getLHS()
                                                          : op->getRHS())
                    ->IgnoreParenImpCasts();
          }
          const clang::Decl *named = nullptr;
          if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(e))
            named = reference->getDecl();
          else if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
            named = member->getMemberDecl();
          const auto *local = llvm::dyn_cast_or_null<clang::VarDecl>(named);
          if (named != nullptr && pattern(named) == target)
            continue;
          if (local != nullptr && found.contains(local)) {
            pure = pure && found[local];
            continue;
          }
          all = false;
        }
        if (all) {
          found[variable] = pure;
          changed = true;
        }
      }
    }
    return found;
  }

  Evidence gather(const clang::Decl *target) {
    index();
    Evidence found;
    const auto each = [&](const clang::FunctionDecl *function,
                          const clang::Expr *expression) {
      use(expression, target, function, true, found);
    };
    if (llvm::isa<clang::FunctionDecl>(target)) {
      // A result: what each definition returns (E6).
      for (const clang::FunctionDecl *definition : definitions)
        if (pattern(definition) == target)
          returns(definition, found);
      return found;
    }
    if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(target))
      allocated(field, found);
    llvm::DenseSet<const clang::FunctionDecl *> seen;
    for (const Use &each2 : uses.lookup(target)) {
      each(each2.function, each2.expression);
      if (!seen.insert(each2.function).second)
        continue;
      // Locals that copy or step it.
      for (const auto &[local, pure] : aliases(each2.function, target))
        for (const Use &aliased : uses.lookup(pattern(local)))
          if (aliased.function == each2.function)
            use(aliased.expression, target, each2.function, pure, found);
    }
    return found;
  }

  /// E6, exact: a member of the class set from `new T[n]` (directly, or
  /// through a local) or from an array of `N`, in a function that sets a
  /// member of the same object from `n` or `N`: that member counts it.
  void allocated(const clang::FieldDecl *target, Evidence &found) {
    const auto *record =
        llvm::dyn_cast<clang::CXXRecordDecl>(pattern(target)->getDeclContext());
    if (record == nullptr)
      return;
    for (const clang::FunctionDecl *function : definitions) {
      const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
      if (method == nullptr || method->getParent() == nullptr ||
          pattern(method->getParent()) != pattern(record))
        continue;
      // What the member is set from in this function.
      std::vector<const clang::Expr *> values;
      if (const clang::Expr *value =
              givenTo(function, target->getName(), nullptr))
        values.push_back(value);
      LocalValues locals;
      localValues(function->getBody(), locals);
      for (std::size_t i = 0; i < values.size() && i < 8; ++i)
        if (const clang::VarDecl *local = localPointer(values[i]))
          for (const clang::Expr *each : locals[local])
            values.push_back(each);
      for (const clang::Expr *value : values) {
        const clang::Expr *e = value->IgnoreParenImpCasts();
        const clang::Expr *size = nullptr;
        std::optional<llvm::APInt> length;
        if (const auto *made = llvm::dyn_cast<clang::CXXNewExpr>(e);
            made != nullptr && made->isArray() && made->getArraySize())
          size = (*made->getArraySize())->IgnoreParenImpCasts();
        else if (const auto *array = ast.getAsConstantArrayType(e->getType()))
          length = array->getSize();
        if (size == nullptr && !length)
          continue;
        for (const clang::FieldDecl *field : record->fields()) {
          if (field == target || !field->getType()->isIntegerType())
            continue;
          const clang::Expr *count =
              givenTo(function, field->getName(), nullptr);
          if (count == nullptr)
            continue;
          count = count->IgnoreParenImpCasts();
          clang::Expr::EvalResult result;
          const auto *sized = size == nullptr
                                  ? nullptr
                                  : llvm::dyn_cast<clang::DeclRefExpr>(size);
          const auto *counted = llvm::dyn_cast<clang::DeclRefExpr>(count);
          const bool same =
              (sized != nullptr && counted != nullptr &&
               sized->getDecl() == counted->getDecl()) ||
              (length && !count->isValueDependent() &&
               count->EvaluateAsInt(result, ast) &&
               result.Val.getInt().getExtValue() ==
                   static_cast<std::int64_t>(length->getZExtValue()));
          if (same)
            found.clues.push_back(
                {.fact = {.kind = "counted_by",
                          .name = field->getName().str(),
                          .parameter = -1},
                 .why = "it is allocated with as many elements as '" +
                        field->getName().str() + "' is set to in '" +
                        function->getNameAsString() + "'",
                 .weak = false,
                 .exact = true});
        }
      }
    }
  }

  /// The member or variable an expression names.
  static const clang::ValueDecl *named(const clang::Expr *value) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(value))
      return member->getMemberDecl();
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(value))
      return reference->getDecl();
    return nullptr;
  }

  /// E6: a result is what every return gives: a member or parameter with a
  /// bound, a string, null.
  void returns(const clang::FunctionDecl *definition, Evidence &found) {
    const auto visit = [&](const clang::Stmt *statement, const auto &self) {
      if (statement == nullptr || llvm::isa<clang::LambdaExpr>(statement))
        return;
      if (const auto *made = llvm::dyn_cast<clang::ReturnStmt>(statement);
          made != nullptr && made->getRetValue() != nullptr) {
        const clang::Expr *value = made->getRetValue()->IgnoreParenImpCasts();
        const std::string line =
            " at line " +
            std::to_string(sources.getExpansionLineNumber(made->getBeginLoc()));
        if (value->isNullPointerConstant(
                ast, clang::Expr::NPC_ValueDependentIsNotNull)) {
          found.null = true;
        } else if (llvm::isa<clang::StringLiteral>(value)) {
          found.clues.push_back(
              {.fact = {.kind = "null_terminated", .name = "", .parameter = -1},
               .why = "it returns a string" + line,
               .weak = false});
        } else if (const clang::ValueDecl *given = named(value)) {
          std::optional<Fact> fact = stated(given->getType());
          if (fact && !fact->name.empty()) {
            // The count, named where the result is declared.
            const auto *member = llvm::dyn_cast<clang::MemberExpr>(value);
            if (member != nullptr &&
                !llvm::isa<clang::CXXThisExpr>(
                    member->getBase()->IgnoreParenImpCasts()))
              fact.reset();
            else if (llvm::isa<clang::ParmVarDecl>(given))
              for (const clang::ParmVarDecl *each : definition->parameters())
                if (each->getName() == fact->name)
                  fact->parameter =
                      static_cast<int>(each->getFunctionScopeIndex());
          }
          if (fact)
            found.clues.push_back(
                {.fact = *fact,
                 .why = "it returns '" + given->getNameAsString() + "'" + line,
                 .weak = false});
          else
            found.other = true;
        } else {
          found.other = true;
        }
      }
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(definition->getBody(), visit);
  }

  /// §3.2: the facts the evidence agrees on: one is likely, several a
  /// choice.
  static std::vector<Clue> decide(const Evidence &found,
                                  const clang::Decl *target) {
    std::map<std::string, Clue> strong;
    std::map<std::string, Clue> weak;
    const bool exact =
        llvm::any_of(found.clues, [](const Clue &clue) { return clue.exact; });
    for (const Clue &clue : found.clues)
      if (clue.exact || !exact)
        (clue.weak ? weak : strong).emplace(clue.fact.str(), clue);
    std::map<std::string, Clue> &facts = strong.empty() ? weak : strong;
    std::vector<Clue> decided;
    for (auto &[name, clue] : facts) {
      if (found.written &&
          llvm::StringRef(clue.fact.kind).starts_with("null_terminated"))
        continue;
      if (found.null && clue.fact.kind == "null_terminated")
        clue.fact.kind = "null_terminated_or_null";
      if (llvm::none_of(decided, [&](const Clue &each) {
            return each.fact.str() == clue.fact.str();
          }))
        decided.push_back(clue);
    }
    // E5: constant indices only.
    if (decided.empty() && found.highest >= 0 && !found.other &&
        !global(target))
      decided.push_back({.fact = {.kind = "counted_by",
                                  .name = std::to_string(found.highest + 1),
                                  .parameter = -1},
                         .why = "only its elements 0 to " +
                                std::to_string(found.highest) + " are read",
                         .weak = false});
    return decided;
  }

  //===--------------------------------------------------------------------===//
  // Edits
  //===--------------------------------------------------------------------===//

  using Insertions = std::vector<std::pair<clang::SourceLocation, std::string>>;

  /// The place after a pointer's `*` (or a reference's `&`) in its type as
  /// written: where an annotation of the pointer goes.
  static std::optional<clang::SourceLocation> sigil(clang::TypeLoc type) {
    for (unsigned depth = 0; !type.isNull() && depth < 16; ++depth) {
      if (auto qualified = type.getAs<clang::QualifiedTypeLoc>())
        type = qualified.getUnqualifiedLoc();
      else if (auto attributed = type.getAs<clang::AttributedTypeLoc>())
        type = attributed.getModifiedLoc();
      else if (auto macro = type.getAs<clang::MacroQualifiedTypeLoc>())
        type = macro.getInnerLoc();
      else if (auto parens = type.getAs<clang::ParenTypeLoc>())
        type = parens.getInnerLoc();
      else if (auto array = type.getAs<clang::ArrayTypeLoc>())
        type = array.getElementLoc();
      else if (auto pointer = type.getAs<clang::PointerTypeLoc>())
        return pointer.getStarLoc();
      else if (auto reference = type.getAs<clang::LValueReferenceTypeLoc>())
        return reference.getAmpLoc();
      else if (auto moved = type.getAs<clang::RValueReferenceTypeLoc>())
        return moved.getAmpAmpLoc();
      else
        return std::nullopt;
    }
    return std::nullopt;
  }

  /// An insertion after the token at `at`: ` text` before a space or after
  /// a name (`leading`), `text ` otherwise.
  bool after(clang::SourceLocation at, const std::string &text, Insertions &out,
             bool leading = false) const {
    if (at.isInvalid() || !at.isFileID() || sources.isInSystemHeader(at))
      return false;
    const clang::SourceLocation end =
        clang::Lexer::getLocForEndOfToken(at, 0, sources, ast.getLangOpts());
    if (end.isInvalid())
      return false;
    bool invalid = false;
    const char *next = sources.getCharacterData(end, &invalid);
    const bool space = !invalid && (*next == ' ' || *next == '\t');
    out.emplace_back(end, space || leading ? " " + text : text + " ");
    return true;
  }

  /// `text` on a declaration's pointer, or on each declaration's of a
  /// parameter or a result of a function; `arguments`, a fact's name in each.
  bool annotate(const clang::Decl *target, const Fact &fact, Insertions &out) {
    const auto name = [&](const clang::FunctionDecl *function) {
      if (fact.parameter < 0)
        return fact.name;
      if (static_cast<unsigned>(fact.parameter) >= function->getNumParams())
        return std::string();
      return function->getParamDecl(static_cast<unsigned>(fact.parameter))
          ->getName()
          .str();
    };
    const auto put = [&](clang::TypeLoc type, const std::string &argument) {
      if (!fact.name.empty() && argument.empty())
        return false;
      const auto at = sigil(type);
      return at && after(*at, macroOf(fact, argument), out);
    };
    if (const auto *parameter = llvm::dyn_cast<clang::ParmVarDecl>(target)) {
      const auto *function =
          llvm::cast<clang::FunctionDecl>(parameter->getDeclContext());
      bool any = false;
      for (const clang::FunctionDecl *each : function->redecls()) {
        const clang::ParmVarDecl *own =
            each->getParamDecl(parameter->getFunctionScopeIndex());
        if (own->getTypeSourceInfo() == nullptr)
          continue;
        if (fact.kind == "lifetimebound") {
          // The attribute follows a parameter's name, and an array's
          // bound after it (`buf[N]`).
          clang::SourceLocation last = own->getLocation();
          if (own->getTypeSourceInfo() != nullptr) {
            const clang::SourceLocation typeEnd =
                own->getTypeSourceInfo()->getTypeLoc().getEndLoc();
            if (typeEnd.isFileID() &&
                sources.isBeforeInTranslationUnit(last, typeEnd))
              last = typeEnd;
          }
          if (own->getName().empty() ||
              !after(last, "WEAVEC_LIFETIMEBOUND", out, /*leading=*/true))
            return false;
          any = true;
          continue;
        }
        if (!put(own->getTypeSourceInfo()->getTypeLoc(), name(each)))
          return false;
        any = true;
      }
      return any;
    }
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(target)) {
      for (const clang::FunctionDecl *each : function->redecls()) {
        if (fact.kind == "escapes") {
          if (!afterQualifiers(*each, out))
            return false;
          continue;
        }
        if (!put(each->getFunctionTypeLoc().getReturnLoc(), name(each)))
          return false;
      }
      return true;
    }
    const auto *declarator = llvm::dyn_cast<clang::DeclaratorDecl>(target);
    if (declarator == nullptr || declarator->getTypeSourceInfo() == nullptr)
      return false;
    if (fact.kind == "const" && !declarator->getType()->isPointerType() &&
        !declarator->getType()->isArrayType()) {
      const clang::SourceLocation at = declarator->getTypeSpecStartLoc();
      if (!at.isFileID())
        return false;
      out.emplace_back(at, "const ");
      return true;
    }
    return put(declarator->getTypeSourceInfo()->getTypeLoc(), fact.name);
  }

  /// `WEAVEC_ESCAPES` for `this`: after the parameter list and its
  /// qualifiers (`const`, `&`, `noexcept`).
  bool afterQualifiers(const clang::FunctionDecl &function, Insertions &out) {
    const clang::FunctionTypeLoc type = function.getFunctionTypeLoc();
    if (type.isNull() || !type.getRParenLoc().isFileID())
      return false;
    clang::SourceLocation last = type.getRParenLoc();
    for (clang::SourceLocation at = last;;) {
      const std::optional<clang::Token> next =
          clang::Lexer::findNextToken(at, sources, ast.getLangOpts());
      if (!next || next->getLocation().isMacroID())
        break;
      // (The raw lexer gives keywords as identifiers.)
      const llvm::StringRef word = next->is(clang::tok::raw_identifier)
                                       ? next->getRawIdentifier()
                                       : llvm::StringRef();
      const bool qualifier =
          next->isOneOf(clang::tok::amp, clang::tok::ampamp) ||
          word == "const" || word == "volatile" || word == "noexcept";
      if (!qualifier)
        break;
      last = next->getLocation();
      at = last;
    }
    // An exception specification, a macro's included, comes before it.
    if (const clang::SourceRange except = type.getExceptionSpecRange();
        except.isValid()) {
      const clang::SourceLocation end =
          sources.getExpansionRange(except.getEnd()).getEnd();
      if (sources.isBeforeInTranslationUnit(last, end))
        last = end;
    }
    return after(last, "WEAVEC_ESCAPES", out, /*leading=*/true);
  }

  /// What a declaration reads as with the insertions at it: the note's
  /// text.
  std::string spelled(const clang::Decl *target, const Insertions &insertions) {
    if (insertions.empty())
      return "";
    const clang::SourceLocation begin =
        sources.getExpansionLoc(target->getBeginLoc());
    clang::SourceLocation end = sources.getExpansionLoc(target->getLocation());
    end = clang::Lexer::getLocForEndOfToken(end, 0, sources, ast.getLangOpts());
    const clang::SourceLocation inserted = insertions.front().first;
    // An unnamed parameter's location is what follows its type.
    if (const auto *named = llvm::dyn_cast<clang::NamedDecl>(target);
        named != nullptr && named->getDeclName().isEmpty())
      end = inserted;
    if (sources.getFileID(begin) != sources.getFileID(inserted))
      return "";
    if (sources.isBeforeInTranslationUnit(end, inserted))
      end = inserted;
    const unsigned from = sources.getFileOffset(begin);
    const unsigned to = sources.getFileOffset(end);
    const unsigned at = sources.getFileOffset(inserted);
    bool invalid = false;
    const llvm::StringRef buffer =
        sources.getBufferData(sources.getFileID(begin), &invalid);
    if (invalid || to < from || at < from || to > buffer.size() ||
        to - from > 160)
      return "";
    std::string text = buffer.substr(from, at - from).str() +
                       insertions.front().second +
                       buffer.substr(at, to - at).str();
    std::string collapsed;
    for (char c : llvm::StringRef(text).trim()) {
      const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
      if (space && (collapsed.empty() || collapsed.back() == ' '))
        continue;
      collapsed += space ? ' ' : c;
    }
    return collapsed;
  }

  /// Whether WeaveC's macros are defined where `at` is in this unit.
  bool defined(clang::SourceLocation at) const {
    if (preprocessor == nullptr)
      return true;
    const clang::IdentifierInfo *name =
        preprocessor->getIdentifierInfo("WEAVEC_ESCAPES");
    const clang::MacroDirective *directive =
        name == nullptr ? nullptr
                        : preprocessor->getLocalMacroDirectiveHistory(name);
    if (directive == nullptr)
      return false;
    while (directive->getPrevious() != nullptr)
      directive = directive->getPrevious();
    return sources.isBeforeInTranslationUnit(directive->getLocation(), at);
  }

  /// A fix of `target` stating `fact`, at `level`.
  std::optional<Planned> plan(const clang::Decl *target, const Fact &fact,
                              llvm::StringRef level, llvm::StringRef aspect,
                              const std::string &why) {
    target = pattern(target);
    if (target == nullptr || isLibrary(*target))
      return std::nullopt;
    Planned planned;
    if (!annotate(target, fact, planned.insertions) ||
        planned.insertions.empty())
      return std::nullopt;
    planned.at = planned.insertions.front().first;
    planned.what = spelled(target, planned.insertions);
    if (planned.what.empty())
      planned.what = macroOf(fact, fact.name) + " on '" + nameOf(target) + "'";
    planned.fix.level = level.str();
    planned.fix.declaration = key(target);
    planned.fix.name = nameOf(target);
    planned.fix.aspect = aspect.str();
    planned.fix.fact = fact.str();
    planned.fix.text = planned.what;
    planned.fix.evidence = why;
    for (const auto &[at, text] : planned.insertions)
      planned.fix.edits.push_back({.file = path(at),
                                   .offset = sources.getFileOffset(at),
                                   .text = text,
                                   .needsInclude = !defined(at)});
    return planned;
  }

  /// A choice (never written): what it says, on what.
  Planned choice(const clang::Decl *target, llvm::StringRef aspect,
                 const std::string &fact, const std::string &what) const {
    Planned planned;
    planned.at = pattern(target)->getLocation();
    planned.what = what;
    planned.fix.level = "choice";
    planned.fix.declaration = key(target);
    planned.fix.name = nameOf(target);
    planned.fix.aspect = aspect.str();
    planned.fix.fact = fact;
    planned.fix.text = what;
    return planned;
  }

  //===--------------------------------------------------------------------===//
  // Wants
  //===--------------------------------------------------------------------===//

  /// Whether a pointer already states a bound.
  bool bounded(const clang::Decl *target) const {
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(target))
      return stated(function->getReturnType()).has_value();
    const auto *value = llvm::dyn_cast<clang::ValueDecl>(target);
    return value == nullptr || stated(value->getType()).has_value();
  }

  /// A bound for `target`, from its evidence: once for each declaration,
  /// which many errors may want.
  llvm::DenseMap<const clang::Decl *, std::vector<Planned>> bounds;
  std::vector<Planned> bound(const clang::Decl *target) {
    if (target == nullptr)
      return {};
    const auto found = bounds.find(target);
    if (found != bounds.end())
      return found->second;
    std::vector<Planned> planned = boundOnce(target);
    bounds[target] = planned;
    return planned;
  }
  std::vector<Planned> boundOnce(const clang::Decl *target) {
    if (bounded(target))
      return {};
    const clang::QualType type =
        llvm::isa<clang::FunctionDecl>(target)
            ? llvm::cast<clang::FunctionDecl>(target)->getReturnType()
            : llvm::cast<clang::ValueDecl>(target)->getType();
    if (!type->isPointerType() || type->isFunctionPointerType())
      return {};
    std::vector<Clue> decided = decide(gather(target), target);
    // A string is a character type's.
    llvm::erase_if(decided, [&](const Clue &clue) {
      return llvm::StringRef(clue.fact.kind).starts_with("null_terminated") &&
             !type->getPointeeType()->isAnyCharacterType();
    });
    std::vector<Planned> planned;
    if (decided.size() == 1) {
      if (auto each = plan(target, decided.front().fact, "likely", "bound",
                           decided.front().why))
        planned.push_back(std::move(*each));
      return planned;
    }
    if (decided.size() > 1) {
      std::string what;
      std::string facts;
      for (const Clue &clue : decided) {
        what += (what.empty() ? "" : ", or ") +
                macroOf(clue.fact, clue.fact.name) + " (" + clue.why + ")";
        facts += (facts.empty() ? "" : "|") + clue.fact.str();
      }
      planned.push_back(choice(target, "bound", facts,
                               "'" + nameOf(target) + "' is " + what));
    }
    return planned;
  }

  /// D5: a member of a class of plain values (no destructor, copy, move,
  /// base or virtual function of its own; members that are scalars,
  /// pointers, references and views) holds a view.
  static bool plain(const clang::FieldDecl *member) {
    const auto *record =
        llvm::dyn_cast<clang::CXXRecordDecl>(pattern(member)->getDeclContext());
    if (record == nullptr || !record->hasDefinition())
      return false;
    record = record->getDefinition();
    if (record->isPolymorphic() || record->getNumBases() != 0 ||
        record->hasUserDeclaredDestructor() ||
        record->hasUserDeclaredCopyConstructor() ||
        record->hasUserDeclaredCopyAssignment() ||
        record->hasUserDeclaredMoveConstructor() ||
        record->hasUserDeclaredMoveAssignment())
      return false;
    return llvm::all_of(record->fields(), [](const clang::FieldDecl *field) {
      const clang::QualType type = field->getType();
      if (type->isScalarType() || type->isReferenceType())
        return true;
      const auto *view = type->getAsCXXRecordDecl();
      if (view == nullptr || !isLibrary(*view))
        return false;
      const std::string name = CxxKinds::qualifiedName(*view);
      return name == "std::basic_string_view" || name == "std::span";
    });
  }

  std::vector<Planned> wanted(const Want &want,
                              const clang::FunctionDecl *function,
                              std::vector<std::string> &subjects) {
    const auto subject = [&](const clang::Decl *declaration) {
      if (declaration != nullptr && !isLibrary(*pattern(declaration)))
        subjects.push_back(key(declaration));
    };
    for (const clang::Decl *each : want.subjects)
      subject(each);
    std::vector<Planned> planned;
    switch (want.kind) {
    case Want::Kind::Bound: {
      const clang::Decl *target = want.declaration != nullptr
                                      ? want.declaration
                                      : origin(want.expression, function);
      subject(target);
      if (target != nullptr && !isLibrary(*pattern(target)))
        return bound(pattern(target));
      return {};
    }
    case Want::Kind::End: {
      subject(want.declaration);
      if (want.declaration != nullptr && ends(want.expression))
        if (auto each =
                plan(want.declaration,
                     {.kind = "counted_by", .name = "0", .parameter = -1},
                     "certain", "bound", ""))
          planned.push_back(std::move(*each));
      return planned;
    }
    case Want::Kind::Escapes: {
      subject(want.declaration);
      subject(want.keeper);
      const auto *parameter =
          llvm::dyn_cast_or_null<clang::ParmVarDecl>(want.declaration);
      if (parameter != nullptr && want.keeper != nullptr &&
          parameter->getType()->isPointerType() && plain(want.keeper))
        if (auto each = plan(want.keeper,
                             {.kind = "borrowed", .name = "", .parameter = -1},
                             "likely", "borrowed",
                             "'" + nameOf(want.keeper) +
                                 "' is a member of a class of plain values"))
          planned.push_back(std::move(*each));
      if (auto each = plan(want.declaration,
                           {.kind = "escapes", .name = "", .parameter = -1},
                           "certain", "escapes", ""))
        planned.push_back(std::move(*each));
      return planned;
    }
    case Want::Kind::Const:
      subject(want.declaration);
      if (const auto *global =
              llvm::dyn_cast_or_null<clang::VarDecl>(want.declaration);
          global != nullptr && unchanged(*global))
        if (auto each =
                plan(global, {.kind = "const", .name = "", .parameter = -1},
                     "certain", "const", ""))
          planned.push_back(std::move(*each));
      return planned;
    case Want::Kind::Aliased:
      subject(want.declaration);
      if (want.declaration != nullptr)
        planned.push_back(
            choice(want.declaration, "aliased", "aliased",
                   "declare '" + nameOf(want.declaration) +
                       "' WEAVEC_ALIASED if its objects are changed through "
                       "pointers other objects keep"));
      return planned;
    case Want::Kind::Lifetime:
      subject(want.declaration);
      if (const auto *parameter =
              llvm::dyn_cast_or_null<clang::ParmVarDecl>(want.declaration))
        return lifetime(parameter);
      return {};
    case Want::Kind::Subject:
      subject(want.declaration != nullptr ? want.declaration
                                          : origin(want.expression, function));
      return {};
    case Want::Kind::Agree: {
      const auto *first =
          llvm::dyn_cast_or_null<clang::FunctionDecl>(want.declaration);
      const auto *other =
          want.subjects.empty()
              ? nullptr
              : llvm::dyn_cast_or_null<clang::FunctionDecl>(want.subjects[0]);
      if (first != nullptr && other != nullptr)
        if (auto each = agreed(*first, *other))
          planned.push_back(std::move(*each));
      return planned;
    }
    case Want::Kind::Remove:
      if (std::optional<Planned> each = removal(want.removal, want.replacement))
        planned.push_back(std::move(*each));
      return planned;
    }
    return {};
  }

  /// RFC 0051 §4.1: what only asserts, taken out: certain. Its text in the
  /// file, and the place, which names the fix (no declaration does).
  std::optional<Planned> removal(clang::CharSourceRange range,
                                 const std::string &replacement) const {
    range = sources.getExpansionRange(range);
    if (range.isInvalid() || range.getBegin().isMacroID())
      return std::nullopt;
    const clang::SourceLocation begin = range.getBegin();
    const clang::SourceLocation end =
        range.isTokenRange()
            ? clang::Lexer::getLocForEndOfToken(range.getEnd(), 0, sources,
                                                ast.getLangOpts())
            : range.getEnd();
    if (end.isInvalid() || sources.getFileID(begin) != sources.getFileID(end))
      return std::nullopt;
    const unsigned from = sources.getFileOffset(begin);
    const unsigned to = range.isTokenRange()
                            ? sources.getFileOffset(end)
                            : sources.getFileOffset(range.getEnd());
    if (to <= from)
      return std::nullopt;
    bool invalid = false;
    const llvm::StringRef buffer =
        sources.getBufferData(sources.getFileID(begin), &invalid);
    if (invalid || to > buffer.size())
      return std::nullopt;
    const std::string text = buffer.substr(from, to - from).str();
    Planned planned;
    planned.at = begin;
    planned.removals.push_back(clang::CharSourceRange::getCharRange(
        begin, sources.getComposedLoc(sources.getFileID(begin), to)));
    planned.what = replacement.empty()
                       ? "remove '" + text + "'"
                       : "write '" + replacement + "' for '" + text + "'";
    if (!replacement.empty())
      planned.insertions.emplace_back(begin, replacement);
    planned.fix.level = "certain";
    planned.fix.declaration =
        path(begin) + ":" +
        std::to_string(sources.getSpellingLineNumber(begin)) + ":" +
        std::to_string(sources.getSpellingColumnNumber(begin));
    planned.fix.name = text;
    planned.fix.aspect = "removal";
    planned.fix.fact = "removal";
    planned.fix.text = planned.what;
    planned.fix.edits.push_back({.file = path(begin),
                                 .offset = from,
                                 .text = replacement,
                                 .removed = text,
                                 .needsInclude = false});
    return planned;
  }

  /// C3: a value that is, by its form, the end of its own bound: `p + n`
  /// of a pointer counted by `n`, `&a[N]` or `a + N` of an array of `N`.
  bool ends(const clang::Expr *value) const {
    if (value == nullptr)
      return false;
    const clang::Expr *e = value->IgnoreParenImpCasts();
    const auto counted = [&](const clang::Expr *pointer,
                             const clang::Expr *count) {
      pointer = pointer->IgnoreParenImpCasts();
      count = count->IgnoreParenImpCasts();
      if (const auto *array = ast.getAsConstantArrayType(pointer->getType())) {
        clang::Expr::EvalResult result;
        return !count->isValueDependent() &&
               count->EvaluateAsInt(result, ast) &&
               result.Val.getInt().getExtValue() ==
                   static_cast<std::int64_t>(array->getSize().getZExtValue());
      }
      const clang::ValueDecl *declaration = nullptr;
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(pointer))
        declaration = member->getMemberDecl();
      else if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(pointer))
        declaration = named->getDecl();
      const std::optional<Fact> fact = declaration == nullptr
                                           ? std::nullopt
                                           : stated(declaration->getType());
      if (!fact || fact->kind != "counted_by" || fact->name.empty())
        return false;
      const clang::ValueDecl *counter = nullptr;
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(count))
        counter = member->getMemberDecl();
      else if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(count))
        counter = named->getDecl();
      return counter != nullptr && counter->getName() == fact->name;
    };
    if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(e);
        op != nullptr && op->getOpcode() == clang::BO_Add)
      return op->getLHS()->getType()->isPointerType() ||
                     op->getLHS()
                         ->IgnoreParenImpCasts()
                         ->getType()
                         ->isArrayType()
                 ? counted(op->getLHS(), op->getRHS())
                 : counted(op->getRHS(), op->getLHS());
    if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(e);
        op != nullptr && op->getOpcode() == clang::UO_AddrOf)
      if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(
              op->getSubExpr()->IgnoreParens()))
        return counted(subscript->getBase(), subscript->getIdx());
    return false;
  }

  /// C4: a global of the unit with internal linkage (or a static local),
  /// initialized by a constant, that no function assigns or takes the
  /// address of but to read.
  bool unchanged(const clang::VarDecl &global) {
    if (global.isExternallyVisible() && !global.isStaticLocal())
      return false;
    if (global.getType().isConstQualified() &&
        !global.getType()->isPointerType())
      return false;
    if (!global.hasInit() || !global.hasConstantInitialization())
      return false;
    index();
    // Each use is read: an rvalue, or an array element read as one.
    return llvm::all_of(uses.lookup(pattern(&global)), [&](const Use &each) {
      const clang::ParentMap &map = parents(each.function);
      const auto *cast = llvm::dyn_cast_or_null<clang::ImplicitCastExpr>(
          map.getParent(each.expression));
      if (cast != nullptr && cast->getCastKind() == clang::CK_LValueToRValue)
        return true;
      if (cast == nullptr ||
          cast->getCastKind() != clang::CK_ArrayToPointerDecay)
        return false;
      const auto *subscript = llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(
          map.getParent(cast));
      const auto *read = subscript == nullptr
                             ? nullptr
                             : llvm::dyn_cast_or_null<clang::ImplicitCastExpr>(
                                   map.getParent(subscript));
      return read != nullptr && read->getCastKind() == clang::CK_LValueToRValue;
    });
  }

  /// C2: each bound one declaration of a function states and the other
  /// does not, written on the other.
  std::optional<Planned> agreed(const clang::FunctionDecl &first,
                                const clang::FunctionDecl &other) {
    Insertions insertions;
    std::string facts;
    const auto copy = [&](const clang::FunctionDecl &from,
                          const clang::FunctionDecl &to) {
      if (isLibrary(to))
        return;
      const auto put = [&](clang::QualType has, clang::QualType lacks,
                           clang::TypeLoc written, const clang::Decl *on) {
        std::optional<Fact> fact = stated(has);
        if (!fact || stated(lacks))
          return;
        Fact renamed = *fact;
        for (const clang::ParmVarDecl *each : from.parameters())
          if (!fact->name.empty() && each->getName() == fact->name)
            renamed.parameter = static_cast<int>(each->getFunctionScopeIndex());
        if (renamed.parameter >= 0) {
          const auto index = static_cast<unsigned>(renamed.parameter);
          if (index >= to.getNumParams() ||
              to.getParamDecl(index)->getName().empty())
            return;
          renamed.name = to.getParamDecl(index)->getName().str();
        }
        if (const auto at = sigil(written);
            at && after(*at, macroOf(renamed, renamed.name), insertions))
          facts += (facts.empty() ? "" : ", ") +
                   macroOf(renamed, renamed.name) + " on '" + nameOf(on) + "'";
      };
      for (unsigned i = 0; i < from.getNumParams() && i < to.getNumParams();
           ++i)
        if (to.getParamDecl(i)->getTypeSourceInfo() != nullptr)
          put(from.getParamDecl(i)->getType(), to.getParamDecl(i)->getType(),
              to.getParamDecl(i)->getTypeSourceInfo()->getTypeLoc(),
              to.getParamDecl(i));
      put(from.getReturnType(), to.getReturnType(),
          to.getFunctionTypeLoc().getReturnLoc(), &to);
      // RFC 0048 §4: WEAVEC_ESCAPES on a parameter, or after the list.
      for (unsigned i = 0; i < from.getNumParams() && i < to.getNumParams();
           ++i)
        if (hasAnnotation(annotationsOf(from.getParamDecl(i)->getType(), ast),
                          EscapesAnnotation) &&
            !hasAnnotation(annotationsOf(to.getParamDecl(i)->getType(), ast),
                           EscapesAnnotation) &&
            to.getParamDecl(i)->getTypeSourceInfo() != nullptr)
          if (const auto at =
                  sigil(to.getParamDecl(i)->getTypeSourceInfo()->getTypeLoc());
              at && after(*at, "WEAVEC_ESCAPES", insertions))
            facts += (facts.empty() ? "" : ", ") +
                     std::string("WEAVEC_ESCAPES on '") +
                     nameOf(to.getParamDecl(i)) + "'";
    };
    copy(first, other);
    copy(other, first);
    if (insertions.empty())
      return std::nullopt;
    Planned planned;
    planned.at = insertions.front().first;
    planned.what = "write " + facts;
    planned.insertions = insertions;
    planned.fix.level = "certain";
    planned.fix.declaration = key(&first);
    planned.fix.aspect = "copy";
    planned.fix.fact = facts;
    planned.fix.text = planned.what;
    for (const auto &[at, text] : insertions)
      planned.fix.edits.push_back({.file = path(at),
                                   .offset = sources.getFileOffset(at),
                                   .text = text,
                                   .needsInclude = !defined(at)});
    return planned;
  }

  /// E7: a function that returns one parameter (a position in it, or
  /// null) on every return; `parameter` is that one, if it is.
  std::vector<Planned> lifetime(const clang::ParmVarDecl *parameter) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(parameter->getDeclContext());
    const clang::FunctionDecl *definition = nullptr;
    if (function == nullptr || !function->isDefined(definition) ||
        parameter->hasAttr<clang::LifetimeBoundAttr>() || isLibrary(*function))
      return {};
    const clang::ParmVarDecl *own =
        definition->getParamDecl(parameter->getFunctionScopeIndex());
    bool every = true;
    bool any = false;
    const auto visit = [&](const clang::Stmt *statement, const auto &self) {
      if (statement == nullptr || llvm::isa<clang::LambdaExpr>(statement))
        return;
      if (const auto *made = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
        const clang::Expr *value = made->getRetValue();
        if (value == nullptr ||
            value->IgnoreParenImpCasts()->isNullPointerConstant(
                ast, clang::Expr::NPC_ValueDependentIsNotNull))
          return;
        const clang::Decl *from = origin(value, definition);
        any = true;
        every = every && from == own;
      }
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(definition->getBody(), visit);
    if (!every || !any)
      return {};
    std::vector<Planned> planned;
    if (auto each =
            plan(own, {.kind = "lifetimebound", .name = "", .parameter = -1},
                 "likely", "lifetimebound",
                 "every return of '" + function->getNameAsString() +
                     "' gives '" + own->getName().str() + "'"))
      planned.push_back(std::move(*each));
    return planned;
  }

  /// E7 for an error of the C++ flow, whose message names a local first:
  /// the calls that gave it a value whose callee returns one parameter.
  std::vector<Planned> fromMessage(const RuleError &error,
                                   const clang::FunctionDecl *function,
                                   std::vector<std::string> &subjects) {
    if (function == nullptr || function->getBody() == nullptr ||
        (error.rule != "call-alias" && error.rule != "stale-borrow"))
      return {};
    const std::size_t open = error.message.find('\'');
    const std::size_t close = error.message.find('\'', open + 1);
    if (open == std::string::npos || close == std::string::npos)
      return {};
    const std::string name = error.message.substr(open + 1, close - open - 1);
    LocalValues values;
    localValues(function->getBody(), values);
    std::vector<Planned> planned;
    for (const auto &[variable, given] : values) {
      if (variable->getName() != name)
        continue;
      for (const clang::Expr *value : given) {
        const auto *call =
            llvm::dyn_cast<clang::CallExpr>(value->IgnoreParenImpCasts());
        const clang::FunctionDecl *callee =
            call == nullptr ? nullptr : call->getDirectCallee();
        if (callee == nullptr || isLibrary(*callee))
          continue;
        for (const clang::ParmVarDecl *parameter : callee->parameters())
          for (Planned &each : lifetime(parameter)) {
            if (llvm::any_of(planned, [&](const Planned &done) {
                  return done.fix.declaration == each.fix.declaration;
                }))
              continue;
            subjects.push_back(each.fix.declaration);
            planned.push_back(std::move(each));
          }
      }
    }
    return planned;
  }
};

FixPlanner::FixPlanner(clang::ASTContext &ast,
                       const clang::Preprocessor *preprocessor)
    : impl(std::make_unique<Impl>(ast, preprocessor)) {}
FixPlanner::~FixPlanner() = default;

void FixPlanner::want(clang::SourceLocation at, Want wanted) {
  impl->pending = Impl::Pending{.at = at, .want = std::move(wanted)};
}

std::optional<FixPlanner::Note>
FixPlanner::attach(clang::SourceLocation at, RuleError &error,
                   const clang::FunctionDecl *function) {
  std::optional<Impl::Pending> taken = std::move(impl->pending);
  impl->pending.reset();
  if (at.isValid()) {
    const clang::SourceLocation place = impl->sources.getExpansionLoc(at);
    error.file = impl->path(place);
    error.line = impl->sources.getSpellingLineNumber(place);
    error.column = impl->sources.getSpellingColumnNumber(place);
  }
  std::vector<Planned> planned;
  if (taken &&
      impl->sources.getFileLoc(taken->at) == impl->sources.getFileLoc(at))
    planned = impl->wanted(taken->want, function, error.subjects);
  else
    planned = impl->fromMessage(error, function, error.subjects);
  if (planned.empty())
    return std::nullopt;
  Note note;
  note.at = planned.front().at;
  for (std::size_t i = 0; i < planned.size(); ++i) {
    const Planned &each = planned[i];
    error.fixes.push_back(each.fix);
    std::string text;
    if (each.fix.level == "choice") {
      text = "choice: " + each.what;
    } else if (each.fix.aspect == "borrowed") {
      text = "fix (likely): declare the member '" + each.what + "', a view's";
    } else if (i != 0 && planned.front().fix.aspect == "borrowed") {
      // D5: the parameter, if the class's objects are kept after all.
      text = "(certain) the parameter '" + each.what + "' if '" +
             llvm::StringRef(planned.front().fix.name).split("::").first.str() +
             "' objects are kept";
    } else {
      text = "fix (" + each.fix.level + "): " +
             (each.fix.aspect == "copy" || each.fix.aspect == "removal"
                  ? each.what
                  : "declare it '" + each.what + "'");
      std::set<std::string> elsewhere;
      for (const auto &[place, inserted] : each.insertions)
        if (place != each.at && each.fix.aspect != "copy") {
          const clang::PresumedLoc there = impl->sources.getPresumedLoc(place);
          if (there.isValid())
            elsewhere.insert(std::string(there.getFilename()) + ":" +
                             std::to_string(there.getLine()) + ":" +
                             std::to_string(there.getColumn()));
        }
      for (const std::string &place : elsewhere)
        text += ", and at " + place;
      if (!each.fix.evidence.empty())
        text += "; " + each.fix.evidence;
    }
    if (note.text.empty()) {
      note.text = text;
      if (each.fix.level != "choice") {
        note.insertions = each.insertions;
        note.removals = each.removals;
      }
    } else {
      note.text += "; or " + text;
    }
  }
  return note;
}

std::optional<FixPlanner::Note>
FixPlanner::agree(const clang::FunctionDecl &first,
                  const clang::FunctionDecl &other, RuleError &error) {
  std::optional<Planned> planned = impl->agreed(first, other);
  if (!planned)
    return std::nullopt;
  error.subjects.push_back(planned->fix.declaration);
  error.fixes.push_back(planned->fix);
  return Note{.at = planned->at,
              .text = "fix (certain): " + planned->what,
              .insertions = std::move(planned->insertions)};
}

void FixPlanner::evidence(const std::vector<std::string> &wants,
                          std::vector<RuleFix> &out) {
  if (wants.empty())
    return;
  const std::set<std::string> wanted(wants.begin(), wants.end());
  impl->index();
  std::set<const clang::Decl *> seen;
  const auto consider = [&](const clang::Decl *declaration) {
    declaration = Impl::pattern(declaration);
    if (declaration == nullptr || !seen.insert(declaration).second ||
        !wanted.contains(impl->key(declaration)))
      return;
    for (Planned &each : impl->bound(declaration))
      out.push_back(std::move(each.fix));
  };
  for (const auto &[declaration, uses] : impl->uses)
    consider(declaration);
  for (const clang::FunctionDecl *definition : impl->definitions)
    consider(definition);
}

void FixPlanner::files(std::vector<ReadFile> &out) const {
  const clang::SourceManager &sources = impl->sources;
  std::set<std::string> seen;
  for (unsigned i = 0; i < sources.local_sloc_entry_size(); ++i) {
    const clang::SrcMgr::SLocEntry &entry = sources.getLocalSLocEntry(i);
    if (!entry.isFile() ||
        entry.getFile().getFileCharacteristic() != clang::SrcMgr::C_User)
      continue;
    const clang::SrcMgr::ContentCache &content =
        entry.getFile().getContentCache();
    if (!content.OrigEntry)
      continue;
    const clang::FileID file = sources.translateFile(*content.OrigEntry);
    if (file.isInvalid())
      continue;
    const std::string path = impl->path(sources.getLocForStartOfFile(file));
    if (path.empty() || !seen.insert(path).second)
      continue;
    bool invalid = false;
    const llvm::StringRef data = sources.getBufferData(file, &invalid);
    if (invalid)
      continue;
    out.push_back({.path = path,
                   .size = data.size(),
                   .hash = llvm::utohexstr(llvm::xxh3_64bits(data))});
  }
}

} // namespace weavec::frontend
