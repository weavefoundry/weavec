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
enum class RootKind : std::uint8_t { Var, This, Temp, Guard, Global };

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
    // RFC 0053 §7 (D6): "the globals" (a null base) overlaps every global.
    if (kind == RootKind::Global && other.kind == RootKind::Global &&
        (base == nullptr || other.base == nullptr))
      return true;
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
    Unlocked,
    /// RFC 0047 §5.2 (S): the characters of a string the function does not
    /// own, at a call that may reach the string.
    Characters,
    /// RFC 0052 §4.1: a call that reaches the root through a view it is
    /// given (`with`), or through its object.
    Reached,
    /// RFC 0052 §5.1: a call of a lambda the function sees (`with`), or
    /// one it is passed to.
    Runs
  };
  Why why = Why::None;
  clang::SourceLocation at;
  Root root;
  /// The function that changed the root.
  const clang::NamedDecl *by = nullptr;
  /// The view or the callable it changed the root through.
  const clang::Expr *with = nullptr;
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
  /// A thread may have started on some path (a constructor that started
  /// one is no longer alone with its object).
  bool started = false;
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
  if (from.started && !into.started) {
    into.started = true;
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
  llvm::DenseSet<const clang::VarDecl *> reseated;
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
    if (root.kind == RootKind::Global)
      return root.base == nullptr
                 ? std::string("a global")
                 : "the global '" +
                       static_cast<const clang::VarDecl *>(root.base)
                           ->getNameAsString() +
                       "'";
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
      // RFC 0053 §7 (D6): a call of the program may change a global.
      if (end.root.kind == RootKind::Global && !loop) {
        message = name + " points into " + owner + ", which the call to " +
                  called(end.by) + " may change";
        note = "called here; take " + name +
               " again after the call, or copy what is needed before it";
        break;
      }
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
    case End::Why::Reached: {
      // RFC 0053 §1, §6: a call through a pointer to a member function; a
      // virtual call that is not final.
      const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(end.by);
      std::string the = "the call to " + called(end.by);
      if (end.by == nullptr)
        the = "the call through a pointer to a member function";
      else if (method != nullptr && method->isVirtual() &&
               !method->hasAttr<clang::FinalAttr>() &&
               !method->getParent()->hasAttr<clang::FinalAttr>())
        the = "the virtual call to " + called(end.by);
      message =
          loop ? "the loop over '" + *loop + "' continues after " + the +
                     ", which may reach " + owner
               : name + " points into " + owner + ", which " + the +
                     " may reach" +
                     (end.with != nullptr ? " through " + spell(end.with, ast)
                                          : std::string(" through its object"));
      note = "called here; take " + name +
             " again after the call, or give the call a view of what is "
             "'const' ('std::span<const T>', 'const T *')" +
             (the.starts_with("the virtual")
                  ? std::string("; or call through a 'final' class, whose "
                                "override the rules see")
                  : std::string());
      break;
    }
    case End::Why::Runs: {
      const std::string callable =
          end.with != nullptr ? callableName(end.with) : "a lambda";
      const bool direct =
          end.by != nullptr && llvm::isa<clang::CXXMethodDecl>(end.by) &&
          llvm::cast<clang::CXXMethodDecl>(end.by)->getOverloadedOperator() ==
              clang::OO_Call;
      const std::string runs = direct ? "the call of " + callable
                                      : "the call to " + called(end.by) +
                                            ", which may call " + callable;
      // RFC 0053 §1: a callable whose body the function did not write may
      // change what its loans reach.
      const std::string changes =
          end.with == nullptr || lambdaOf(end.with) != nullptr
              ? ", which changes " + owner
              : ", which may change what " + callable + " holds, " + owner;
      message = loop ? "the loop over '" + *loop + "' continues after " + runs +
                           changes
                     : name + " was taken from " + owner + " before " + runs +
                           changes;
      note = "called here; take " + name +
             " again after the call, or let the lambda return what to change "
             "and change " +
             owner + " after the call";
      break;
    }
    case End::Why::Characters:
      message = name + " points at the characters of " + owner +
                ", which the call to " + called(end.by) + " may have moved";
      note = "called here; take " + name +
             " again after the call, or keep an index into the string";
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
        // RFC 0053 §7 (D7): a `const` one with a `mutable` owner may change.
        value.reference = (!variable->getType().isConstQualified() ||
                           kinds.mutableOwners(variable->getType())) &&
                          !isLibrary(*variable);
        if (value.reference)
          value.loans.push_back({.root = {.kind = RootKind::Global,
                                          .base = variable->getCanonicalDecl()},
                                 .depth = 0});
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
      // RFC 0053 §5: a pointer kept in a local the flow follows (a member
      // of a `std::pair`, an element of a `std::vector<T *>`, what a
      // `std::optional<T *>` holds) points where the local's loans do.
      if (const Value within = heldIn(flow, inner); !within.loans.empty())
        return within;
      Value value;
      value.reference = true;
      return value;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
      if (unary->getOpcode() == clang::UO_AddrOf)
        return object(flow, unary->getSubExpr());
      return pointee(flow, unary->getSubExpr());
    }
    // RFC 0053 §5: a pointer bound to a reference parameter
    // (`std::pair(&x, 1)`, `push_back(&x)`) is a temporary that holds it.
    if (const auto *temporary =
            llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
      return pointee(flow, temporary->getSubExpr());
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

  /// RFC 0053 §5: where the pointers kept in the place `expression` names
  /// point, when the place may be in a local of a library class the flow
  /// follows (a `std::pair`, a `std::optional`, a container of pointers;
  /// not a reference or a pointer): the local's loans, and a reference for
  /// any other place it may be. Empty when it is in no such local. (A
  /// pointer member of a class of the program is a reference, RFC 0044
  /// §8.)
  [[nodiscard]] Value heldIn(const Flow &flow, const clang::Expr *expression) {
    const Value objects = object(flow, expression);
    Value value;
    bool other = objects.reference;
    for (const Loan &loan : objects.loans) {
      const clang::VarDecl *variable = loan.root.variable();
      const auto found = variable == nullptr || loan.root.beyond
                             ? flow.borrows.end()
                             : flow.borrows.find(variable);
      const clang::CXXRecordDecl *record =
          found == flow.borrows.end()
              ? nullptr
              : variable->getType()->getAsCXXRecordDecl();
      if (record == nullptr || !isLibrary(*record) ||
          !kinds.tracked(variable->getType())) {
        other = true;
        continue;
      }
      for (const Loan &inner : found->second.loans)
        value.add(inner);
      value.reference |= found->second.reference;
    }
    if (!value.loans.empty())
      value.reference |= other;
    return value;
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
    // RFC 0053 §5: an array made from braces (the elements of a
    // `std::array`) points where its elements do.
    if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(expression);
        list != nullptr && type->isArrayType())
      return listed(flow, list->inits(), type);
    if (const auto *temporary =
            llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
      return points(flow, temporary->getSubExpr());
    if (type->isPointerType() || type->isArrayType())
      return pointee(flow, expression);
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression))
      return points(flow, cast->getSubExpr());
    if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(expression))
      return captured(flow, *lambda);
    if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(expression))
      return listed(flow, list->inits(), type);
    if (const auto *list =
            llvm::dyn_cast<clang::CXXParenListInitExpr>(expression))
      return listed(flow, list->getInitExprs(), type);
    if (const auto *list =
            llvm::dyn_cast<clang::CXXStdInitializerListExpr>(expression)) {
      // RFC 0052 §6: a list is a view of the array the compiler made for
      // it, which lives as long as the full expression or the variable
      // the list initializes.
      Value value = points(flow, list->getSubExpr());
      value.add(object(flow, list->getSubExpr()));
      return value;
    }
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
    if (type->isPointerType()) {
      const Value pointed = pointee(flow, expression);
      // RFC 0053 §5: through a pointer to a class that borrows and owns
      // nothing (`o->get()` of an optional of a reference wrapper), where
      // the class points as well.
      const clang::QualType target = type->getPointeeType();
      if (kinds.tracked(target) && !kinds.holdsOwners(target) &&
          !kinds.erased(target)) {
        Value value = viewOf(flow, pointed);
        value.add(pointed.inside());
        return value;
      }
      return pointed.inside();
    }
    if (kinds.pureView(type) && (CxxKinds::pointerClass(type) || !namesView))
      return points(flow, expression);
    // RFC 0053 §5: what erases its contents (a `std::any` holding a pointer
    // or a reference wrapper) gives a copy of them, which points where they
    // do, as well as a place in itself.
    if (kinds.erased(type) && !kinds.view(type.getNonReferenceType())) {
      Value value = points(flow, expression);
      value.add(object(flow, expression).inside());
      return value;
    }
    // RFC 0053 §5, §8: through a class that borrows and owns nothing (a
    // reference wrapper, a struct of a pointer), where it points, and into
    // itself.
    if (kinds.tracked(type) && !kinds.holdsOwners(type) &&
        !kinds.erased(type) && !kinds.view(type.getNonReferenceType())) {
      Value value = points(flow, expression);
      value.add(object(flow, expression).inside());
      return value;
    }
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
    Call call = describe(*expression);
    // RFC 0052 §6: a call of one of the library's function objects
    // (`std::views::all(v)`) is a function's, not a member's.
    if (functionObject(call)) {
      call.object = nullptr;
      call.method = nullptr;
    }
    const bool constructs = llvm::isa<clang::CXXConstructExpr>(expression);
    const clang::QualType type =
        constructs || call.callee == nullptr
            ? expression->getType()
            : call.callee->getReturnType().getNonReferenceType();
    const bool names = !constructs && expression->isGLValue();
    if (!names && !kinds.tracked(expression->getType()))
      return {};
    Value value;
    // RFC 0052 §3: `lifetimebound` on a parameter, or on the implicit
    // object, narrows the result to those.
    const bool boundThis = call.method != nullptr && call.object != nullptr &&
                           lifetimeBoundThis(*call.method);
    const bool marked =
        boundThis ||
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
    } else if (call.object != nullptr && (!marked || boundThis) && !owning &&
               (!onlyPolymorphic(expression->getType()) ||
                kinds.mayBorrow(expression->getType()->getAsCXXRecordDecl(),
                                objectType(call)))) {
      // RFC 0053 §6 (D5): a smart pointer to a base a member function
      // returns borrows its object only if its type could.
      value.add(through(flow, call.object, namesView));
    }
    bool views = false;
    for (const Call::Argument &argument : call.arguments) {
      const clang::QualType given = argument.expression->getType();
      const bool byReference = argument.parameter != nullptr &&
                               argument.parameter->getType()->isReferenceType();
      if (marked) {
        if (argument.parameter != nullptr &&
            argument.parameter->hasAttr<clang::LifetimeBoundAttr>()) {
          value.add(contribution(flow, argument, type, names, constructs));
          // RFC 0053 §6: a value a parameter marked `lifetimebound` gives
          // borrows it, whatever the value's type shows.
          if (!names && !type->isPointerType() &&
              argument.expression->isGLValue()) {
            const Value bound = object(flow, argument.expression);
            value.add(bound);
            value.add(bound.inside());
          }
        }
      } else if (!byReference &&
                 (given->isPointerType() || kinds.tracked(given)) &&
                 !given->isFunctionPointerType()) {
        // A pointer can be what a reference or pointer result names only
        // if it points to the result's type; RFC 0052 §3: or into what it
        // points to, when that reaches the result's type.
        if (given->isPointerType() && (names || type->isPointerType()) &&
            !constructs && !CxxKinds::related(given->getPointeeType(), type)) {
          if (kinds.reaches(given->getPointeeType(), type)) {
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
            !(given->isPointerType()
                  ? kinds.mayBorrow(type->getAsCXXRecordDecl(),
                                    given->getPointeeType())
                  : kinds.mayBorrowThrough(type->getAsCXXRecordDecl(), given)))
          continue;
        // RFC 0053 §5: a class that copies the elements of a list (a
        // `std::vector` made from braces) points where they do, not into
        // the array the braces made.
        const auto *list = llvm::dyn_cast<clang::CXXStdInitializerListExpr>(
            strip(argument.expression));
        value.add(list != nullptr && constructs &&
                          !kinds.viewClass(type->getAsCXXRecordDecl())
                      ? points(flow, list->getSubExpr())
                      : points(flow, argument.expression));
        views = true;
      }
    }
    // RFC 0052 §3: a member function's reference or pointer result may
    // point into a reference argument whose type reaches it, as well as
    // into its object.
    if (!marked && !owning && !constructs && call.object != nullptr &&
        (names || type->isPointerType()))
      for (const Call::Argument &argument : call.arguments)
        if (argument.parameter != nullptr &&
            argument.parameter->getType()->isReferenceType() &&
            argument.expression->isGLValue() &&
            kinds.reaches(argument.expression->getType(), type))
          value.add(object(flow, argument.expression).inside());
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
    // RFC 0053 §1: what a callable's call returns points where the
    // callable's loans reach.
    if (call.method != nullptr && call.object != nullptr &&
        call.method->getOverloadedOperator() == clang::OO_Call &&
        anyCallable(objectType(call)))
      value.add(reachOf(flow, call.object));
    // §7: a reference or pointer a function of the program returns with
    // nothing it could point into is the globals'.
    // So is a value that may hold a borrow (`R get()` of a struct of a
    // reference).
    if ((names || type->isPointerType() || kinds.view(type) ||
         kinds.borrowing(type->getAsCXXRecordDecl())) &&
        value.loans.empty() && call.callee != nullptr &&
        !isLibrary(*call.callee) && !constructs && !value.reference &&
        !type.isConstQualified() && !kinds.erased(type))
      value.add({.root = {.kind = RootKind::Global}, .depth = 0});
    if (names || type->isPointerType())
      return fit(value, type);
    return value;
  }

  /// RFC 0052 §3: a reference or pointer to `type` is at a root only if the
  /// root has a related type; at any other it is inside it (`rv[0]` of a
  /// `std::ranges::ref_view` at a vector is an element, not the vector).
  [[nodiscard]] Value fit(Value value, clang::QualType type) const {
    for (Loan &loan : value.loans) {
      if (loan.depth != 0 || loan.root.beyond)
        continue;
      const clang::QualType root = rootType(loan.root);
      if (!root.isNull() && !CxxKinds::related(root, type))
        loan.depth = 1;
    }
    Value unique;
    unique.add(value);
    return unique;
  }

  /// The type of what a root names, with arrays taken as their elements.
  [[nodiscard]] clang::QualType rootType(const Root &root) const {
    clang::QualType type;
    if (root.field != nullptr)
      type = root.field->getType();
    else if (const clang::VarDecl *variable = root.variable())
      type = variable->getType();
    else if (root.kind == RootKind::Temp)
      type = static_cast<const clang::Expr *>(root.base)->getType();
    else if (const auto *method =
                 llvm::dyn_cast<clang::CXXMethodDecl>(&function);
             root.kind == RootKind::This && method != nullptr &&
             !method->isStatic())
      type = method->getFunctionObjectParameterType();
    if (type.isNull())
      return type;
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    return type;
  }

  /// RFC 0052 §6: a call of the library's function object, an empty class
  /// whose `operator()` stands for a function (`std::views::all`).
  [[nodiscard]] static bool functionObject(const Call &call) {
    if (call.method == nullptr || call.object == nullptr ||
        call.method->getOverloadedOperator() != clang::OO_Call ||
        !isLibrary(*call.method))
      return false;
    const clang::CXXRecordDecl *record = call.method->getParent();
    return record != nullptr && record->hasDefinition() &&
           record->getDefinition()->isEmpty();
  }

  /// `[[clang::lifetimebound]]` on a member function's implicit object, in
  /// any of its declarations.
  [[nodiscard]] static bool
  lifetimeBoundThis(const clang::FunctionDecl &method) {
    for (const clang::FunctionDecl *declaration : method.redecls()) {
      const clang::TypeSourceInfo *written = declaration->getTypeSourceInfo();
      if (written == nullptr)
        continue;
      clang::AttributedTypeLoc attributed;
      for (clang::TypeLoc at = written->getTypeLoc();
           (attributed = at.getAsAdjusted<clang::AttributedTypeLoc>());
           at = attributed.getModifiedLoc())
        if (attributed.getAttrAs<clang::LifetimeBoundAttr>() != nullptr)
          return true;
    }
    return false;
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
    // RFC 0053 §6: a conversion of a smart pointer to a base
    // (`std::unique_ptr<Base>` from `std::unique_ptr<D>`) is a move.
    const auto *from =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            given->getAsCXXRecordDecl());
    const auto *to =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            type->getAsCXXRecordDecl());
    const bool converts =
        from != nullptr && to != nullptr &&
        from->getSpecializedTemplate() == to->getSpecializedTemplate() &&
        !CxxKinds::typeArguments(*from).empty() &&
        !CxxKinds::typeArguments(*to).empty() &&
        CxxKinds::related(CxxKinds::typeArguments(*from)[0],
                          CxxKinds::typeArguments(*to)[0]);
    const bool copies = constructs && (same || converts);
    const bool erases = kinds.erased(type) && !kinds.view(type);
    if ((names || type->isPointerType()) && !constructs) {
      // A reference or pointer result can be the argument itself only if
      // it has its type.
      if (same)
        value.add(object(flow, expression));
      else if (kinds.reaches(given, type))
        value.add(object(flow, expression).inside());
    } else if (!kinds.pureView(given) && !copies && !erases &&
               (!type->isRecordType() ||
                kinds.mayBorrow(type->getAsCXXRecordDecl(), given))) {
      // A copy or a move of a view points where the view does, not into
      // the object it was copied from; and a class whose members could
      // not refer to the argument keeps no borrow of it. RFC 0052 §6: a
      // view whose members name an object of the argument's type points
      // at it (`std::ref(v)`, a span of an array).
      const clang::CXXRecordDecl *made = type->getAsCXXRecordDecl();
      if (made != nullptr && kinds.pointsAt(made, given))
        value.add(object(flow, expression));
      else
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
                         const clang::NamedDecl *by,
                         const clang::Expr *with = nullptr) {
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
        // ("The globals" a call may change: the one the borrow is into.)
        borrow.end = {.why = why,
                      .at = at,
                      .root = hit->root.kind == RootKind::Global &&
                                      hit->root.base == nullptr
                                  ? loan.root
                                  : hit->root,
                      .by = by,
                      .with = with};
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
    // RFC 0053 §7 (D7): `mutable` makes `const` shallow for the caller.
    return (!target.isConstQualified() || kinds.mutableOwners(target)) &&
           kinds.holdsOwners(target) &&
           !CxxKinds::accessorParameter(*parameter);
  }

  /// A parameter through which the callee may assign what it is given:
  /// a reference or pointer to what is not `const`.
  [[nodiscard]] static bool outParameter(const clang::ParmVarDecl *parameter) {
    if (parameter == nullptr)
      return false;
    const clang::QualType type = parameter->getType();
    return (type->isLValueReferenceType() || type->isPointerType()) &&
           !type->getPointeeType().isConstQualified();
  }

  /// The object `&object` names, or the expression itself.
  [[nodiscard]] static const clang::Expr *
  addressed(const clang::Expr *expression) {
    if (const auto *unary =
            llvm::dyn_cast<clang::UnaryOperator>(strip(expression));
        unary != nullptr && unary->getOpcode() == clang::UO_AddrOf)
      return unary->getSubExpr();
    return expression;
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
    // RFC 0048 §4: a function of the program keeps the address a pointer or
    // reference parameter is given only if the parameter says
    // WEAVEC_ESCAPES (or its class is aliased), which a safe one is
    // checked for and one that is not is trusted with (X-A1).
    if (into != nullptr && into->callee != nullptr &&
        !isLibrary(*into->callee) && argument.parameter != nullptr &&
        CxxKinds::pointerLike(argument.parameter->getType()) &&
        !hasAnnotation(annotationsOf(argument.parameter->getType(), ast),
                       EscapesAnnotation) &&
        !sends(argument.parameter) &&
        (into->method == nullptr ||
         !CxxKinds::aliased(into->method->getParent())))
      return {};
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

  /// What an out parameter `into` may take from another argument: what a
  /// view takes, and RFC 0053 §5: what a reference parameter that says
  /// WEAVEC_ESCAPES refers to, or a part of it when the local's type could
  /// point into it (a `std::optional` of a reference wrapper to a string
  /// of a variant).
  [[nodiscard]] Value outStored(const Flow &flow,
                                const Call::Argument &argument,
                                const clang::VarDecl &into) {
    Value value = stored(flow, argument);
    const clang::Expr *expression = argument.expression;
    if (argument.parameter == nullptr ||
        !argument.parameter->getType()->isReferenceType() ||
        !expression->isGLValue() || kinds.tracked(expression->getType()) ||
        !hasAnnotation(annotationsOf(argument.parameter->getType(), ast),
                       EscapesAnnotation))
      return value;
    const Value given = object(flow, expression);
    value.add(given);
    const clang::CXXRecordDecl *record = into.getType()->getAsCXXRecordDecl();
    if (record == nullptr || !kinds.pointsAt(record, expression->getType()))
      value.add(given.inside());
    return value;
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
    // RFC 0047 §5.2 (W): through a pointer kept in memory or a shared
    // pointer, only an aliased object is changed, or a library one that
    // holds no storage that changes type in place.
    const bool shared =
        !objects.reference && throughShared(objects, expression);
    if (objects.reference || shared) {
      const clang::QualType object = reached(expression);
      const clang::CXXRecordDecl *record = object->getAsCXXRecordDecl();
      // A shareable class (one that locks its members) is changed only
      // under its lock, which a borrow of its members does not outlive.
      if (record != nullptr &&
          (CxxKinds::aliased(record) || kinds.shareable(object) ||
           (isLibrary(*record) &&
            !kinds.switchesAny(object, /*strings=*/true))))
        return;
      if (record != nullptr && !isLibrary(*record))
        kinds.want(at, {.kind = Want::Kind::Aliased,
                        .declaration = record,
                        .expression = nullptr,
                        .keeper = nullptr,
                        .subjects = {}});
      const std::string declare =
          record != nullptr && !isLibrary(*record)
              ? "declare '" + record->getNameAsString() + "' WEAVEC_ALIASED " +
                    (shared ? "if its objects are changed while shared; "
                            : "if its objects are changed through pointers "
                              "other objects keep; ")
              : "";
      error(at, rule::OwnerPath,
            what +
                (shared ? " is reached through a 'std::shared_ptr', and "
                        : " is reached through a pointer kept in memory, "
                          "and ") +
                called(by) + " may change what it owns",
            at,
            declare + (shared ? "or replace the whole object, or keep this "
                                "function WEAVEC_GUARDED"
                              : "or own it, take it as a parameter, make the "
                                "member a reference, or keep this function "
                                "WEAVEC_GUARDED"));
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
  /// RFC 0047 §5.2 (W): whether `expression` (or the borrow it names) is
  /// reached through a `std::shared_ptr`.
  [[nodiscard]] bool throughShared(const Value &objects,
                                   const clang::Expr *expression) {
    const auto sharedType = [](clang::QualType type) {
      return CxxKinds::shared(type.getNonReferenceType()->getAsCXXRecordDecl());
    };
    // Into what the handle points to (depth 1 and more), not the handle.
    if (llvm::any_of(objects.loans, [&](const Loan &loan) {
          const clang::VarDecl *variable = loan.root.variable();
          return loan.depth >= 1 &&
                 ((variable != nullptr && sharedType(variable->getType()) &&
                   !freshShared(*variable)) ||
                  (loan.root.field != nullptr &&
                   sharedType(loan.root.field->getType())));
        }))
      return true;
    for (const clang::Expr *e = expression; e != nullptr;) {
      e = e->IgnoreParenImpCasts();
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
        e = member->getBase();
      } else if (const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(e);
                 op != nullptr && op->getNumArgs() >= 1 &&
                 (op->getOperator() == clang::OO_Arrow ||
                  op->getOperator() == clang::OO_Star)) {
        if (sharedType(op->getArg(0)->getType()))
          return !freshShared(op->getArg(0));
        e = op->getArg(0);
      } else if (const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(e);
                 call != nullptr &&
                 call->getImplicitObjectArgument() != nullptr) {
        return sharedType(call->getImplicitObjectArgument()->getType()) &&
               !freshShared(call->getImplicitObjectArgument());
      } else {
        return false;
      }
    }
    return false;
  }

  /// RFC 0047 §5.2 (W): a local handle no other handle shares: made by a
  /// library function the table calls `fresh` (`std::make_shared`), and
  /// otherwise only dereferenced or moved; the object is the function's
  /// until it is published.
  [[nodiscard]] bool freshShared(const clang::Expr *expression) {
    const auto *named =
        llvm::dyn_cast<clang::DeclRefExpr>(expression->IgnoreParenImpCasts());
    const auto *variable =
        named == nullptr ? nullptr
                         : llvm::dyn_cast<clang::VarDecl>(named->getDecl());
    return variable != nullptr && freshShared(*variable);
  }
  [[nodiscard]] bool freshShared(const clang::VarDecl &variable) {
    const auto known = fresh.find(&variable);
    if (known != fresh.end())
      return known->second;
    bool only =
        ownLocal(variable) && !llvm::isa<clang::ParmVarDecl>(variable) &&
        !variable.getType()->isReferenceType() && variable.getInit() != nullptr;
    if (only) {
      const auto *call =
          llvm::dyn_cast<clang::CallExpr>(variable.getInit()->IgnoreImplicit());
      const clang::FunctionDecl *callee =
          call == nullptr ? nullptr : call->getDirectCallee();
      only = callee != nullptr && isLibrary(*callee) &&
             core::LibrarySpec::shipped().functionIs(
                 CxxKinds::qualifiedName(*callee), "fresh");
    }
    const auto visit = [&](const clang::Stmt *statement, const clang::Stmt *up,
                           const auto &self) -> void {
      if (statement == nullptr || !only)
        return;
      if (const auto *use = llvm::dyn_cast<clang::DeclRefExpr>(statement);
          use != nullptr && use->getDecl() == &variable) {
        const auto *operation =
            llvm::dyn_cast_or_null<clang::CXXOperatorCallExpr>(up);
        const auto *moved = llvm::dyn_cast_or_null<clang::CallExpr>(up);
        only = (operation != nullptr &&
                (operation->getOperator() == clang::OO_Arrow ||
                 operation->getOperator() == clang::OO_Star)) ||
               (moved != nullptr && moved->isCallToStdMove());
      }
      for (const clang::Stmt *child : statement->children())
        self(child,
             llvm::isa<clang::ImplicitCastExpr, clang::ParenExpr>(statement)
                 ? up
                 : statement,
             self);
    };
    visit(&body, nullptr, visit);
    return fresh[&variable] = only;
  }
  llvm::DenseMap<const clang::VarDecl *, bool> fresh;

  /// RFC 0047 §5.2 (W): the object such a change reaches: what the changed
  /// pointer points to, what is below the last dereference in it, or what
  /// a borrow names.
  [[nodiscard]] static clang::QualType reached(const clang::Expr *expression) {
    const clang::Expr *e = expression->IgnoreParenImpCasts();
    if (e->getType()->isPointerType())
      return e->getType()->getPointeeType();
    for (unsigned depth = 0; depth < 16; ++depth) {
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
        if (member->isArrow())
          return member->getBase()->getType()->getPointeeType();
        e = member->getBase()->IgnoreParenImpCasts();
        continue;
      }
      if (const auto *op = llvm::dyn_cast<clang::CXXOperatorCallExpr>(e);
          op != nullptr && (op->getOperator() == clang::OO_Arrow ||
                            op->getOperator() == clang::OO_Star))
        return op->getType()->isPointerType()
                   ? op->getType()->getPointeeType()
                   : op->getType().getNonReferenceType();
      if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
          unary != nullptr && unary->getOpcode() == clang::UO_Deref)
        return unary->getType();
      break;
    }
    return e->getType().getNonReferenceType();
  }

  /// RFC 0047 §5.2 (S): a call that may reach a string the function does
  /// not own ends every writable borrow of its characters: one of the
  /// program's functions, or a change through a pointer kept in memory or
  /// a shared pointer.
  void endCharacters(Flow &flow, const Call &call, bool aliasing) {
    if (call.callee != nullptr && isLibrary(*call.callee) && !aliasing)
      return;
    for (auto &[variable, borrow] : flow.borrows) {
      if (borrow.end.why != End::Why::None || !writesCharacters(*variable))
        continue;
      const auto *hit = llvm::find_if(borrow.loans, [&](const Loan &loan) {
        if (localRoot(loan.root))
          return false;
        if (loan.root.field != nullptr)
          return CxxKinds::holdsString(loan.root.field->getType());
        const clang::VarDecl *base = loan.root.variable();
        if (base == nullptr)
          return false;
        const clang::QualType type = base->getType().getNonReferenceType();
        return CxxKinds::holdsString(
            type->isPointerType() ? type->getPointeeType() : type);
      });
      if (hit == borrow.loans.end())
        continue;
      borrow.end = {.why = End::Why::Characters,
                    .at = call.at,
                    .root = hit->root,
                    .by = call.callee};
    }
  }

  /// A local through which characters can be written: a pointer or a
  /// reference to characters that are not `const`, or a span of them.
  [[nodiscard]] static bool writesCharacters(const clang::VarDecl &variable) {
    clang::QualType type = variable.getType();
    if (type->isPointerType() || type->isReferenceType()) {
      type = type->getPointeeType();
    } else if (const auto *span = llvm::dyn_cast_or_null<
                   clang::ClassTemplateSpecializationDecl>(
                   type->getAsCXXRecordDecl());
               span != nullptr && isLibrary(*span) &&
               span->getTemplateArgs().size() != 0 &&
               span->getTemplateArgs()[0].getKind() ==
                   clang::TemplateArgument::Type &&
               core::LibrarySpec::shipped()
                   .countOf(CxxKinds::qualifiedName(*span), "data")
                   .has_value()) {
      type = span->getTemplateArgs()[0].getAsType();
    } else {
      return false;
    }
    return type->isAnyCharacterType() && !type.isConstQualified();
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
    // RFC 0053 §5: what another parameter refers to (an out parameter).
    const auto intoParameter = [&](const clang::ParmVarDecl *parameter) {
      return llvm::any_of(target.loans, [&](const Loan &loan) {
        const auto *into =
            llvm::dyn_cast_or_null<clang::ParmVarDecl>(loan.root.variable());
        return into != nullptr && into != parameter &&
               CxxKinds::pointerLike(into->getType());
      });
    };
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
      if (viewTarget && lent && !loan.root.beyond && ownLocal(*parameter) &&
          intoParameter(parameter) &&
          !hasAnnotation(annotationsOf(parameter->getType(), ast),
                         EscapesAnnotation)) {
        error(at, rule::Escape,
              where + " would keep a borrow of the parameter " +
                  nameOf(loan.root) + ", which is lent for the call only",
              at,
              "declare the parameter WEAVEC_ESCAPES (after its '&'), so that "
              "callers see that what they pass is kept; or return the view");
        return;
      }
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

  /// RFC 0052 §5.1: what a lambda's body may change of the enclosing
  /// function's objects.
  struct Changes {
    llvm::SmallPtrSet<const clang::VarDecl *, 4> variables;
    /// Members of `*this`, or all of it.
    llvm::SmallPtrSet<const clang::FieldDecl *, 4> members;
    bool self = false;
  };

  /// RFC 0052 §5.1: what a lambda's body may change: what it changes,
  /// passes on to be changed, or writes through (a view, a reference or a
  /// pointer it captured, §4), and what a lambda it calls or passes on may
  /// change.
  void lambdaChanges(const clang::Stmt *statement, Changes &changed,
                     unsigned depth = 0) {
    if (statement == nullptr || depth > 8)
      return;
    if (const auto *expression = llvm::dyn_cast<clang::Expr>(statement);
        expression != nullptr &&
        llvm::isa<clang::CallExpr, clang::CXXConstructExpr>(expression)) {
      const Call call = describe(*expression);
      if (call.object != nullptr &&
          ((changing(call) && kinds.holdsOwners(objectType(call))) ||
           writesThroughObject(call)))
        noteChanged(call.object, changed);
      for (const Call::Argument &argument : call.arguments) {
        if (changes(argument.parameter) ||
            (!llvm::isa<clang::CXXConstructExpr>(expression) &&
             (call.callee == nullptr || !isLibrary(*call.callee)) &&
             writable(argument.expression->getType())))
          noteChanged(argument.expression, changed);
        if (const clang::LambdaExpr *inner = lambdaOf(argument.expression))
          for (const clang::Stmt *instance : bodiesOf(*inner))
            lambdaChanges(instance, changed, depth + 1);
      }
      if (call.method != nullptr && call.object != nullptr &&
          call.method->getOverloadedOperator() == clang::OO_Call)
        if (const clang::LambdaExpr *inner = lambdaOf(call.object))
          for (const clang::Stmt *instance : bodiesOf(*inner))
            lambdaChanges(instance, changed, depth + 1);
    }
    for (const clang::Stmt *child : statement->children())
      lambdaChanges(child, changed, depth);
  }

  /// The capture a place in a lambda's body is part of, or reached
  /// through: `v`, `v.member`, `*p`, `span[0]`, `std::get<0>(v)`; or the
  /// member of `*this` (all of it, through `this` itself).
  static void noteChanged(const clang::Expr *expression, Changes &changed) {
    const clang::FieldDecl *member = nullptr;
    for (unsigned step = 0; expression != nullptr && step < 16; ++step) {
      expression = strip(expression)->IgnoreParenImpCasts();
      if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (named->refersToEnclosingVariableOrCapture())
          if (const clang::VarDecl *variable = variableOf(*named))
            changed.variables.insert(variable->getCanonicalDecl());
        return;
      }
      if (llvm::isa<clang::CXXThisExpr>(expression)) {
        if (member != nullptr)
          changed.members.insert(member);
        else
          changed.self = true;
        return;
      }
      if (const auto *access = llvm::dyn_cast<clang::MemberExpr>(expression)) {
        member = llvm::dyn_cast<clang::FieldDecl>(access->getMemberDecl());
        expression = access->getBase();
        continue;
      }
      member = nullptr;
      if (const auto *unary =
              llvm::dyn_cast<clang::UnaryOperator>(expression)) {
        expression = unary->getSubExpr();
      } else if (const auto *subscript =
                     llvm::dyn_cast<clang::ArraySubscriptExpr>(expression)) {
        expression = subscript->getBase();
      } else if (const auto *temporary =
                     llvm::dyn_cast<clang::MaterializeTemporaryExpr>(
                         expression)) {
        expression = temporary->getSubExpr();
      } else if (llvm::isa<clang::CallExpr>(expression)) {
        // A call's result is in its object, or in its first argument.
        const Call call = describe(*expression);
        if (call.object != nullptr)
          expression = call.object;
        else if (!call.arguments.empty())
          expression = call.arguments.front().expression;
        else
          return;
      } else {
        return;
      }
    }
  }

  /// RFC 0052 §5.1: the bodies a lambda's calls run: every instance of a
  /// generic lambda's call operator (its pattern, when it has none).
  [[nodiscard]] static llvm::SmallVector<const clang::Stmt *, 2>
  bodiesOf(const clang::LambdaExpr &lambda) {
    llvm::SmallVector<const clang::Stmt *, 2> bodies;
    if (const clang::FunctionTemplateDecl *generic =
            lambda.getDependentCallOperator())
      for (const clang::FunctionDecl *instance : generic->specializations())
        if (instance->hasBody())
          bodies.push_back(instance->getBody());
    if (bodies.empty())
      bodies.push_back(lambda.getBody());
    return bodies;
  }

  /// RFC 0053 §6 (D5): a type the flow follows only because it is, or
  /// holds, a polymorphic class.
  [[nodiscard]] bool onlyPolymorphic(clang::QualType type) {
    type = type.getNonReferenceType();
    if (type->isPointerType())
      type = type->getPointeeType();
    return kinds.polymorphicHolder(type) && !kinds.view(type) &&
           !kinds.erased(type) && !kinds.borrowing(type->getAsCXXRecordDecl());
  }

  /// RFC 0053 §6 (D5): the loans the flow knows a local (or what a pointer
  /// or reference local names) holds: none by default.
  [[nodiscard]] Value known(const Flow &flow, const clang::Expr *expression) {
    Value objects = expression->getType()->isPointerType()
                        ? pointee(flow, expression)
                        : object(flow, expression);
    Value value;
    for (const Loan &loan : objects.loans)
      if (const clang::VarDecl *variable = loan.root.variable())
        if (const auto found = flow.borrows.find(variable);
            found != flow.borrows.end() &&
            !variable->getType()->isReferenceType() &&
            !variable->getType()->isPointerType())
          for (const Loan &inner : found->second.loans)
            if (!inner.root.beyond)
              value.add(inner);
    return value;
  }

  /// A view as RFC 0044 has it, or what erases its contents: not a class
  /// that borrows only through raw pointer members (RFC 0052 §6).
  [[nodiscard]] bool viewOrErased(clang::QualType type) {
    return kinds.view(type.getNonReferenceType()) || kinds.erased(type);
  }

  /// RFC 0052 §4.1: a view of this type, given to a call, may change what
  /// it points to: a view (or a class of the program that holds one) that
  /// writes through. A callable is not read here: what a lambda changes
  /// is read from its body (§5.1), and one the function cannot see is
  /// believed (§5.2).
  [[nodiscard]] bool writable(clang::QualType type) {
    type = type.getNonReferenceType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    return record != nullptr && !record->isLambda() && !kinds.erased(type) &&
           kinds.tracked(type) && kinds.writesThrough(type);
  }

  /// RFC 0052 §4.1: a member function of the program's own view class may
  /// change what the view points to, whatever its `const`.
  [[nodiscard]] bool writesThroughObject(const Call &call) {
    return call.object != nullptr && call.method != nullptr &&
           !call.method->isStatic() && !isLibrary(*call.method) &&
           !llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(
               call.method) &&
           call.method->getOverloadedOperator() != clang::OO_Call &&
           writable(objectType(call));
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
             bool objectChanges, bool starts) {
    struct Passed {
      Value value;
      bool changer;
      const clang::Expr *expression;
      /// A callable the callee keeps (§5.3 checks it) or a thread runs.
      bool kept = false;
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
                        .expression = expression,
                        .kept = starts || keeps(argument.parameter)});
    }
    // §9, RFC 0052 §5.2 (D5): a callable passed beside what it may
    // change, or a borrow into it, for every callee; a library function is
    // trusted with plain borrows.
    for (const Passed &callable : passed) {
      // RFC 0053 §2 (D2): every callable, by its loans.
      if (callable.kept || callable.expression == nullptr ||
          !anyCallable(callable.expression->getType()))
        continue;
      const Value changes = changedBy(flow, callable.expression);
      for (const Passed &other : passed) {
        if (&other == &callable)
          continue;
        const bool closure = lambdaOf(other.expression) != nullptr;
        for (const Loan &loan : other.value.loans)
          for (const Loan &change : changes.loans)
            if (loan.root.overlaps(change.root) &&
                (closure ? loan.depth > change.depth
                         : loan.depth >= change.depth)) {
              error(call.at, rule::CallAlias,
                    callableName(callable.expression) + " may change " +
                        nameOf(change.root) + ", which is passed with it",
                    other.expression->getExprLoc(),
                    "collect the changes in a local and apply them after "
                    "the call");
              return;
            }
      }
    }
    if (library)
      return;
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
  /// increment (`++i`, `i++`, `i += n`, `n` a nonzero constant) and only
  /// read anywhere else in it.
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
             compound->getOpcode() == clang::BO_AddAssign &&
             compound->getRHS()->getIntegerConstantExpr(ast).value_or(
                 llvm::APSInt()) != 0)
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
    flow.started = true;
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
  /// variable named directly, but not a pointer that may be pointed
  /// elsewhere unseen; nothing for any other expression.
  [[nodiscard]] std::optional<const clang::VarDecl *>
  lockedObject(const clang::Expr *base) const {
    base = base->IgnoreParenImpCasts();
    if (llvm::isa<clang::CXXThisExpr>(base))
      return nullptr;
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(base))
      if (const clang::VarDecl *variable = variableOf(*named);
          variable != nullptr &&
          !reseated.contains(variable->getCanonicalDecl()))
        return variable->getCanonicalDecl();
    return std::nullopt;
  }

  /// The lock member a guard is constructed from, and of which object.
  [[nodiscard]] std::optional<HeldGuard>
  guardOn(const clang::Expr *argument) const {
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
    // object, unless its own thread, or one it started, may already run;
    // member initializers run before it starts.
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    if (object.has_value() && *object == nullptr && method != nullptr &&
        llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(
            method) &&
        ((info->threads.empty() && !flow.started) || thread ||
         !parents.hasParent(&member)))
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
            crossing = kinds.startOf(expression))
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
            kinds.startOf(*strip(call.arguments.front().expression)) ==
                std::nullopt)
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

  /// RFC 0052 §5.1: the objects of the enclosing function a lambda's body
  /// may change: a variable it captured, what a captured reference names,
  /// what a captured view points to, and `*this` when it captures `this`.
  [[nodiscard]] Value lambdaObjects(const Flow &flow,
                                    const clang::LambdaExpr &lambda) {
    Changes changed;
    for (const clang::Stmt *instance : bodiesOf(lambda))
      lambdaChanges(instance, changed);
    Value objects;
    for (const clang::VarDecl *variable : changed.variables) {
      const clang::QualType type = variable->getType();
      // A capture by copy is the lambda's own: what it changes of the
      // enclosing function is what the copy points to; through a shared
      // pointer, what other holders already change (RFC 0044 §8).
      const auto *capture = llvm::find_if(
          lambda.captures(), [&](const clang::LambdaCapture &each) {
            return each.capturesVariable() &&
                   each.getCapturedVar()->getCanonicalDecl() == variable;
          });
      if (capture != lambda.capture_end() &&
          capture->getCaptureKind() == clang::LCK_ByCopy) {
        if (kinds.tracked(type) &&
            !CxxKinds::shared(type->getAsCXXRecordDecl()))
          objects.add(borrowOf(flow, *variable));
        continue;
      }
      if (type->isReferenceType()) {
        objects.add(borrowOf(flow, *variable));
        continue;
      }
      objects.add(at(*variable));
      if (kinds.tracked(type))
        objects.add(borrowOf(flow, *variable));
    }
    objects.through.clear();
    // `*this`'s members the body changes, or all of it; and what a view
    // member of it points to.
    if (changed.self) {
      objects.add({.root = {.kind = RootKind::This}, .depth = 0});
      objects.add(
          {.root = {.kind = RootKind::This, .beyond = true}, .depth = 0});
    }
    for (const clang::FieldDecl *member : changed.members) {
      objects.add(
          {.root = {.kind = RootKind::This, .field = member}, .depth = 0});
      objects.add(
          {.root = {.kind = RootKind::This, .field = member, .beyond = true},
           .depth = 0});
    }
    return objects;
  }

  /// RFC 0052 §5.1 (D5): a call of a lambda the function can see, or a
  /// call it is passed to, ends the borrows into what its body may change.
  /// A callable the function cannot see is lent (and believed, §5.2) or
  /// kept (and captures nothing, §5.3): its call ends nothing.
  /// RFC 0053 §1: a callable type: a lambda, an erased callable, any class
  /// with a call operator (a functor, a binder), a pointer to one, or a
  /// pointer to a member function.
  [[nodiscard]] static bool anyCallable(clang::QualType type) {
    type = type.getNonReferenceType();
    if (type->isPointerType())
      type = type->getPointeeType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr)
      return type->isMemberFunctionPointerType();
    return callOperator(record, 0);
  }
  [[nodiscard]] static bool callOperator(const clang::CXXRecordDecl *record,
                                         unsigned depth) {
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr || depth > 8)
      return false;
    for (const clang::Decl *declared : record->decls()) {
      const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(declared);
      if (const auto *generic =
              llvm::dyn_cast<clang::FunctionTemplateDecl>(declared))
        method =
            llvm::dyn_cast<clang::CXXMethodDecl>(generic->getTemplatedDecl());
      if (method != nullptr &&
          method->getOverloadedOperator() == clang::OO_Call)
        return true;
    }
    return llvm::any_of(
        record->bases(), [&](const clang::CXXBaseSpecifier &base) {
          return callOperator(base.getType()->getAsCXXRecordDecl(), depth + 1);
        });
  }

  /// RFC 0053 §1 (D1): what a call of a callable reaches: where its loans
  /// point. For a lambda the function wrote, only the captures its body
  /// does not provably only read (the narrowing): a capture of `this`
  /// reaches the members the body changes (RFC 0052 amendment 4); a view
  /// or a callable captured reaches where it points. A callable the loans
  /// reach (a lambda behind a pointer another captured) reaches what its
  /// own loans do.
  [[nodiscard]] Value reachOf(const Flow &flow, const clang::Expr *callable) {
    Value reach;
    if (const clang::LambdaExpr *lambda = lambdaOf(callable))
      reach = lambdaReach(flow, *lambda);
    else
      reach = callable->getType()->isPointerType()
                  ? viewOf(flow, pointee(flow, callable))
                  : points(flow, callable);
    reach.through.clear();
    return deepen(flow, std::move(reach));
  }

  /// RFC 0053 §1, §5: what a call that reaches `value` reaches through the
  /// views and callables it names: a local view or callable it points at
  /// or into (a lambda behind a pointer another one captured; a holder of
  /// an `M &` whose `M` holds a `V &`) points where its own loans do, and
  /// so on.
  [[nodiscard]] static Value deepen(const Flow &flow, Value value) {
    llvm::SmallPtrSet<const clang::VarDecl *, 4> seen;
    for (std::size_t at = 0; at < value.loans.size(); ++at) {
      const clang::VarDecl *variable = value.loans[at].root.variable();
      if (variable == nullptr || value.loans[at].root.beyond ||
          variable->getType()->isReferenceType() ||
          variable->getType()->isPointerType() || !seen.insert(variable).second)
        continue;
      const auto found = flow.borrows.find(variable);
      if (found == flow.borrows.end())
        continue;
      const Loans inner = found->second.loans;
      for (const Loan &loan : inner)
        value.add(loan);
    }
    return value;
  }

  /// What a call of a callable may change: what its loans reach, unless it
  /// holds views of `const` only (`std::bind(f, std::cref(v))`).
  [[nodiscard]] Value changedBy(const Flow &flow, const clang::Expr *callable) {
    clang::QualType type = callable->getType().getNonReferenceType();
    if (type->isPointerType())
      type = type->getPointeeType();
    if (lambdaOf(callable) == nullptr && kinds.readsOnly(type))
      return {};
    return reachOf(flow, callable);
  }

  [[nodiscard]] Value lambdaReach(const Flow &flow,
                                  const clang::LambdaExpr &lambda) {
    Value objects;
    const auto *init = lambda.capture_init_begin();
    for (const clang::LambdaCapture &capture : lambda.captures()) {
      const clang::Expr *given = *init++;
      if (capture.capturesThis()) {
        if (capture.getCaptureKind() == clang::LCK_This) {
          Changes changed;
          for (const clang::Stmt *instance : bodiesOf(lambda))
            lambdaChanges(instance, changed);
          if (changed.self) {
            objects.add({.root = {.kind = RootKind::This}, .depth = 0});
            objects.add(
                {.root = {.kind = RootKind::This, .beyond = true}, .depth = 0});
          }
          for (const clang::FieldDecl *member : changed.members) {
            objects.add({.root = {.kind = RootKind::This, .field = member},
                         .depth = 0});
            objects.add({.root = {.kind = RootKind::This,
                                  .field = member,
                                  .beyond = true},
                         .depth = 0});
          }
        }
        continue;
      }
      if (!capture.capturesVariable() || given == nullptr)
        continue;
      const clang::ValueDecl *variable = capture.getCapturedVar();
      // A view's `const` is shallow: only a view of `const` is read.
      const clang::QualType held = variable->getType().getNonReferenceType();
      const bool view = kinds.tracked(held) && kinds.writesThrough(held);
      if (!view && !anyCallable(variable->getType()) &&
          !kinds.mutableOwners(held) && lambdaReads(lambda, *variable))
        continue;
      if (capture.getCaptureKind() == clang::LCK_ByRef)
        objects.add(object(flow, given));
      if (kinds.tracked(variable->getType()) &&
          !CxxKinds::shared(variable->getType()->getAsCXXRecordDecl()))
        objects.add(points(flow, given));
    }
    return objects;
  }

  /// RFC 0052 §5.1, RFC 0053 §1 (D1): a call of a callable value, or a call
  /// it is passed to, ends the borrows into what its loans reach. A
  /// callable the function was lent is believed (§5.2); one that is kept
  /// holds no borrow (§5.3).
  void closures(Flow &flow, const Call &call, const clang::Expr &expression) {
    if (call.method != nullptr && call.object != nullptr &&
        call.method->getOverloadedOperator() == clang::OO_Call &&
        anyCallable(objectType(call))) {
      invalidate(flow, changedBy(flow, call.object), Ends::Into, End::Why::Runs,
                 call.at, call.method, call.object);
    }
    // A call through a pointer to a member function reaches what its object
    // points to, as a member function of a view does.
    if (const auto *plain = llvm::dyn_cast<clang::CallExpr>(&expression))
      if (const auto *member = llvm::dyn_cast<clang::BinaryOperator>(
              plain->getCallee()->IgnoreParens());
          member != nullptr && member->isPtrMemOp()) {
        const clang::Expr *of = member->getLHS();
        invalidate(flow,
                   deepen(flow, of->getType()->isPointerType()
                                    ? viewOf(flow, pointee(flow, of))
                                    : viewOf(flow, object(flow, of))),
                   Ends::Into, End::Why::Reached, call.at, nullptr, of);
      }
    // RFC 0053 §1, §4: a library object that holds a callable (a range
    // adaptor, `std::views::transform`'s view and its iterators) runs it
    // when a library call is given it or is called on it.
    // (Access to an element, which has a `const` twin, runs nothing: a
    // vector of lambdas gives one, an adaptor's iterator runs it.)
    if (call.callee != nullptr && isLibrary(*call.callee)) {
      if (call.object != nullptr && !anyCallable(objectType(call)) &&
          (call.method == nullptr || !kinds.accessor(*call.method)) &&
          holdsCallable(objectType(call)))
        invalidate(flow, deepen(flow, points(flow, call.object)), Ends::Into,
                   End::Why::Runs, call.at, call.callee, call.object);
      for (const Call::Argument &argument : call.arguments)
        if (!anyCallable(argument.expression->getType()) &&
            holdsCallable(argument.expression->getType()) &&
            !keeps(argument.parameter) &&
            (argument.parameter == nullptr ||
             !CxxKinds::accessorParameter(*argument.parameter)))
          invalidate(flow, deepen(flow, points(flow, argument.expression)),
                     Ends::Into, End::Why::Runs, call.at, call.callee,
                     argument.expression);
    }
    // §5.3: a local that keeps lambdas the function sees.
    for (const clang::LambdaExpr *lambda : keptIn(call.object))
      invalidate(flow, lambdaObjects(flow, *lambda), Ends::Into, End::Why::Runs,
                 call.at, call.callee, lambda);
    for (const Call::Argument &argument : call.arguments)
      for (const clang::LambdaExpr *lambda : keptIn(argument.expression))
        invalidate(flow, lambdaObjects(flow, *lambda), Ends::Into,
                   End::Why::Runs, call.at, call.callee, lambda);
    // A callable the callee keeps captures nothing (§5.3); the library's
    // callable made from one wraps it, and runs it when it is called.
    if (llvm::isa_and_nonnull<clang::CXXConstructorDecl>(call.callee) &&
        isLibrary(*call.callee) &&
        kinds.erased(llvm::cast<clang::CXXConstructorDecl>(call.callee)
                         ->getFunctionObjectParameterType()))
      return;
    if (llvm::isa_and_nonnull<clang::CXXConstructorDecl>(call.callee))
      return;
    for (const Call::Argument &argument : call.arguments)
      if (anyCallable(argument.expression->getType()) &&
          !keeps(argument.parameter))
        invalidate(flow, changedBy(flow, argument.expression), Ends::Into,
                   End::Why::Runs, call.at, call.callee, argument.expression);
  }

  /// A library class one of whose type arguments (or its enclosing
  /// class's, for an iterator) is a callable, or holds one.
  [[nodiscard]] bool holdsCallable(clang::QualType type, unsigned depth = 0) {
    type = type.getNonReferenceType();
    if (type->isPointerType())
      type = type->getPointeeType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > 8 || !isLibrary(*record))
      return false;
    if (kinds.erased(type) && anyCallable(type))
      return true;
    for (const clang::DeclContext *context = record; context != nullptr;
         context = context->getParent())
      if (const auto *specialization =
              llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(context);
          specialization != nullptr &&
          llvm::any_of(CxxKinds::typeArguments(*specialization),
                       [&](clang::QualType argument) {
                         // A lambda or a functor of the program; a
                         // callable of the library only through what it
                         // holds (`std::reference_wrapper<V>` has a call
                         // operator for every `V`), or by erasing it. (A
                         // deleter, a comparator or a hash with no state
                         // holds no loan.)
                         const clang::CXXRecordDecl *of =
                             argument->getAsCXXRecordDecl();
                         return (of != nullptr && of->hasDefinition() &&
                                 !of->getDefinition()->isEmpty() &&
                                 anyCallable(argument) && !isLibrary(*of)) ||
                                holdsCallable(argument, depth + 1);
                       }))
        return true;
    return false;
  }

  /// How a callable is named in a message: a lambda written in place is
  /// "the lambda", anything else as it is spelled.
  [[nodiscard]] std::string callableName(const clang::Expr *expression) const {
    if (CxxKinds::lambdaIn(expression) != nullptr)
      return "the lambda";
    return spell(expression, ast);
  }

  /// A callable that erases what it holds (`std::function`), or a library
  /// container of them; a thread object runs what it is given (RFC 0046).
  [[nodiscard]] bool keepsCallables(clang::QualType type, unsigned depth = 0) {
    type = type.getNonReferenceType();
    if (callableType(type) && !type->getAsCXXRecordDecl()->isLambda())
      return true;
    const auto *specialization =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            type->getAsCXXRecordDecl());
    return depth < 4 && specialization != nullptr &&
           isLibrary(*specialization) && !kinds.holdsThread(type) &&
           llvm::any_of(
               CxxKinds::typeArguments(*specialization),
               [&](clang::QualType argument) {
                 return keepsCallables(argument, depth + 1);
               });
  }

  /// A lambda, or a class with a call operator that erases
  /// (`std::function`).
  [[nodiscard]] bool callableType(clang::QualType type) {
    type = type.getNonReferenceType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr)
      return false;
    if (record->isLambda())
      return true;
    if (!kinds.erased(type))
      return false;
    record = record->getDefinition();
    return record != nullptr &&
           llvm::any_of(record->methods(), [](const clang::CXXMethodDecl *m) {
             return m->getOverloadedOperator() == clang::OO_Call;
           });
  }

  /// RFC 0052 §5.3 (D6): a callable that is kept (in a member, an element,
  /// a global or a `static`; by a parameter that keeps it) captures no
  /// reference, no `this` and no pointer or view. Whether it was reported.
  bool keptCallable(const Flow &flow, const clang::Expr *given,
                    clang::SourceLocation at, const std::string &keeper) {
    const clang::LambdaExpr *lambda = lambdaOf(given);
    if (lambda == nullptr) {
      // RFC 0053 §3 (D2): any other callable, by its loans: a binder, a
      // functor, a factory's result.
      if (given == nullptr)
        return false;
      const clang::Expr *source = strip(given);
      if (const auto *plain = llvm::dyn_cast<clang::CallExpr>(source);
          plain != nullptr && passesThrough(*plain))
        source = plain->getArg(0);
      source = source->IgnoreParenImpCasts();
      const bool named =
          llvm::isa<clang::DeclRefExpr, clang::MemberExpr>(source);
      const Value held =
          named ? viewOf(flow, object(flow, source)) : points(flow, given);
      // What a parameter or a member was handed is its giver's to check;
      // an owner shared by a `std::shared_ptr` is kept by it.
      const auto *const loan = llvm::find_if(held.loans, [](const Loan &each) {
        return !each.root.beyond && each.root.kind != RootKind::Global &&
               (each.root.variable() == nullptr ||
                !CxxKinds::shared(
                    each.root.variable()->getType()->getAsCXXRecordDecl()));
      });
      if (loan == held.loans.end())
        return false;
      error(at, rule::Escape,
            spell(given, ast) + " is kept by " + keeper +
                " and holds a borrow of " + nameOf(loan->root),
            at, CxxKinds::keptFix(false).str());
      return true;
    }
    const std::string what = kinds.keptCapture(*lambda);
    if (what.empty())
      return false;
    const bool self = what == "'this'";
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    const bool sharedClass = self && method != nullptr &&
                             CxxKinds::sharesFromThis(method->getParent());
    error(at, rule::Escape,
          callableName(given) + " is kept by " + keeper + " and captures " +
              what,
          lambda->getBeginLoc(), CxxKinds::keptFix(sharedClass).str());
    return true;
  }

  void call(Flow &flow, const clang::Expr *expression) {
    if (const auto *plain = llvm::dyn_cast<clang::CallExpr>(expression);
        plain != nullptr && passesThrough(*plain))
      return;
    const Call call = describe(*expression);
    if (kinds.threadRules)
      threads(flow, call, *expression);
    // RFC 0052 §5: what a thread start gives a thread is RFC 0046's.
    const bool starts = kinds.startOf(*expression).has_value();
    if (!starts)
      closures(flow, call, *expression);
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
    // RFC 0053 §5: an assignment of a library object of references (a
    // `std::tuple<V &>`, `std::tie`'s) changes what they refer to; a call
    // that runs a destructor the program wrote (`reset` of an optional of
    // a view) reaches what the object's views point to. (Read before the
    // assignment rebinds the object.)
    Value referred;
    if (call.object != nullptr && call.method != nullptr &&
        isLibrary(*call.method) &&
        ((assigns && referencesIn(objectType(call))) ||
         (changing(call) && kinds.runsProgram(objectType(call)))))
      referred = deepen(flow, viewOf(flow, target));
    alias(flow, call, call.object != nullptr ? &target : nullptr, targetChanges,
          starts);
    endCharacters(flow, call,
                  targetChanges &&
                      (target.reference || throughShared(target, call.object)));

    // §5: moves.
    for (const Call::Argument &argument : call.arguments)
      if (takes(argument.parameter))
        if (const std::optional<Place> place = movedFrom(argument.expression))
          move(flow, *place, argument.expression->getExprLoc());

    // RFC 0052 §5.3: a callable stored in what erases its contents (a
    // `std::function`, a container of them) is kept.
    bool keptReported = false;
    if (call.object != nullptr && call.method != nullptr &&
        !call.method->isConst() && keepsCallables(objectType(call)))
      for (const Call::Argument &argument : call.arguments)
        keptReported |= keptCallable(flow, argument.expression,
                                     argument.expression->getExprLoc(),
                                     spell(call.object, ast));
    // §7: a view that is assigned, or that may store what it is passed.
    const clang::VarDecl *view =
        call.object != nullptr ? viewLocal(call.object) : nullptr;
    // A member of a local view that is assigned or changed: the local
    // takes the borrows too.
    const clang::VarDecl *holder = call.object != nullptr && view == nullptr
                                       ? memberOfLocal(call.object)
                                       : nullptr;
    if (holder != nullptr && changing(call) && !taken.loans.empty() &&
        kinds.tracked(objectType(call))) {
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
      } else if (const clang::VarDecl *keeper = ownObject(call.object);
                 keeper != nullptr && call.arguments.size() == 1 &&
                 !kinds.tracked(keeper->getType())) {
        // RFC 0053 §5: a local assigned what keeps a borrow (a shared
        // pointer the aliasing constructor made) holds that borrow, and
        // no longer the one it held.
        const clang::Expr *given = strip(call.arguments[0].expression);
        if (const auto *temporary =
                llvm::dyn_cast_or_null<clang::MaterializeTemporaryExpr>(given))
          given = temporary->getSubExpr();
        const Value kept = keptBy(flow, given);
        if (!kept.loans.empty())
          flow.borrows[keeper] = {
              .loans = kept.loans, .reference = false, .end = {}};
        else if (const auto found = flow.borrows.find(keeper);
                 found != flow.borrows.end() && !found->second.loans.empty())
          flow.borrows.erase(found);
      }
    } else if (view != nullptr && changing(call)) {
      Borrow &borrow = flow.borrows[view];
      for (const Loan &loan : taken.loans)
        if (!llvm::is_contained(borrow.loans, loan))
          borrow.loans.push_back(loan);
    }
    // (What a parameter that keeps its argument stores is reported as kept
    // by the callee, below.)
    const bool keptByCallee =
        llvm::any_of(call.arguments, [&](const Call::Argument &argument) {
          return keeps(argument.parameter) &&
                 (argument.expression->getType()->isPointerType() ||
                  kinds.tracked(argument.expression->getType()));
        });
    if (call.object != nullptr && view == nullptr && changing(call) &&
        !keptReported && !keptByCallee && kinds.tracked(objectType(call)) &&
        !onlyPolymorphic(objectType(call)) && outside(target)) {
      if (global(call.object) && !taken.loans.empty())
        error(call.at, rule::Escape,
              "a borrow is stored in the global " + spell(call.object, ast) +
                  ", which outlives every function",
              call.at,
              "keep in a global only what owns its data (a 'std::string', "
              "not a 'std::string_view')");
      else
        // RFC 0052 §6: a class that borrows only through raw pointer
        // members keeps references (RFC 0044 §8), stored as a pointer is.
        storedOutside(target, taken, call.at, spell(call.object, ast),
                      /*viewTarget=*/viewOrErased(objectType(call)));
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

    // RFC 0053 §7 (D7): a `const` member function of a class with
    // `mutable` owners may change them: the caller's borrows into its
    // object end (`owner-path` is not asked of it).
    if (call.object != nullptr && call.method != nullptr &&
        call.method->isConst() && !call.method->isStatic() &&
        kinds.mutableOwners(objectType(call)))
      invalidate(flow, target, Ends::Into, End::Why::Changed, call.at,
                 call.method);
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
    if (!referred.loans.empty())
      invalidate(flow, referred, Ends::Into, End::Why::Changed, call.at,
                 call.method);
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
        keepLambda(keeper, given);
      } else if (constructs && declaresLocal(expression)) {
        // RFC 0052 §5.3: a local made with a callable it keeps keeps one
        // the function sees.
        keepLambda(declaredBy(expression), given);
      } else if (sends(argument.parameter) ||
                 !keepsCallables(argument.parameter->getType()) ||
                 !keptCallable(flow, given, given->getExprLoc(),
                               "what " + called(call.callee) + " keeps")) {
        // RFC 0052 §5.3: a kept callable is reported as one, else as a view.
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
      // §7: a view passed where the callee may store the other borrows;
      // RFC 0053 §5: one the callee may assign, owners or not (an out
      // parameter of a `std::optional` of a reference wrapper).
      if (const clang::VarDecl *widened = outParameter(argument.parameter)
                                              ? viewLocal(addressed(given))
                                              : nullptr) {
        Borrow &borrow = flow.borrows[widened];
        for (const Call::Argument &other : call.arguments)
          if (&other != &argument)
            for (const Loan &loan : outStored(flow, other, *widened).loans)
              if (!llvm::is_contained(borrow.loans, loan))
                borrow.loans.push_back(loan);
      }
      if (!changes(argument.parameter) || movedFrom(given).has_value())
        continue;
      const Value objects = given->getType()->isPointerType()
                                ? pointee(flow, given)
                                : object(flow, given);
      // RFC 0053 §7 (D7): what is changed only through a `mutable` member
      // ends the caller's borrows, and asks nothing of `owner-path`.
      if (!argument.parameter->getType()->getPointeeType().isConstQualified())
        owners(objects, given->getExprLoc(), spell(given, ast), call.callee,
               given, argument.parameter->getType()->getPointeeType());
      invalidate(flow, objects, Ends::Into, End::Why::Called, call.at,
                 call.callee);
    }
    // RFC 0052 §4.1 (D4): a view a function of the program is given may
    // change what it points to, whatever the view's own `const`; so may a
    // member function of the program's own view class, through its
    // object. The library's functions are trusted with what their
    // specification allows (X-A4).
    // RFC 0053 §4 (D3): but a library function given a callable, or the
    // library's callable itself, calls it with the rest.
    const bool givenCallable =
        llvm::any_of(call.arguments,
                     [](const Call::Argument &argument) {
                       const clang::QualType given =
                           argument.expression->getType();
                       return anyCallable(given) ||
                              given->isFunctionPointerType() ||
                              given->isFunctionType();
                     }) ||
        (call.method != nullptr && call.object != nullptr &&
         call.method->getOverloadedOperator() == clang::OO_Call &&
         anyCallable(objectType(call)));
    // RFC 0053 §7 (D6): a call of a function of the program may change any
    // global: a function or constructor it wrote, a call through a pointer
    // to a function, a library function that is given a callable or makes,
    // copies or destroys objects whose class the program wrote.
    const bool program =
        call.callee == nullptr
            ? !constructs
            : !isLibrary(*call.callee) || givenCallable ||
                  (call.object != nullptr &&
                   kinds.runsProgram(objectType(call))) ||
                  llvm::any_of(call.arguments, [&](const Call::Argument &a) {
                    return kinds.runsProgram(a.expression->getType());
                  });
    if (program)
      changesGlobals(flow, call.at, call.callee);
    if (constructs ||
        (call.callee != nullptr && isLibrary(*call.callee) && !givenCallable))
      return;
    for (const Call::Argument &argument : call.arguments)
      if (writable(argument.expression->getType()))
        invalidate(flow,
                   deepen(flow, onlyPolymorphic(argument.expression->getType())
                                    ? known(flow, argument.expression)
                                    : points(flow, argument.expression)),
                   Ends::Into, End::Why::Reached, call.at, call.callee,
                   argument.expression);
    if (writesThroughObject(call)) {
      Value reached;
      if (llvm::isa<clang::CXXThisExpr>(call.object->IgnoreParenImpCasts())) {
        // RFC 0044 amendment 3 stands for a `const` member function of the
        // class's own: what its reference members name is a root of the
        // function's own. One that is not `const` may change it.
        if (!call.method->isConst() && !onlyPolymorphic(objectType(call)))
          reached.add(
              {.root = {.kind = RootKind::This, .beyond = true}, .depth = 0});
      } else {
        reached = deepen(flow, onlyPolymorphic(objectType(call))
                                   ? known(flow, call.object)
                                   : viewOf(flow, objectOf(flow, call)));
      }
      // (A virtual call names the object it reaches through.)
      invalidate(flow, reached, Ends::Into, End::Why::Reached, call.at,
                 call.method, call.method->isVirtual() ? call.object : nullptr);
    }
  }

  /// A library template with a reference among its type arguments.
  [[nodiscard]] static bool referencesIn(clang::QualType type) {
    const auto *specialization =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            type.getNonReferenceType()->getAsCXXRecordDecl());
    return specialization != nullptr &&
           llvm::any_of(CxxKinds::typeArguments(*specialization),
                        [](clang::QualType argument) {
                          return argument->isReferenceType();
                        });
  }

  /// RFC 0053 §7 (D6): code of the program runs, which may change any
  /// global.
  static void changesGlobals(Flow &flow, clang::SourceLocation at,
                             const clang::NamedDecl *by) {
    invalidate(
        flow,
        Value{.loans = {{.root = {.kind = RootKind::Global}, .depth = 0}}},
        Ends::Into, End::Why::Called, at, by);
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
    // RFC 0046 §6: a pointer used as an lvalue but by an assignment (its
    // address taken, bound to a reference) may change where the flow does
    // not see.
    if (const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(statement))
      if (const clang::VarDecl *variable = variableOf(*named);
          variable != nullptr && variable->getType()->isPointerType() &&
          !assigned.contains(named)) {
        const auto *cast = llvm::dyn_cast_or_null<clang::ImplicitCastExpr>(
            parents.getParent(named));
        if (cast == nullptr || cast->getCastKind() != clang::CK_LValueToRValue)
          reseated.insert(variable->getCanonicalDecl());
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
    // Assigning a reference writes what it names: a use of the borrow
    // (RFC 0047 amendment; RFC 0044's flow skipped it).
    const bool written = assigned.contains(&named);
    if (written && !variable->getType()->isReferenceType())
      return;
    variable = variable->getCanonicalDecl();
    if (!written)
      usePlace(flow, {variable, nullptr}, named.getLocation(),
               variable->getNameAsString(), &named);
    const auto found = flow.borrows.find(variable);
    if (found == flow.borrows.end())
      return;
    // What was moved from may be assigned (X-A5).
    if (found->second.end.why != End::Why::None &&
        (!written || found->second.end.why != End::Why::Moved)) {
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
    if (!variable.hasLocalStorage()) {
      // RFC 0052 §5.3: a `static` callable is kept.
      if (keepsCallables(variable.getType()))
        keptCallable(flow, variable.getInit(), variable.getLocation(),
                     "the static '" + variable.getNameAsString() + "'");
      return;
    }
    if (kinds.threadRules) {
      if (CxxKinds::guardClass(variable.getType()))
        declareGuard(flow, variable);
      // §5: a thread object moved into a new local brings its loans.
      if (kinds.holdsThread(variable.getType()) &&
          variable.getInit() != nullptr)
        moveLends(flow, variable.getInit(), canonical);
    }
    // RFC 0052 §5.3: a copy or a move of a local keeper keeps its lambdas.
    if (const clang::Expr *init = variable.getInit())
      for (const clang::LambdaExpr *lambda :
           llvm::SmallVector<const clang::LambdaExpr *, 1>(
               keptIn(returnedName(init))))
        if (!llvm::is_contained(keptLambdas[canonical], lambda))
          keptLambdas[canonical].push_back(lambda);
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
    // RFC 0052 §6: a shared pointer the aliasing constructor makes is a
    // view of what its second argument points into.
    if (CxxKinds::aliasing(*construct->getConstructor()))
      kept.add(pointee(flow, construct->getArg(1)));
    return kept;
  }

  /// RFC 0052 §5.3: the lambdas each local the function made keeps, which
  /// the function sees: a call on the local, or one it is passed to, may
  /// call them.
  llvm::DenseMap<const clang::VarDecl *,
                 llvm::SmallVector<const clang::LambdaExpr *, 1>>
      keptLambdas;

  void keepLambda(const clang::VarDecl *keeper, const clang::Expr *given) {
    const clang::LambdaExpr *lambda = lambdaOf(given);
    if (keeper == nullptr || lambda == nullptr)
      return;
    auto &lambdas = keptLambdas[keeper->getCanonicalDecl()];
    if (!llvm::is_contained(lambdas, lambda))
      lambdas.push_back(lambda);
  }

  /// The local a construction initializes where it is declared.
  [[nodiscard]] const clang::VarDecl *
  declaredBy(const clang::Expr *construct) const {
    const clang::Stmt *parent = parents.getParent(construct);
    while (parent != nullptr &&
           llvm::isa<clang::ExprWithCleanups, clang::CXXBindTemporaryExpr,
                     clang::ParenExpr>(parent))
      parent = parents.getParent(parent);
    const auto *declaration = llvm::dyn_cast_or_null<clang::DeclStmt>(parent);
    return declaration == nullptr || !declaration->isSingleDecl()
               ? nullptr
               : llvm::dyn_cast<clang::VarDecl>(declaration->getSingleDecl());
  }

  /// The lambdas a local keeper named by `expression` (or a pointer to
  /// one) keeps.
  [[nodiscard]] llvm::ArrayRef<const clang::LambdaExpr *>
  keptIn(const clang::Expr *expression) {
    if (expression == nullptr)
      return {};
    expression = strip(expression)->IgnoreParenImpCasts();
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expression);
        unary != nullptr && unary->getOpcode() == clang::UO_AddrOf)
      expression = unary->getSubExpr()->IgnoreParenImpCasts();
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(expression);
    const clang::VarDecl *variable =
        named == nullptr ? nullptr : variableOf(*named);
    if (variable == nullptr)
      return {};
    const auto found = keptLambdas.find(variable->getCanonicalDecl());
    return found == keptLambdas.end()
               ? llvm::ArrayRef<const clang::LambdaExpr *>()
               : llvm::ArrayRef<const clang::LambdaExpr *>(found->second);
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
        // RFC 0046 §6: the guards taken through it were on another object.
        llvm::erase_if(flow.guards, [&](const HeldGuard &held) {
          return held.object == variable->getCanonicalDecl();
        });
        // What was moved out through it was another object's.
        unmove(flow, {variable->getCanonicalDecl(), nullptr});
        return;
      }
    // RFC 0053 §5: a pointer member of a local the flow follows (a struct
    // of a pointer) that is assigned the address of a local: the struct
    // does not outlive it. (Its pointer is a reference otherwise, RFC 0044
    // §8, tested where it is loaded.)
    if (const clang::VarDecl *holder = memberOfLocal(left);
        holder != nullptr && kinds.tracked(holder->getType()))
      for (const Loan &loan : value.loans)
        if (localRoot(loan.root)) {
          Borrow &borrow = flow.borrows[holder];
          if (!llvm::is_contained(borrow.loans, loan))
            borrow.loans.push_back(loan);
        }
    const Value target = object(flow, left);
    if (outside(target)) {
      storedOutside(target, value, binary.getOperatorLoc(), spell(left, ast),
                    /*viewTarget=*/false);
      switchingStore(value, binary.getOperatorLoc(), spell(left, ast),
                     left->getType()->getPointeeType());
    }
  }

  /// `std::initializer_list`: a library view whose only members point at
  /// the array of `const` elements the compiler made for it.
  [[nodiscard]] static bool initializerList(clang::QualType type) {
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || !isLibrary(*record) || !record->hasDefinition())
      return false;
    return llvm::any_of(
               record->getDefinition()->fields(),
               [](const clang::FieldDecl *field) {
                 return field->getType()->isPointerType() &&
                        field->getType()->getPointeeType().isConstQualified();
               }) &&
           llvm::isa<clang::ClassTemplateSpecializationDecl>(record) &&
           record->getDefinition()->field_begin() !=
               record->getDefinition()->field_end() &&
           std::next(record->getDefinition()->field_begin(), 2) ==
               record->getDefinition()->field_end();
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
    } else {
      value = keptBy(flow, returnedName(expression));
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
    // RFC 0053 §8: a function that marks a parameter `lifetimebound`
    // returns a borrow only of what it marks.
    if (llvm::any_of(function.parameters(), [](const clang::ParmVarDecl *each) {
          return each->hasAttr<clang::LifetimeBoundAttr>();
        }))
      for (const Loan &loan : value.loans) {
        const auto *parameter =
            llvm::dyn_cast_or_null<clang::ParmVarDecl>(loan.root.variable());
        if (parameter == nullptr ||
            parameter->hasAttr<clang::LifetimeBoundAttr>() ||
            !CxxKinds::pointerLike(parameter->getType()))
          continue;
        error(statement.getReturnLoc(), rule::Escape,
              "what is returned points into " + nameOf(loan.root) +
                  ", which is not marked WEAVEC_LIFETIMEBOUND",
              statement.getReturnLoc(),
              "mark the parameter it returns from WEAVEC_LIFETIMEBOUND, and "
              "only that one");
        return;
      }
    // RFC 0053 §6 (its Unresolved question 3): a value is returned with a
    // borrow of a parameter only if its type shows it (as its caller reads
    // the result: a member could name what the parameter reaches) or the
    // parameter is marked `lifetimebound` (a `std::unique_ptr<Base>` of a
    // derived class that holds a reference).
    if (const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
        record != nullptr && !type->isReferenceType() && !kinds.erased(type))
      for (const Loan &loan : value.loans) {
        const auto *parameter =
            llvm::dyn_cast_or_null<clang::ParmVarDecl>(loan.root.variable());
        if (parameter == nullptr || !ownLocal(*parameter) ||
            parameter->hasAttr<clang::LifetimeBoundAttr>() ||
            shows(record, *parameter, loan))
          continue;
        error(statement.getReturnLoc(), rule::Escape,
              "what is returned holds a borrow of " + nameOf(loan.root) +
                  ", which its type " + typeName(type) + " does not show",
              statement.getReturnLoc(),
              "mark the parameter WEAVEC_LIFETIMEBOUND, so that callers see "
              "what the result borrows; or return a type whose members show "
              "it");
        return;
      }
    // RFC 0053 §7 (D6): a caller reads a reference or pointer a function
    // of the program returns with nothing it could point into as at a
    // global, which a call of the program may assign but not destroy; so
    // one is not returned into a global, whose next value would not hold
    // it.
    if (CxxKinds::pointerLike(type))
      for (const Loan &loan : value.loans)
        if (loan.root.kind == RootKind::Global && loan.depth > 0) {
          error(statement.getReturnLoc(), rule::Escape,
                "what is returned points into " + nameOf(loan.root) +
                    ", which a call of the program may change",
                statement.getReturnLoc(),
                "return the global itself, or a copy of what is needed");
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
      // RFC 0052 §6: a value built from a borrow (a struct with a pointer
      // member, a list, an aliasing shared pointer, a range adaptor).
      const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
      if (record != nullptr && !kinds.viewClass(record) &&
          !kinds.erased(type)) {
        const bool list = initializerList(type);
        const std::string written = spell(expression, ast);
        error(
            statement.getReturnLoc(), rule::Escape,
            list ? "the 'std::initializer_list' that is returned points "
                   "into the array its braces made, which does not outlive "
                   "the function"
                 : (written.size() <= 40 ? written : std::string("the value")) +
                       " holds a borrow of " + nameOf(loan.root) +
                       " and is returned",
            statement.getReturnLoc(),
            list ? "return a container that owns its elements "
                   "('std::vector', 'std::array')"
                 : "return what owns its data, or take " + nameOf(loan.root) +
                       " as a parameter so that the caller keeps it");
        return;
      }
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

  /// Whether a caller that reads a result of class `record` by its type
  /// (`result`) sees the borrow `loan` of `parameter` in it.
  [[nodiscard]] bool shows(const clang::CXXRecordDecl *record,
                           const clang::ParmVarDecl &parameter,
                           const Loan &loan) {
    const clang::QualType type = parameter.getType();
    // What a shared pointer owns, another made from it owns too.
    if (CxxKinds::shared(record) &&
        CxxKinds::shared(type.getNonReferenceType()->getAsCXXRecordDecl()))
      return true;
    if (type->isPointerType())
      return kinds.mayBorrow(record, type->getPointeeType());
    if (type->isReferenceType())
      return kinds.mayBorrow(record, type->getPointeeType()) ||
             (loan.root.beyond &&
              kinds.mayBorrowThrough(record, type->getPointeeType()));
    return !kinds.tracked(type) || kinds.mayBorrowThrough(record, type);
  }

  /// RFC 0052 §5.3: a callable that initializes a member or an element of
  /// an aggregate is kept by it.
  void keptInList(const Flow &flow, const clang::InitListExpr &list) {
    const clang::CXXRecordDecl *record = list.getType()->getAsCXXRecordDecl();
    if (record != nullptr && record->hasDefinition() &&
        list.getType()->isAggregateType() && !record->isUnion()) {
      auto field = record->field_begin();
      for (unsigned i = record->getNumBases();
           i < list.getNumInits() && field != record->field_end(); ++i, ++field)
        if (keepsCallables(field->getType()))
          keptCallable(flow, list.getInit(i), list.getInit(i)->getExprLoc(),
                       "'" + field->getNameAsString() + "'");
      return;
    }
    if (const clang::ArrayType *array = ast.getAsArrayType(list.getType());
        array != nullptr && keepsCallables(array->getElementType()))
      for (const clang::Expr *init : list.inits())
        keptCallable(flow, init, init->getExprLoc(), "an element of an array");
  }

  void initialize(Flow &flow, const clang::CXXCtorInitializer &initializer) {
    const clang::FieldDecl *field = initializer.getMember();
    if (field == nullptr || initializer.getInit() == nullptr)
      return;
    const clang::QualType type = field->getType();
    if (keepsCallables(type) &&
        keptCallable(flow, initializer.getInit(),
                     initializer.getSourceLocation(),
                     "'" + field->getNameAsString() + "'"))
      return;
    if (!kinds.tracked(type) || type->isFunctionPointerType())
      return;
    const clang::Expr *init = initializer.getInit();
    const Value value = held(flow, type, init);
    Value target;
    target.loans.push_back(
        {.root = {.kind = RootKind::This, .field = field}, .depth = 0});
    storedOutside(target, value, initializer.getSourceLocation(),
                  "'" + field->getNameAsString() + "'",
                  /*viewTarget=*/!type->isPointerType() && viewOrErased(type));
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
    } else if (const auto *list =
                   llvm::dyn_cast<clang::InitListExpr>(statement)) {
      keptInList(flow, *list);
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
      } else if (const auto destructor =
                     element.getAs<clang::CFGImplicitDtor>()) {
        // RFC 0053 §7 (D6): a destructor the program wrote may change any
        // global.
        // RFC 0053 §1, §5: and what the object's views point to.
        const auto automatic = element.getAs<clang::CFGAutomaticObjDtor>();
        const auto temporary = element.getAs<clang::CFGTemporaryDtor>();
        if (const clang::CXXDestructorDecl *run =
                destructor->getDestructorDecl(ast);
            run != nullptr &&
            kinds.runsProgram(ast.getCanonicalTagType(run->getParent()))) {
          const clang::SourceLocation at =
              automatic ? automatic->getTriggerStmt()->getEndLoc()
                        : run->getLocation();
          changesGlobals(flow, at, run);
          Value held;
          if (automatic && flow.borrows.contains(
                               automatic->getVarDecl()->getCanonicalDecl()))
            held = borrowOf(flow, *automatic->getVarDecl());
          else if (temporary)
            held = points(flow, temporary->getBindTemporaryExpr());
          invalidate(flow, deepen(flow, held), Ends::Into, End::Why::Reached,
                     at, run);
        }
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
      std::string name = "*this";
      if (place.second != nullptr)
        name = place.second->getNameAsString();
      else if (place.first != nullptr)
        name = place.first->getNameAsString();
      if (emit && reportedMoves.emplace(place, where.getRawEncoding()).second)
        report(at, rule::OwnerMoved,
               "'" + name + "' is left moved when the function returns", where,
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
