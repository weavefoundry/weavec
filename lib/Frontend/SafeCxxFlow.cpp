//===- SafeCxxFlow.cpp - Safe C++'s moves, borrows and views --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0044 §5 to §9. One forward dataflow over Clang's CFG of a safe C++
// function (every expression an element, with the ends of locals' lifetimes
// and of temporaries), to a fixed point. The state is, for each local that
// is a view (a pointer, a reference, an iterator, a class that holds one),
// the owners it points at or into and what ended it, if anything did; and
// the objects that were moved. A final pass over the converged states
// reports `owner-moved`, `stale-borrow`, `call-alias`, `owner-path` and
// `escape`.
//
// A borrow's loan names a root (a local, a parameter, `this`, a temporary;
// with the member it went through, when it went through one) and says
// whether the borrow points at the root itself or into what it holds. A
// root that is destroyed or moved ends every loan of it; a root that is
// changed ends the loans into it.
//
//===----------------------------------------------------------------------===//

#include "SafeCxxKinds.h"

#include "clang/AST/ASTLambda.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMap.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/Builtins.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"

#include <map>
#include <set>

namespace weavec::frontend {

namespace {

namespace rule {
constexpr llvm::StringLiteral OwnerMoved = "owner-moved";
constexpr llvm::StringLiteral StaleBorrow = "stale-borrow";
constexpr llvm::StringLiteral CallAlias = "call-alias";
constexpr llvm::StringLiteral OwnerPath = "owner-path";
constexpr llvm::StringLiteral Escape = "escape";
} // namespace rule

enum class RootKind : std::uint8_t { Var, This, Temp };

/// What a borrow points at or into.
struct Root {
  RootKind kind = RootKind::Var;
  /// The variable, or the temporary's expression; null for `this`.
  const void *base = nullptr;
  /// The member the borrow went through, when it names one.
  const clang::FieldDecl *field = nullptr;
  /// Not the object but what a view kept in it points at, which the
  /// function was handed and nothing it does ends (a `std::string_view`
  /// parameter's string, where a view member of `*this` points).
  bool beyond = false;

  bool operator==(const Root &) const = default;
  [[nodiscard]] bool overlaps(const Root &other) const {
    return kind == other.kind && base == other.base && beyond == other.beyond &&
           (field == nullptr || other.field == nullptr || field == other.field);
  }
  [[nodiscard]] const clang::VarDecl *variable() const {
    return kind == RootKind::Var ? static_cast<const clang::VarDecl *>(base)
                                 : nullptr;
  }
};

struct Loan {
  Root root;
  /// 0: at the root itself (ended only when it is destroyed or moved).
  /// 1: into what the root holds (ended when the root changes). 2 and
  /// more: into what an object at the level above holds (ended when that
  /// object changes too): an element's own buffer, below a container's.
  unsigned depth = 0;
  static constexpr unsigned Deepest = 6;
  bool operator==(const Loan &) const = default;
};
using Loans = llvm::SmallVector<Loan, 2>;

/// What ended a borrow.
struct End {
  enum class Why : std::uint8_t { None, Changed, Called, Destroyed, Moved };
  Why why = Why::None;
  clang::SourceLocation at;
  Root root;
  /// The function that changed the root.
  const clang::NamedDecl *by = nullptr;
  bool operator==(const End &) const = default;
};

struct Borrow {
  Loans loans;
  /// Loaded from a pointer member or element: a reference (§8).
  bool reference = false;
  End end;
};

/// A value: where a view points, or which object a glvalue names.
struct Value {
  Loans loans;
  bool reference = false;
  /// The borrow locals it was read through.
  llvm::SmallVector<const clang::VarDecl *, 2> through;

  void add(const Loan &loan) {
    if (!llvm::is_contained(loans, loan))
      loans.push_back(loan);
  }
  void add(const Value &other) {
    for (const Loan &loan : other.loans)
      add(loan);
    reference |= other.reference;
    for (const clang::VarDecl *variable : other.through)
      if (!llvm::is_contained(through, variable))
        through.push_back(variable);
  }
  /// Into what these objects hold.
  [[nodiscard]] Value inside() const {
    Value value = *this;
    for (Loan &loan : value.loans)
      loan.depth = std::min(loan.depth + 1, Loan::Deepest);
    Value unique;
    unique.add(value);
    return unique;
  }
};

using Place = std::pair<const clang::VarDecl *, const clang::FieldDecl *>;

struct Flow {
  bool reached = false;
  llvm::DenseMap<const clang::VarDecl *, Borrow> borrows;
  /// Moved objects: a local or parameter (null: `this`) and a member of it
  /// (null: all of it), and where.
  std::map<Place, clang::SourceLocation> moved;
};

} // namespace

static bool join(Flow &into, const Flow &from) {
  if (!from.reached)
    return false;
  if (!into.reached) {
    into = from;
    return true;
  }
  bool changed = false;
  for (const auto &[variable, borrow] : from.borrows) {
    const auto [found, inserted] = into.borrows.try_emplace(variable, borrow);
    if (inserted) {
      changed = true;
      continue;
    }
    Borrow &mine = found->second;
    for (const Loan &loan : borrow.loans)
      if (!llvm::is_contained(mine.loans, loan)) {
        mine.loans.push_back(loan);
        changed = true;
      }
    if (borrow.reference && !mine.reference) {
      mine.reference = true;
      changed = true;
    }
    if (borrow.end.why != End::Why::None && mine.end.why == End::Why::None) {
      mine.end = borrow.end;
      changed = true;
    }
  }
  for (const auto &entry : from.moved)
    changed |= into.moved.insert(entry).second;
  return changed;
}

static const clang::Expr *strip(const clang::Expr *expression) {
  while (expression != nullptr) {
    const clang::Expr *inner = expression->IgnoreParens();
    if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(inner))
      inner = cleanups->getSubExpr();
    else if (const auto *bind =
                 llvm::dyn_cast<clang::CXXBindTemporaryExpr>(inner))
      inner = bind->getSubExpr();
    else if (const auto *opaque =
                 llvm::dyn_cast<clang::OpaqueValueExpr>(inner);
             opaque != nullptr && opaque->getSourceExpr() != nullptr)
      inner = opaque->getSourceExpr();
    if (inner == expression)
      break;
    expression = inner;
  }
  return expression;
}

/// `std::move`, `std::forward`, `std::addressof` and their kin: the result
/// is the argument.
static bool passesThrough(const clang::CallExpr &call) {
  switch (call.getBuiltinCallee()) {
  case clang::Builtin::BImove:
  case clang::Builtin::BImove_if_noexcept:
  case clang::Builtin::BIforward:
  case clang::Builtin::BIforward_like:
  case clang::Builtin::BIas_const:
  case clang::Builtin::BIaddressof:
  case clang::Builtin::BI__addressof:
  case clang::Builtin::BI__builtin_addressof:
    return call.getNumArgs() == 1;
  default:
    return false;
  }
}

namespace {

/// A call, a member call, an operator call or a construction, with each
/// argument beside its parameter.
struct Call {
  const clang::FunctionDecl *callee = nullptr;
  const clang::CXXMethodDecl *method = nullptr;
  /// The object of a member call (a pointer for `p->f()`).
  const clang::Expr *object = nullptr;
  struct Argument {
    const clang::Expr *expression;
    const clang::ParmVarDecl *parameter;
  };
  llvm::SmallVector<Argument, 4> arguments;
  clang::SourceLocation at;
};

class CxxFlow {
public:
  CxxFlow(clang::ASTContext &ast, CxxKinds &kinds,
          const clang::FunctionDecl &function, const clang::Stmt &body,
          const RuleReport &report)
      : ast(ast), kinds(kinds), function(function), body(body), report(report),
        // ParentMap and the CFG take a mutable body; neither changes it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        parents(const_cast<clang::Stmt *>(&body)) {}

