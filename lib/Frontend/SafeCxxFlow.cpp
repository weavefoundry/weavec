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
// RFC 0046 adds threads. The state also holds the guards held on the
// locks of locked classes (a borrow of a locked member points into its
// guard too, and ends with it), and the locals each live thread object has
// been lent, to read or to write; a use of a lent local against its loan is
// `thread-lent`, a locked member used with no guard `lock-held`. At each
// thread start what crosses is checked (`thread-send`, `thread-share`).
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
constexpr llvm::StringLiteral ThreadSend = "thread-send";
constexpr llvm::StringLiteral ThreadShare = "thread-share";
constexpr llvm::StringLiteral ThreadLent = "thread-lent";
constexpr llvm::StringLiteral LockHeld = "lock-held";
constexpr llvm::StringLiteral NotModeled = "not-modeled";
} // namespace rule

/// RFC 0046 §6: `Guard` is a guard local a borrow of a locked member points
/// into as well.
enum class RootKind : std::uint8_t { Var, This, Temp, Guard };

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
  [[nodiscard]] const clang::VarDecl *guard() const {
    return kind == RootKind::Guard ? static_cast<const clang::VarDecl *>(base)
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
  enum class Why : std::uint8_t {
    None,
    Changed,
    Called,
    Destroyed,
    Moved,
    Unlocked
  };
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

/// RFC 0046 §5: a local a thread object has been lent, to write or to read,
/// at a thread start.
struct Lend {
  const clang::VarDecl *local = nullptr;
  bool write = true;
  clang::SourceLocation at;
  /// Amendment: only the element at this loop counter's value, which each
  /// iteration of its loop lends anew from the same capture.
  const clang::VarDecl *slot = nullptr;
  bool operator==(const Lend &) const = default;
};
using Lends = llvm::SmallVector<std::pair<const clang::VarDecl *, Lend>, 2>;

struct Flow {
  bool reached = false;
  llvm::DenseMap<const clang::VarDecl *, Borrow> borrows;
  /// Moved objects: a local or parameter (null: `this`) and a member of it
  /// (null: all of it), and where.
  std::map<Place, clang::SourceLocation> moved;
  /// RFC 0046 §5: each live thread object's loans (a local, a container of
  /// thread objects, the result of a `spawns` function), held on some path.
  Lends lends;
  /// §6: the guards held on every path.
  llvm::SmallVector<HeldGuard, 2> guards;
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
  for (const auto &entry : from.lends)
    if (!llvm::is_contained(into.lends, entry)) {
      into.lends.push_back(entry);
      changed = true;
    }
  const std::size_t guards = into.guards.size();
  llvm::erase_if(into.guards, [&](const HeldGuard &guard) {
    return !llvm::is_contained(from.guards, guard);
  });
  return changed || into.guards.size() != guards;
}

static const clang::Expr *strip(const clang::Expr *expression) {
  while (expression != nullptr) {
    const clang::Expr *inner = expression->IgnoreParens();
    if (const auto *cleanups = llvm::dyn_cast<clang::ExprWithCleanups>(inner))
      inner = cleanups->getSubExpr();
    else if (const auto *bind =
                 llvm::dyn_cast<clang::CXXBindTemporaryExpr>(inner))
      inner = bind->getSubExpr();
    else if (const auto *opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(inner);
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
          const clang::LambdaExpr *lambda, const RuleReport &report)
      : ast(ast), kinds(kinds), function(function), body(body),
        writtenAs(lambda), report(report),
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
  /// The lambda whose body this is, if it is one.
  const clang::LambdaExpr *writtenAs;
  const RuleReport &report;
  clang::ParentMap parents;
  const clang::CFG *graph = nullptr;
  bool emit = false;
  /// Names that are assigned as a whole where they stand: not uses.
  llvm::DenseSet<const clang::Expr *> assigned;
  /// The variables so assigned anywhere in the function.
  llvm::DenseSet<const clang::VarDecl *> assignedLocals;
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
    if (const clang::VarDecl *guard = root.guard())
      return "'" + guard->getNameAsString() + "'";
    if (const clang::VarDecl *variable = root.variable())
      name = loopRange(*variable).value_or(variable->getNameAsString());
    if (root.field != nullptr)
      name += (name.empty() ? "" : ".") + root.field->getNameAsString();
    if (name.empty())
      name = "this object";
    return "'" + name + "'";
  }

  /// RFC 0046 §5: whether the range-for whose hidden variable this is only
  /// reads its range.
  [[nodiscard]] std::optional<bool>
  rangeReads(const clang::VarDecl &variable) const {
    if (!variable.isImplicit())
      return std::nullopt;
    for (const clang::Stmt *up = variable.getInit(); up != nullptr;
         up = parents.getParent(up))
      if (const auto *loop = llvm::dyn_cast<clang::CXXForRangeStmt>(up)) {
        const clang::QualType type = loop->getLoopVariable()->getType();
        return !type->isReferenceType() ||
               type->getPointeeType().isConstQualified();
      }
    return std::nullopt;
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
      message = loop ? "the loop over '" + *loop +
                           "' continues after the "
                           "call to " +
                           called(end.by) +
                           ", which may change "
                           "what " +
                           owner + " owns"
                     : name + " was taken from " + owner +
                           " before the call to " + called(end.by) +
                           ", which may change what " + owner + " owns";
      note = "take the borrow again after the call, or declare the "
             "parameter 'const' if " +
             called(end.by) + " changes nothing";
      break;
    case End::Why::Destroyed:
      message = name + " points into " + owner +
                (end.root.kind == RootKind::Temp
                     ? ", which was destroyed at the end of its statement"
                     : ", whose lifetime has ended");
      note = end.root.kind == RootKind::Temp
                 ? "the temporary is destroyed here; keep it in a local "
                   "that lives as long as " +
                       name
                 : owner +
                       " is destroyed here; declare it in the scope "
                       "that uses " +
                       name;
      break;
    case End::Why::Moved:
      message = name + " points into " + owner + ", which was moved";
      note = "moved here; take " + name + " from the object it was moved to";
      break;
    case End::Why::Unlocked:
      // RFC 0046 §6.
      message = name + " was taken under the lock " + owner +
                ", which was released here";
      note = "released here; take " + name +
             " again under the lock, or copy what is needed while it is "
             "held";
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
    if (root.beyond || root.kind == RootKind::Guard)
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
    // A parameter taken by value is a local too: its own storage, and what
    // a view parameter owns itself. Only a pointer or reference parameter
    // stands for the caller's object.
    return !llvm::isa<clang::ParmVarDecl>(variable) ||
           !CxxKinds::pointerLike(variable->getType());
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
  [[nodiscard]] static Value borrowOf(const Flow &flow,
                                      const clang::VarDecl &variable) {
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
        // The library's own (`std::cout`) are the library's to keep.
        Value value;
        value.reference =
            !variable->getType().isConstQualified() && !isLibrary(*variable);
        return value;
      }
      return at(*variable);
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
      const auto *field =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (field == nullptr)
        return {};
      Value base = member->isArrow() ? pointee(flow, member->getBase())
                                     : object(flow, member->getBase());
      if (field->getType()->isReferenceType()) {
        // A reference member names another object: what the view was
        // given, not a part of the object that holds it.
        Value named;
        named.reference = base.reference;
        named.through = base.through;
        for (Loan loan : viewOf(flow, base).loans) {
          if (loan.root.beyond) {
            loan.root.field = field;
            loan.depth = 0;
          }
          named.add(loan);
        }
        return named;
      }
      for (Loan &loan : base.loans)
        if (loan.depth == 0 && loan.root.field == nullptr)
          loan.root.field = field;
      // RFC 0046 §6: a locked member points into the guards on its lock.
      if (const LockedClass *info = lockedMember(*field);
          info != nullptr && info->lock != nullptr)
        if (const std::optional<const clang::VarDecl *> owner =
                lockedObject(member->getBase()))
          for (const HeldGuard &guard : heldOn(flow, *owner, info->lock))
            base.add({.root = {.kind = RootKind::Guard, .base = guard.guard},
                      .depth = 0});
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
    if (const auto *binary =
            llvm::dyn_cast<clang::BinaryOperator>(expression)) {
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
    if (const auto *binary =
            llvm::dyn_cast<clang::BinaryOperator>(expression)) {
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

  /// What a place of type `type` takes from `expression`: the object a
  /// reference names, what a pointer points at, where a view points.
  [[nodiscard]] Value held(const Flow &flow, clang::QualType type,
                           const clang::Expr *expression) {
    if (type->isReferenceType())
      return object(flow, expression);
    if (type->isPointerType())
      return pointee(flow, expression);
    return points(flow, expression);
  }

  /// Where the views kept in the named objects point: a view local's own
  /// borrows; for anything else, into the object.
  [[nodiscard]] static Value viewOf(const Flow &flow, const Value &objects) {
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
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    const bool fields = record != nullptr && type->isAggregateType() &&
                        record->getNumBases() == 0;
    auto field =
        fields ? record->field_begin() : clang::RecordDecl::field_iterator();
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
    const auto *init = lambda.capture_init_begin();
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

  /// A borrow taken through `expression`, the object of a member call.
  /// Through a view that points elsewhere (an iterator, a
  /// `std::string_view`) it points where the view points. Through a view
  /// that owns nothing but holds views (an optional of one, a struct of
  /// them) it does too, unless the result names one of the views it holds
  /// (`*optional`), which is in the object. Through anything else it
  /// points into the object.
  [[nodiscard]] Value through(const Flow &flow, const clang::Expr *expression,
                              bool namesView) {
    const clang::QualType type = expression->getType();
    if (type->isPointerType())
      return pointee(flow, expression).inside();
    if (kinds.pureView(type) && (CxxKinds::pointerClass(type) || !namesView))
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
      if (const auto *member =
              llvm::dyn_cast<clang::CXXMemberCallExpr>(&plain)) {
        call.method = member->getMethodDecl();
        call.object = member->getImplicitObjectArgument();
      } else if (llvm::isa<clang::CXXOperatorCallExpr>(plain) &&
                 method != nullptr &&
                 method->isImplicitObjectMemberFunction() &&
                 !arguments.empty()) {
        call.method = method;
        call.object = arguments.front();
        arguments = arguments.drop_front();
      }
    }
    for (unsigned i = 0; i < arguments.size(); ++i)
      call.arguments.push_back(
          {.expression = arguments[i],
           .parameter =
               call.callee != nullptr && i < call.callee->getNumParams()
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
    // §7: a callable returned in a type that hides its captures owns
    // them (the callee is checked for it): only a view passed by value
    // comes back out.
    // The result is a reference or a pointer to a view.
    const clang::QualType named =
        type->isPointerType() ? type->getPointeeType() : type;
    const bool namesView = (names || type->isPointerType()) &&
                           (named->isPointerType() ||
                            kinds.viewClass(named->getAsCXXRecordDecl()));
    const bool owning = !constructs && !expression->isGLValue() &&
                        kinds.erased(expression->getType()) &&
                        !kinds.view(expression->getType()) &&
                        call.callee != nullptr && !isLibrary(*call.callee);
    // RFC 0046, amendment: an erased object a library member function
    // returns by value (`get_future()`) shares the state the object holds,
    // not a place in it.
    const bool sharesState =
        call.object != nullptr && !constructs && !expression->isGLValue() &&
        call.method != nullptr && isLibrary(*call.method) &&
        kinds.erased(expression->getType()) &&
        !kinds.view(expression->getType());
    if (sharesState) {
      if (kinds.tracked(objectType(call)))
        value.add(points(flow, call.object));
    } else if (call.object != nullptr && !marked && !owning) {
      value.add(through(flow, call.object, namesView));
    }
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
        // if it points to the result's type; RFC 0045 §7: or, for the
        // library's (`std::get_if(&variant)`), into what it points to.
        if (given->isPointerType() && (names || type->isPointerType()) &&
            !constructs && !CxxKinds::related(given->getPointeeType(), type)) {
          if (call.callee != nullptr && isLibrary(*call.callee) &&
              kinds.contains(given->getPointeeType(),
                             type->isPointerType() ? type->getPointeeType()
                                                   : type)) {
            value.add(points(flow, argument.expression).inside());
            views = true;
          }
          continue;
        }
        // An owning callable takes borrows only from a callable it wraps;
        // a class of the program keeps a view only if a member could.
        if (owning && !kinds.erased(given))
          continue;
        if (!names && type->isRecordType() &&
            !kinds.mayBorrow(type->getAsCXXRecordDecl(),
                             given->isPointerType() ? given->getPointeeType()
                                                    : given))
          continue;
        value.add(points(flow, argument.expression));
        views = true;
      }
    }
    // §9: without a view passed by value, the result of a function that is
    // not a member points into its reference arguments. What is
    // constructed may keep every one.
    if (!marked && !owning &&
        (constructs || (!views && call.object == nullptr)))
      for (const Call::Argument &argument : call.arguments)
        if (argument.parameter != nullptr &&
            argument.parameter->getType()->isReferenceType()) {
          // What a library accessor returns is inside its argument
          // (`std::get<T>(variant)`), whatever its type.
          if (CxxKinds::accessorParameter(*argument.parameter) ||
              accessorTwin(*argument.parameter))
            value.add(through(flow, argument.expression, namesView));
          else
            value.add(contribution(flow, argument, type, names, constructs));
        }
    return value;
  }

  /// The const side of a library accessor that is not a member.
  [[nodiscard]] static bool accessorTwin(const clang::ParmVarDecl &parameter) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(parameter.getDeclContext());
    const clang::QualType type = parameter.getType();
    if (function == nullptr || !isLibrary(*function) ||
        llvm::isa<clang::CXXMethodDecl>(function) ||
        !type->isLValueReferenceType() ||
        !type->getPointeeType().isConstQualified() ||
        function->getNumParams() != 1)
      return false;
    const clang::QualType result = function->getReturnType();
    return result->isLValueReferenceType() || result->isPointerType();
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
    const bool same = CxxKinds::related(given, type);
    // A copy or a move of a view; and what erases its contents copies them
    // in (`std::function`, `std::async`): it keeps their borrows, not
    // their place.
    const bool copies = constructs && same;
    const bool erases = kinds.erased(type) && !kinds.view(type);
    if ((names || type->isPointerType()) && !constructs) {
      // A reference or pointer result can be the argument itself only if
      // it has its type.
      if (same)
        value.add(object(flow, expression));
    } else if (!kinds.pureView(given) && !copies && !erases &&
               (!type->isRecordType() ||
                kinds.mayBorrow(type->getAsCXXRecordDecl(), given))) {
      // A copy or a move of a view points where the view does, not into
      // the object it was copied from; and a class whose members could
      // not refer to the argument keeps no borrow of it.
      value.add(object(flow, expression).inside());
    }
    return value;
  }

  //===--------------------------------------------------------------------===//
  // Effects
  //===--------------------------------------------------------------------===//

  enum class Ends : std::uint8_t { Into, All };

  static void invalidate(Flow &flow, const Value &objects, Ends ends,
                         End::Why why, clang::SourceLocation at,
                         const clang::NamedDecl *by) {
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
    while (
        const auto *temporary =
            llvm::dyn_cast_or_null<clang::MaterializeTemporaryExpr>(expression))
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
        {.root = {.kind =
                      place.first == nullptr ? RootKind::This : RootKind::Var,
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

  /// §7: the callee keeps the argument: the parameter says WEAVEC_ESCAPES,
  /// or takes by value a type that hides what it holds (a `std::function`
  /// taken by value is one the callee means to store; one it only calls is
  /// taken by reference to const).
  [[nodiscard]] bool keeps(const clang::ParmVarDecl *parameter) {
    if (parameter == nullptr)
      return false;
    const clang::QualType type = parameter->getType();
    if (hasAnnotation(annotationsOf(type, ast), EscapesAnnotation) ||
        hasAnnotation(annotationsOf(type, ast), SendsAnnotation))
      return true;
    if (type->isLValueReferenceType() || !kinds.erased(type))
      return false;
    // A template's parameter that an instantiation gave such a type was
    // not declared with it: a callable taken as `F f` is called.
    const auto *callee =
        llvm::dyn_cast<clang::FunctionDecl>(parameter->getDeclContext());
    const clang::FunctionDecl *pattern =
        callee == nullptr ? nullptr : callee->getTemplateInstantiationPattern();
    const unsigned index = parameter->getFunctionScopeIndex();
    return pattern == nullptr || index >= pattern->getNumParams() ||
           !pattern->getParamDecl(index)->getType()->isDependentType();
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
    return !target.isConstQualified() && kinds.holdsOwners(target) &&
           !CxxKinds::accessorParameter(*parameter);
  }

  [[nodiscard]] bool changing(const Call &call) {
    return call.method != nullptr && !call.method->isConst() &&
           !call.method->isStatic() &&
           !llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(
               call.method) &&
           !kinds.accessor(*call.method);
  }

  [[nodiscard]] static clang::QualType objectType(const Call &call) {
    const clang::QualType type = call.object->getType();
    return type->isPointerType() ? type->getPointeeType() : type;
  }

  [[nodiscard]] Value objectOf(const Flow &flow, const Call &call) {
    return call.object->getType()->isPointerType() ? pointee(flow, call.object)
                                                   : object(flow, call.object);
  }

  /// A local view or erased object named directly.
  [[nodiscard]] const clang::VarDecl *viewLocal(const clang::Expr *expression) {
    const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
        expression == nullptr ? nullptr : expression->IgnoreParenImpCasts());
    const clang::VarDecl *variable =
        named == nullptr ? nullptr : variableOf(*named);
    if (variable == nullptr || !variable->hasLocalStorage() ||
        CxxKinds::pointerLike(variable->getType()) ||
        !kinds.tracked(variable->getType()))
      return nullptr;
    return variable->getCanonicalDecl();
  }

  /// The local a member (of a member…) belongs to.
  [[nodiscard]] const clang::VarDecl *
  memberOfLocal(const clang::Expr *expression) const {
    expression = expression->IgnoreParenImpCasts();
    bool member = false;
    while (const auto *access = llvm::dyn_cast<clang::MemberExpr>(expression)) {
      if (access->isArrow() ||
          !llvm::isa<clang::FieldDecl>(access->getMemberDecl()))
        return nullptr;
      expression = access->getBase()->IgnoreParenImpCasts();
      member = true;
    }
    return member ? ownObject(expression) : nullptr;
  }

  /// A local object of the function, named directly.
  [[nodiscard]] const clang::VarDecl *
  ownObject(const clang::Expr *expression) const {
    const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
        expression == nullptr ? nullptr : expression->IgnoreParenImpCasts());
    const clang::VarDecl *variable =
        named == nullptr ? nullptr : variableOf(*named);
    return variable != nullptr && ownLocal(*variable) &&
                   !llvm::isa<clang::ParmVarDecl>(variable) &&
                   variable->getType()->isRecordType()
               ? variable->getCanonicalDecl()
               : nullptr;
  }

  /// What a view takes from an argument it may store (§7, widened).
  [[nodiscard]] Value stored(const Flow &flow, const Call::Argument &argument,
                             const Call *into = nullptr) {
    const clang::Expr *expression = argument.expression;
    const clang::QualType given = expression->getType();
    if (given->isPointerType())
      return given->isFunctionPointerType() ? Value{}
                                            : pointee(flow, expression);
    if (kinds.tracked(given))
      return points(flow, expression);
    // An owner passed by reference is lent for the call: a callee that is
    // not a constructor does not keep a view of it (checked in the callee).
    // A library container makes its elements from what it is passed
    // (`emplace_back`), and they keep what their class could.
    if (into != nullptr && into->object != nullptr && into->method != nullptr &&
        isLibrary(*into->method) && expression->isGLValue() &&
        given->isRecordType() && kinds.view(objectType(*into)) &&
        kinds.mayBorrow(objectType(*into)->getAsCXXRecordDecl(), given))
      return object(flow, expression).inside();
    return {};
  }

  void owners(const Value &objects, clang::SourceLocation at,
              const std::string &what, const clang::NamedDecl *by,
              const clang::Expr *expression, clang::QualType type) {
    if (objects.reference && globalBase(expression)) {
      // RFC 0046 §4: a global of shareable type is used in place.
      // One that is not `thread_local` is `thread-share` where it is
      // declared, unless it is shareable.
      const clang::VarDecl *global = globalOf(expression);
      if (kinds.threadRules && global != nullptr &&
          (kinds.shareable(global->getType()) ||
           global->getTLSKind() == clang::VarDecl::TLS_None))
        return;
      error(at, rule::OwnerPath,
            what + " is a global, which every function can reach, and " +
                called(by) + " may change what it owns",
            at,
            "move the 'thread_local' into a local, change the local and move "
            "it back ('auto local = std::move(global); ...; global = "
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
    // Amendment 19: what a reference member names is not `*this`'s, and a
    // member no borrow can point into (a mutex) may be changed.
    if (!readOnlyThis || !kinds.lends(type))
      return;
    if (llvm::any_of(objects.loans,
                     [](const Loan &loan) {
                       return loan.root.kind == RootKind::This &&
                              !loan.root.beyond;
                     }) &&
        !constPaths.contains(ast.getSourceManager().getExpansionLineNumber(at)))
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
  /// RFC 0046 (D4): the lines where a write through a `const` path was
  /// reported, which the rule above does not report again.
  std::set<unsigned> constPaths;

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

  /// RFC 0045 §7: a pointer into storage that changes type in place is not
  /// kept where the rules stop following it, unless what it points to is
  /// `const` and holds no pointer.
  void switchingStore(const Value &source, clang::SourceLocation at,
                      const std::string &where, clang::QualType pointee) {
    if (pointee.isNull() ||
        (pointee.isConstQualified() && !kinds.holdsPointer(pointee)))
      return;
    for (const Loan &loan : source.loans) {
      clang::QualType type;
      if (loan.root.kind == RootKind::Guard)
        continue;
      if (loan.root.field != nullptr)
        type = loan.root.field->getType();
      else if (const clang::VarDecl *variable = loan.root.variable())
        type = variable->getType();
      else if (loan.root.kind == RootKind::Temp)
        type = static_cast<const clang::Expr *>(loan.root.base)->getType();
      else if (const auto *method =
                   llvm::dyn_cast<clang::CXXMethodDecl>(&function))
        type = method->getFunctionObjectParameterType();
      // (A pointer to the object itself is no pointer into its storage.)
      if (type.isNull() || loan.root.beyond ||
          clang::ASTContext::hasSameUnqualifiedType(type.getNonReferenceType(),
                                                    pointee) ||
          !kinds.switchesFor(type, pointee))
        continue;
      error(at, rule::Escape,
            "a pointer into " + nameOf(loan.root) +
                ", whose contents change type, is stored in " + where,
            at,
            "keep the index or the key and look the value up when it is "
            "used; 'weavec-cc --weavec-explain escape' shows the forms");
      return;
    }
  }

  /// §7: a view stored where it outlives the function's locals, or in the
  /// object it points into.
  void storedOutside(const Value &target, const Value &source,
                     clang::SourceLocation at, const std::string &where,
                     bool viewTarget, const std::string &fix = {}) {
    const bool toThis = llvm::any_of(target.loans, [](const Loan &loan) {
      return loan.root.kind == RootKind::This;
    });
    for (const Loan &loan : source.loans) {
      if (loan.root.kind == RootKind::Guard) {
        error(at, rule::Escape,
              "a borrow of a locked member is stored in " + where +
                  ", which outlives the lock " + nameOf(loan.root),
              at, "store a copy: values leave a lock by copy or by move");
        return;
      }
      if (localRoot(loan.root)) {
        error(at, rule::Escape,
              "a borrow of " + nameOf(loan.root) + " is stored in " + where +
                  ", which outlives it",
              at,
              !fix.empty() ? fix
                           : "store a copy that owns its data, or take " +
                                 nameOf(loan.root) +
                                 " as a parameter so that the caller keeps "
                                 "it");
        return;
      }
      const auto *parameter =
          llvm::dyn_cast_or_null<clang::ParmVarDecl>(loan.root.variable());
      // §7, received: a callable handed in a type that hides what it
      // captured may hold borrows of the caller's locals.
      if (parameter != nullptr && loan.root.beyond &&
          kinds.erased(parameter->getType()) && ownLocal(*parameter) &&
          !keeps(parameter)) {
        error(at, rule::Escape,
              where + " would keep " + nameOf(loan.root) +
                  ", which may hold borrows of its caller's locals",
              parameter->getLocation(),
              "take the callable by value, which says it is kept, or "
              "declare the parameter WEAVEC_ESCAPES, so that callers pass a "
              "callable that captures by value");
        return;
      }
      // In the function's own view member, any parameter it borrowed; in
      // what another object keeps, one that could be a temporary.
      const bool lent = parameter != nullptr &&
                        parameter->getType()->isReferenceType() &&
                        !kinds.tracked(parameter->getType()->getPointeeType());
      if (viewTarget && lent && !loan.root.beyond &&
          (toThis ||
           parameter->getType()->getPointeeType().isConstQualified()) &&
          ownLocal(*parameter) &&
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

  [[nodiscard]] static const clang::VarDecl *
  globalOf(const clang::Expr *expression) {
    while (expression != nullptr) {
      expression = expression->IgnoreParenImpCasts();
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expression);
          member != nullptr && !member->isArrow())
        expression = member->getBase();
      else
        break;
    }
    const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression);
    return named == nullptr ? nullptr
                            : llvm::dyn_cast<clang::VarDecl>(named->getDecl());
  }

  [[nodiscard]] static bool global(const clang::Expr *expression) {
    const auto *named =
        llvm::dyn_cast<clang::DeclRefExpr>(expression->IgnoreParenImpCasts());
    const auto *variable =
        named == nullptr ? nullptr
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
        const auto *named =
            llvm::dyn_cast<clang::DeclRefExpr>(argument->IgnoreParenImpCasts());
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

  [[nodiscard]] static const clang::LambdaExpr *
  lambdaOf(const clang::Expr *expression) {
    expression = strip(expression);
    while (expression != nullptr) {
      if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(expression))
        return lambda;
      if (const auto *temporary =
              llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
        expression = strip(temporary->getSubExpr());
      } else if (const auto *cast =
                     llvm::dyn_cast<clang::CastExpr>(expression)) {
        expression = strip(cast->getSubExpr());
      } else if (const auto *construct =
                     llvm::dyn_cast<clang::CXXConstructExpr>(expression);
                 construct != nullptr && construct->getNumArgs() == 1) {
        expression = strip(construct->getArg(0));
      } else if (const auto *named =
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
  void alias(const Flow &flow, const Call &call, const Value *target,
             bool objectChanges) {
    struct Passed {
      Value value;
      bool changer;
      const clang::Expr *expression;
    };
    llvm::SmallVector<Passed, 4> passed;
    const bool library = call.callee != nullptr && isLibrary(*call.callee);
    if (target != nullptr)
      passed.push_back({.value = *target,
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
                                      : object(flow, expression);
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
            // A borrow below what the callee may change, or the same
            // object passed twice (not two members under one guard).
            if (mine.root.kind != RootKind::Guard &&
                ((theirs.depth > mine.depth &&
                  mine.root.overlaps(theirs.root)) ||
                 (theirs.depth == 0 && mine.depth == 0 &&
                  mine.root == theirs.root))) {
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
  //===--------------------------------------------------------------------===//
  // RFC 0046: reads, thread starts and loans
  //===--------------------------------------------------------------------===//

  /// §5: whether the object `expression` designates (with `pointer`, what
  /// its value points to; with `through`, what the pointer variable it
  /// names points to) is only read where it is used: read, copied, passed
  /// by value or by reference to `const`, given a `const` member function.
  [[nodiscard]] bool constUse(const clang::Expr *expression,
                              bool pointer = false, bool through = false,
                              unsigned depth = 0) {
    const clang::Expr *at = expression;
    for (unsigned step = 0; step < 64; ++step) {
      const clang::Stmt *up = parents.getParentIgnoreParens(at);
      if (up == nullptr)
        return false;
      if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(up)) {
        switch (cast->getCastKind()) {
        case clang::CK_LValueToRValue:
          if (pointer || !through || !cast->getType()->isPointerType())
            return true;
          pointer = true;
          through = false;
          break;
        case clang::CK_NoOp:
          if (pointer ? cast->getType()->getPointeeType().isConstQualified()
                      : cast->getType().isConstQualified())
            return true;
          break;
        case clang::CK_ArrayToPointerDecay:
          pointer = true;
          break;
        case clang::CK_DerivedToBase:
        case clang::CK_UncheckedDerivedToBase:
        case clang::CK_ConstructorConversion:
        case clang::CK_UserDefinedConversion:
          break;
        case clang::CK_PointerToBoolean:
        case clang::CK_PointerToIntegral:
        case clang::CK_ToVoid:
          return true;
        default:
          return !pointer;
        }
        at = cast;
        continue;
      }
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(up)) {
        if (member->isArrow() != pointer)
          return false;
        pointer = false;
        if (const auto *method =
                llvm::dyn_cast<clang::CXXMethodDecl>(member->getMemberDecl()))
          return calledConst(*method, parents.getParentIgnoreParens(member), at,
                             pointer, depth);
        if (const auto *field =
                llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
            field != nullptr && field->isMutable() &&
            kinds.shareable(field->getType()))
          return true;
        at = member;
        continue;
      }
      if (const auto *operation =
              llvm::dyn_cast<clang::CXXOperatorCallExpr>(up);
          operation != nullptr && operation->getNumArgs() != 0 &&
          operation->getArg(0)->IgnoreParens() == at)
        if (const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
                operation->getDirectCallee())) {
          if (pointer)
            return false;
          if (!calledConstStep(*method, operation, at, pointer))
            return method->isConst() || method->isStatic();
          continue;
        }
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(up)) {
        if (passesThrough(*call)) {
          at = call;
          continue;
        }
        const clang::FunctionDecl *callee = call->getDirectCallee();
        const unsigned shift =
            llvm::isa<clang::CXXOperatorCallExpr>(call) &&
                    llvm::isa_and_nonnull<clang::CXXMethodDecl>(callee)
                ? 1
                : 0;
        for (unsigned i = 0; i < call->getNumArgs(); ++i)
          if (call->getArg(i)->IgnoreParens() == at) {
            const clang::ParmVarDecl *parameter =
                callee != nullptr && i >= shift &&
                        i - shift < callee->getNumParams()
                    ? callee->getParamDecl(i - shift)
                    : nullptr;
            if (parameter != nullptr && !pointer &&
                parameter->getType()->isLValueReferenceType() &&
                !parameter->getType()->getPointeeType().isConstQualified() &&
                CxxKinds::accessorParameter(*parameter) &&
                call->getType()->isReferenceType()) {
              at = call;
              break;
            }
            return passedConst(parameter, pointer);
          }
        if (at == call)
          continue;
        return false;
      }
      if (const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(up)) {
        const clang::CXXConstructorDecl *constructor =
            construct->getConstructor();
        for (unsigned i = 0; i < construct->getNumArgs(); ++i)
          if (construct->getArg(i)->IgnoreParens() == at)
            return passedConst(i < constructor->getNumParams()
                                   ? constructor->getParamDecl(i)
                                   : nullptr,
                               pointer);
        return false;
      }
      if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(up)) {
        if (unary->isIncrementDecrementOp())
          return false;
        if (unary->getOpcode() == clang::UO_Deref && pointer) {
          pointer = false;
        } else if (unary->getOpcode() == clang::UO_AddrOf && !pointer) {
          pointer = true;
        } else {
          return true;
        }
        at = unary;
        continue;
      }
      if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(up)) {
        if (binary->isAssignmentOp())
          return binary->getRHS()->IgnoreParens() == at &&
                 (!pointer ||
                  binary->getType()->getPointeeType().isConstQualified());
        if (binary->getOpcode() == clang::BO_Comma &&
            binary->getLHS()->IgnoreParens() == at)
          return true;
        if (binary->getOpcode() == clang::BO_Comma ||
            (pointer && binary->isAdditiveOp())) {
          at = binary;
          continue;
        }
        return true;
      }
      if (const auto *subscript =
              llvm::dyn_cast<clang::ArraySubscriptExpr>(up)) {
        if (!pointer || subscript->getBase()->IgnoreParens() != at)
          return true;
        pointer = false;
        at = subscript;
        continue;
      }
      if (const auto *conditional =
              llvm::dyn_cast<clang::AbstractConditionalOperator>(up)) {
        if (conditional->getCond()->IgnoreParens() == at)
          return true;
        at = conditional;
        continue;
      }
      if (llvm::isa<clang::MaterializeTemporaryExpr, clang::ExprWithCleanups,
                    clang::CXXBindTemporaryExpr>(up)) {
        at = llvm::cast<clang::Expr>(up);
        continue;
      }
      if (const auto *declaration = llvm::dyn_cast<clang::DeclStmt>(up))
        return declaredConst(*declaration, at, pointer);
      if (const auto *capturing = llvm::dyn_cast<clang::LambdaExpr>(up))
        return capturedConst(*capturing, at, depth);
      if (llvm::isa<clang::ReturnStmt, clang::InitListExpr,
                    clang::CXXParenListInitExpr>(up))
        return false;
      return !llvm::isa<clang::Expr>(up);
    }
    return false;
  }

  /// A member function called on the object: `const`, or an accessor whose
  /// result names a part of it, followed on.
  [[nodiscard]] bool calledConst(const clang::CXXMethodDecl &method,
                                 const clang::Stmt *call,
                                 const clang::Expr *object, bool pointer,
                                 unsigned depth) {
    if (method.isConst() || method.isStatic())
      return true;
    const auto *member = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(call);
    if (member == nullptr || !kinds.accessor(method))
      return false;
    const clang::QualType result = method.getReturnType();
    if (!result->isLValueReferenceType() && !result->isPointerType())
      return false;
    (void)object;
    return constUse(member, result->isPointerType() && !pointer, false, depth);
  }

  /// The same for an operator; whether to go on from the call (`at` and
  /// `pointer` updated), else the method decides.
  [[nodiscard]] bool calledConstStep(const clang::CXXMethodDecl &method,
                                     const clang::Expr *call,
                                     const clang::Expr *&at, bool &pointer) {
    if (method.isConst() || method.isStatic() || !kinds.accessor(method))
      return false;
    const clang::QualType result = method.getReturnType();
    if (!result->isLValueReferenceType() && !result->isPointerType())
      return false;
    at = call;
    pointer = result->isPointerType();
    return true;
  }

  [[nodiscard]] static bool passedConst(const clang::ParmVarDecl *parameter,
                                        bool pointer) {
    if (parameter == nullptr)
      return true;
    const clang::QualType type = parameter->getType();
    if (pointer)
      return type->isPointerType() ? type->getPointeeType().isConstQualified()
                                   : !type->isReferenceType();
    if (type->isRValueReferenceType())
      return false;
    if (type->isLValueReferenceType())
      return type->getPointeeType().isConstQualified();
    return true;
  }

  [[nodiscard]] bool declaredConst(const clang::DeclStmt &declaration,
                                   const clang::Expr *at, bool pointer) {
    // `for (const auto &x : v)` reads `v` through a hidden reference.
    if (const auto *loop = llvm::dyn_cast_or_null<clang::CXXForRangeStmt>(
            parents.getParent(&declaration));
        loop != nullptr && loop->getRangeStmt() == &declaration) {
      const clang::QualType type = loop->getLoopVariable()->getType();
      return !type->isReferenceType() ||
             type->getPointeeType().isConstQualified();
    }
    for (const clang::Decl *declared : declaration.decls()) {
      const auto *variable = llvm::dyn_cast<clang::VarDecl>(declared);
      if (variable == nullptr || variable->getInit() == nullptr ||
          variable->getInit()->IgnoreParens() != at)
        continue;
      const clang::QualType type = variable->getType();
      if (type->isReferenceType() || (pointer && type->isPointerType()))
        return type->getPointeeType().isConstQualified();
      return !pointer;
    }
    return false;
  }

  [[nodiscard]] bool capturedConst(const clang::LambdaExpr &capturing,
                                   const clang::Expr *at, unsigned depth) {
    const auto *init = capturing.capture_init_begin();
    for (const clang::LambdaCapture &capture : capturing.captures()) {
      const clang::Expr *expression = *init++;
      if (expression == nullptr || expression->IgnoreParens() != at)
        continue;
      if (capture.getCaptureKind() != clang::LCK_ByRef)
        return true;
      return capture.capturesVariable() && depth < 4 &&
             lambdaReads(capturing, *capture.getCapturedVar(), depth + 1);
    }
    return false;
  }

  /// §5: the lambda's body uses the variable it captured by reference only
  /// as `const`.
  [[nodiscard]] bool lambdaReads(const clang::LambdaExpr &capturing,
                                 const clang::ValueDecl &captured,
                                 unsigned depth = 0) {
    bool reads = true;
    const auto visit = [&](const clang::Stmt *statement,
                           const auto &self) -> void {
      if (statement == nullptr || !reads)
        return;
      if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement);
          named != nullptr && named->refersToEnclosingVariableOrCapture() &&
          named->getDecl()->getCanonicalDecl() == captured.getCanonicalDecl())
        reads = constUse(named, false, false, depth);
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(capturing.getBody(), visit);
    return reads;
  }

  /// A view or pointer type through which what it points to is only read.
  [[nodiscard]] static bool readOnlyView(clang::QualType type) {
    type = type.getNonReferenceType();
    if (type->isPointerType())
      return type->getPointeeType().isConstQualified();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || !record->hasDefinition())
      return false;
    bool found = false;
    for (const clang::CXXMethodDecl *method :
         record->getDefinition()->methods()) {
      const clang::OverloadedOperatorKind kind =
          method->getOverloadedOperator();
      if (kind != clang::OO_Star && kind != clang::OO_Subscript &&
          kind != clang::OO_Arrow &&
          !llvm::isa<clang::CXXConversionDecl>(method))
        continue;
      const clang::QualType result = method->getReturnType();
      if (!CxxKinds::pointerLike(result))
        continue;
      found = true;
      if (!result->getPointeeType().isConstQualified())
        return false;
    }
    return found;
  }

  /// §8: an expression that names (or moves) the function's own
  /// WEAVEC_SENDS parameter.
  [[nodiscard]] bool sendsParameter(const clang::Expr *expression) const {
    const clang::VarDecl *variable = movedName(expression);
    return sends(llvm::dyn_cast_or_null<clang::ParmVarDecl>(variable));
  }

  /// The variable an expression names through copies, moves and
  /// conversions (`std::move(task)`, `std::function<void()>(task)`).
  [[nodiscard]] static const clang::VarDecl *
  movedName(const clang::Expr *expression) {
    for (expression = strip(expression); expression != nullptr;) {
      const auto *construct =
          llvm::dyn_cast<clang::CXXConstructExpr>(expression);
      const auto *call = llvm::dyn_cast<clang::CallExpr>(expression);
      if (construct != nullptr && construct->getNumArgs() == 1)
        expression = strip(construct->getArg(0)->IgnoreParenImpCasts());
      else if (call != nullptr && passesThrough(*call))
        expression = strip(call->getArg(0));
      else
        break;
    }
    const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
        expression == nullptr ? nullptr : expression->IgnoreParenImpCasts());
    return named == nullptr ? nullptr : variableOf(*named);
  }

  [[nodiscard]] bool sends(const clang::ParmVarDecl *parameter) const {
    return parameter != nullptr &&
           hasAnnotation(annotationsOf(parameter->getType(), ast),
                         SendsAnnotation);
  }

  [[nodiscard]] std::string typeName(clang::QualType type) const {
    type = type.getNonReferenceType().getUnqualifiedType();
    std::string text = type.getAsString(ast.getPrintingPolicy());
    // The library's own aliases (`__libcpp_remove_reference_t<...>`) say
    // nothing to the reader.
    if (llvm::StringRef(text).contains("__"))
      text = type.getCanonicalType().getUnqualifiedType().getAsString(
          ast.getPrintingPolicy());
    return "'" + text + "'";
  }

  /// What a shared pointer of `type` points to, or `type`.
  [[nodiscard]] static clang::QualType subject(clang::QualType type) {
    type = type.getNonReferenceType();
    const auto *specialization =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            type->getAsCXXRecordDecl());
    if (specialization == nullptr || !CxxKinds::shared(specialization))
      return type;
    const llvm::SmallVector<clang::QualType, 4> arguments =
        CxxKinds::typeArguments(*specialization);
    return arguments.empty() ? type : arguments.front();
  }

  /// The note that says why `type` may not be shared (or sent).
  [[nodiscard]] std::string why(clang::QualType type, bool toShare) {
    const clang::QualType object = subject(type);
    const bool viaShared = object != type.getNonReferenceType();
    const std::string orConst =
        viaShared ? ", or share a 'std::shared_ptr<const " +
                        object.getUnqualifiedType().getAsString(
                            ast.getPrintingPolicy()) +
                        ">'"
                  : "";
    const clang::CXXRecordDecl *record = object->getAsCXXRecordDecl();
    if (record != nullptr && record->hasDefinition() &&
        kinds.lockedClass(*record).severalLocks)
      return typeName(object) +
             " has two lock members; give each lock and the members it "
             "guards a class of their own";
    if (const clang::FieldDecl *field =
            kinds.blocker(object, toShare || viaShared)) {
      const std::string name = "'" + field->getNameAsString() + "'";
      if (!toShare && !viaShared && CxxKinds::pointerLike(field->getType()))
        return name + " points to " +
               typeName(field->getType()->getPointeeType()) +
               ", which another thread could change at the same time; keep "
               "an owner or an index instead, or point to an object that is "
               "safe to share";
      return name + " is a member with no lock; give " + typeName(object) +
             " a 'std::mutex' member, make " + name +
             " 'const' or a 'std::atomic'" + orConst;
    }
    return typeName(object) +
           " is not synchronized; share an object of a class that locks its "
           "members, a 'std::atomic', or a 'std::shared_ptr<const T>'";
  }

  /// §1: the class of `method` lists it under `clause` in the table.
  [[nodiscard]] static bool listed(const clang::CXXMethodDecl &method,
                                   clang::QualType object,
                                   llvm::StringRef clause) {
    if (method.getIdentifier() == nullptr)
      return false;
    const auto has = [&](const clang::CXXRecordDecl *record) {
      return record != nullptr && isLibrary(*record) &&
             core::LibrarySpec::shipped().classMember(
                 CxxKinds::qualifiedName(*record), clause.str(),
                 method.getName().str());
    };
    if (!object.isNull() && object->isPointerType())
      object = object->getPointeeType();
    return has(object.isNull()
                   ? nullptr
                   : object.getNonReferenceType()->getAsCXXRecordDecl()) ||
           has(method.getParent());
  }

  /// Where a thread object a thread start makes goes.
  enum class Holder : std::uint8_t { Local, Member, Temporary, Elsewhere };
  struct Start {
    Holder kind = Holder::Elsewhere;
    const clang::VarDecl *local = nullptr;
    const clang::FieldDecl *member = nullptr;
    bool replaces = false;
  };

  [[nodiscard]] static bool contains(const clang::Stmt *statement,
                                     const clang::Stmt *inner) {
    if (statement == nullptr)
      return false;
    if (statement == inner)
      return true;
    return llvm::any_of(statement->children(), [&](const clang::Stmt *child) {
      return contains(child, inner);
    });
  }

  [[nodiscard]] Start holderFor(const clang::Expr *object, bool replaces) {
    object = object->IgnoreParenImpCasts();
    // An element of a container of thread objects (`threads[i] = ...`).
    if (const auto *element =
            llvm::dyn_cast<clang::CXXOperatorCallExpr>(object);
        element != nullptr && element->getNumArgs() != 0 &&
        element->getOperator() != clang::OO_Equal) {
      const Start whole = holderFor(element->getArg(0), false);
      if (whole.kind == Holder::Local)
        return whole;
    }
    if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(object))
      if (const clang::CXXMethodDecl *method = member->getMethodDecl();
          method != nullptr && kinds.accessor(*method)) {
        const Start whole =
            holderFor(member->getImplicitObjectArgument(), false);
        if (whole.kind == Holder::Local)
          return whole;
      }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(object))
      if (const auto *field =
              llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
          field != nullptr && llvm::isa<clang::CXXThisExpr>(
                                  member->getBase()->IgnoreParenImpCasts()))
        return {.kind = Holder::Member, .member = field};
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(object))
      if (const clang::VarDecl *variable = variableOf(*named);
          variable != nullptr && ownLocal(*variable) &&
          !variable->getType()->isReferenceType() &&
          kinds.holdsThread(variable->getType()))
        return {.kind = Holder::Local,
                .local = variable->getCanonicalDecl(),
                .replaces = replaces};
    return {};
  }

  /// §5: what holds the thread object `start` makes.
  [[nodiscard]] Start holderOf(const clang::Expr &start) {
    const clang::Stmt *at = &start;
    for (unsigned step = 0; step < 32; ++step) {
      const clang::Stmt *up = parents.getParent(at);
      if (up == nullptr) {
        if (const auto *constructor =
                llvm::dyn_cast<clang::CXXConstructorDecl>(&function))
          for (const clang::CXXCtorInitializer *initializer :
               constructor->inits())
            if (initializer->getMember() != nullptr &&
                contains(initializer->getInit(), &start))
              return {.kind = Holder::Member,
                      .member = initializer->getMember()};
        return {};
      }
      const auto *construct = llvm::dyn_cast<clang::CXXConstructExpr>(up);
      const auto *through = llvm::dyn_cast<clang::CallExpr>(up);
      if (llvm::isa<clang::MaterializeTemporaryExpr,
                    clang::CXXBindTemporaryExpr, clang::ExprWithCleanups,
                    clang::ImplicitCastExpr, clang::ParenExpr,
                    clang::CXXFunctionalCastExpr>(up) ||
          (construct != nullptr && construct->getNumArgs() == 1) ||
          (through != nullptr && passesThrough(*through))) {
        at = up;
        continue;
      }
      if (const auto *declaration = llvm::dyn_cast<clang::DeclStmt>(up)) {
        for (const clang::Decl *declared : declaration->decls())
          if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(declared);
              variable != nullptr && contains(variable->getInit(), &start))
            return variable->hasLocalStorage()
                       ? Start{.kind = Holder::Local,
                               .local = variable->getCanonicalDecl()}
                       : Start{};
        return {};
      }
      if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(up)) {
        const clang::Expr *object = member->getImplicitObjectArgument();
        if (contains(object, &start))
          return {.kind = Holder::Temporary};
        return holderFor(object, false);
      }
      if (const auto *operation =
              llvm::dyn_cast<clang::CXXOperatorCallExpr>(up);
          operation != nullptr && operation->getOperator() == clang::OO_Equal &&
          operation->getNumArgs() == 2 &&
          !contains(operation->getArg(0), &start))
        return holderFor(operation->getArg(0), true);
      return llvm::isa<clang::Expr>(up) ? Start{}
                                        : Start{.kind = Holder::Temporary};
    }
    return {};
  }

  /// A local that is immutable: `const` with a frozen type (§2).
  [[nodiscard]] bool immutable(const clang::VarDecl &variable) {
    return variable.getType().isConstQualified() &&
           kinds.frozen(variable.getType());
  }

  /// §3, rule 2: a borrow that crosses into a thread: of a shareable
  /// object, of an immutable one, or of a local, which is lent.
  void cross(Flow &flow, const Value &value, clang::QualType type, bool write,
             clang::SourceLocation at, const std::string &what,
             const Start &holder, const clang::VarDecl *slot = nullptr) {
    type = type.getNonReferenceType();
    const bool fine =
        kinds.shareable(type) ||
        (type.isConstQualified() && kinds.frozen(type) && value.loans.empty());
    const auto refuse = [&](const std::string &where) {
      error(at, rule::ThreadShare,
            what + " is shared with a thread, and " + typeName(type) +
                " is not safe to share",
            at, where.empty() ? why(type, true) : where);
    };
    if (value.reference && !fine) {
      refuse("it is reached through a pointer kept in memory; " +
             why(type, true));
      return;
    }
    for (const Loan &loan : value.loans) {
      if (loan.root.kind == RootKind::Temp || loan.root.kind == RootKind::Guard)
        continue;
      const clang::VarDecl *variable = loan.root.variable();
      if (variable != nullptr && localRoot(loan.root)) {
        const clang::QualType local =
            loan.root.field != nullptr && loan.depth == 0
                ? loan.root.field->getType()
                : variable->getType();
        if (kinds.shareable(local) || kinds.shareable(variable->getType()) ||
            immutable(*variable))
          continue;
        if (holder.kind == Holder::Local) {
          const Lend lend{.local = variable->getCanonicalDecl(),
                          .write = write || !kinds.frozen(variable->getType()),
                          .at = at,
                          .slot = slot};
          if (!llvm::is_contained(flow.lends, std::pair(holder.local, lend)))
            flow.lends.emplace_back(holder.local, lend);
        } else if (holder.kind == Holder::Elsewhere) {
          error(at, rule::Escape,
                what + " is lent to a thread object that is kept where the "
                       "rules stop following it",
                at,
                "keep the thread object in a local of this function, or a "
                "container local, and join it there; or capture by value");
          return;
        }
        continue;
      }
      if (loan.root.kind == RootKind::This) {
        const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
        const clang::QualType self =
            method != nullptr && !method->isStatic()
                ? method->getFunctionObjectParameterType()
                : type;
        if (holder.kind != Holder::Member && !kinds.shareable(self) &&
            !kinds.shareable(type)) {
          error(at, rule::ThreadShare,
                "'this' is shared with a thread, and " + typeName(self) +
                    " is not safe to share",
                at, why(self, true));
          return;
        }
        continue;
      }
      // What a shared pointer points to: shareable, or `const` and frozen.
      if (variable != nullptr && loan.depth > 0 &&
          CxxKinds::shared(variable->getType()->getAsCXXRecordDecl()) &&
          kinds.sendable(variable->getType()))
        continue;
      if (fine)
        continue;
      if (variable != nullptr && llvm::isa<clang::ParmVarDecl>(variable) &&
          !loan.root.beyond) {
        refuse("a parameter is not lent to a thread; share it as a "
               "'std::shared_ptr<const T>', or start the threads in the "
               "function that owns it");
        return;
      }
      refuse("");
      return;
    }
  }

  /// §3, rule 1: a value that crosses by copy.
  void crossCopy(Flow &flow, const clang::Expr *given, clang::QualType type,
                 clang::SourceLocation at, const std::string &what,
                 const Start &holder) {
    type = type.getNonReferenceType();
    if (type->isFunctionType() || type->isFunctionPointerType() ||
        type->isMemberPointerType())
      return;
    if (kinds.erased(type) && !kinds.view(type)) {
      if (sendsParameter(given) || kinds.sendable(type))
        return;
      // A local made from a function, or from a lambda, and never assigned:
      // what crosses is what it was made from.
      if (const std::optional<const clang::Expr *> made = madeFrom(given)) {
        if (*made != nullptr)
          crossCopy(flow, *made, (*made)->getType(), at, what, holder);
        return;
      }
      error(at, rule::ThreadSend,
            what + " is a " + typeName(type) +
                ", which does not say what it captured, and is handed to a "
                "thread",
            at,
            "pass the lambda itself, or take the callable as a WEAVEC_SENDS "
            "parameter, which says that callers give one that may be sent");
      return;
    }
    if (type->isPointerType() || kinds.view(type)) {
      const Value value =
          type->isPointerType() ? pointee(flow, given) : points(flow, given);
      const clang::QualType object =
          type->isPointerType() ? type->getPointeeType() : type;
      cross(flow, value, object,
            !readOnlyView(type) ||
                (type->isPointerType() && !kinds.frozen(object)),
            at, what, holder);
      return;
    }
    if (kinds.sendable(type))
      return;
    error(at, rule::ThreadSend,
          what + " is copied into a thread, and " + typeName(subject(type)) +
              " can be changed by two threads at once",
          at, why(type, false));
  }

  /// `expression` past parentheses and integer conversions that lose
  /// nothing (to a type at least as wide), which keep distinct values
  /// distinct.
  [[nodiscard]] const clang::Expr *
  widened(const clang::Expr *expression) const {
    for (;;) {
      expression = expression->IgnoreParens();
      const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression);
      if (cast == nullptr)
        return expression;
      const clang::QualType to = cast->getType();
      const clang::QualType from = cast->getSubExpr()->getType();
      const bool keeps = cast->getCastKind() == clang::CK_LValueToRValue ||
                         cast->getCastKind() == clang::CK_NoOp ||
                         (cast->getCastKind() == clang::CK_IntegralCast &&
                          to->isIntegerType() && from->isIntegerType() &&
                          ast.getTypeSize(to) >= ast.getTypeSize(from));
      if (!keeps)
        return expression;
      expression = cast->getSubExpr();
    }
  }

  /// The parent of `statement` past parentheses and implicit casts.
  [[nodiscard]] const clang::Stmt *above(const clang::Stmt *statement) const {
    const clang::Stmt *up = parents.getParent(statement);
    while (llvm::isa_and_nonnull<clang::ParenExpr, clang::ImplicitCastExpr>(up))
      up = parents.getParent(up);
    return up;
  }

  /// Amendment: a lambda that uses a local container only as `v[i]`, `i` a
  /// by-copy capture it does not change of the counter of the innermost
  /// loop around the thread start, which nothing else in the loop changes:
  /// each iteration lends a different element. The counter, or null.
  [[nodiscard]] const clang::VarDecl *
  slotOf(const clang::Expr &start, const clang::LambdaExpr &closure,
         const clang::ValueDecl &container) {
    const clang::VarDecl *counter = nullptr;
    bool only = true;
    const clang::ValueDecl *index = nullptr;
    const auto visit = [&](const clang::Stmt *statement,
                           const auto &self) -> void {
      if (statement == nullptr || !only)
        return;
      if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement);
          named != nullptr && named->getDecl()->getCanonicalDecl() ==
                                  container.getCanonicalDecl()) {
        const clang::Expr *at = named;
        const clang::Stmt *up = above(at);
        const clang::Expr *subscript = nullptr;
        if (const auto *operation =
                llvm::dyn_cast_or_null<clang::CXXOperatorCallExpr>(up);
            operation != nullptr &&
            operation->getOperator() == clang::OO_Subscript &&
            operation->getNumArgs() == 2) {
          const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
              operation->getDirectCallee());
          if (method != nullptr && kinds.accessor(*method) &&
              method->getReturnType()->isLValueReferenceType())
            subscript = operation->getArg(1);
        } else if (const auto *array =
                       llvm::dyn_cast_or_null<clang::ArraySubscriptExpr>(up)) {
          subscript = array->getIdx();
        }
        const auto *key = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
            subscript == nullptr ? nullptr : widened(subscript));
        if (key == nullptr || !key->refersToEnclosingVariableOrCapture() ||
            (index != nullptr && key->getDecl() != index)) {
          only = false;
          return;
        }
        index = key->getDecl();
        return;
      }
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(closure.getBody(), visit);
    if (!only || index == nullptr || !index->getType()->isIntegerType())
      return nullptr;
    // The index is a by-copy capture of a loop counter, unchanged in the
    // body.
    const auto *init = closure.capture_init_begin();
    for (const clang::LambdaCapture &capture : closure.captures()) {
      const clang::Expr *expression = *init++;
      if (!capture.capturesVariable() || capture.getCapturedVar() != index)
        continue;
      if (capture.getCaptureKind() != clang::LCK_ByCopy ||
          !lambdaReads(closure, *index))
        return nullptr;
      const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
          expression == nullptr ? nullptr : expression->IgnoreParenImpCasts());
      counter = named == nullptr ? nullptr : variableOf(*named);
    }
    if (counter == nullptr)
      return nullptr;
    const clang::Stmt *at = &start;
    for (const clang::Stmt *up = parents.getParent(at); up != nullptr;
         at = up, up = parents.getParent(up)) {
      if (llvm::isa<clang::WhileStmt, clang::DoStmt, clang::CXXForRangeStmt>(
              up))
        return nullptr;
      const auto *loop = llvm::dyn_cast<clang::ForStmt>(up);
      if (loop == nullptr)
        continue;
      if (at != loop->getBody())
        return nullptr;
      return countsBy(*loop, *counter) ? counter->getCanonicalDecl() : nullptr;
    }
    return nullptr;
  }

  /// Amendment: `[&mine = v[i]]`, an element of a local container at the
  /// counter of the innermost loop around the thread start.
  [[nodiscard]] std::optional<const clang::VarDecl *>
  elementSlot(const clang::Expr &start, const clang::Expr *init) {
    const auto *element =
        llvm::dyn_cast<clang::CXXOperatorCallExpr>(strip(init));
    const auto *method = element == nullptr
                             ? nullptr
                             : llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
                                   element->getDirectCallee());
    if (method == nullptr || element->getOperator() != clang::OO_Subscript ||
        element->getNumArgs() != 2 || !kinds.accessor(*method) ||
        !method->getReturnType()->isLValueReferenceType())
      return std::nullopt;
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(
        element->getArg(1)->IgnoreParenImpCasts());
    const clang::VarDecl *counter =
        named == nullptr ? nullptr : variableOf(*named);
    if (counter == nullptr)
      return std::nullopt;
    const clang::Stmt *at = &start;
    for (const clang::Stmt *up = parents.getParent(at); up != nullptr;
         at = up, up = parents.getParent(up)) {
      if (llvm::isa<clang::WhileStmt, clang::DoStmt, clang::CXXForRangeStmt>(
              up))
        return std::nullopt;
      if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(up))
        return at == loop->getBody() && countsBy(*loop, *counter)
                   ? std::optional(counter->getCanonicalDecl())
                   : std::nullopt;
    }
    return std::nullopt;
  }

  /// The loop's counter is declared in its init, stepped only by its
  /// increment (`++i`, `i++`, `i += n`) and only read anywhere else in it.
  [[nodiscard]] bool countsBy(const clang::ForStmt &loop,
                              const clang::VarDecl &counter) {
    const auto *declaration =
        llvm::dyn_cast_or_null<clang::DeclStmt>(loop.getInit());
    if (declaration == nullptr ||
        !llvm::is_contained(declaration->decls(), &counter))
      return false;
    const clang::Expr *increment = loop.getInc();
    const clang::Expr *stepped = nullptr;
    if (const auto *unary = llvm::dyn_cast_or_null<clang::UnaryOperator>(
            increment == nullptr ? nullptr : increment->IgnoreParens());
        unary != nullptr && unary->isIncrementOp())
      stepped = unary->getSubExpr();
    else if (const auto *compound =
                 llvm::dyn_cast_or_null<clang::CompoundAssignOperator>(
                     increment == nullptr ? nullptr
                                          : increment->IgnoreParens());
             compound != nullptr &&
             compound->getOpcode() == clang::BO_AddAssign)
      stepped = compound->getLHS();
    const auto *named = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
        stepped == nullptr ? nullptr : stepped->IgnoreParenImpCasts());
    if (named == nullptr || named->getDecl() != &counter)
      return false;
    bool reads = true;
    const auto visit = [&](const clang::Stmt *statement,
                           const auto &self) -> void {
      if (statement == nullptr || !reads)
        return;
      if (const auto *use = llvm::dyn_cast<clang::DeclRefExpr>(statement);
          use != nullptr && use->getDecl() == &counter &&
          !llvm::isa_and_nonnull<clang::LambdaExpr>(above(use)))
        reads = constUse(use);
      for (const clang::Stmt *child : statement->children())
        self(child, self);
    };
    visit(loop.getCond(), visit);
    visit(loop.getBody(), visit);
    return reads;
  }

  /// §8, amendment: the callable an erased local was made from (null for a
  /// function), when it is never assigned again.
  [[nodiscard]] std::optional<const clang::Expr *>
  madeFrom(const clang::Expr *given) {
    const clang::VarDecl *variable = movedName(given);
    if (variable == nullptr || !ownLocal(*variable) ||
        llvm::isa<clang::ParmVarDecl>(variable) ||
        assignedLocals.contains(variable->getCanonicalDecl()))
      return std::nullopt;
    const auto *construct = llvm::dyn_cast_or_null<clang::CXXConstructExpr>(
        strip(variable->getInit()));
    if (construct == nullptr || construct->getNumArgs() != 1)
      return std::nullopt;
    const clang::Expr *source = construct->getArg(0)->IgnoreParenImpCasts();
    if (const auto *callee = llvm::dyn_cast<clang::DeclRefExpr>(source);
        callee != nullptr && llvm::isa<clang::FunctionDecl>(callee->getDecl()))
      return nullptr;
    if (lambdaOf(source) != nullptr)
      return source;
    return std::nullopt;
  }

  /// §3: what a thread start gives the thread it starts.
  void threadStart(Flow &flow, const clang::Expr &start,
                   llvm::ArrayRef<const clang::Expr *> crossing) {
    // A container that makes the thread object from its arguments holds it.
    const auto *container = llvm::dyn_cast<clang::CXXMemberCallExpr>(&start);
    const Start holder =
        container != nullptr
            ? holderFor(container->getImplicitObjectArgument(), false)
            : holderOf(start);
    if (holder.kind == Holder::Local && holder.replaces)
      llvm::erase_if(flow.lends, [&](const auto &entry) {
        return entry.first == holder.local;
      });
    if (holder.kind == Holder::Member)
      threadMember(start, *holder.member);
    for (const clang::Expr *given : crossing) {
      const clang::LambdaExpr *closure = lambdaOf(given);
      if (closure == nullptr) {
        crossCopy(flow, given, given->getType(), given->getExprLoc(),
                  spell(given, ast), holder);
        continue;
      }
      const auto *init = closure->capture_init_begin();
      for (const clang::LambdaCapture &capture : closure->captures()) {
        const clang::Expr *expression = *init++;
        const clang::SourceLocation at = capture.getLocation().isValid()
                                             ? capture.getLocation()
                                             : closure->getBeginLoc();
        if (capture.capturesThis()) {
          const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
          if (method == nullptr || method->isStatic())
            continue;
          const clang::QualType self = method->getFunctionObjectParameterType();
          if (capture.getCaptureKind() == clang::LCK_StarThis)
            crossCopy(flow, nullptr, self, at, "'*this'", holder);
          else if (holder.kind != Holder::Member && !kinds.shareable(self))
            error(at, rule::ThreadShare,
                  "'this' is shared with a thread, and " + typeName(self) +
                      " is not safe to share",
                  at, why(self, true));
          continue;
        }
        if (!capture.capturesVariable() || expression == nullptr)
          continue;
        const clang::ValueDecl *variable = capture.getCapturedVar();
        const std::string what = "'" + variable->getNameAsString() + "'";
        if (capture.getCaptureKind() == clang::LCK_ByRef) {
          const clang::QualType type =
              variable->getType().getNonReferenceType();
          const clang::VarDecl *slot =
              elementSlot(start, expression)
                  .value_or(slotOf(start, *closure, *variable));
          // A slot is lent again by the same expression in the loop's next
          // iteration: its loan is where that expression begins.
          cross(flow, object(flow, expression), type,
                !(lambdaReads(*closure, *variable) && kinds.frozen(type)),
                slot != nullptr ? expression->getBeginLoc() : at, what, holder,
                slot);
          continue;
        }
        crossCopy(flow, expression, variable->getType(), at, what, holder);
      }
    }
  }

  /// §7: a thread member started with `this`.
  void threadMember(const clang::Expr &start, const clang::FieldDecl &member) {
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    if (method == nullptr)
      return;
    const clang::CXXRecordDecl *record = method->getParent();
    if (record == nullptr || !record->hasDefinition())
      return;
    record = record->getDefinition();
    const std::string name = "'" + member.getNameAsString() + "'";
    std::string fault;
    clang::SourceLocation noteAt = member.getLocation();
    const clang::QualType self = method->getFunctionObjectParameterType();
    const clang::FieldDecl *later = nullptr;
    for (const clang::FieldDecl *field : record->fields())
      if (field->getFieldIndex() > member.getFieldIndex() &&
          !kinds.holdsThread(field->getType()))
        later = field;
    // The implicit members Clang has not declared yet count too.
    const bool copies =
        (record->needsImplicitCopyConstructor() &&
         !record->defaultedCopyConstructorIsDeleted()) ||
        (record->needsImplicitMoveConstructor() &&
         !record->defaultedMoveConstructorIsDeleted()) ||
        llvm::any_of(record->ctors(),
                     [](const clang::CXXConstructorDecl *constructor) {
                       return constructor->isCopyOrMoveConstructor() &&
                              !constructor->isDeleted();
                     });
    const bool assigns =
        llvm::any_of(record->methods(), [](const clang::CXXMethodDecl *other) {
          return (other->isCopyAssignmentOperator() ||
                  other->isMoveAssignmentOperator()) &&
                 !other->isDeleted() && !other->isImplicit();
        });
    if (later != nullptr) {
      fault = "the thread in " + name + " uses 'this', and '" +
              later->getNameAsString() + "' is destroyed before it is joined";
      noteAt = later->getLocation();
      error(start.getExprLoc(), rule::Escape, fault, noteAt,
            "declare " + name + " after every other member");
      return;
    }
    // Without virtual functions the thread reaches only this class's own
    // members, which a derived class's destructor leaves alone.
    if (record->isPolymorphic() && !record->hasAttr<clang::FinalAttr>()) {
      error(start.getExprLoc(), rule::Escape,
            "the thread in " + name +
                " uses 'this', and a class derived from " + typeName(self) +
                " would destroy its members before it is joined",
            record->getLocation(), "declare the class 'final'");
      return;
    }
    if (copies || assigns) {
      error(start.getExprLoc(), rule::Escape,
            "the thread in " + name + " uses 'this', and " + typeName(self) +
                " can be copied or moved away from it",
            record->getLocation(),
            "delete the copy constructor and the copy assignment: '" +
                record->getNameAsString() + "(const " +
                record->getNameAsString() + " &) = delete;'");
      return;
    }
    if (!kinds.shareable(self))
      error(start.getExprLoc(), rule::ThreadShare,
            "'this' is shared with the thread in " + name + ", and " +
                typeName(self) + " is not safe to share",
            noteAt, why(self, true));
  }

  /// §5: a use of a local against a loan a live thread object holds.
  void lentUse(const Flow &flow, const clang::VarDecl &variable,
               const clang::Expr &named, bool pointerBorrow,
               std::optional<bool> reads = std::nullopt) {
    for (const auto &[holder, lend] : flow.lends) {
      if (lend.local != variable.getCanonicalDecl())
        continue;
      if (!lend.write && reads.value_or(constUse(&named, false, pointerBorrow)))
        continue;
      // The same capture, lending the next element in the loop's next
      // iteration.
      if (lend.slot != nullptr && named.getBeginLoc() == lend.at)
        continue;
      if (!emit ||
          !reportedLends.emplace(&variable, lend.at.getRawEncoding()).second)
        return;
      const std::string name = "'" + variable.getNameAsString() + "'";
      const std::string thread = "'" + holder->getNameAsString() + "'";
      std::string message = name;
      message += lend.write ? " is used while the thread "
                            : " is changed while the thread ";
      message += thread;
      message += lend.write ? " may be changing it" : " reads it";
      std::string note = thread;
      note += " was given ";
      note += name;
      note += " by reference here; join ";
      note += thread;
      note += " or end its scope first, or give each thread its own data and "
              "collect the results";
      report(named.getExprLoc(), rule::ThreadLent, message, lend.at, note);
      return;
    }
  }
  std::set<std::pair<const clang::VarDecl *, unsigned>> reportedLends;

  /// The thread start an expression is, with what it gives the thread.
  [[nodiscard]] std::optional<llvm::SmallVector<const clang::Expr *, 4>>
  startOf(const clang::Expr &expression) {
    llvm::SmallVector<const clang::Expr *, 4> crossing;
    if (const auto *construct =
            llvm::dyn_cast<clang::CXXConstructExpr>(&expression)) {
      if (!CxxKinds::threadClass(construct->getType()) ||
          construct->getNumArgs() == 0 ||
          CxxKinds::threadClass(construct->getArg(0)->getType()))
        return std::nullopt;
      for (const clang::Expr *argument : construct->arguments())
        crossing.push_back(argument);
      return crossing;
    }
    const auto *call = llvm::dyn_cast<clang::CallExpr>(&expression);
    const clang::FunctionDecl *callee =
        call == nullptr ? nullptr : call->getDirectCallee();
    if (callee == nullptr)
      return std::nullopt;
    if (isLibrary(*callee) && !llvm::isa<clang::CXXMethodDecl>(callee) &&
        core::LibrarySpec::shipped().spawnsFunction(
            CxxKinds::qualifiedName(*callee))) {
      for (const clang::Expr *argument : call->arguments())
        if (!argument->getType()->isEnumeralType())
          crossing.push_back(argument);
      return crossing;
    }
    // A container of thread objects that makes one from its arguments
    // (`emplace_back`).
    const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call);
    const clang::CXXMethodDecl *method =
        member == nullptr ? nullptr : member->getMethodDecl();
    if (method == nullptr || method->isConst() || call->getNumArgs() == 0 ||
        !isLibrary(*method) ||
        CxxKinds::threadClass(member->getImplicitObjectArgument()->getType()) ||
        !kinds.holdsThread(member->getImplicitObjectArgument()->getType()) ||
        kinds.holdsThread(call->getArg(0)->getType()))
      return std::nullopt;
    for (const clang::Expr *argument : call->arguments())
      crossing.push_back(argument);
    return crossing;
  }

  /// §5: the loans of the thread object `from` names move to `to`.
  static void moveLends(Flow &flow, const clang::Expr *from,
                        const clang::VarDecl *to) {
    const clang::VarDecl *variable = movedName(from);
    if (variable == nullptr || to == nullptr)
      return;
    variable = variable->getCanonicalDecl();
    for (auto &[holder, lend] : flow.lends)
      if (holder == variable)
        holder = to;
  }

  //===--------------------------------------------------------------------===//
  // RFC 0046 §6: guards and locked members
  //===--------------------------------------------------------------------===//

  /// The object a locked member is reached through: `this` (null), or a
  /// variable named directly; nothing for any other expression.
  [[nodiscard]] static std::optional<const clang::VarDecl *>
  lockedObject(const clang::Expr *base) {
    base = base->IgnoreParenImpCasts();
    if (llvm::isa<clang::CXXThisExpr>(base))
      return nullptr;
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(base))
      if (const clang::VarDecl *variable = variableOf(*named))
        return variable->getCanonicalDecl();
    return std::nullopt;
  }

  /// The lock member a guard is constructed from, and of which object.
  [[nodiscard]] static std::optional<HeldGuard>
  guardOn(const clang::Expr *argument) {
    const auto *member =
        llvm::dyn_cast<clang::MemberExpr>(argument->IgnoreParenImpCasts());
    const auto *field =
        member == nullptr
            ? nullptr
            : llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr || !CxxKinds::lockClass(field->getType()))
      return std::nullopt;
    const std::optional<const clang::VarDecl *> object =
        lockedObject(member->getBase());
    if (!object.has_value())
      return std::nullopt;
    return HeldGuard{.object = *object, .lock = field};
  }

  /// A guard local constructed where it is declared.
  void declareGuard(Flow &flow, const clang::VarDecl &variable) {
    const auto *construct = llvm::dyn_cast_or_null<clang::CXXConstructExpr>(
        strip(variable.getInit()));
    if (construct == nullptr)
      return;
    const bool shared = CxxKinds::tableIs(variable.getType(), "guard(shared)");
    for (const clang::Expr *argument : construct->arguments()) {
      if (llvm::isa<clang::CXXDefaultArgExpr>(argument))
        continue;
      if (!CxxKinds::lockClass(argument->getType())) {
        error(argument->getExprLoc(), rule::NotModeled,
              "a guard constructed with a tag or from another guard is not "
              "followed by the rules",
              argument->getExprLoc(),
              "construct the guard from the lock alone ('std::unique_lock "
              "lock(mu_);'), or keep this function WEAVEC_GUARDED");
        return;
      }
      if (std::optional<HeldGuard> guard = guardOn(argument)) {
        guard->guard = variable.getCanonicalDecl();
        guard->shared = shared;
        flow.guards.push_back(*guard);
        guardLocks[guard->guard].push_back(*guard);
      }
    }
  }

  /// What each guard local was constructed on, to take again (`locks`).
  llvm::DenseMap<const clang::VarDecl *, llvm::SmallVector<HeldGuard, 1>>
      guardLocks;

  /// §6: a guard that ends, or gives its lock away for a while, ends the
  /// borrows taken under it.
  static void unlock(Flow &flow, const clang::VarDecl *guard,
                     clang::SourceLocation at, bool ends) {
    Value value;
    value.loans.push_back(
        {.root = {.kind = RootKind::Guard, .base = guard}, .depth = 0});
    invalidate(flow, value, Ends::All, End::Why::Unlocked, at, nullptr);
    if (ends)
      llvm::erase_if(flow.guards, [&](const HeldGuard &held) {
        return held.guard == guard;
      });
  }

  /// The guards held on `object`'s lock `lock`.
  [[nodiscard]] static llvm::SmallVector<HeldGuard, 2>
  heldOn(const Flow &flow, const clang::VarDecl *object,
         const clang::FieldDecl *lock) {
    llvm::SmallVector<HeldGuard, 2> held;
    for (const HeldGuard &guard : flow.guards)
      if (guard.object == object && guard.lock == lock)
        held.push_back(guard);
    return held;
  }

  /// A locked member, or a thread member, of a class with such members.
  [[nodiscard]] const LockedClass *lockedMember(const clang::FieldDecl &field) {
    const auto *record =
        llvm::dyn_cast<clang::CXXRecordDecl>(field.getParent());
    if (record == nullptr || record->isLambda() || isLibrary(*record))
      return nullptr;
    const LockedClass &info = kinds.lockedClass(*record);
    if (info.locked.contains(&field) || info.threads.contains(&field))
      return &info;
    return nullptr;
  }

  /// §6, the rule: a locked member used where no guard on its object's lock
  /// is held.
  void lockedUse(const Flow &flow, const clang::MemberExpr &member) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member.getMemberDecl());
    const LockedClass *info = field == nullptr ? nullptr : lockedMember(*field);
    if (info == nullptr || !emit)
      return;
    const std::optional<const clang::VarDecl *> object =
        lockedObject(member.getBase());
    const bool thread = info->threads.contains(field);
    // A constructor and a destructor run where no other thread has the
    // object, unless its own thread may already run; member initializers
    // run before it starts.
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    if (object.has_value() && *object == nullptr && method != nullptr &&
        llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(
            method) &&
        (info->threads.empty() || thread || !parents.hasParent(&member)))
      return;
    const std::string name = "'" + field->getNameAsString() + "'";
    const std::string lock =
        info->lock == nullptr ? "" : "'" + info->lock->getNameAsString() + "'";
    if (!reportedLocks.insert(member.getMemberLoc().getRawEncoding()).second)
      return;
    if (!object.has_value()) {
      report(member.getMemberLoc(), rule::LockHeld,
             name + " is reached through an expression the rules do not "
                    "follow to its lock",
             member.getMemberLoc(),
             "name the object first ('auto &queue = *owner;'), then take "
             "its lock with a guard");
      return;
    }
    const llvm::SmallVector<HeldGuard, 2> held =
        info->lock == nullptr ? llvm::SmallVector<HeldGuard, 2>()
                              : heldOn(flow, *object, info->lock);
    if (held.empty()) {
      report(member.getMemberLoc(), rule::LockHeld,
             info->lock == nullptr
                 ? name + " is a thread member, used outside the constructor "
                          "and the destructor of a class with no lock"
                 : name + " is used without a lock on " + lock,
             member.getMemberLoc(),
             info->lock == nullptr
                 ? "give the class a 'std::mutex' member, and use " + name +
                       " under a guard on it"
                 : "hold one for as long as " + name +
                       " or a reference into it is used: 'std::lock_guard "
                       "lock(" +
                       lock.substr(1, lock.size() - 2) + ");'");
      return;
    }
    if (llvm::any_of(held,
                     [](const HeldGuard &guard) { return !guard.shared; }))
      return;
    if (!constUse(&member) || !kinds.frozen(field->getType()))
      report(member.getMemberLoc(), rule::LockHeld,
             name + " is changed under a shared lock on " + lock +
                 ", which other readers hold too",
             member.getMemberLoc(),
             "change it under an exclusive guard ('std::unique_lock "
             "lock(" +
                 lock.substr(1, lock.size() - 2) + ");')");
  }
  std::set<unsigned> reportedLocks;

  /// §6: the guards a member of a locked class finds held at its start (a
  /// guard parameter), and those a lambda inherits from where it is
  /// written.
  void entryGuards(Flow &flow) {
    if (writtenAs != nullptr) {
      if (const auto found = kinds.lambdaGuards.find(writtenAs);
          found != kinds.lambdaGuards.end())
        flow.guards = found->second;
      return;
    }
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    if (method == nullptr || method->isStatic())
      return;
    const LockedClass &info = kinds.lockedClass(*method->getParent());
    if (info.lock == nullptr)
      return;
    for (const clang::ParmVarDecl *parameter : function.parameters())
      if (parameter->getType()->isLValueReferenceType() &&
          CxxKinds::guardClass(parameter->getType()))
        flow.guards.push_back({.guard = parameter->getCanonicalDecl(),
                               .object = nullptr,
                               .lock = info.lock,
                               .shared = CxxKinds::tableIs(parameter->getType(),
                                                           "guard(shared)")});
  }

  /// §6 at a call: locks taken by hand, guards used as the rules do not
  /// follow, guards handed to helpers, and what a lambda inherits.
  void lockCall(Flow &flow, const Call &call, const clang::Expr &expression) {
    const bool library = call.callee != nullptr && isLibrary(*call.callee);
    if (call.object != nullptr && call.method != nullptr &&
        !call.method->isConst() && !call.method->isStatic() &&
        !llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(
            call.method)) {
      const clang::QualType type = objectType(call);
      if (CxxKinds::lockClass(type))
        error(call.at, rule::LockHeld,
              "'" + call.method->getNameAsString() +
                  "' takes or releases a lock by hand",
              call.at,
              "take the lock with a guard object, which releases it on every "
              "path: 'std::lock_guard lock(mu);'");
      if (CxxKinds::guardClass(type))
        if (const clang::VarDecl *guard = guardNamed(call.object)) {
          if (listed(*call.method, type, "resets")) {
            if (llvm::isa<clang::ParmVarDecl>(guard))
              error(call.at, rule::LockHeld,
                    "a helper releases the guard it was given", call.at,
                    "release the lock in the function that took it");
            unlock(flow, guard, call.at, /*ends=*/true);
          } else if (listed(*call.method, type, "locks") &&
                     guardLocks.contains(guard)) {
            // The guard takes its lock again; borrows taken before stay
            // ended.
            for (const HeldGuard &held : guardLocks[guard])
              if (!llvm::is_contained(flow.guards, held))
                flow.guards.push_back(held);
          } else {
            error(call.at, rule::NotModeled,
                  "'" + call.method->getNameAsString() +
                      "' on a guard is not followed by the rules",
                  call.at,
                  "hold the lock for a scope with one guard, and end the "
                  "scope to release it");
          }
        }
    }
    for (const Call::Argument &argument : call.arguments) {
      const clang::QualType given = argument.expression->getType();
      const clang::QualType parameter = argument.parameter == nullptr
                                            ? clang::QualType()
                                            : argument.parameter->getType();
      if (CxxKinds::lockClass(given) &&
          !CxxKinds::guardClass(expression.getType()) &&
          !(parameter.isNull() ||
            (parameter->isReferenceType() &&
             parameter->getPointeeType().isConstQualified())))
        error(argument.expression->getExprLoc(), rule::NotModeled,
              "a lock handed to " + called(call.callee) +
                  " is not followed by the rules",
              argument.expression->getExprLoc(),
              "take each lock with a guard; 'std::scoped_lock both(a.mu_, "
              "b.mu_);' takes two");
      if (!CxxKinds::guardClass(given))
        continue;
      const clang::VarDecl *guard = guardNamed(argument.expression);
      if (guard == nullptr)
        continue;
      if (parameter.isNull() || !parameter->isLValueReferenceType()) {
        error(argument.expression->getExprLoc(), rule::NotModeled,
              "a guard that is moved or copied is not followed by the rules",
              argument.expression->getExprLoc(),
              "pass the guard by reference ('std::unique_lock<std::mutex> "
              "&lock')");
        continue;
      }
      if (parameter->getPointeeType().isConstQualified())
        continue;
      // A guard passed to be given away for a while (a condition
      // variable's `wait`) ends the borrows taken under it.
      unlock(flow, guard, argument.expression->getExprLoc(), /*ends=*/false);
      if (library || call.method == nullptr)
        continue;
      // Helpers take the guard: one held on the lock of the object the
      // helper is called on.
      const LockedClass &info = kinds.lockedClass(*call.method->getParent());
      std::optional<const clang::VarDecl *> object =
          call.object == nullptr
              ? std::optional<const clang::VarDecl *>(nullptr)
              : lockedObject(call.object);
      if (info.lock == nullptr || !object.has_value() ||
          llvm::none_of(
              heldOn(flow, *object, info.lock),
              [&](const HeldGuard &held) { return held.guard == guard; }))
        error(argument.expression->getExprLoc(), rule::LockHeld,
              "the guard passed to " + called(call.callee) +
                  " is not held on the lock of the object it is called on",
              argument.expression->getExprLoc(),
              "pass a guard on that object's lock, taken in this function");
    }
    // Lambdas run where they are written, with the guards held there.
    for (const Call::Argument &argument : call.arguments) {
      const clang::LambdaExpr *written = lambdaInPlace(argument.expression);
      if (written == nullptr || keeps(argument.parameter) ||
          sends(argument.parameter))
        continue;
      kinds.lambdaGuards[written] = flow.guards;
    }
  }

  /// A guard local or parameter named directly.
  [[nodiscard]] static const clang::VarDecl *
  guardNamed(const clang::Expr *expression) {
    const auto *named =
        llvm::dyn_cast<clang::DeclRefExpr>(expression->IgnoreParenImpCasts());
    const clang::VarDecl *variable =
        named == nullptr ? nullptr : variableOf(*named);
    return variable == nullptr ? nullptr : variable->getCanonicalDecl();
  }

  /// A lambda written in place as an expression (not through a local).
  [[nodiscard]] static const clang::LambdaExpr *
  lambdaInPlace(const clang::Expr *expression) {
    expression = strip(expression);
    while (expression != nullptr) {
      if (const auto *written = llvm::dyn_cast<clang::LambdaExpr>(expression))
        return written;
      if (const auto *temporary =
              llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
        expression = strip(temporary->getSubExpr());
      else if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression))
        expression = strip(cast->getSubExpr());
      else
        return nullptr;
    }
    return nullptr;
  }

  /// RFC 0046 at a call: thread starts, joins and detaches, locks and
  /// guards, WEAVEC_SENDS arguments.
  void threads(Flow &flow, const Call &call, const clang::Expr &expression) {
    if (const std::optional<llvm::SmallVector<const clang::Expr *, 4>>
            crossing = startOf(expression))
      threadStart(flow, expression, *crossing);
    lockCall(flow, call, expression);
    // §2 (D4): what an owning pointer reached through a `const` path holds
    // is used as `const`.
    if (call.object != nullptr && call.method != nullptr &&
        call.method->isConst() &&
        CxxKinds::libraryOwner(call.method->getParent()) &&
        call.object->IgnoreParenImpCasts()->getType().isConstQualified()) {
      const clang::QualType result = call.method->getReturnType();
      if (CxxKinds::pointerLike(result) &&
          !result->getPointeeType().isConstQualified() &&
          !constUse(&expression, result->isPointerType()) && emit &&
          constPaths
              .insert(ast.getSourceManager().getExpansionLineNumber(call.at))
              .second)
        error(call.at, rule::OwnerPath,
              spell(call.object, ast) +
                  " is 'const' here, and what it points to is written",
              call.at,
              "write it in a member function that is not 'const', or keep "
              "what changes in a 'std::atomic' or behind a lock");
    }
    if (call.object != nullptr && call.method != nullptr) {
      const clang::QualType type = objectType(call);
      const clang::VarDecl *holder = viewLocal(call.object);
      if (listed(*call.method, type, "joins") && holder != nullptr) {
        llvm::erase_if(flow.lends, [&](const auto &entry) {
          return entry.first == holder;
        });
        flow.borrows[holder].loans.clear();
      } else if (listed(*call.method, type, "detaches")) {
        // §5: a detached thread outlives everything it borrowed.
        const Value held = points(flow, call.object);
        if (!held.loans.empty())
          error(call.at, rule::Escape,
                "the thread " + spell(call.object, ast) +
                    " is detached while it holds a reference to " +
                    nameOf(held.loans.front().root),
                call.at,
                "capture by value what the thread needs, or keep the thread "
                "object and join it");
      } else if (holder != nullptr && kinds.holdsThread(type) &&
                 (listed(*call.method, type, "resets") ||
                  call.method->getOverloadedOperator() == clang::OO_Equal)) {
        if (call.method->getOverloadedOperator() == clang::OO_Equal &&
            !call.arguments.empty() &&
            startOf(*strip(call.arguments.front().expression)) == std::nullopt)
          llvm::erase_if(flow.lends, [&](const auto &entry) {
            return entry.first == holder;
          });
        if (call.method->getOverloadedOperator() == clang::OO_Equal &&
            !call.arguments.empty())
          moveLends(flow, call.arguments.front().expression, holder);
        else
          llvm::erase_if(flow.lends, [&](const auto &entry) {
            return entry.first == holder;
          });
      } else if (holder != nullptr && kinds.holdsThread(type) &&
                 !call.method->isConst()) {
        for (const Call::Argument &argument : call.arguments)
          if (kinds.holdsThread(argument.expression->getType()))
            moveLends(flow, argument.expression, holder);
      }
    }
    // §8: what a WEAVEC_SENDS parameter is given, and what an erased
    // member of a shareable class is.
    const bool erasedMember =
        call.object != nullptr && call.method != nullptr &&
        !call.method->isConst() && erasedLocked(call.object);
    for (const Call::Argument &argument : call.arguments)
      if ((sends(argument.parameter) ||
           (erasedMember && (kinds.erased(argument.expression->getType()) ||
                             lambdaOf(argument.expression) != nullptr))) &&
          !sendsParameter(argument.expression))
        sendsArgument(flow, argument,
                      sends(argument.parameter) ? nullptr : call.object);
  }

  /// §8: a locked member of erased type (or holding erased values), which
  /// other threads may take a callable from.
  [[nodiscard]] bool erasedLocked(const clang::Expr *object) {
    const auto *member =
        llvm::dyn_cast<clang::MemberExpr>(object->IgnoreParenImpCasts());
    const auto *field =
        member == nullptr
            ? nullptr
            : llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    const LockedClass *info = field == nullptr ? nullptr : lockedMember(*field);
    return info != nullptr && info->locked.contains(field) &&
           kinds.erased(field->getType());
  }

  /// §8: the argument for a WEAVEC_SENDS parameter is a callable of
  /// sendable type that is no view.
  void sendsArgument(Flow &flow, const Call::Argument &argument,
                     const clang::Expr *member = nullptr) {
    const clang::Expr *given = argument.expression;
    const clang::LambdaExpr *closure = lambdaOf(given);
    if (closure == nullptr) {
      const clang::QualType type = given->getType().getNonReferenceType();
      if (type->isFunctionPointerType() || type->isFunctionType())
        return;
      if (member != nullptr)
        error(given->getExprLoc(), rule::ThreadSend,
              spell(given, ast) + " is stored in " + spell(member, ast) +
                  ", which another thread may run, and what it captured is "
                  "not in its type",
              given->getExprLoc(),
              "take the callable as a WEAVEC_SENDS parameter "
              "('std::function<void()> WEAVEC_SENDS task'), so that callers "
              "give one that captures by value");
      else
        error(given->getExprLoc(), rule::ThreadSend,
              spell(given, ast) +
                  " is handed to a WEAVEC_SENDS parameter, and what it "
                  "captured is not in its type",
              given->getExprLoc(),
              "pass a lambda written there that captures by value, or the "
              "function's own WEAVEC_SENDS parameter");
      return;
    }
    const auto *init = closure->capture_init_begin();
    for (const clang::LambdaCapture &capture : closure->captures()) {
      const clang::Expr *expression = *init++;
      const clang::SourceLocation at = capture.getLocation().isValid()
                                           ? capture.getLocation()
                                           : closure->getBeginLoc();
      if (capture.capturesThis() ||
          capture.getCaptureKind() == clang::LCK_ByRef) {
        error(
            at, rule::ThreadSend,
            std::string(
                capture.capturesThis()
                    ? "'this'"
                    : "'" + capture.getCapturedVar()->getNameAsString() + "'") +
                " is captured by reference by a callable handed to a "
                "WEAVEC_SENDS parameter, which may run it on another "
                "thread after this function returns",
            at,
            "capture by value what the callable needs ('[name]', '[p = "
            "std::move(p)]'), or a 'std::shared_ptr' to an object that is "
            "safe to share");
        return;
      }
      if (!capture.capturesVariable() || expression == nullptr)
        continue;
      const clang::QualType type = capture.getCapturedVar()->getType();
      if (type->isPointerType() || kinds.view(type) ||
          (kinds.erased(type) && !sendsParameter(expression)) ||
          !kinds.sendable(type)) {
        crossCopy(flow, expression, type, at,
                  "'" + capture.getCapturedVar()->getNameAsString() + "'",
                  Start{});
        if (type->isPointerType() || kinds.view(type))
          error(at, rule::ThreadSend,
                "'" + capture.getCapturedVar()->getNameAsString() +
                    "' is a borrow, copied into a callable handed to a "
                    "WEAVEC_SENDS parameter",
                at, "capture what owns its data, or a 'std::shared_ptr'");
        return;
      }
    }
  }

  /// RFC 0046: a guard that ends releases its lock; a thread object that
  /// ends joins its thread, which is a use of what it borrowed (Z5).
  void ended(Flow &flow, const clang::VarDecl &variable,
             clang::SourceLocation at) {
    if (CxxKinds::guardClass(variable.getType()))
      unlock(flow, &variable, at, /*ends=*/true);
    if (!kinds.holdsThread(variable.getType()))
      return;
    if (const auto found = flow.borrows.find(&variable);
        found != flow.borrows.end() &&
        found->second.end.why == End::Why::Destroyed &&
        !found->second.loans.empty() && emit &&
        reported.emplace(&variable, found->second.end.at.getRawEncoding())
            .second)
      report(at, rule::StaleBorrow,
             "the thread '" + variable.getNameAsString() +
                 "' is still running when " + nameOf(found->second.end.root) +
                 ", which it uses, ends",
             found->second.end.at,
             nameOf(found->second.end.root) +
                 " ends here; declare it before the thread object, or join "
                 "the thread first");
    llvm::erase_if(flow.lends,
                   [&](const auto &entry) { return entry.first == &variable; });
  }

  /// §5: the container of thread objects a range-for joins element by
  /// element, when its body does nothing but that (perhaps under a test of
  /// the element's own `const` member function, `joinable()`).
  [[nodiscard]] const clang::VarDecl *
  joinsEach(const clang::CXXForRangeStmt &loop) {
    const clang::VarDecl *element = loop.getLoopVariable();
    const auto single = [](const clang::Stmt *statement) {
      while (const auto *block =
                 llvm::dyn_cast_or_null<clang::CompoundStmt>(statement)) {
        if (block->size() != 1)
          return static_cast<const clang::Stmt *>(nullptr);
        statement = block->body_front();
      }
      return statement;
    };
    const auto onElement = [&](const clang::Expr *object) {
      const auto *named =
          llvm::dyn_cast<clang::DeclRefExpr>(object->IgnoreParenImpCasts());
      return named != nullptr && named->getDecl() == element;
    };
    const clang::Stmt *step = single(loop.getBody());
    if (const auto *test = llvm::dyn_cast_or_null<clang::IfStmt>(step)) {
      const auto *condition = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(
          test->getCond()->IgnoreParenImpCasts());
      if (test->getElse() != nullptr || test->hasInitStorage() ||
          condition == nullptr || condition->getMethodDecl() == nullptr ||
          !condition->getMethodDecl()->isConst() ||
          !onElement(condition->getImplicitObjectArgument()))
        return nullptr;
      step = single(test->getThen());
    }
    const auto *expression = llvm::dyn_cast_or_null<clang::Expr>(step);
    const auto *join = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(
        expression == nullptr ? nullptr : strip(expression));
    if (join == nullptr || join->getMethodDecl() == nullptr ||
        !onElement(join->getImplicitObjectArgument()) ||
        !listed(*join->getMethodDecl(),
                join->getImplicitObjectArgument()->getType(), "joins"))
      return nullptr;
    const auto *range = llvm::dyn_cast<clang::DeclRefExpr>(
        loop.getRangeInit()->IgnoreParenImpCasts());
    const clang::VarDecl *holder =
        range == nullptr ? nullptr : variableOf(*range);
    return holder != nullptr && ownLocal(*holder) &&
                   kinds.holdsThread(holder->getType())
               ? holder->getCanonicalDecl()
               : nullptr;
  }

  void call(Flow &flow, const clang::Expr *expression) {
    if (const auto *plain = llvm::dyn_cast<clang::CallExpr>(expression);
        plain != nullptr && passesThrough(*plain))
      return;
    const Call call = describe(*expression);
    if (kinds.threadRules)
      threads(flow, call, *expression);
    const bool constructs = llvm::isa<clang::CXXConstructExpr>(expression);
    const bool assigns =
        call.method != nullptr &&
        (call.method->getOverloadedOperator() == clang::OO_Equal ||
         CxxKinds::resets(*call.method,
                          call.object != nullptr
                              ? call.object->IgnoreParenImpCasts()->getType()
                              : clang::QualType()));
    // Everything is evaluated before anything is changed.
    Value target;
    bool targetChanges = false;
    if (call.object != nullptr) {
      target = objectOf(flow, call);
      targetChanges = changing(call) && kinds.holdsOwners(objectType(call));
    }
    Value taken;
    for (const Call::Argument &argument : call.arguments)
      taken.add(stored(flow, argument, &call));
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
    // A member of a local view that is assigned or changed: the local
    // takes the borrows too.
    const clang::VarDecl *holder = call.object != nullptr && view == nullptr
                                       ? memberOfLocal(call.object)
                                       : nullptr;
    if (holder != nullptr && changing(call) && !taken.loans.empty()) {
      Borrow &borrow = flow.borrows[holder];
      for (const Loan &loan : taken.loans)
        if (!llvm::is_contained(borrow.loans, loan))
          borrow.loans.push_back(loan);
    }
    if (assigns && call.object != nullptr) {
      if (const std::optional<Place> place = placeOf(call.object))
        unmove(flow, *place);
      if (view != nullptr) {
        Borrow &borrow = flow.borrows[view];
        borrow = {
            .loans = taken.loans, .reference = taken.reference, .end = {}};
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
    // RFC 0045 §7: a pointer kept by a container that outlives the call.
    if (call.object != nullptr && changing(call) && outside(target))
      for (const Call::Argument &argument : call.arguments)
        if (argument.expression->getType()->isPointerType())
          // (A pointer passed by reference is a temporary bound to it.)
          switchingStore(pointee(flow, argument.expression->IgnoreImplicit()),
                         argument.expression->getExprLoc(),
                         spell(call.object, ast),
                         argument.expression->getType()->getPointeeType());

    // §6, rule 2: the object of a member function that is not const and
    // not an accessor.
    if (targetChanges) {
      // RFC 0043 §9: a global is assigned as a whole, moved out and back.
      if (!(assigns && global(call.object)))
        owners(target, call.at, spell(call.object, ast), call.method,
               call.object, objectType(call));
      invalidate(flow, target, Ends::Into, End::Why::Changed, call.at,
                 call.method);
    }
    // §6, rule 3: what a call may change through its parameters.
    for (const Call::Argument &argument : call.arguments) {
      const clang::Expr *given = argument.expression;
      if (!(given->getType()->isPointerType() ||
            kinds.tracked(given->getType())) ||
          !keeps(argument.parameter)) {
        // Nothing is kept.
      } else if (const clang::VarDecl *keeper = ownObject(call.object)) {
        // A local that keeps what it is passed holds its borrows.
        Borrow &borrow = flow.borrows[keeper];
        for (const Loan &loan : stored(flow, argument).loans)
          if (!llvm::is_contained(borrow.loans, loan))
            borrow.loans.push_back(loan);
      } else if (!(constructs && declaresLocal(expression))) {
        if (given->getType()->isPointerType())
          switchingStore(stored(flow, argument), given->getExprLoc(),
                         "what " + called(call.callee) + " keeps",
                         given->getType()->getPointeeType());
        storedOutside(
            {}, stored(flow, argument), given->getExprLoc(),
            "what " + called(call.callee) + " keeps",
            /*viewTarget=*/!given->getType()->isPointerType(),
            given->getType()->isPointerType()
                ? ""
                : "capture by value what the callable needs ('[count]', "
                  "'[p = std::move(p)]', a 'std::shared_ptr'); a callee that "
                  "only calls it takes it by reference to const");
      }
      if (!changes(argument.parameter) || movedFrom(given).has_value())
        continue;
      const Value objects = given->getType()->isPointerType()
                                ? pointee(flow, given)
                                : object(flow, given);
      owners(objects, given->getExprLoc(), spell(given, ast), call.callee,
             given, argument.parameter->getType()->getPointeeType());
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
  }

  [[nodiscard]] static std::optional<Place>
  placeOf(const clang::Expr *expression) {
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

  static void move(Flow &flow, const Place &place, clang::SourceLocation at) {
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
    // §5: a member function that resets its object is as good.
    if (const auto *member =
            llvm::dyn_cast<clang::CXXMemberCallExpr>(statement);
        member != nullptr && member->getMethodDecl() != nullptr &&
        CxxKinds::resets(*member->getMethodDecl(),
                         member->getImplicitObjectArgument()
                             ->IgnoreParenImpCasts()
                             ->getType()))
      target = member->getImplicitObjectArgument();
    if (target != nullptr) {
      assigned.insert(target->IgnoreParenImpCasts());
      if (const auto *named =
              llvm::dyn_cast<clang::DeclRefExpr>(target->IgnoreParenImpCasts()))
        if (const clang::VarDecl *variable = variableOf(*named))
          assignedLocals.insert(variable->getCanonicalDecl());
    }
    for (const clang::Stmt *child : statement->children())
      collectAssigned(child);
  }

  void use(Flow &flow, const clang::DeclRefExpr &named) {
    const clang::VarDecl *variable = variableOf(named);
    if (variable == nullptr)
      return;
    // RFC 0046 §5: a lent local, or a borrow of one, used against the loan.
    if (!flow.lends.empty()) {
      // A range-for's hidden variables read the range unless the loop
      // variable is a reference to what is not `const`.
      const std::optional<bool> reads = rangeReads(*variable);
      lentUse(flow, *variable, named, false, reads);
      if (const auto found = flow.borrows.find(variable->getCanonicalDecl());
          found != flow.borrows.end() && kinds.view(variable->getType()))
        for (const Loan &loan : found->second.loans)
          if (const clang::VarDecl *root = loan.root.variable();
              root != nullptr && root != variable->getCanonicalDecl())
            lentUse(flow, *root, named, variable->getType()->isPointerType(),
                    reads);
    }
    if (assigned.contains(&named))
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
      const auto holder = root == nullptr || root == variable
                              ? flow.borrows.end()
                              : flow.borrows.find(root);
      if (holder != flow.borrows.end() &&
          holder->second.end.why != End::Why::None &&
          !CxxKinds::pointerLike(root->getType())) {
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
        report(
            at, rule::OwnerMoved,
            whole || same ? "'" + name + "' is used after it was moved"
                          : "'" + name + "' is used while its member '" +
                                moved.second->getNameAsString() + "' is moved",
            where, "moved here; use it before the move, or assign to it first");
      return;
    }
  }
  std::set<std::pair<Place, unsigned>> reportedMoves;

  void declare(Flow &flow, const clang::VarDecl &variable) {
    const clang::VarDecl *canonical = variable.getCanonicalDecl();
    unmove(flow, {canonical, nullptr});
    if (!variable.hasLocalStorage())
      return;
    if (kinds.threadRules) {
      if (CxxKinds::guardClass(variable.getType()))
        declareGuard(flow, variable);
      // §5: a thread object moved into a new local brings its loans.
      if (kinds.holdsThread(variable.getType()) &&
          variable.getInit() != nullptr)
        moveLends(flow, variable.getInit(), canonical);
    }
    // §7: what a constructor keeps of its arguments (a callable taken by
    // value, a WEAVEC_ESCAPES parameter) the new local holds.
    const Value kept = keptBy(flow, variable.getInit());
    if (!kinds.tracked(variable.getType())) {
      if (!kept.loans.empty())
        flow.borrows[canonical] = {
            .loans = kept.loans, .reference = false, .end = {}};
      return;
    }
    Borrow borrow;
    if (const clang::Expr *init = variable.getInit()) {
      const clang::QualType type = variable.getType();
      Value value = held(flow, type, init);
      value.add(kept);
      borrow.loans = value.loans;
      borrow.reference = value.reference;
    }
    flow.borrows[canonical] = std::move(borrow);
  }

  /// The arguments a construction passes for parameters that keep them.
  [[nodiscard]] Value keptBy(const Flow &flow, const clang::Expr *init) {
    Value kept;
    const auto *construct =
        llvm::dyn_cast_or_null<clang::CXXConstructExpr>(strip(init));
    if (construct == nullptr)
      return kept;
    for (const Call::Argument &argument : describe(*construct).arguments)
      if (keeps(argument.parameter))
        kept.add(stored(flow, argument));
    return kept;
  }

  /// A local that is constructed where it is declared.
  [[nodiscard]] bool declaresLocal(const clang::Expr *construct) const {
    const clang::Stmt *parent = parents.getParent(construct);
    while (parent != nullptr &&
           llvm::isa<clang::ExprWithCleanups, clang::CXXBindTemporaryExpr,
                     clang::ParenExpr>(parent))
      parent = parents.getParent(parent);
    const auto *declaration = llvm::dyn_cast_or_null<clang::DeclStmt>(parent);
    if (declaration == nullptr || !declaration->isSingleDecl())
      return false;
    const auto *variable =
        llvm::dyn_cast<clang::VarDecl>(declaration->getSingleDecl());
    return variable != nullptr && variable->hasLocalStorage() &&
           strip(variable->getInit()) == construct;
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
            .loans = value.loans, .reference = value.reference, .end = {}};
        // What was moved out through it was another object's.
        unmove(flow, {variable->getCanonicalDecl(), nullptr});
        return;
      }
    const Value target = object(flow, left);
    if (outside(target)) {
      storedOutside(target, value, binary.getOperatorLoc(), spell(left, ast),
                    /*viewTarget=*/false);
      switchingStore(value, binary.getOperatorLoc(), spell(left, ast),
                     left->getType()->getPointeeType());
    }
  }

  /// The local a `return` names, through the copy or move that returns it.
  [[nodiscard]] static const clang::Expr *
  returnedName(const clang::Expr *expression) {
    expression = strip(expression);
    while (const auto *construct =
               llvm::dyn_cast_or_null<clang::CXXConstructExpr>(expression)) {
      if (construct->getNumArgs() != 1)
        break;
      expression = strip(construct->getArg(0)->IgnoreParenImpCasts());
    }
    return expression;
  }

  void returned(Flow &flow, const clang::ReturnStmt &statement) {
    const clang::Expr *expression = statement.getRetValue();
    const clang::QualType type = function.getReturnType();
    if (expression == nullptr)
      return;
    Value value;
    if (kinds.tracked(type)) {
      value = held(flow, type, expression);
    } else if (const clang::VarDecl *keeper =
                   ownObject(returnedName(expression))) {
      // A local that kept what it was passed (§7).
      if (const auto found = flow.borrows.find(keeper);
          found != flow.borrows.end())
        value.loans = found->second.loans;
    }
    // §7: a callable returned in a type that hides its captures owns them.
    if (!type->isReferenceType() && kinds.erased(type) && !kinds.view(type))
      for (const Loan &loan : value.loans) {
        // A callable the function was handed and passes on is the
        // caller's.
        const bool handed = loan.root.beyond &&
                            loan.root.variable() != nullptr &&
                            kinds.erased(loan.root.variable()->getType());
        if (localRoot(loan.root) || handed)
          continue;
        error(statement.getReturnLoc(), rule::Escape,
              "the callable that is returned captures " + nameOf(loan.root) +
                  " by reference, and its type does not show it",
              statement.getReturnLoc(),
              "capture by value what the callable needs ('[name]', "
              "'[value = member_]', a 'std::shared_ptr'), so that it owns "
              "what it uses");
        return;
      }
    // RFC 0046 §6: values leave a lock by copy or by move.
    for (const Loan &loan : value.loans)
      if (loan.root.kind == RootKind::Guard) {
        const auto *const field =
            llvm::find_if(value.loans, [](const Loan &other) {
              return other.root.field != nullptr;
            });
        error(statement.getReturnLoc(), rule::Escape,
              "a reference into " +
                  (field != value.loans.end()
                       ? nameOf(field->root)
                       : std::string("a locked member")) +
                  " is returned past the lock that guards it",
              statement.getReturnLoc(),
              "return a copy: values leave a lock by copy or by move");
        return;
      }
    for (const Loan &loan : value.loans) {
      if (!localRoot(loan.root))
        continue;
      const bool whole = CxxKinds::pointerLike(type) && loan.depth == 0;
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
    const Value value = held(flow, type, init);
    Value target;
    target.loans.push_back(
        {.root = {.kind = RootKind::This, .field = field}, .depth = 0});
    storedOutside(target, value, initializer.getSourceLocation(),
                  "'" + field->getNameAsString() + "'",
                  /*viewTarget=*/!type->isPointerType());
    if (type->isPointerType())
      switchingStore(value, initializer.getSourceLocation(),
                     "'" + field->getNameAsString() + "'",
                     type->getPointeeType());
  }

  void transfer(Flow &flow, const clang::Stmt *statement) {
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement)) {
      use(flow, *named);
    } else if (const auto *member =
                   llvm::dyn_cast<clang::MemberExpr>(statement)) {
      if (kinds.threadRules)
        lockedUse(flow, *member);
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
      // RFC 0046 §5: a loop that only joins each thread of a container
      // ends them all before anything else runs.
      if (const auto *loop = llvm::dyn_cast_or_null<clang::CXXForRangeStmt>(
              parents.getParent(declaration));
          kinds.threadRules && loop != nullptr &&
          loop->getRangeStmt() == declaration)
        if (const clang::VarDecl *holder = joinsEach(*loop)) {
          llvm::erase_if(flow.lends, [&](const auto &entry) {
            return entry.first == holder;
          });
          flow.borrows[holder].loans.clear();
        }
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
        if (kinds.threadRules)
          ended(flow, *variable, lifetime->getTriggerStmt()->getEndLoc());
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
    if (kinds.threadRules)
      entryGuards(flow);
    for (const clang::ParmVarDecl *parameter : function.parameters()) {
      const clang::QualType type = parameter->getType();
      if (!kinds.tracked(type))
        continue;
      const clang::VarDecl *canonical = parameter->getCanonicalDecl();
      Borrow borrow;
      if (CxxKinds::pointerLike(type))
        borrow.loans.push_back(
            {.root = {.kind = RootKind::Var, .base = canonical}, .depth = 0});
      if (!CxxKinds::pointerLike(type) || kinds.tracked(type->getPointeeType()))
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
        (method != nullptr && method->getRefQualifier() == clang::RQ_RValue);
    for (const auto &[place, where] : flow.moved) {
      if (place.first == nullptr
              ? mayLeave
              : !place.first->getType()->isLValueReferenceType())
        continue;
      if (emit && reportedMoves.emplace(place, where.getRawEncoding()).second)
        report(at, rule::OwnerMoved,
               "'" +
                   (place.second != nullptr ? place.second->getNameAsString()
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
        if (const clang::CFGBlock *next = adjacent.getReachableBlock();
            next != nullptr && join(in[next->getBlockID()], out))
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
                  const clang::LambdaExpr *lambda, const RuleReport &report) {
  return CxxFlow(ast, kinds, function, body, lambda, report).run();
}

} // namespace weavec::frontend