  /// Whether the dataflow settled.
  bool run() {
    collectAssigned(&body);
    clang::CFG::BuildOptions options;
    options.setAllAlwaysAdd();
    options.AddLifetime = true;
    options.AddImplicitDtors = true;
    options.AddTemporaryDtors = true;
    options.AddInitializers = true;
    // §10: a handler is reached from each call of its try block that may
    // throw, with what was moved and ended by then.
    options.AddEHEdges = true;
    const std::unique_ptr<clang::CFG> cfg = clang::CFG::buildCFG(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        &function, const_cast<clang::Stmt *>(&body), &ast, options);
    graph = cfg.get();
    return graph != nullptr && solve();
  }

private:
  clang::ASTContext &ast;
  CxxKinds &kinds;
  const clang::FunctionDecl &function;
  const clang::Stmt &body;
  const RuleReport &report;
  clang::ParentMap parents;
  const clang::CFG *graph = nullptr;
  bool emit = false;
  /// Names that are assigned as a whole where they stand: not uses.
  llvm::DenseSet<const clang::Expr *> assigned;
  /// One report for each borrow and what ended it.
  std::set<std::pair<const clang::VarDecl *, unsigned>> reported;

  //===--------------------------------------------------------------------===//
  // Names and reports
  //===--------------------------------------------------------------------===//

  void error(clang::SourceLocation at, llvm::StringRef id,
             const std::string &message, clang::SourceLocation noteAt,
             const std::string &note) {
    if (emit)
      report(at, id, message, noteAt, note);
  }

  [[nodiscard]] std::string nameOf(const Root &root) const {
    std::string name;
    if (root.kind == RootKind::Temp) {
      const auto *temporary = static_cast<const clang::Expr *>(root.base);
      return "a temporary '" +
             temporary->getType().getUnqualifiedType().getAsString(
                 ast.getPrintingPolicy()) +
             "'";
    }
    if (const clang::VarDecl *variable = root.variable())
      name = loopRange(*variable).value_or(variable->getNameAsString());
    if (root.field != nullptr)
      name += (name.empty() ? "" : ".") + root.field->getNameAsString();
    if (name.empty())
      name = "this object";
    return "'" + name + "'";
  }

  /// The range a range-for's hidden variable iterates, as written.
  [[nodiscard]] std::optional<std::string>
  loopRange(const clang::VarDecl &variable) const {
    if (!variable.isImplicit())
      return std::nullopt;
    for (const clang::Stmt *up = variable.getInit(); up != nullptr;
         up = parents.getParent(up))
      if (const auto *loop = llvm::dyn_cast<clang::CXXForRangeStmt>(up)) {
        if (const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
                loop->getRangeInit()->IgnoreParenImpCasts()))
          return named->getDecl()->getNameAsString();
        return "the range";
      }
    return std::nullopt;
  }

  [[nodiscard]] static std::string called(const clang::NamedDecl *by) {
    return by == nullptr ? "a call" : "'" + by->getNameAsString() + "'";
  }

  void reportStale(const clang::VarDecl &variable, clang::SourceLocation at,
                   const End &end) {
    if (!emit || !reported.emplace(&variable, end.at.getRawEncoding()).second)
      return;
    const std::string owner = nameOf(end.root);
    const std::optional<std::string> loop = loopRange(variable);
    const std::string name = "'" + variable.getNameAsString() + "'";
    std::string message;
    std::string note;
    switch (end.why) {
    case End::Why::Changed:
      message = loop ? "the loop over '" + *loop + "' continues after " +
                           called(end.by) + " may have changed " + owner
                     : name + " points into " + owner + ", which " +
                           called(end.by) + " may have changed";
      note = loop ? "changed here; loop by index, or collect what to add or "
                    "remove and apply it after the loop"
                  : "changed here; take " + name +
                        " again after this, or keep an index instead of a "
                        "reference";
      break;
    case End::Why::Called:
      message = loop ? "the loop over '" + *loop + "' continues after the "
                       "call to " + called(end.by) + ", which may change "
                       "what " + owner + " owns"
                     : name + " was taken from " + owner +
                           " before the call to " + called(end.by) +
                           ", which may change what " + owner + " owns";
      note = "take the borrow again after the call, or declare the "
             "parameter 'const' if " + called(end.by) + " changes nothing";
      break;
    case End::Why::Destroyed:
      message = name + " points into " + owner +
                (end.root.kind == RootKind::Temp
                     ? ", which was destroyed at the end of its statement"
                     : ", whose lifetime has ended");
      note = end.root.kind == RootKind::Temp
                 ? "the temporary is destroyed here; keep it in a local "
                   "that lives as long as " + name
                 : owner + " is destroyed here; declare it in the scope "
                           "that uses " + name;
      break;
    case End::Why::Moved:
      message = name + " points into " + owner + ", which was moved";
      note = "moved here; take " + name + " from the object it was moved to";
      break;
    case End::Why::None:
      return;
    }
    report(at, rule::StaleBorrow, message, end.at, note);
  }

  //===--------------------------------------------------------------------===//
  // Values: which object a glvalue names, where a view points
  //===--------------------------------------------------------------------===//

  [[nodiscard]] bool ownLocal(const clang::VarDecl &variable) const {
    const clang::DeclContext *context = variable.getDeclContext();
    while (context != nullptr && !llvm::isa<clang::FunctionDecl>(context))
      context = context->getParent();
    return context == &function && variable.hasLocalStorage();
  }

  /// A local the function owns: one whose end ends what points at it. A
  /// parameter taken by value is one; a pointer, a reference or a view
  /// passed in points at the caller's objects.
  [[nodiscard]] bool localRoot(const Root &root) {
    if (root.beyond)
      return false;
    if (root.kind == RootKind::Temp)
      return true;
    const clang::VarDecl *variable = root.variable();
    if (variable == nullptr || !ownLocal(*variable))
      return false;
    // §8: what a shared pointer points to has other owners the function
    // does not see.
    if (CxxKinds::shared(variable->getType()->getAsCXXRecordDecl()))
      return false;
    return !llvm::isa<clang::ParmVarDecl>(variable) ||
           !kinds.tracked(variable->getType());
  }

  [[nodiscard]] static Value at(const clang::VarDecl &variable) {
    Value value;
    value.loans.push_back(
        {.root = {.kind = RootKind::Var, .base = variable.getCanonicalDecl()},
         .depth = 0});
    return value;
  }

  /// What a borrow local holds. One the flow does not know (a parameter,
  /// a capture of an enclosing function) stands for what it was given.
  [[nodiscard]] Value borrowOf(const Flow &flow,
                               const clang::VarDecl &variable) const {
    Value value;
    const clang::VarDecl *canonical = variable.getCanonicalDecl();
    const auto found = flow.borrows.find(canonical);
    if (found != flow.borrows.end()) {
      value.loans = found->second.loans;
      value.reference = found->second.reference;
    } else {
      value.loans.push_back(
          {.root = {.kind = RootKind::Var, .base = canonical}, .depth = 0});
      value.loans.push_back(
          {.root = {.kind = RootKind::Var, .base = canonical, .beyond = true},
           .depth = 1});
    }
    value.through.push_back(canonical);
    return value;
  }

  [[nodiscard]] static const clang::VarDecl *
  variableOf(const clang::DeclRefExpr &named) {
    if (const auto *binding =
            llvm::dyn_cast<clang::BindingDecl>(named.getDecl()))
      return llvm::dyn_cast_or_null<clang::VarDecl>(
          binding->getDecomposedDecl());
    return llvm::dyn_cast<clang::VarDecl>(named.getDecl());
  }

  /// The object(s) a glvalue names.
  [[nodiscard]] Value object(const Flow &flow, const clang::Expr *expression) {
    expression = strip(expression);
    if (expression == nullptr)
      return {};
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
      const clang::VarDecl *variable = variableOf(*named);
      if (variable == nullptr)
        return {};
      if (variable->getType()->isReferenceType())
        return borrowOf(flow, *variable);
      if (!variable->hasLocalStorage()) {
        // §10: a global that is not const is read in place as a reference.
        Value value;
        value.reference = !variable->getType().isConstQualified();
        return value;
      }
      return at(*variable);
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
      const auto *field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (field == nullptr)
        return {};
      Value base = member->isArrow() ? pointee(flow, member->getBase())
                                     : object(flow, member->getBase());
      if (field->getType()->isReferenceType())
        return base;
      for (Loan &loan : base.loans)
        if (loan.depth == 0 && loan.root.field == nullptr)
          loan.root.field = field;
      return base;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
      if (unary->getOpcode() == clang::UO_Deref)
        return pointee(flow, unary->getSubExpr());
      if (unary->isIncrementDecrementOp())
        return object(flow, unary->getSubExpr());
      return {};
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(expression))
      return pointee(flow, subscript->getBase());
    if (const auto *temporary =
            llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
      if (const auto *extended = llvm::dyn_cast_or_null<clang::VarDecl>(
              temporary->getExtendingDecl()))
        return at(*extended);
      Value value;
      value.loans.push_back(
          {.root = {.kind = RootKind::Temp, .base = temporary}, .depth = 0});
      return value;
    }
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression))
      return object(flow, cast->getSubExpr());
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(expression)) {
      Value value = object(flow, conditional->getTrueExpr());
      value.add(object(flow, conditional->getFalseExpr()));
      return value;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
      if (binary->isAssignmentOp())
        return object(flow, binary->getLHS());
      if (binary->getOpcode() == clang::BO_Comma)
        return object(flow, binary->getRHS());
      return {};
    }
    if (llvm::isa<clang::CallExpr, clang::CXXConstructExpr>(expression))
      return result(flow, expression);
    return {};
  }

  /// What a pointer points at.
  [[nodiscard]] Value pointee(const Flow &flow, const clang::Expr *expression) {
    expression = strip(expression);
    if (expression == nullptr)
      return {};
    if (!expression->getType()->isPointerType() &&
        !expression->getType()->isArrayType())
      // `p->f` on a class is a call of its `operator->`.
      return expression->isGLValue() ? object(flow, expression)
                                     : points(flow, expression);
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression)) {
      const clang::Expr *inner = strip(cast->getSubExpr());
      switch (cast->getCastKind()) {
      case clang::CK_LValueToRValue:
        break;
      case clang::CK_ArrayToPointerDecay:
        return object(flow, inner);
      case clang::CK_NullToPointer:
        return {};
      default:
        return pointee(flow, inner);
      }
      // A pointer read from a place: a local's is what the flow follows;
      // a borrowed member's or a capture's is its object's; any other was
      // loaded from memory, and is a reference.
      if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(inner)) {
        const clang::VarDecl *variable = variableOf(*named);
        if (variable != nullptr && variable->hasLocalStorage() &&
            !variable->getType()->isReferenceType())
          return borrowOf(flow, *variable);
      }
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(inner))
        if (const auto *field =
                llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
            field != nullptr &&
            (field->getParent()->isLambda() ||
             hasAnnotation(annotationsOf(field->getType(), ast),
                           BorrowedAnnotation)))
          return viewOf(flow, member->isArrow()
                                  ? pointee(flow, member->getBase())
                                  : object(flow, member->getBase()));
      Value value;
      value.reference = true;
      return value;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
      if (unary->getOpcode() == clang::UO_AddrOf)
        return object(flow, unary->getSubExpr());
      return pointee(flow, unary->getSubExpr());
    }
    if (llvm::isa<clang::CXXThisExpr>(expression)) {
      Value value;
      value.loans.push_back({.root = {.kind = RootKind::This}, .depth = 0});
      return value;
    }
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(expression)) {
      Value value = pointee(flow, conditional->getTrueExpr());
      value.add(pointee(flow, conditional->getFalseExpr()));
      return value;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
      if (binary->getOpcode() == clang::BO_Comma)
        return pointee(flow, binary->getRHS());
      return pointee(flow, binary->getLHS()->getType()->isPointerType()
                               ? binary->getLHS()
                               : binary->getRHS());
    }
    if (llvm::isa<clang::CallExpr>(expression))
      return result(flow, expression);
    return {};
  }

  /// Where the views kept in the named objects point: a view local's own
  /// borrows; for anything else, into the object.
  [[nodiscard]] Value viewOf(const Flow &flow, const Value &objects) const {
    Value value;
    value.reference = objects.reference;
    value.through = objects.through;
    for (const Loan &loan : objects.loans) {
      const clang::VarDecl *variable = loan.root.variable();
      const auto found = variable == nullptr ? flow.borrows.end()
                                             : flow.borrows.find(variable);
      if (found != flow.borrows.end() &&
          !variable->getType()->isReferenceType() &&
          !variable->getType()->isPointerType()) {
        for (const Loan &inner : found->second.loans)
          value.add(inner);
        value.reference |= found->second.reference;
      } else {
        Root held = loan.root;
        held.beyond = true;
        value.add({.root = held, .depth = 1});
      }
    }
    return value;
  }

  /// Where a view (a pointer, or an object of a view or erased class)
  /// points.
  [[nodiscard]] Value points(const Flow &flow, const clang::Expr *expression) {
    expression = strip(expression);
    if (expression == nullptr)
      return {};
    const clang::QualType type = expression->getType();
    if (type->isPointerType() || type->isArrayType())
      return pointee(flow, expression);
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression))
      return points(flow, cast->getSubExpr());
    if (const auto *temporary =
            llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
      return points(flow, temporary->getSubExpr());
    if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(expression))
      return captured(flow, *lambda);
    if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(expression))
      return listed(flow, list->inits(), type);
    if (const auto *list =
            llvm::dyn_cast<clang::CXXParenListInitExpr>(expression))
      return listed(flow, list->getInitExprs(), type);
    if (const auto *list =
            llvm::dyn_cast<clang::CXXStdInitializerListExpr>(expression))
      return points(flow, list->getSubExpr());
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(expression)) {
      Value value = points(flow, conditional->getTrueExpr());
      value.add(points(flow, conditional->getFalseExpr()));
      return value;
    }
    if (llvm::isa<clang::CXXConstructExpr>(expression) ||
        (llvm::isa<clang::CallExpr>(expression) && expression->isPRValue()))
      return result(flow, expression);
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(expression))
      if (const clang::VarDecl *variable = variableOf(*named);
          variable != nullptr && !variable->getType()->isReferenceType() &&
          variable->hasLocalStorage())
        return borrowOf(flow, *variable);
    if (expression->isGLValue())
      return viewOf(flow, object(flow, expression));
    return {};
  }

  [[nodiscard]] Value listed(const Flow &flow,
                             llvm::ArrayRef<clang::Expr *> inits,
                             clang::QualType type) {
    Value value;
    const clang::RecordDecl *record = type->getAsRecordDecl();
    auto field = record != nullptr && !llvm::isa<clang::CXXRecordDecl>(record)
                     ? record->field_end()
                     : (record != nullptr ? record->field_begin()
                                          : clang::RecordDecl::field_iterator());
    const bool fields = record != nullptr && type->isAggregateType() &&
                        llvm::cast<clang::CXXRecordDecl>(record)->getNumBases() == 0;
    for (const clang::Expr *init : inits) {
      const bool bindsReference = fields && field != record->field_end() &&
                                  field->getType()->isReferenceType();
      if (fields && field != record->field_end())
        ++field;
      if (bindsReference)
        value.add(object(flow, init));
      else if (kinds.tracked(init->getType()) || init->getType()->isArrayType())
        value.add(points(flow, init));
    }
    return value;
  }

  [[nodiscard]] Value captured(const Flow &flow,
                               const clang::LambdaExpr &lambda) {
    Value value;
    auto init = lambda.capture_init_begin();
    for (const clang::LambdaCapture &capture : lambda.captures()) {
      const clang::Expr *expression = *init++;
      if (capture.capturesThis()) {
        if (capture.getCaptureKind() == clang::LCK_This)
          value.add({.root = {.kind = RootKind::This}, .depth = 0});
        continue;
      }
      if (!capture.capturesVariable() || expression == nullptr)
        continue;
      if (capture.getCaptureKind() == clang::LCK_ByRef)
        value.add(object(flow, expression));
      else if (kinds.tracked(capture.getCapturedVar()->getType()))
        value.add(points(flow, expression));
    }
    return value;
  }

  /// A borrow taken through `expression`, the object of a member call: one
  /// through a view that owns nothing points where the view points; any
  /// other points into the object.
  [[nodiscard]] Value through(const Flow &flow, const clang::Expr *expression) {
    const clang::QualType type = expression->getType();
    if (type->isPointerType())
      return pointee(flow, expression).inside();
    if (kinds.pureView(type))
      return points(flow, expression);
    return object(flow, expression).inside();
  }

  [[nodiscard]] static Call describe(const clang::Expr &expression) {
    Call call;
    call.at = expression.getExprLoc();
    llvm::ArrayRef<const clang::Expr *> arguments;
    if (const auto *construct =
            llvm::dyn_cast<clang::CXXConstructExpr>(&expression)) {
      call.callee = construct->getConstructor();
      arguments = {construct->getArgs(), construct->getNumArgs()};
    } else {
      const auto &plain = llvm::cast<clang::CallExpr>(expression);
      call.callee = plain.getDirectCallee();
      arguments = {plain.getArgs(), plain.getNumArgs()};
      const auto *method =
          llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call.callee);
      if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(&plain)) {
        call.method = member->getMethodDecl();
        call.object = member->getImplicitObjectArgument();
      } else if (llvm::isa<clang::CXXOperatorCallExpr>(plain) &&
                 method != nullptr && method->isImplicitObjectMemberFunction() &&
                 !arguments.empty()) {
        call.method = method;
        call.object = arguments.front();
        arguments = arguments.drop_front();
      }
    }
    for (unsigned i = 0; i < arguments.size(); ++i)
      call.arguments.push_back(
          {.expression = arguments[i],
           .parameter = call.callee != nullptr && i < call.callee->getNumParams()
                            ? call.callee->getParamDecl(i)
                            : nullptr});
    return call;
  }

  /// §9: where the result of a call points, when it is a view.
  [[nodiscard]] Value result(const Flow &flow, const clang::Expr *expression) {
    if (const auto *plain = llvm::dyn_cast<clang::CallExpr>(expression);
        plain != nullptr && passesThrough(*plain)) {
      const clang::Expr *argument = plain->getArg(0);
      return plain->getType()->isPointerType() || argument->isGLValue()
                 ? object(flow, argument)
                 : points(flow, argument);
    }
    const Call call = describe(*expression);
    const bool constructs = llvm::isa<clang::CXXConstructExpr>(expression);
    const clang::QualType type =
        constructs || call.callee == nullptr
            ? expression->getType()
            : call.callee->getReturnType().getNonReferenceType();
    const bool names = !constructs && expression->isGLValue();
    if (!names && !kinds.tracked(expression->getType()))
      return {};
    Value value;
    const bool marked =
        llvm::any_of(call.arguments, [](const Call::Argument &argument) {
          return argument.parameter != nullptr &&
                 argument.parameter->hasAttr<clang::LifetimeBoundAttr>();
        });
    if (call.object != nullptr && !marked)
      value.add(through(flow, call.object));
    bool views = false;
    for (const Call::Argument &argument : call.arguments) {
      const clang::QualType given = argument.expression->getType();
      const bool byReference = argument.parameter != nullptr &&
                               argument.parameter->getType()->isReferenceType();
      if (marked) {
        if (argument.parameter != nullptr &&
            argument.parameter->hasAttr<clang::LifetimeBoundAttr>())
          value.add(contribution(flow, argument, type, names, constructs));
      } else if (!byReference &&
                 (given->isPointerType() || kinds.tracked(given)) &&
                 !given->isFunctionPointerType()) {
        // A pointer can be what a reference or pointer result names only
        // if it points to the result's type.
        if (given->isPointerType() && (names || type->isPointerType()) &&
            !constructs && !related(given->getPointeeType(), type))
          continue;
        value.add(points(flow, argument.expression));
        views = true;
      }
    }
    // §9: without a view passed by value, the result of a function that is
    // not a member points into its reference arguments. What is
    // constructed may keep every one.
    if (!marked && (constructs || (!views && call.object == nullptr)))
      for (const Call::Argument &argument : call.arguments)
        if (argument.parameter != nullptr &&
            argument.parameter->getType()->isReferenceType())
          value.add(contribution(flow, argument, type, names, constructs));
    return value;
  }

  /// Whether an object of type `given` can be what a result of type `type`
  /// (or a pointer to it) names: the same type, or one derived from it.
  [[nodiscard]] bool related(clang::QualType given, clang::QualType type) const {
    type = type.getNonReferenceType();
    if (type->isPointerType())
      type = type->getPointeeType();
    given = given.getNonReferenceType();
    if (ast.hasSameUnqualifiedType(given, type))
      return true;
    const clang::CXXRecordDecl *derived = given->getAsCXXRecordDecl();
    const clang::CXXRecordDecl *base = type->getAsCXXRecordDecl();
    return derived != nullptr && base != nullptr && derived->hasDefinition() &&
           base->hasDefinition() && derived->isDerivedFrom(base);
  }

  /// What a reference (or marked) argument gives a result of `type`.
  [[nodiscard]] Value contribution(const Flow &flow,
                                   const Call::Argument &argument,
                                   clang::QualType type, bool names,
                                   bool constructs) {
    const clang::Expr *expression = argument.expression;
    const clang::QualType given = expression->getType();
    if (given->isPointerType())
      return pointee(flow, expression);
    if (!expression->isGLValue())
      return kinds.tracked(given) ? points(flow, expression) : Value{};
    Value value;
    if (kinds.tracked(given))
      value.add(points(flow, expression));
    const bool same = related(given, type);
    if ((names || type->isPointerType()) && !constructs) {
      // A reference or pointer result can be the argument itself only if
      // it has its type.
      if (same)
        value.add(object(flow, expression));
    } else if (!kinds.pureView(given) && !(constructs && same)) {
      // A copy or a move of a view points where the view does, not into
      // the object it was copied from.
      value.add(object(flow, expression).inside());
    }
    return value;
  }

  //===--------------------------------------------------------------------===//
  // Effects
  //===--------------------------------------------------------------------===//

  enum class Ends : std::uint8_t { Into, All };

  void invalidate(Flow &flow, const Value &objects, Ends ends, End::Why why,
                  clang::SourceLocation at, const clang::NamedDecl *by) {
    for (auto &[variable, borrow] : flow.borrows) {
      if (borrow.end.why != End::Why::None ||
          llvm::is_contained(objects.through, variable))
        continue;
      for (const Loan &loan : borrow.loans) {
        // A change of an object ends what points into it, not what points
        // at it or beside it.
        const auto *hit = llvm::find_if(objects.loans, [&](const Loan &object) {
          return loan.root.overlaps(object.root) &&
                 (ends == Ends::All || loan.depth > object.depth);
        });
        if (hit == objects.loans.end())
          continue;
        borrow.end = {.why = why, .at = at, .root = hit->root, .by = by};
        break;
      }
    }
  }

  /// §5: the place `std::move(x)` (or a cast to an rvalue) moves from.
  [[nodiscard]] std::optional<Place> movedFrom(const clang::Expr *expression) {
    expression = strip(expression);
    while (const auto *temporary =
               llvm::dyn_cast_or_null<clang::MaterializeTemporaryExpr>(
                   expression))
      expression = strip(temporary->getSubExpr());
    const clang::Expr *operand = nullptr;
    if (const auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(expression);
        call != nullptr && call->isCallToStdMove())
      operand = call->getArg(0);
    else if (const auto *cast =
                 llvm::dyn_cast_or_null<clang::CXXStaticCastExpr>(expression);
             cast != nullptr && cast->isXValue())
      operand = cast->getSubExpr();
    if (operand == nullptr || !kinds.holdsOwners(operand->getType()))
      return std::nullopt;
    operand = operand->IgnoreParenImpCasts();
    const clang::FieldDecl *field = nullptr;
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(operand)) {
      field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (field == nullptr)
        return std::nullopt;
      operand = member->getBase()->IgnoreParenImpCasts();
      if (llvm::isa<clang::CXXThisExpr>(operand))
        return Place{nullptr, field};
    }
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(operand);
    const clang::VarDecl *variable =
        named == nullptr ? nullptr : variableOf(*named);
    if (variable == nullptr || !variable->hasLocalStorage())
      return std::nullopt;
    return Place{variable->getCanonicalDecl(), field};
  }

  [[nodiscard]] static Value rootOf(const Place &place) {
    Value value;
    value.loans.push_back(
        {.root = {.kind = place.first == nullptr ? RootKind::This
                                                  : RootKind::Var,
                  .base = place.first,
                  .field = place.second},
         .depth = 0});
    return value;
  }

  /// Whether a parameter takes what `std::move` offers.
  [[nodiscard]] static bool takes(const clang::ParmVarDecl *parameter) {
    if (parameter == nullptr)
      return false;
    const clang::QualType type = parameter->getType();
    return type->isRValueReferenceType() ||
           (!type->isReferenceType() && type->isRecordType());
  }

  /// Whether a call can change what an argument for `parameter` owns (§6,
  /// rule 3).
  [[nodiscard]] bool changes(const clang::ParmVarDecl *parameter) const {
    if (parameter == nullptr)
      return false;
    const clang::QualType type = parameter->getType();
    if (!type->isLValueReferenceType() && !type->isPointerType())
      return false;
    const clang::QualType target = type->getPointeeType();
    return !target.isConstQualified() && kinds.holdsOwners(target);
  }

  [[nodiscard]] bool changing(const Call &call) {
    return call.method != nullptr && !call.method->isConst() &&
           !call.method->isStatic() &&
           !llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(
               call.method) &&
           !kinds.accessor(*call.method);
  }

  [[nodiscard]] clang::QualType objectType(const Call &call) const {
    const clang::QualType type = call.object->getType();
    return type->isPointerType() ? type->getPointeeType() : type;
  }

  [[nodiscard]] Value objectOf(const Flow &flow, const Call &call) {
    return call.object->getType()->isPointerType()
               ? pointee(flow, call.object)
               : object(flow, call.object);
  }

  /// A local view or erased object named directly.
  [[nodiscard]] const clang::VarDecl *
  viewLocal(const clang::Expr *expression) {
    const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
        expression == nullptr ? nullptr : expression->IgnoreParenImpCasts());
    const clang::VarDecl *variable =
        named == nullptr ? nullptr : variableOf(*named);
    if (variable == nullptr || !variable->hasLocalStorage() ||
        kinds.pointerLike(variable->getType()) ||
        !kinds.tracked(variable->getType()))
      return nullptr;
    return variable->getCanonicalDecl();
  }

  /// What a view takes from an argument it may store (§7, widened).
  [[nodiscard]] Value stored(const Flow &flow, const Call::Argument &argument) {
    const clang::Expr *expression = argument.expression;
    const clang::QualType given = expression->getType();
    if (given->isPointerType())
      return given->isFunctionPointerType() ? Value{}
                                            : pointee(flow, expression);
    // An owner passed by reference is lent for the call: a callee that is
    // not a constructor does not keep a view of it (checked in the callee).
    return kinds.tracked(given) ? points(flow, expression) : Value{};
  }

  void owners(const Value &objects, clang::SourceLocation at,
              const std::string &what, const clang::NamedDecl *by,
              const clang::Expr *expression) {
    if (objects.reference && globalBase(expression)) {
      error(at, rule::OwnerPath,
            what + " is a global, which every function can reach, and " +
                called(by) + " may change what it owns",
            at,
            "move the global into a local, change the local and move it "
            "back ('auto local = std::move(global); ...; global = "
            "std::move(local);'), pass the object as a parameter instead, "
            "or keep this function WEAVEC_GUARDED");
      return;
    }
    if (objects.reference) {
      error(at, rule::OwnerPath,
            what + " is reached through a pointer kept in memory, and " +
                called(by) + " may change what it owns",
            at,
            "change an object from what owns it or is passed it: take it as "
            "a parameter of this function, hold it by 'std::shared_ptr', "
            "make the member a reference, or keep this function "
            "WEAVEC_GUARDED");
      return;
    }
    if (!readOnlyThis)
      return;
    if (llvm::any_of(objects.loans, [](const Loan &loan) {
          return loan.root.kind == RootKind::This;
        }))
      error(at, rule::OwnerPath,
            what + " is changed in a member function that is 'const' or "
                   "has a 'const' overload, which only gives access",
            at,
            "change it in a member function that is not 'const' and has no "
            "'const' twin, or in a WEAVEC_GUARDED helper");
  }
  /// The function is a `const` member function or an accessor (§6): it
  /// changes nothing `*this` owns.
  bool readOnlyThis = false;

  [[nodiscard]] static std::string spell(const clang::Expr *expression,
                                         const clang::ASTContext &ast) {
    std::string text;
    llvm::raw_string_ostream out(text);
    expression->IgnoreParenImpCasts()->printPretty(out, nullptr,
                                                   ast.getPrintingPolicy());
    if (text.size() > 40)
      text = text.substr(0, 37) + "...";
    return "'" + text + "'";
  }

  /// §7: a view stored where it outlives the function's locals, or in the
  /// object it points into.
  void storedOutside(const Value &target, const Value &source,
                     clang::SourceLocation at, const std::string &where,
                     bool viewTarget) {
    const bool toThis = llvm::any_of(target.loans, [](const Loan &loan) {
      return loan.root.kind == RootKind::This;
    });
    for (const Loan &loan : source.loans) {
      if (localRoot(loan.root)) {
        error(at, rule::Escape,
              "a borrow of " + nameOf(loan.root) + " is stored in " + where +
                  ", which outlives it",
              at,
              "store a copy that owns its data, or take " +
                  nameOf(loan.root) +
                  " as a parameter so that the caller keeps it");
        return;
      }
      const auto *parameter =
          llvm::dyn_cast_or_null<clang::ParmVarDecl>(loan.root.variable());
      // §7, received: a callable handed in a type that hides what it
      // captured may hold borrows of the caller's locals.
      if (parameter != nullptr && loan.root.beyond &&
          kinds.erased(parameter->getType()) && ownLocal(*parameter) &&
          !hasAnnotation(annotationsOf(parameter->getType(), ast),
                         EscapesAnnotation)) {
        error(at, rule::Escape,
              where + " would keep " + nameOf(loan.root) +
                  ", which may hold borrows of its caller's locals",
              parameter->getLocation(),
              "declare the parameter WEAVEC_ESCAPES ('" +
                  parameter->getType().getNonReferenceType().getUnqualifiedType().getAsString(
                      ast.getPrintingPolicy()) +
                  " WEAVEC_ESCAPES " + parameter->getNameAsString() +
                  "'), so that callers pass a callable that captures by "
                  "value");
        return;
      }
      if (viewTarget && toThis && parameter != nullptr && !loan.root.beyond &&
          parameter->getType()->isReferenceType() &&
          !kinds.tracked(parameter->getType()->getPointeeType()) &&
          !llvm::isa<clang::CXXConstructorDecl>(&function)) {
        error(at, rule::Escape,
              where + " would keep a borrow of the parameter " +
                  nameOf(loan.root) + ", which is lent for the call only",
              at,
              "take a view of what is kept (a 'std::string_view', a "
              "'std::span', a pointer), so that callers see it; or bind it "
              "in the constructor");
        return;
      }
      if (viewTarget && toThis && loan.root.kind == RootKind::This &&
          !loan.root.beyond && loan.root.field != nullptr) {
        error(at, rule::Escape,
              where + " would point into " + nameOf(loan.root) +
                  ", a part of the same object",
              at,
              "an object that points into itself breaks when it is moved: "
              "keep an index or an offset and a length instead of a view, "
              "or capture by value");
        return;
      }
    }
  }

  /// Whether a place is a global or a part of one.
  [[nodiscard]] static bool globalBase(const clang::Expr *expression) {
    while (expression != nullptr) {
      expression = expression->IgnoreParenImpCasts();
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression);
          member != nullptr && !member->isArrow())
        expression = member->getBase();
      else
        break;
    }
    return expression != nullptr && global(expression);
  }

  [[nodiscard]] static bool global(const clang::Expr *expression) {
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(
        expression->IgnoreParenImpCasts());
    const auto *variable = named == nullptr
                               ? nullptr
                               : llvm::dyn_cast<clang::VarDecl>(named->getDecl());
    return variable != nullptr && !variable->hasLocalStorage();
  }

  [[nodiscard]] bool outside(const Value &objects) {
    return objects.reference ||
           llvm::any_of(objects.loans, [this](const Loan &loan) {
             return !localRoot(loan.root);
           });
  }

  /// The captures by reference a lambda's body may change.
  void lambdaChanges(const clang::Stmt *statement,
                     llvm::SmallPtrSetImpl<const clang::VarDecl *> &changed) {
    if (statement == nullptr)
      return;
    if (const auto *expression = llvm::dyn_cast<clang::Expr>(statement);
        expression != nullptr &&
        llvm::isa<clang::CallExpr, clang::CXXConstructExpr>(expression)) {
      const Call call = describe(*expression);
      const auto note = [&](const clang::Expr *argument) {
        const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(
            argument->IgnoreParenImpCasts());
        if (named != nullptr && named->refersToEnclosingVariableOrCapture())
          if (const clang::VarDecl *variable = variableOf(*named))
            changed.insert(variable->getCanonicalDecl());
      };
      if (call.object != nullptr && changing(call) &&
          kinds.holdsOwners(objectType(call)))
        note(call.object);
      for (const Call::Argument &argument : call.arguments)
        if (changes(argument.parameter))
          note(argument.expression);
    }
    for (const clang::Stmt *child : statement->children())
      lambdaChanges(child, changed);
  }

  [[nodiscard]] const clang::LambdaExpr *
  lambdaOf(const clang::Expr *expression) const {
    expression = strip(expression);
    while (expression != nullptr) {
      if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(expression))
        return lambda;
      if (const auto *temporary =
              llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
        expression = strip(temporary->getSubExpr());
      else if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression))
        expression = strip(cast->getSubExpr());
      else if (const auto *construct =
                   llvm::dyn_cast<clang::CXXConstructExpr>(expression);
               construct != nullptr && construct->getNumArgs() == 1)
        expression = strip(construct->getArg(0));
      else if (const auto *named =
                   llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        const clang::VarDecl *variable = variableOf(*named);
        expression = variable != nullptr && variable->hasLocalStorage() &&
                             variable->hasInit()
                         ? strip(variable->getInit())
                         : nullptr;
      } else {
        return nullptr;
      }
    }
    return nullptr;
  }

  /// RFC 0043 §5's rule, and the callable a library function is given.
  void alias(const Flow &flow, const Call &call, const Value *object,
             bool objectChanges) {
    struct Passed {
      Value value;
      bool changer;
      const clang::Expr *expression;
    };
    llvm::SmallVector<Passed, 4> passed;
    const bool library = call.callee != nullptr && isLibrary(*call.callee);
    if (object != nullptr)
      passed.push_back({.value = *object,
                        .changer = objectChanges,
                        .expression = call.object});
    for (const Call::Argument &argument : call.arguments) {
      const clang::Expr *expression = argument.expression;
      const clang::QualType given = expression->getType();
      Value value;
      if (argument.parameter != nullptr &&
          argument.parameter->getType()->isReferenceType() &&
          expression->isGLValue())
        value = kinds.pureView(given) ? points(flow, expression)
                                      : object_(flow, expression);
      else if (given->isPointerType() || kinds.tracked(given))
        value = points(flow, expression);
      passed.push_back({.value = value,
                        .changer = changes(argument.parameter),
                        .expression = expression});
    }
    if (library) {
      // §9: a library function is trusted with plain borrows; a callable
      // that changes what it captured, beside a borrow into it, is not.
      for (const Passed &callable : passed) {
        const clang::LambdaExpr *lambda = lambdaOf(callable.expression);
        if (lambda == nullptr)
          continue;
        llvm::SmallPtrSet<const clang::VarDecl *, 4> changed;
        lambdaChanges(lambda->getBody(), changed);
        for (const Passed &other : passed)
          for (const Loan &loan : other.value.loans)
            if (&other != &callable && loan.depth > 0 &&
                loan.root.variable() != nullptr &&
                changed.contains(loan.root.variable())) {
              error(call.at, rule::CallAlias,
                    spell(other.expression, ast) + " points into " +
                        nameOf(loan.root) +
                        ", which the callable passed with it may change",
                    callable.expression->getExprLoc(),
                    "collect the changes in a local and apply them after "
                    "the call");
              return;
            }
      }
      return;
    }
    for (const Passed &changer : passed) {
      if (!changer.changer)
        continue;
      for (const Passed &other : passed) {
        if (&other == &changer)
          continue;
        for (const Loan &mine : changer.value.loans)
          for (const Loan &theirs : other.value.loans)
            if (mine.root.overlaps(theirs.root) &&
                (theirs.depth > mine.depth ||
                 (theirs.depth == 0 && mine.depth == 0))) {
              error(call.at, rule::CallAlias,
                    spell(other.expression, ast) + " points into " +
                        nameOf(mine.root) + ", which " + called(call.callee) +
                        " may change through " + spell(changer.expression, ast),
                    call.at,
                    "copy what the call needs into a local first, pass only "
                    "the object the callee changes, or make the other "
                    "parameter 'const'");
              return;
            }
      }
    }
  }
  [[nodiscard]] Value object_(const Flow &flow, const clang::Expr *expression) {
    return object(flow, expression);
  }

  void call(Flow &flow, const clang::Expr *expression) {
    if (const auto *plain = llvm::dyn_cast<clang::CallExpr>(expression);
        plain != nullptr && passesThrough(*plain))
      return;
    const Call call = describe(*expression);
    const bool constructs = llvm::isa<clang::CXXConstructExpr>(expression);
    const bool assigns = call.method != nullptr &&
                         call.method->getOverloadedOperator() == clang::OO_Equal;
    // Everything is evaluated before anything is changed.
    Value target;
    bool targetChanges = false;
    if (call.object != nullptr) {
      target = objectOf(flow, call);
      targetChanges = changing(call) && kinds.holdsOwners(objectType(call));
    }
    Value taken;
    for (const Call::Argument &argument : call.arguments)
      taken.add(stored(flow, argument));
    alias(flow, call, call.object != nullptr ? &target : nullptr,
          targetChanges);

    // §5: moves.
    for (const Call::Argument &argument : call.arguments)
      if (takes(argument.parameter))
        if (const std::optional<Place> place = movedFrom(argument.expression))
          move(flow, *place, argument.expression->getExprLoc());

    // §7: a view that is assigned, or that may store what it is passed.
    const clang::VarDecl *view =
        call.object != nullptr ? viewLocal(call.object) : nullptr;
    if (assigns && call.object != nullptr) {
      if (const std::optional<Place> place = placeOf(call.object))
        unmove(flow, *place);
      if (view != nullptr) {
        Borrow &borrow = flow.borrows[view];
        borrow = {.loans = taken.loans, .reference = taken.reference};
      }
    } else if (view != nullptr && changing(call)) {
      Borrow &borrow = flow.borrows[view];
      for (const Loan &loan : taken.loans)
        if (!llvm::is_contained(borrow.loans, loan))
          borrow.loans.push_back(loan);
    }
    if (call.object != nullptr && view == nullptr && changing(call) &&
        kinds.tracked(objectType(call)) && outside(target)) {
      if (global(call.object) && !taken.loans.empty())
        error(call.at, rule::Escape,
              "a borrow is stored in the global " + spell(call.object, ast) +
                  ", which outlives every function",
              call.at,
              "keep in a global only what owns its data (a 'std::string', "
              "not a 'std::string_view')");
      else
        storedOutside(target, taken, call.at, spell(call.object, ast),
                      /*viewTarget=*/true);
    }

    // §6, rule 2: the object of a member function that is not const and
    // not an accessor.
    if (targetChanges) {
      // RFC 0043 §9: a global is assigned as a whole, moved out and back.
      if (!(assigns && global(call.object)))
        owners(target, call.at, spell(call.object, ast), call.method,
               call.object);
      invalidate(flow, target, Ends::Into, End::Why::Changed, call.at,
                 call.method);
    }
    // §6, rule 3: what a call may change through its parameters.
    for (const Call::Argument &argument : call.arguments) {
      const clang::Expr *given = argument.expression;
      if (argument.parameter != nullptr &&
          (given->getType()->isPointerType() ||
           kinds.tracked(given->getType())) &&
          hasAnnotation(annotationsOf(argument.parameter->getType(), ast),
                        EscapesAnnotation))
        storedOutside({}, stored(flow, argument), given->getExprLoc(),
                      "what " + called(call.callee) + " keeps",
                      /*viewTarget=*/false);
      if (!changes(argument.parameter) || movedFrom(given).has_value())
        continue;
      const Value objects = given->getType()->isPointerType()
                                ? pointee(flow, given)
                                : object(flow, given);
      owners(objects, given->getExprLoc(), spell(given, ast), call.callee,
             given);
      invalidate(flow, objects, Ends::Into, End::Why::Called, call.at,
                 call.callee);
      // §7: a view passed where the callee may store the other borrows.
      if (const clang::VarDecl *widened = viewLocal(given)) {
        Borrow &borrow = flow.borrows[widened];
        for (const Call::Argument &other : call.arguments)
          if (&other != &argument)
            for (const Loan &loan : stored(flow, other).loans)
              if (!llvm::is_contained(borrow.loans, loan))
                borrow.loans.push_back(loan);
      }
    }
    (void)constructs;
  }

  [[nodiscard]] std::optional<Place>
  placeOf(const clang::Expr *expression) const {
    expression = expression->IgnoreParenImpCasts();
    const clang::FieldDecl *field = nullptr;
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
      field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (field == nullptr)
        return std::nullopt;
      expression = member->getBase()->IgnoreParenImpCasts();
      if (llvm::isa<clang::CXXThisExpr>(expression))
        return Place{nullptr, field};
    }
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(expression);
    const clang::VarDecl *variable =
        named == nullptr ? nullptr : variableOf(*named);
    if (variable == nullptr)
      return std::nullopt;
    return Place{variable->getCanonicalDecl(), field};
  }

  void move(Flow &flow, const Place &place, clang::SourceLocation at) {
    flow.moved.emplace(place, at);
    invalidate(flow, rootOf(place), Ends::All, End::Why::Moved, at, nullptr);
  }

  static void unmove(Flow &flow, const Place &place) {
    for (auto entry = flow.moved.begin(); entry != flow.moved.end();)
      if (entry->first.first == place.first &&
          (place.second == nullptr || entry->first.second == place.second))
        entry = flow.moved.erase(entry);
      else
        ++entry;
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void collectAssigned(const clang::Stmt *statement) {
    if (statement == nullptr)
      return;
    const clang::Expr *target = nullptr;
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(statement);
        binary != nullptr && binary->getOpcode() == clang::BO_Assign)
      target = binary->getLHS();
    else if (const auto *operation =
                 llvm::dyn_cast<clang::CXXOperatorCallExpr>(statement);
             operation != nullptr &&
             operation->getOperator() == clang::OO_Equal &&
             operation->getNumArgs() == 2)
      target = operation->getArg(0);
    if (target != nullptr)
      assigned.insert(target->IgnoreParenImpCasts());
    for (const clang::Stmt *child : statement->children())
      collectAssigned(child);
  }

  void use(Flow &flow, const clang::DeclRefExpr &named) {
    const clang::VarDecl *variable = variableOf(named);
    if (variable == nullptr || assigned.contains(&named))
      return;
    variable = variable->getCanonicalDecl();
    usePlace(flow, {variable, nullptr}, named.getLocation(),
             variable->getNameAsString(), &named);
    const auto found = flow.borrows.find(variable);
    if (found == flow.borrows.end())
      return;
    if (found->second.end.why != End::Why::None) {
      reportStale(*variable, named.getLocation(), found->second.end);
      return;
    }
    // A reference to a view is stale when the view is.
    for (const Loan &loan : found->second.loans) {
      const clang::VarDecl *root = loan.root.variable();
      const auto holder =
          root == nullptr || root == variable ? flow.borrows.end()
                                              : flow.borrows.find(root);
      if (holder != flow.borrows.end() &&
          holder->second.end.why != End::Why::None &&
          !kinds.pointerLike(root->getType())) {
        reportStale(*variable, named.getLocation(), holder->second.end);
        return;
      }
    }
  }

  void usePlace(Flow &flow, const Place &place, clang::SourceLocation at,
                const std::string &name, const clang::Expr *expression) {
    for (const auto &[moved, where] : flow.moved) {
      if (moved.first != place.first)
        continue;
      const bool whole = moved.second == nullptr;
      const bool same = moved.second == place.second;
      // A member of a moved object, the moved member itself, or the whole
      // object while a member is moved.
      bool partUse = false;
      if (!whole && place.second == nullptr && expression != nullptr) {
        const clang::Stmt *parent = parents.getParent(expression);
        while (parent != nullptr &&
               llvm::isa<clang::ParenExpr, clang::ImplicitCastExpr>(parent))
          parent = parents.getParent(parent);
        partUse = llvm::isa_and_nonnull<clang::MemberExpr>(parent);
      }
      if (!(whole || same) && (place.second != nullptr || partUse))
        continue;
      if (emit && reportedMoves.emplace(moved, where.getRawEncoding()).second)
        report(at, rule::OwnerMoved,
               whole || same
                   ? "'" + name + "' is used after it was moved"
                   : "'" + name + "' is used while its member '" +
                         moved.second->getNameAsString() + "' is moved",
               where,
               "moved here; use it before the move, or assign to it first");
      return;
    }
  }
  std::set<std::pair<Place, unsigned>> reportedMoves;

  void declare(Flow &flow, const clang::VarDecl &variable) {
    const clang::VarDecl *canonical = variable.getCanonicalDecl();
    unmove(flow, {canonical, nullptr});
    if (!variable.hasLocalStorage() || !kinds.tracked(variable.getType()))
      return;
    Borrow borrow;
    if (const clang::Expr *init = variable.getInit()) {
      const clang::QualType type = variable.getType();
      const Value value = type->isReferenceType() ? object(flow, init)
                          : type->isPointerType() ? pointee(flow, init)
                                                  : points(flow, init);
      borrow.loans = value.loans;
      borrow.reference = value.reference;
    }
    flow.borrows[canonical] = std::move(borrow);
  }

  void assign(Flow &flow, const clang::BinaryOperator &binary) {
    const clang::Expr *left = binary.getLHS()->IgnoreParens();
    if (!left->getType()->isPointerType() ||
        left->getType()->isFunctionPointerType())
      return;
    const Value value = pointee(flow, binary.getRHS());
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(left))
      if (const clang::VarDecl *variable = variableOf(*named);
          variable != nullptr && variable->hasLocalStorage() &&
          !variable->getType()->isReferenceType()) {
        flow.borrows[variable->getCanonicalDecl()] = {
            .loans = value.loans, .reference = value.reference};
        return;
      }
    const Value target = object(flow, left);
    if (outside(target))
      storedOutside(target, value, binary.getOperatorLoc(), spell(left, ast),
                    /*viewTarget=*/false);
  }

  void returned(Flow &flow, const clang::ReturnStmt &statement) {
    const clang::Expr *expression = statement.getRetValue();
    const clang::QualType type = function.getReturnType();
    if (expression == nullptr || !kinds.tracked(type))
      return;
    const Value value = type->isReferenceType()  ? object(flow, expression)
                        : type->isPointerType() ? pointee(flow, expression)
                                                : points(flow, expression);
    for (const Loan &loan : value.loans) {
      if (!localRoot(loan.root))
        continue;
      const bool whole = kinds.pointerLike(type) && loan.depth == 0;
      error(statement.getReturnLoc(), rule::Escape,
            whole ? "a " +
                        std::string(type->isReferenceType() ? "reference"
                                                            : "pointer") +
                        " to " + nameOf(loan.root) +
                        " is returned, and it does not outlive the function"
                  : "what is returned points into " + nameOf(loan.root) +
                        ", which does not outlive the function",
            statement.getReturnLoc(),
            "return by value something that owns its data, or take " +
                nameOf(loan.root) +
                " as a parameter so that the caller keeps it");
      return;
    }
  }

  void initialize(Flow &flow, const clang::CXXCtorInitializer &initializer) {
    const clang::FieldDecl *field = initializer.getMember();
    if (field == nullptr || initializer.getInit() == nullptr)
      return;
    const clang::QualType type = field->getType();
    if (!kinds.tracked(type) || type->isFunctionPointerType())
      return;
    const clang::Expr *init = initializer.getInit();
    const Value value = type->isReferenceType()  ? object(flow, init)
                        : type->isPointerType() ? pointee(flow, init)
                                                : points(flow, init);
    Value target;
    target.loans.push_back(
        {.root = {.kind = RootKind::This, .field = field}, .depth = 0});
    storedOutside(target, value, initializer.getSourceLocation(),
                  "'" + field->getNameAsString() + "'",
                  /*viewTarget=*/!type->isPointerType());
  }

  void transfer(Flow &flow, const clang::Stmt *statement) {
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement)) {
      use(flow, *named);
    } else if (const auto *member =
                   llvm::dyn_cast<clang::MemberExpr>(statement)) {
      if (!assigned.contains(member))
        if (const std::optional<Place> place = placeOf(member);
            place.has_value() && place->second != nullptr)
          usePlace(flow, *place, member->getMemberLoc(),
                   place->second->getNameAsString(), nullptr);
    } else if (const auto *declaration =
                   llvm::dyn_cast<clang::DeclStmt>(statement)) {
      for (const clang::Decl *declared : declaration->decls())
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(declared))
          declare(flow, *variable);
    } else if (const auto *binary =
                   llvm::dyn_cast<clang::BinaryOperator>(statement)) {
      if (binary->getOpcode() == clang::BO_Assign)
        assign(flow, *binary);
    } else if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
      returned(flow, *ret);
    } else if (llvm::isa<clang::CallExpr, clang::CXXConstructExpr>(statement)) {
      call(flow, llvm::cast<clang::Expr>(statement));
    }
  }

  void transferBlock(const clang::CFGBlock &block, Flow &flow) {
    for (const clang::CFGElement &element : block) {
      if (const auto statement = element.getAs<clang::CFGStmt>()) {
        transfer(flow, statement->getStmt());
      } else if (const auto initializer =
                     element.getAs<clang::CFGInitializer>()) {
        initialize(flow, *initializer->getInitializer());
      } else if (const auto lifetime =
                     element.getAs<clang::CFGLifetimeEnds>()) {
        const clang::VarDecl *variable =
            lifetime->getVarDecl()->getCanonicalDecl();
        invalidate(flow, at(*variable), Ends::All, End::Why::Destroyed,
                   lifetime->getTriggerStmt()->getEndLoc(), nullptr);
        flow.borrows.erase(variable);
        unmove(flow, {variable, nullptr});
      } else if (const auto cleanup =
                     element.getAs<clang::CFGFullExprCleanup>()) {
        for (const clang::MaterializeTemporaryExpr *temporary :
             cleanup->getExpiringMTEs()) {
          Value value;
          value.loans.push_back(
              {.root = {.kind = RootKind::Temp, .base = temporary},
               .depth = 0});
          invalidate(flow, value, Ends::All, End::Why::Destroyed,
                     cleanup->getCleanupLoc(), nullptr);
        }
      }
    }
  }

  [[nodiscard]] Flow entryFlow() {
    Flow flow;
    flow.reached = true;
    for (const clang::ParmVarDecl *parameter : function.parameters()) {
      const clang::QualType type = parameter->getType();
      if (!kinds.tracked(type))
        continue;
      const clang::VarDecl *canonical = parameter->getCanonicalDecl();
      Borrow borrow;
      if (kinds.pointerLike(type))
        borrow.loans.push_back(
            {.root = {.kind = RootKind::Var, .base = canonical}, .depth = 0});
      if (!kinds.pointerLike(type) || kinds.tracked(type->getPointeeType()))
        borrow.loans.push_back(
            {.root = {.kind = RootKind::Var, .base = canonical, .beyond = true},
             .depth = 1});
      flow.borrows[parameter->getCanonicalDecl()] = std::move(borrow);
    }
    return flow;
  }

  /// §5: a member moved out of `*this`, or out of a parameter taken by
  /// lvalue reference, is assigned again before the function returns.
  void exitChecks(const Flow &flow, clang::SourceLocation at) {
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    const bool mayLeave =
        llvm::isa<clang::CXXDestructorDecl>(&function) ||
        (method != nullptr &&
         method->getRefQualifier() == clang::RQ_RValue);
    for (const auto &[place, where] : flow.moved) {
      if (place.first == nullptr ? mayLeave
                                 : !place.first->getType()
                                        ->isLValueReferenceType())
        continue;
      if (emit && reportedMoves.emplace(place, where.getRawEncoding()).second)
        report(at, rule::OwnerMoved,
               "'" +
                   (place.second != nullptr
                        ? place.second->getNameAsString()
                        : place.first->getNameAsString()) +
                   "' is left moved when the function returns",
               where,
               "moved here; assign to it before returning ('x = {}', or "
               "move with 'std::exchange(x, {})'), or take the object by "
               "value or by rvalue reference");
    }
  }

  bool solve() {
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
        method != nullptr && !isLambdaCallOperator(method) &&
        !method->isStatic())
      readOnlyThis = method->isConst() || kinds.accessor(*method);
    const unsigned count = graph->getNumBlockIDs();
    std::vector<Flow> in(count);
    std::vector<const clang::CFGBlock *> order;
    std::vector<unsigned> rank(count, 0);
    for (const clang::CFGBlock *block :
         llvm::ReversePostOrderTraversal<const clang::CFG *>(graph)) {
      rank[block->getBlockID()] = static_cast<unsigned>(order.size());
      order.push_back(block);
    }
    const clang::CFGBlock &entry = graph->getEntry();
    in[entry.getBlockID()] = entryFlow();
    std::set<unsigned> work;
    work.insert(rank[entry.getBlockID()]);
    unsigned steps = 0;
    const unsigned limit = 64 * (count + 4);
    while (!work.empty()) {
      if (++steps > limit)
        return false;
      const clang::CFGBlock *block = order[*work.begin()];
      work.erase(work.begin());
      Flow out = in[block->getBlockID()];
      transferBlock(*block, out);
      for (const clang::CFGBlock::AdjacentBlock &adjacent : block->succs())
        if (const clang::CFGBlock *next = adjacent.getReachableBlock())
          if (join(in[next->getBlockID()], out))
            work.insert(rank[next->getBlockID()]);
    }
    emit = true;
    const clang::CFGBlock &exit = graph->getExit();
    for (const clang::CFGBlock *block : order) {
      Flow flow = in[block->getBlockID()];
      if (!flow.reached)
        continue;
      transferBlock(*block, flow);
      if (block == &exit || block->hasNoReturnElement())
        continue;
      // The function returns from a block that ends in a `return`, or
      // whose every way on is the exit; a call that may throw has an edge
      // to the exit too, which is not a return.
      clang::SourceLocation at = body.getEndLoc();
      bool returns = false;
      bool throws = false;
      for (const clang::CFGElement &element : *block)
        if (const auto statement = element.getAs<clang::CFGStmt>()) {
          if (const auto *ret =
                  llvm::dyn_cast<clang::ReturnStmt>(statement->getStmt())) {
            at = ret->getReturnLoc();
            returns = true;
          }
          throws = llvm::isa<clang::CXXThrowExpr>(statement->getStmt());
        }
      const bool fallsOff =
          !throws && block->succ_size() != 0 &&
          llvm::all_of(block->succs(),
                       [&](const clang::CFGBlock::AdjacentBlock &adjacent) {
                         return adjacent.getReachableBlock() == &exit;
                       });
      if (!returns && !fallsOff)
        continue;
      exitChecks(flow, at);
    }
    return true;
  }
};

} // namespace

bool checkCxxFlow(clang::ASTContext &ast, CxxKinds &kinds,
                  const clang::FunctionDecl &function, const clang::Stmt &body,
                  const RuleReport &report) {
  return CxxFlow(ast, kinds, function, body, report).run();
}

} // namespace weavec::frontend
