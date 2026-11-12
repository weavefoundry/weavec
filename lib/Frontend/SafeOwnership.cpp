//===- SafeOwnership.cpp - Ownership's rules ------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0043 §2–§10. A pointer declared `WEAVEC_OWNED` is the one responsible
// for releasing its block. For each safe function, one forward dataflow
// over Clang's CFG (every expression an element, in evaluation order, and
// the ends of locals' lifetimes) follows:
//
//   - the owners the function names exactly (§4): a local or a parameter,
//     and a path of field selections and of dereferences of owners from
//     one, each _null_, _holds_, _moved_ or waiting for `realloc`'s result
//     to be tested, or a mix of these where paths joined;
//   - its local borrows (§5): the owners each points below, and whether one
//     of them was moved, released or replaced since, by the function or by
//     a call that could.
//
// Errors are reported in a last pass over the converged states. Nothing
// here is load-bearing at run time: no guard and no quarantine is removed
// on its strength. Rules local to one function, written to be satisfied by
// rewriting; an expression the checker does not model for owners is
// rejected, not guessed at.
//
//===----------------------------------------------------------------------===//

#include "SafeCheckImpl.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace weavec::frontend {

// RFC 0043 G7: measures the rules' compile time (`-mllvm
// -weavec-ownership-rules=0`); not a user's switch.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static llvm::cl::opt<bool> ownershipRules("weavec-ownership-rules",
                                          llvm::cl::init(true),
                                          llvm::cl::Hidden);

static std::uintptr_t stepOf(const clang::FieldDecl *field) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<std::uintptr_t>(field);
}

static const clang::FieldDecl *fieldOf(std::uintptr_t step) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
  return reinterpret_cast<const clang::FieldDecl *>(step);
}

/// Concatenates strings without temporaries.
template <typename... Parts>
static std::string cat(const Parts &...parts) {
  std::string out;
  ((out += parts), ...);
  return out;
}

namespace {

namespace rule {
constexpr llvm::StringLiteral OwnerMoved = "owner-moved";
constexpr llvm::StringLiteral OwnerLeak = "owner-leak";
constexpr llvm::StringLiteral OwnerSource = "owner-source";
constexpr llvm::StringLiteral OwnerPath = "owner-path";
constexpr llvm::StringLiteral OwnerSlot = "owner-slot";
constexpr llvm::StringLiteral StaleBorrow = "stale-borrow";
constexpr llvm::StringLiteral CallAlias = "call-alias";
} // namespace rule

/// A step of a place's path: a field (its `FieldDecl *`), or one of these.
constexpr std::uintptr_t StepDeref = 1;
/// An element at an index (a dereference too): what follows is not named.
constexpr std::uintptr_t StepIndex = 2;
/// §4: an implementation limit on the fields of a named place.
constexpr unsigned MaxFields = 4;

/// A place in memory: a variable and a path from it.
struct Place {
  const clang::VarDecl *root = nullptr;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  llvm::SmallVector<std::uintptr_t, 6> path = {};

  bool operator<(const Place &other) const {
    if (root != other.root)
      return std::less<>()(root, other.root);
    return std::ranges::lexicographical_compare(path, other.path);
  }
  bool operator==(const Place &other) const {
    return root == other.root && path == other.path;
  }
  /// Whether `other` is this place or below it.
  [[nodiscard]] bool covers(const Place &other) const {
    return root == other.root && path.size() <= other.path.size() &&
           std::equal(path.begin(), path.end(), other.path.begin());
  }
  [[nodiscard]] Place with(std::uintptr_t step) const {
    Place next = *this;
    next.path.push_back(step);
    return next;
  }
};

/// Somewhere a borrow may point: into the block `place` owns, or, with
/// `below`, anywhere below `place`.
struct Anchor {
  Place place;
  bool below = false;
  bool operator==(const Anchor &other) const {
    return place == other.place && below == other.below;
  }
};

/// Why changing an owner through a place is not allowed (`owner-path`).
enum class Barrier : std::uint8_t { None, Reference, Const, Global };

/// What an lvalue expression names.
struct Located {
  bool known = false;
  Place place;
  /// §4: a place the function names exactly.
  bool named = false;
  /// The place's type is `WEAVEC_OWNED`.
  bool owner = false;
  /// Its declared type, with the annotations' sugar.
  clang::QualType type;
  /// The borrow locals the place is reached through.
  llvm::SmallVector<const clang::VarDecl *, 2> through;
  Barrier barrier = Barrier::None;
  const clang::Expr *barrierAt = nullptr;
  /// The owner whose block holds the place (null: a local, a parameter's
  /// memory, or not known), exactly or (`storageBelow`) somewhere below.
  std::optional<Anchor> storage;
  /// The place lies in what this borrow local points to.
  const clang::VarDecl *storageVia = nullptr;
};

/// What a pointer expression's value is, for an owner.
struct Value {
  enum class Kind : std::uint8_t {
    Other,
    Null,
    OwnedCall,
    Load,
    Address,
    Arithmetic,
    Literal,
    Conditional,
  };
  Kind kind = Kind::Other;
  const clang::CallExpr *call = nullptr;
  /// Load: the place read; Address: the place whose address it is;
  /// Arithmetic: what the pointer it steps is (its `inner`).
  Located place;
  Kind inner = Kind::Other;
};

constexpr unsigned StateNull = 1U;
constexpr unsigned StateHolds = 2U;
constexpr unsigned StateMoved = 4U;
constexpr unsigned StatePending = 8U;

/// How an owner stopped holding its block, or how a borrow ended.
enum class How : std::uint8_t {
  None,
  Moved,
  Released,
  Realloc,
  Returned,
  Stored,
  Replaced,
  Call,
};

struct OwnerState {
  unsigned bits = 0;
  /// The owners below this one are null (a fresh allocation, a zeroed
  /// local).
  bool fresh = false;
  /// StatePending: moved if this local, `realloc`'s result, is not null.
  const clang::VarDecl *pending = nullptr;
  How how = How::None;
  const clang::Stmt *cause = nullptr;
  /// Where it got its block.
  const clang::Stmt *since = nullptr;
  /// It took its block from `from` at `took`: a test that finds it null
  /// finds that the place held null.
  const clang::Stmt *took = nullptr;
  std::optional<Place> from = std::nullopt;
  /// Its value was last stored where it is a reference, here.
  const clang::Stmt *copied = nullptr;

  bool operator==(const OwnerState &other) const {
    return bits == other.bits && fresh == other.fresh &&
           pending == other.pending;
  }
};

struct BorrowState {
  llvm::SmallVector<Anchor, 2> anchors;
  llvm::SmallVector<const clang::VarDecl *, 2> roots;
  bool ended = false;
  How how = How::None;
  const clang::Stmt *endedBy = nullptr;
  Place endedAt;

  bool operator==(const BorrowState &other) const {
    return anchors == other.anchors && roots == other.roots &&
           ended == other.ended;
  }
};

struct Flow {
  bool reached = false;
  std::map<Place, OwnerState> owners;
  std::map<const clang::VarDecl *, BorrowState> borrows;

  bool operator==(const Flow &other) const {
    return reached == other.reached && owners == other.owners &&
           borrows == other.borrows;
  }
};

} // namespace

template <typename T>
static void addUnique(llvm::SmallVectorImpl<T> &to, const T &value) {
  if (llvm::find(to, value) == to.end())
    to.push_back(value);
}

/// Joins `from` into `into`; whether `into` changed. `absentInto` and
/// `absentFrom` give the state of a place a side has not recorded.
static bool join(Flow &into, const Flow &from,
                 const std::function<OwnerState(const Place &)> &absentInto,
                 const std::function<OwnerState(const Place &)> &absentFrom) {
  if (!from.reached)
    return false;
  if (!into.reached) {
    into = from;
    return true;
  }
  bool changed = false;
  const auto merge = [&](OwnerState &mine, const OwnerState &theirs) {
    const OwnerState was = mine;
    mine.bits |= theirs.bits;
    mine.fresh = mine.fresh && theirs.fresh;
    if ((theirs.bits & StatePending) != 0 && mine.pending == nullptr)
      mine.pending = theirs.pending;
    if (mine.cause == nullptr) {
      mine.cause = theirs.cause;
      mine.how = theirs.how;
    }
    if (mine.since == nullptr)
      mine.since = theirs.since;
    changed = changed || !(mine == was);
  };
  // What `into` has not recorded is read before its records change: a
  // field's absent state follows its struct's (`fresh`).
  llvm::SmallVector<std::pair<Place, OwnerState>, 8> added;
  for (const auto &[place, state] : from.owners)
    if (!into.owners.contains(place)) {
      OwnerState mine = absentInto(place);
      merge(mine, state);
      added.emplace_back(place, mine);
    }
  for (auto &[place, state] : into.owners)
    if (const auto found = from.owners.find(place); found != from.owners.end())
      merge(state, found->second);
    else
      merge(state, absentFrom(place));
  for (auto &[place, state] : added)
    into.owners.emplace(std::move(place), state);
  changed = changed || !added.empty();
  for (const auto &[variable, state] : from.borrows) {
    auto [found, inserted] = into.borrows.try_emplace(variable);
    BorrowState &mine = found->second;
    const std::size_t anchors = mine.anchors.size();
    const std::size_t roots = mine.roots.size();
    for (const Anchor &anchor : state.anchors)
      addUnique(mine.anchors, anchor);
    for (const clang::VarDecl *root : state.roots)
      addUnique(mine.roots, root);
    changed = changed || inserted || mine.anchors.size() != anchors ||
              mine.roots.size() != roots;
    if (state.ended && !mine.ended) {
      mine.ended = true;
      mine.how = state.how;
      mine.endedBy = state.endedBy;
      mine.endedAt = state.endedAt;
      changed = true;
    }
  }
  return changed;
}

namespace {

/// The checker for one function.
class Ownership {
public:
  Ownership(clang::ASTContext &ast, const clang::FunctionDecl &function,
            SafeUnit &unit, const RuleReport &report)
      : ast(ast), function(function), unit(unit), report(report),
        sources(ast.getSourceManager()),
        // ParentMap and the CFG take a mutable body; neither changes it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        parents(const_cast<clang::Stmt *>(function.getBody())) {}

  void run() {
    const clang::Stmt *body = function.getBody();
    if (body == nullptr)
      return;
    classifyLocals(body);
    // A function that names no owner, makes or takes none and holds no
    // struct with owners has nothing for the rules to follow.
    if (!relevant)
      return;
    clang::CFG::BuildOptions options;
    options.setAllAlwaysAdd();
    options.AddLifetime = true;
    std::unique_ptr<clang::CFG> cfg = clang::CFG::buildCFG(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        &function, const_cast<clang::Stmt *>(body), &ast, options);
    graph = cfg.get();
    if (graph == nullptr || !solve())
      // Amendment 17: no claim is made, and the ledger says so.
      for (SafeUnit::Definition &definition : unit.definitions)
        if (definition.name == function.getName())
          definition.tier = "unsettled";
  }

private:
  clang::ASTContext &ast;
  const clang::FunctionDecl &function;
  SafeUnit &unit;
  const RuleReport &report;
  const clang::SourceManager &sources;
  clang::ParentMap parents;
  const clang::CFG *graph = nullptr;
  /// Owner locals (§2) and borrow locals (§5).
  llvm::DenseSet<const clang::VarDecl *> ownerLocals;
  llvm::DenseSet<const clang::VarDecl *> borrowLocals;
  /// Borrow locals that only ever hold pointers reached from parameters
  /// and owners (`owner-path` may go through them).
  llvm::DenseSet<const clang::VarDecl *> pathLocals;
  /// Pointer parameters the body assigns: borrows that start as the
  /// caller's pointer, not the caller's pointer throughout.
  llvm::DenseSet<const clang::VarDecl *> reassigned;
  bool emit = false;
  /// Caches of the two type questions asked most.
  mutable llvm::DenseMap<const void *, bool> ownerTypes;
  mutable llvm::DenseMap<const void *, bool> holding;
  /// The function names an owner, or makes, takes or holds one.
  bool relevant = false;
  /// The branch being refined cannot be taken.
  bool infeasible = false;
  std::set<std::pair<clang::SourceLocation::UIntTy, std::string>> reported;
  llvm::DenseSet<const clang::VarDecl *> recordedGlobals;

  //===--------------------------------------------------------------------===//
  // Types and declarations
  //===--------------------------------------------------------------------===//

  [[nodiscard]] bool isOwnerType(clang::QualType type) const {
    if (type.isNull() || !type->isPointerType())
      return false;
    const auto [found, inserted] =
        ownerTypes.try_emplace(type.getAsOpaquePtr(), false);
    if (inserted)
      found->second = hasAnnotation(annotationsOf(type, ast), OwnedAnnotation);
    return found->second;
  }

  [[nodiscard]] bool isOwnerVariable(const clang::VarDecl &variable) const {
    if (llvm::isa<clang::ParmVarDecl>(variable))
      return isOwnerType(variable.getType());
    return ownerLocals.contains(&variable) ||
           (!variable.hasLocalStorage() && isOwnerType(variable.getType()));
  }

  /// A record that holds owners in its own bytes (fields, nested records).
  [[nodiscard]] bool holdsOwners(clang::QualType type,
                                 unsigned depth = 0) const {
    if (depth > 8)
      return false;
    if (depth == 0) {
      const void *key = type.getCanonicalType().getAsOpaquePtr();
      if (const auto found = holding.find(key); found != holding.end())
        return found->second;
      const bool holds = holdsOwners(type, 1);
      holding[key] = holds;
      return holds;
    }
    if (const auto *array = ast.getAsArrayType(type))
      return isOwnerType(array->getElementType()) ||
             holdsOwners(array->getElementType(), depth + 1);
    const clang::RecordDecl *record = type->getAsRecordDecl();
    if (record == nullptr || record->getDefinition() == nullptr)
      return false;
    return llvm::any_of(record->getDefinition()->fields(),
                        [&](const clang::FieldDecl *field) {
                          return isOwnerType(field->getType()) ||
                                 holdsOwners(field->getType(), depth + 1);
                        });
  }

  /// The owner places in the bytes of a `type` at `prefix`, through
  /// nested records, not through pointers.
  void ownerFields(clang::QualType type, const Place &prefix,
                   llvm::SmallVectorImpl<Place> &out,
                   unsigned depth = 0) const {
    if (depth > 8)
      return;
    const clang::RecordDecl *record = type->getAsRecordDecl();
    if (record == nullptr || record->isUnion() ||
        record->getDefinition() == nullptr)
      return;
    for (const clang::FieldDecl *field : record->getDefinition()->fields()) {
      const Place place = prefix.with(stepOf(field));
      if (isOwnerType(field->getType()))
        out.push_back(place);
      else if (field->getType()->isRecordType())
        ownerFields(field->getType(), place, out, depth + 1);
    }
  }

  [[nodiscard]] static unsigned fieldCount(const Place &place) {
    return static_cast<unsigned>(
        llvm::count_if(place.path, [](std::uintptr_t step) {
          return step != StepDeref && step != StepIndex;
        }));
  }

  /// The library table's row for a direct call.
  [[nodiscard]] static core::LibraryMatch rowOf(const clang::CallExpr &call) {
    const clang::FunctionDecl *callee = call.getDirectCallee();
    if (callee == nullptr || callee->getIdentifier() == nullptr)
      return {};
    return core::LibrarySpec::shipped()
        .lookup(callee->getName().str(), call.getNumArgs())
        .value_or(core::LibraryMatch{});
  }

  [[nodiscard]] static const clang::FunctionProtoType *
  prototypeOf(const clang::CallExpr &call) {
    if (const clang::FunctionDecl *callee = call.getDirectCallee())
      return callee->getType()->getAs<clang::FunctionProtoType>();
    clang::QualType type = call.getCallee()->IgnoreParenImpCasts()->getType();
    if (type->isPointerType())
      type = type->getPointeeType();
    return type->getAs<clang::FunctionProtoType>();
  }

  /// The declared type of parameter `index` of the called function, or
  /// null for a variadic argument.
  [[nodiscard]] static clang::QualType
  parameterType(const clang::CallExpr &call, unsigned index) {
    if (const clang::FunctionDecl *callee = call.getDirectCallee();
        callee != nullptr && index < callee->getNumParams())
      return callee->getParamDecl(index)->getType();
    if (const auto *prototype = prototypeOf(call);
        prototype != nullptr && index < prototype->getNumParams())
      return prototype->getParamType(index);
    return {};
  }

  [[nodiscard]] bool returnsOwner(const clang::CallExpr &call) const {
    if (const clang::FunctionDecl *callee = call.getDirectCallee()) {
      if (isOwnerType(callee->getReturnType()))
        return true;
      const core::LibraryMatch row = rowOf(call);
      return row.entry != nullptr && row.entry->owned;
    }
    const auto *prototype = prototypeOf(call);
    return prototype != nullptr && isOwnerType(prototype->getReturnType());
  }

  /// §2: a fresh block's owners are null (the runtime zeroes it). So are
  /// those of a block from an allocator of the program's (its owned result
  /// is a `void *`): it has no type to hold owners in.
  [[nodiscard]] bool freshAllocation(const clang::CallExpr &call) const {
    // A function declared `__attribute__((malloc))` says the same of its
    // owned result (amendment: S0's M15).
    if (const clang::FunctionDecl *callee = call.getDirectCallee();
        callee != nullptr && callee->hasAttr<clang::RestrictAttr>() &&
        isOwnerType(callee->getReturnType()))
      return true;
    if (const auto *prototype = prototypeOf(call);
        prototype != nullptr && isOwnerType(prototype->getReturnType()) &&
        prototype->getReturnType()->getPointeeType()->isVoidType())
      return true;
    const core::LibraryMatch row = rowOf(call);
    if (row.entry == nullptr || !row.entry->owned)
      return false;
    return llvm::none_of(row.entry->params, [](const core::LibraryParam &p) {
      return p.effect == core::LibraryParam::Effect::Realloc;
    });
  }

  enum class Effect : std::uint8_t {
    Borrow,
    Consume,
    Release,
    Realloc,
    /// The call stores an owner through the argument; `Replace` drops what
    /// was there first, `Update` reallocates it.
    Replace,
    Update,
  };

  [[nodiscard]] Effect effectOf(const clang::CallExpr &call,
                                unsigned index) const {
    const core::LibraryMatch row = rowOf(call);
    if (row.entry != nullptr) {
      const int rowArgument = row.rowArgument(index);
      const core::LibraryParam *param =
          rowArgument < 0
              ? nullptr
              : row.entry->param(static_cast<unsigned>(rowArgument));
      if (param != nullptr) {
        if (param->effect == core::LibraryParam::Effect::Release)
          return Effect::Release;
        if (param->effect == core::LibraryParam::Effect::Realloc)
          return Effect::Realloc;
        if (param->owner)
          return param->access == core::LibraryParam::Access::ReadWrite
                     ? Effect::Update
                     : Effect::Replace;
      }
    }
    const clang::QualType type = parameterType(call, index);
    if (isOwnerType(type))
      return Effect::Consume;
    if (!type.isNull() && type->isPointerType() &&
        isOwnerType(type->getPointeeType()))
      return Effect::Replace;
    return Effect::Borrow;
  }

  [[nodiscard]] std::string calleeName(const clang::CallExpr &call) const {
    if (const clang::FunctionDecl *callee = call.getDirectCallee())
      return callee->getName().str();
    return text(call.getCallee()->IgnoreParenImpCasts());
  }

  //===--------------------------------------------------------------------===//
  // Text
  //===--------------------------------------------------------------------===//

  [[nodiscard]] std::string text(const clang::Expr *expression) const {
    const llvm::StringRef spelled = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(expression->getSourceRange()),
        sources, ast.getLangOpts());
    if (!spelled.empty() && spelled.size() <= 48)
      return spelled.str();
    const clang::Expr *bare = expression->IgnoreParenImpCasts();
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(bare))
      return reference->getDecl()->getName().str();
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(bare))
      return member->getMemberDecl()->getName().str();
    return "the value";
  }

  [[nodiscard]] static std::string text(const Place &place) {
    std::string out =
        place.root != nullptr ? place.root->getName().str() : "the value";
    bool deref = false;
    for (const std::uintptr_t step : place.path) {
      if (step == StepDeref) {
        if (deref)
          out = cat("(*", out, ")");
        deref = true;
        continue;
      }
      if (step == StepIndex) {
        out += "[...]";
        deref = false;
        continue;
      }
      out += deref ? "->" : ".";
      out += fieldOf(step)->getName().str();
      deref = false;
    }
    if (deref)
      out = "*" + out;
    return out;
  }

  [[nodiscard]] std::string described(How how, const clang::Stmt *cause) const {
    const auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(cause);
    switch (how) {
    case How::Released:
      return "released" + (call != nullptr ? " by '" + calleeName(*call) + "'"
                                           : std::string());
    case How::Realloc:
      return "released by '" +
             (call != nullptr ? calleeName(*call) : std::string("realloc")) +
             "'";
    case How::Returned:
      return "returned";
    case How::Stored:
      if (const auto *assign =
              llvm::dyn_cast_or_null<clang::BinaryOperator>(cause))
        return "moved into '" + text(assign->getLHS()) + "'";
      if (const auto *declaration =
              llvm::dyn_cast_or_null<clang::DeclStmt>(cause);
          declaration != nullptr && declaration->isSingleDecl())
        if (const auto *variable =
                llvm::dyn_cast<clang::VarDecl>(declaration->getSingleDecl()))
          return "moved into '" + variable->getName().str() + "'";
      return "moved";
    case How::Moved:
    case How::Call:
      return "moved" + (call != nullptr ? " to '" + calleeName(*call) + "'"
                                        : std::string());
    case How::Replaced:
      return "replaced";
    case How::None:
      break;
    }
    return "moved";
  }

  [[nodiscard]] static clang::SourceLocation
  locationOf(const clang::Stmt *statement) {
    if (statement == nullptr)
      return {};
    if (const auto *expression = llvm::dyn_cast<clang::Expr>(statement))
      return expression->getExprLoc();
    return statement->getBeginLoc();
  }

  void error(clang::SourceLocation at, llvm::StringRef rule,
             const std::string &message, clang::SourceLocation noteAt,
             const std::string &note) {
    if (!emit)
      return;
    const clang::SourceLocation file =
        at.isValid() ? sources.getFileLoc(at) : at;
    if (!reported.emplace(file.getRawEncoding(), rule.str() + message).second)
      return;
    report(at, rule, message, noteAt, note);
  }

  //===--------------------------------------------------------------------===//
  // Places and values
  //===--------------------------------------------------------------------===//

  [[nodiscard]] static const clang::Expr *
  stripCasts(const clang::Expr *expression) {
    const clang::Expr *e = expression->IgnoreParens();
    while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      const clang::CastKind kind = cast->getCastKind();
      if (kind == clang::CK_LValueToRValue ||
          kind == clang::CK_ArrayToPointerDecay)
        break;
      e = cast->getSubExpr()->IgnoreParens();
    }
    return e;
  }

  [[nodiscard]] Located placeOf(const clang::Expr *lvalue) const {
    const clang::Expr *e = lvalue->IgnoreParens();
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e);
        cast != nullptr && cast->getCastKind() == clang::CK_NoOp)
      return placeOf(cast->getSubExpr());
    if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
      const auto *variable =
          llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
      if (variable == nullptr)
        return {};
      Located located;
      located.known = true;
      located.place.root = variable;
      located.type = variable->getType();
      located.named = variable->hasLocalStorage();
      located.owner = isOwnerVariable(*variable);
      return located;
    }
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e)) {
      const auto *field =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (field == nullptr)
        return {};
      Located located = member->isArrow() ? pointeeOf(member->getBase())
                                          : placeOf(member->getBase());
      if (!located.known)
        return {};
      located.place.path.push_back(stepOf(field));
      located.type = field->getType();
      located.owner = isOwnerType(field->getType());
      if (fieldCount(located.place) > MaxFields)
        located.named = false;
      return located;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
        unary != nullptr && unary->getOpcode() == clang::UO_Deref)
      return pointeeOf(unary->getSubExpr());
    if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(e)) {
      Located located = pointeeOf(subscript->getBase());
      if (!located.known)
        return {};
      // A decayed array's place is the array; a pointer's, what it points
      // to, whose element type the pointer's type gives.
      clang::QualType element;
      if (const auto *array = ast.getAsArrayType(located.type))
        element = array->getElementType();
      else
        element = located.type;
      located.place.path.push_back(StepIndex);
      located.named = false;
      located.type = element;
      located.owner = isOwnerType(element);
      return located;
    }
    return {};
  }

  /// The place a pointer expression points to (its pointee's type in
  /// `type`), or, for a decayed array, the array.
  [[nodiscard]] Located pointeeOf(const clang::Expr *pointer) const {
    const clang::Expr *e = stripCasts(pointer);
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_ArrayToPointerDecay)
        return placeOf(cast->getSubExpr());
      if (cast->getCastKind() != clang::CK_LValueToRValue)
        return {};
      const Located holder = placeOf(cast->getSubExpr());
      if (!holder.known || !holder.type->isPointerType())
        return {};
      Located located = holder;
      located.place.path.push_back(StepDeref);
      located.type = holder.type->getPointeeType();
      located.owner = isOwnerType(located.type);
      const bool rootVariable = holder.place.path.empty();
      const auto *root = holder.place.root;
      const bool parameter = rootVariable &&
                             llvm::isa<clang::ParmVarDecl>(root) &&
                             !reassigned.contains(root);
      const bool global = rootVariable && !root->hasLocalStorage();
      const bool borrow = rootVariable && borrowLocals.contains(root);
      located.storageVia = nullptr;
      if (holder.owner) {
        located.storage = Anchor{.place = holder.place, .below = !holder.named};
        if (!holder.named)
          located.storage->place.path.clear();
      } else {
        located.storage.reset();
      }
      if (!holder.named) {
        located.named = false;
      } else if (!holder.owner && !parameter) {
        located.named = false;
        if (borrow) {
          located.through.push_back(root);
          located.storageVia = root;
        }
      }
      if (located.barrier == Barrier::None) {
        if (global) {
          located.barrier = Barrier::Global;
          located.barrierAt = e;
        } else if (!holder.owner && !parameter &&
                   !(borrow && pathLocals.contains(root))) {
          located.barrier = Barrier::Reference;
          located.barrierAt = e;
        } else if (located.type.isConstQualified()) {
          located.barrier = Barrier::Const;
          located.barrierAt = e;
        }
      }
      return located;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
        unary != nullptr && unary->getOpcode() == clang::UO_AddrOf)
      return placeOf(unary->getSubExpr());
    // §5: what a call returns through a `lifetimebound` parameter is
    // somewhere below that argument's pointee: reached through it.
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      if (callee == nullptr)
        return {};
      for (unsigned i = 0; i < call->getNumArgs() && i < callee->getNumParams();
           ++i) {
        if (!callee->getParamDecl(i)->hasAttr<clang::LifetimeBoundAttr>())
          continue;
        Located located = pointeeOf(call->getArg(i));
        if (!located.known)
          return {};
        located.place.path.push_back(StepIndex);
        located.named = false;
        located.type = call->getType()->isPointerType()
                           ? call->getType()->getPointeeType()
                           : call->getType();
        located.owner = isOwnerType(located.type);
        return located;
      }
      return {};
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e);
        binary != nullptr && binary->isAdditiveOp()) {
      const clang::Expr *base = binary->getLHS()->getType()->isPointerType()
                                    ? binary->getLHS()
                                    : binary->getRHS();
      Located located = pointeeOf(base);
      if (!located.known)
        return {};
      located.place.path.push_back(StepIndex);
      located.named = false;
      return located;
    }
    return {};
  }

  [[nodiscard]] Value classify(const clang::Expr *expression) const {
    Value value;
    const clang::Expr *e = expression->IgnoreParens();
    while (true) {
      if (e->isNullPointerConstant(ast,
                                   clang::Expr::NPC_ValueDependentIsNotNull)) {
        value.kind = Value::Kind::Null;
        return value;
      }
      const auto *cast = llvm::dyn_cast<clang::CastExpr>(e);
      if (cast == nullptr || cast->getCastKind() == clang::CK_LValueToRValue ||
          cast->getCastKind() == clang::CK_ArrayToPointerDecay)
        break;
      e = cast->getSubExpr()->IgnoreParens();
    }
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(e)) {
      value.call = call;
      value.kind =
          returnsOwner(*call) ? Value::Kind::OwnedCall : Value::Kind::Other;
      return value;
    }
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if (llvm::isa<clang::StringLiteral>(cast->getSubExpr()->IgnoreParens())) {
        value.kind = Value::Kind::Literal;
        return value;
      }
      value.place = placeOf(cast->getSubExpr());
      value.kind = cast->getCastKind() == clang::CK_LValueToRValue
                       ? Value::Kind::Load
                       : Value::Kind::Address;
      if (!value.place.known)
        value.kind = Value::Kind::Other;
      return value;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e)) {
      if (unary->getOpcode() == clang::UO_AddrOf) {
        value.place = placeOf(unary->getSubExpr());
        value.kind =
            value.place.known ? Value::Kind::Address : Value::Kind::Other;
        return value;
      }
      if (unary->isIncrementDecrementOp()) {
        const Value inner = classify(unary->getSubExpr());
        value.kind = Value::Kind::Arithmetic;
        value.place = inner.place;
        value.inner = inner.kind;
        return value;
      }
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e);
        binary != nullptr &&
        (binary->isAdditiveOp() || binary->isCompoundAssignmentOp()) &&
        e->getType()->isPointerType()) {
      const clang::Expr *base = binary->getLHS()->getType()->isPointerType()
                                    ? binary->getLHS()
                                    : binary->getRHS();
      const Value inner = classify(base);
      value.kind = Value::Kind::Arithmetic;
      value.place = inner.place;
      value.inner = inner.kind;
      return value;
    }
    if (llvm::isa<clang::StringLiteral>(e)) {
      value.kind = Value::Kind::Literal;
      return value;
    }
    // `a = b = NULL`: the value of an assignment is its right-hand side's.
    if (const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(e);
        assign != nullptr && assign->getOpcode() == clang::BO_Assign)
      return classify(assign->getRHS());
    if (llvm::isa<clang::AbstractConditionalOperator>(e))
      value.kind = Value::Kind::Conditional;
    return value;
  }

  //===--------------------------------------------------------------------===//
  // Locals (§2, §5)
  //===--------------------------------------------------------------------===//

  static void
  forEachStatement(const clang::Stmt *statement,
                   const std::function<void(const clang::Stmt *)> &visit) {
    if (statement == nullptr)
      return;
    visit(statement);
    for (const clang::Stmt *child : statement->children())
      forEachStatement(child, visit);
  }

  [[nodiscard]] static const clang::VarDecl *
  localOf(const clang::Expr *lvalue) {
    const auto *reference =
        llvm::dyn_cast<clang::DeclRefExpr>(lvalue->IgnoreParenImpCasts());
    const auto *variable =
        reference != nullptr
            ? llvm::dyn_cast<clang::VarDecl>(reference->getDecl())
            : nullptr;
    return variable != nullptr && variable->hasLocalStorage() &&
                   !llvm::isa<clang::ParmVarDecl>(variable)
               ? variable
               : nullptr;
  }

  /// §2: which locals are owners (declared so, or given the result of a
  /// call that returns one, or whose address a call stores an owner
  /// through), and which pointer locals are borrows.
  void classifyLocals(const clang::Stmt *body) {
    llvm::SmallVector<const clang::VarDecl *, 16> pointers;
    llvm::DenseMap<const clang::VarDecl *,
                   llvm::SmallVector<const clang::Expr *, 2>>
        assigned;
    for (const clang::ParmVarDecl *parameter : function.parameters())
      if (isOwnerType(parameter->getType()) ||
          hasAnnotation(annotationsOf(parameter->getType(), ast),
                        DropsAnnotation) ||
          (parameter->getType()->isRecordType() &&
           holdsOwners(parameter->getType())))
        relevant = true;
    forEachStatement(body, [&](const clang::Stmt *statement) {
      if (!relevant) {
        if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(statement)) {
          relevant = isOwnerType(member->getMemberDecl()->getType());
        } else if (const auto *reference =
                       llvm::dyn_cast<clang::DeclRefExpr>(statement)) {
          const clang::QualType type = reference->getDecl()->getType();
          relevant =
              isOwnerType(type) ||
              (type->isRecordType() && holdsOwners(type)) ||
              (type->isPointerType() &&
               (isOwnerType(type->getPointeeType()) ||
                holdsOwners(type->getPointeeType())) &&
               llvm::isa_and_nonnull<clang::CallExpr>(
                   parents.getParentIgnoreParenImpCasts(
                       // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                       const_cast<clang::DeclRefExpr *>(reference))));
        } else if (const auto *call =
                       llvm::dyn_cast<clang::CallExpr>(statement)) {
          relevant = returnsOwner(*call);
          for (unsigned i = 0; i < call->getNumArgs() && !relevant; ++i)
            relevant = effectOf(*call, i) != Effect::Borrow;
        }
      }
      if (const auto *declaration =
              llvm::dyn_cast<clang::DeclStmt>(statement)) {
        for (const clang::Decl *decl : declaration->decls())
          if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
              variable != nullptr && variable->hasLocalStorage() &&
              variable->getType()->isPointerType()) {
            pointers.push_back(variable);
            if (isOwnerType(variable->getType()))
              ownerLocals.insert(variable);
            if (const clang::Expr *init = variable->getInit()) {
              assigned[variable].push_back(init);
              if (const auto *call =
                      llvm::dyn_cast<clang::CallExpr>(stripCasts(init));
                  call != nullptr && returnsOwner(*call))
                ownerLocals.insert(variable);
            }
          }
        return;
      }
      if (const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(statement);
          assign != nullptr && assign->getOpcode() == clang::BO_Assign) {
        if (const clang::VarDecl *variable = localOf(assign->getLHS())) {
          assigned[variable].push_back(assign->getRHS());
          // §2: a local that starts as something else (`char *b =
          // stack_buffer;`) is a borrow whatever it is given later.
          const clang::Expr *init = variable->getInit();
          const bool startsBorrowed =
              init != nullptr && classify(init).kind != Value::Kind::Null &&
              classify(init).kind != Value::Kind::OwnedCall;
          if (const auto *call =
                  llvm::dyn_cast<clang::CallExpr>(stripCasts(assign->getRHS()));
              call != nullptr && returnsOwner(*call) && !startsBorrowed)
            ownerLocals.insert(variable);
        } else if (const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(
                       assign->getLHS()->IgnoreParenImpCasts())) {
          // A pointer parameter given another value (`node = node->parent`)
          // is a borrow from there on: it is not the caller's pointer.
          if (const auto *parameter =
                  llvm::dyn_cast<clang::ParmVarDecl>(reference->getDecl());
              parameter != nullptr && parameter->getType()->isPointerType() &&
              !isOwnerType(parameter->getType())) {
            reassigned.insert(parameter);
            assigned[parameter].push_back(assign->getRHS());
          }
        }
        return;
      }
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(statement))
        for (unsigned i = 0; i < call->getNumArgs(); ++i) {
          const Effect effect = effectOf(*call, i);
          if (effect != Effect::Replace && effect != Effect::Update)
            continue;
          if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(
                  stripCasts(call->getArg(i)));
              address != nullptr && address->getOpcode() == clang::UO_AddrOf)
            if (const clang::VarDecl *variable = localOf(address->getSubExpr());
                variable != nullptr && variable->getType()->isPointerType())
              ownerLocals.insert(variable);
        }
    });
    for (const clang::VarDecl *variable : pointers)
      if (!ownerLocals.contains(variable))
        borrowLocals.insert(variable);
    for (const clang::VarDecl *parameter : reassigned)
      borrowLocals.insert(parameter);
    // The borrows reached only from parameters and owners.
    pathLocals = borrowLocals;
    for (bool changed = true; changed;) {
      changed = false;
      for (const clang::VarDecl *variable : borrowLocals) {
        if (!pathLocals.contains(variable))
          continue;
        for (const clang::Expr *value : assigned[variable])
          if (!onPathExpr(value)) {
            pathLocals.erase(variable);
            changed = true;
            break;
          }
      }
    }
  }

  /// `c ? &a->p : &b->q` is on a path when both arms are.
  [[nodiscard]] bool onPathExpr(const clang::Expr *expression) const {
    if (const auto *conditional =
            llvm::dyn_cast<clang::AbstractConditionalOperator>(
                stripCasts(expression)))
      return onPathExpr(conditional->getTrueExpr()) &&
             onPathExpr(conditional->getFalseExpr());
    return onPath(classify(expression));
  }

  [[nodiscard]] bool onPath(const Value &value) const {
    switch (value.kind) {
    case Value::Kind::Null:
      return true;
    case Value::Kind::Load:
    case Value::Kind::Address: {
      const Located &place = value.place;
      if (place.barrier != Barrier::None)
        return false;
      if (place.place.path.empty() && !place.place.root->hasLocalStorage())
        return false;
      if (value.kind == Value::Kind::Load && !place.owner &&
          !place.place.path.empty())
        return false;
      return true;
    }
    case Value::Kind::Arithmetic: {
      Value inner;
      inner.kind = value.inner;
      inner.place = value.place;
      return onPath(inner);
    }
    default:
      return false;
    }
  }

  //===--------------------------------------------------------------------===//
  // States
  //===--------------------------------------------------------------------===//

  /// The state of a named owner place the flow has not recorded.
  [[nodiscard]] OwnerState absent(const Flow &flow, const Place &place) const {
    OwnerState state;
    if (place.path.empty()) {
      // A local before its declaration; a parameter is recorded at entry.
      state.bits = StateNull;
      if (llvm::isa<clang::ParmVarDecl>(place.root))
        state.bits = StateHolds | StateNull;
      return state;
    }
    // The owner (or the local) whose bytes hold the place.
    std::size_t container = 0;
    for (std::size_t i = place.path.size(); i-- > 0;)
      if (place.path[i] == StepDeref || place.path[i] == StepIndex) {
        container = i;
        break;
      }
    Place holder{
        .root = place.root,
        .path = {place.path.begin(),
                 place.path.begin() + static_cast<std::ptrdiff_t>(container)}};
    bool fresh = false;
    if (container == 0 && place.path[0] != StepDeref &&
        place.path[0] != StepIndex) {
      // A field of a local struct: zeroed, unless it was copied from
      // somewhere (recorded at its root); a parameter's is the caller's.
      const auto found = flow.owners.find(Place{.root = place.root});
      fresh = llvm::isa<clang::ParmVarDecl>(place.root)
                  ? false
                  : (found == flow.owners.end() || found->second.fresh);
    } else {
      const auto found = flow.owners.find(holder);
      fresh = found != flow.owners.end() ? found->second.fresh
                                         : absent(flow, holder).fresh;
      if (holder.path.empty() && llvm::isa<clang::ParmVarDecl>(place.root))
        fresh = false;
    }
    state.bits = fresh ? StateNull : (StateHolds | StateNull);
    return state;
  }

  [[nodiscard]] OwnerState stateOf(const Flow &flow, const Place &place) const {
    const auto found = flow.owners.find(place);
    return found != flow.owners.end() ? found->second : absent(flow, place);
  }

  static void eraseBelow(Flow &flow, const Place &place) {
    for (auto it = flow.owners.begin(); it != flow.owners.end();)
      if (place.covers(it->first) && !(it->first == place))
        it = flow.owners.erase(it);
      else
        ++it;
  }

  //===--------------------------------------------------------------------===//
  // Borrows (§5)
  //===--------------------------------------------------------------------===//

  static void endBorrow(BorrowState &borrow, How how, const clang::Stmt *by,
                        const Place &at) {
    if (borrow.ended)
      return;
    borrow.ended = true;
    borrow.how = how;
    borrow.endedBy = by;
    borrow.endedAt = at;
  }

  /// §5 rule 1: an owner the function names was moved, released or
  /// replaced.
  static void endBelow(Flow &flow, const Place &place, How how,
                       const clang::Stmt *by) {
    for (auto &[variable, borrow] : flow.borrows)
      for (const Anchor &anchor : borrow.anchors)
        if (place.covers(anchor.place) ||
            (anchor.below && anchor.place.covers(place))) {
          endBorrow(borrow, how, by, place);
          break;
        }
  }

  /// §5 rule 2: an owner the function does not name exactly: every borrow
  /// below the same root ends but those it was reached through.
  static void endUnnamed(Flow &flow, const Located &located, How how,
                         const clang::Stmt *by) {
    llvm::SmallVector<const clang::VarDecl *, 4> roots{located.place.root};
    for (const clang::VarDecl *via : located.through)
      if (const auto found = flow.borrows.find(via);
          found != flow.borrows.end())
        for (const clang::VarDecl *root : found->second.roots)
          addUnique(roots, root);
    for (auto &[variable, borrow] : flow.borrows) {
      if (llvm::is_contained(located.through, variable))
        continue;
      const bool hit = llvm::any_of(borrow.roots,
                                    [&](const clang::VarDecl *root) {
                                      return llvm::is_contained(roots, root);
                                    }) ||
                       llvm::any_of(borrow.anchors, [&](const Anchor &anchor) {
                         return llvm::is_contained(roots, anchor.place.root);
                       });
      if (hit)
        endBorrow(borrow, how, by, located.place);
    }
  }

  /// Where a pointer argument can reach: the places at or below which a
  /// callee given it can change owners.
  [[nodiscard]] llvm::SmallVector<Anchor, 2> reachOf(const Flow &flow,
                                                     const Value &value) const {
    llvm::SmallVector<Anchor, 2> reach;
    switch (value.kind) {
    case Value::Kind::Load: {
      const Located &place = value.place;
      if (place.place.path.empty() && borrowLocals.contains(place.place.root)) {
        if (const auto found = flow.borrows.find(place.place.root);
            found != flow.borrows.end()) {
          for (const Anchor &anchor : found->second.anchors)
            reach.push_back({.place = anchor.place, .below = true});
          for (const clang::VarDecl *root : found->second.roots)
            reach.push_back({.place = Place{.root = root}, .below = true});
        }
        return reach;
      }
      if (place.barrier != Barrier::None || !place.known)
        return reach;
      reach.push_back({.place = place.named ? place.place.with(StepDeref)
                                            : Place{.root = place.place.root},
                       .below = true});
      return reach;
    }
    case Value::Kind::Address:
      if (value.place.known && value.place.barrier == Barrier::None)
        reach.push_back({.place = value.place.named
                                      ? value.place.place
                                      : Place{.root = value.place.place.root},
                         .below = true});
      return reach;
    case Value::Kind::Arithmetic: {
      Value inner;
      inner.kind = value.inner;
      inner.place = value.place;
      return reachOf(flow, inner);
    }
    default:
      return reach;
    }
  }

  [[nodiscard]] static bool overlaps(llvm::ArrayRef<Anchor> a,
                                     llvm::ArrayRef<Anchor> b) {
    for (const Anchor &x : a)
      for (const Anchor &y : b)
        if (x.place.covers(y.place) || y.place.covers(x.place))
          return true;
    return false;
  }

  /// §5: the borrow a value gives a local.
  [[nodiscard]] BorrowState borrowOf(const Flow &flow,
                                     const clang::Expr *expression) const {
    BorrowState borrow;
    const Value value = classify(expression);
    switch (value.kind) {
    case Value::Kind::Load: {
      const Located &place = value.place;
      if (place.place.path.empty() && borrowLocals.contains(place.place.root)) {
        if (const auto found = flow.borrows.find(place.place.root);
            found != flow.borrows.end())
          return found->second;
        return borrow;
      }
      if (place.owner) {
        borrow.anchors.push_back(
            place.named ? Anchor{.place = place.place}
                        : Anchor{.place = Place{.root = place.place.root},
                                 .below = true});
      }
      // A pointer read from a reference field is a reference: nothing the
      // function does to its own owners ends it.
      if (!place.owner && !place.place.path.empty())
        return borrow;
      if (place.barrier == Barrier::None && place.place.root != nullptr &&
          place.place.root->hasLocalStorage())
        borrow.roots.push_back(place.place.root);
      if (place.storageVia != nullptr)
        addVia(flow, place.storageVia, borrow);
      return borrow;
    }
    case Value::Kind::Address:
      return storageBorrow(flow, value.place);
    case Value::Kind::Arithmetic: {
      if (value.inner == Value::Kind::Load ||
          value.inner == Value::Kind::Address) {
        const auto *binary =
            llvm::dyn_cast<clang::BinaryOperator>(stripCasts(expression));
        const auto *unary =
            llvm::dyn_cast<clang::UnaryOperator>(stripCasts(expression));
        const clang::Expr *base = nullptr;
        if (binary != nullptr)
          base = binary->getLHS()->getType()->isPointerType()
                     ? binary->getLHS()
                     : binary->getRHS();
        else if (unary != nullptr)
          base = unary->getSubExpr();
        if (base != nullptr)
          return borrowOf(flow, base);
      }
      return borrow;
    }
    case Value::Kind::Conditional: {
      const auto *conditional = llvm::cast<clang::AbstractConditionalOperator>(
          expression->IgnoreParenCasts());
      BorrowState a = borrowOf(flow, conditional->getTrueExpr());
      const BorrowState b = borrowOf(flow, conditional->getFalseExpr());
      for (const Anchor &anchor : b.anchors)
        addUnique(a.anchors, anchor);
      for (const clang::VarDecl *root : b.roots)
        addUnique(a.roots, root);
      a.ended = a.ended || b.ended;
      return a;
    }
    case Value::Kind::Other:
      if (value.call != nullptr)
        return lifetimeBorrow(flow, *value.call);
      return borrow;
    default:
      return borrow;
    }
  }

  static void addVia(const Flow &flow, const clang::VarDecl *via,
                     BorrowState &borrow) {
    const auto found = flow.borrows.find(via);
    if (found == flow.borrows.end())
      return;
    for (const Anchor &anchor : found->second.anchors)
      addUnique(borrow.anchors, Anchor{.place = anchor.place, .below = true});
    for (const clang::VarDecl *root : found->second.roots)
      addUnique(borrow.roots, root);
    if (found->second.ended && !borrow.ended) {
      borrow.ended = true;
      borrow.how = found->second.how;
      borrow.endedBy = found->second.endedBy;
      borrow.endedAt = found->second.endedAt;
    }
  }

  /// A pointer to a place: below the owner whose block holds the place.
  [[nodiscard]] static BorrowState storageBorrow(const Flow &flow,
                                                 const Located &place) {
    BorrowState borrow;
    if (!place.known)
      return borrow;
    if (place.storage)
      borrow.anchors.push_back(*place.storage);
    if (place.storageVia != nullptr)
      addVia(flow, place.storageVia, borrow);
    if (place.barrier == Barrier::None && place.place.root->hasLocalStorage())
      addUnique(borrow.roots, place.place.root);
    return borrow;
  }

  /// The result of a call that took a `lifetimebound` argument is below it.
  [[nodiscard]] BorrowState lifetimeBorrow(const Flow &flow,
                                           const clang::CallExpr &call) const {
    BorrowState borrow;
    const clang::FunctionDecl *callee = call.getDirectCallee();
    if (callee == nullptr)
      return borrow;
    for (unsigned i = 0; i < call.getNumArgs() && i < callee->getNumParams();
         ++i) {
      if (!callee->getParamDecl(i)->hasAttr<clang::LifetimeBoundAttr>())
        continue;
      const Value value = classify(call.getArg(i));
      for (const Anchor &anchor : reachOf(flow, value))
        addUnique(borrow.anchors, anchor);
      const BorrowState from = borrowOf(flow, call.getArg(i));
      for (const clang::VarDecl *root : from.roots)
        addUnique(borrow.roots, root);
    }
    return borrow;
  }

  void useBorrow(Flow &flow, const clang::VarDecl *variable,
                 const clang::Expr *at) {
    const auto found = flow.borrows.find(variable);
    if (found == flow.borrows.end() || !found->second.ended)
      return;
    const BorrowState &borrow = found->second;
    const std::string name = variable->getName().str();
    const std::string owner = text(borrow.endedAt);
    const auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(borrow.endedBy);
    std::string message;
    std::string note;
    if (borrow.how == How::Call && call != nullptr) {
      message = "'" + name + "' was taken from '" + owner +
                "' before the call to '" + calleeName(*call) +
                "', which may release or replace what '" + owner + "' owns";
      note = "take '" + name +
             "' again after the call, or declare the parameter 'const' if '" +
             calleeName(*call) + "' changes no owner";
    } else {
      message = "'" + name + "' points into what '" + owner +
                "' owned before " +
                (borrow.how == How::Replaced
                     ? std::string("it was replaced")
                     : "it was " + described(borrow.how, borrow.endedBy));
      note = "the block was given up here; take '" + name +
             "' again afterwards, or use it before";
    }
    error(at->getExprLoc(), rule::StaleBorrow, message,
          locationOf(borrow.endedBy), note);
    found->second.ended = false;
  }

  //===--------------------------------------------------------------------===//
  // Owners (§3, §4)
  //===--------------------------------------------------------------------===//

  void useOwner(Flow &flow, const Located &located, const clang::Expr *at) {
    if (!located.named)
      return;
    OwnerState &state = flow.owners[located.place];
    if (state.bits == 0)
      state = absent(flow, located.place);
    const std::string name = text(located.place);
    if ((state.bits & StatePending) != 0) {
      const std::string result = state.pending != nullptr
                                     ? state.pending->getName().str()
                                     : std::string("its result");
      error(at->getExprLoc(), rule::OwnerMoved,
            "'" + name + "' is used before '" + result +
                "', the result of 'realloc', was tested: the call released '" +
                name + "' if '" + result + "' is not null",
            locationOf(state.cause),
            "test '" + result + "' first: on null '" + name +
                "' still owns its block, otherwise use '" + result + "'");
    } else if ((state.bits & StateMoved) != 0) {
      const bool maybe = (state.bits & (StateHolds | StateNull)) != 0;
      error(at->getExprLoc(), rule::OwnerMoved,
            "'" + name + "' is used after it " +
                (maybe ? "may have been " : "was ") +
                described(state.how, state.cause),
            locationOf(state.cause),
            std::string(state.how == How::Released ? "released" : "moved") +
                " here; use it before this, or keep what you need from it in a "
                "local first");
    } else {
      return;
    }
    // One error per move: the place says nothing more.
    state.bits = StateNull;
    state.pending = nullptr;
  }

  /// Something takes the block an owner holds: a call, a store, a return.
  void moveOut(Flow &flow, const Located &located, const clang::Expr *expr,
               How how, const clang::Stmt *by,
               const clang::VarDecl *pending = nullptr) {
    if (!checkPath(located, expr, how))
      return;
    if (!located.named) {
      checkSlot(expr, by);
      endUnnamed(flow, located, how, by);
      return;
    }
    // A null owner releases nothing.
    if (emit && how == How::Released &&
        (stateOf(flow, located.place).bits & (StateHolds | StatePending)) !=
            0) {
      const std::string name = text(located.place);
      llvm::SmallVector<Place, 8> fields;
      ownerFields(located.type->isPointerType() ? located.type->getPointeeType()
                                                : located.type,
                  located.place.with(StepDeref), fields);
      for (const Place &field : fields) {
        const OwnerState state = stateOf(flow, field);
        if ((state.bits & (StateHolds | StatePending)) == 0)
          continue;
        const std::string inner = text(field);
        error(expr->getExprLoc(), rule::OwnerLeak,
              cat("'", inner, "' may still own its block when '", name,
                  "' is released"),
              {}, cat("release '", inner, "' first: 'free(", inner, ");'"));
      }
    } else {
      checkWhole(flow, located.place, expr, [&] {
        return cat("'", text(located.place), "' is ", described(how, by));
      });
    }
    OwnerState &state = flow.owners[located.place];
    state.bits = StateMoved;
    state.how = how;
    state.cause = by;
    if (how == How::Realloc && pending != nullptr) {
      state.bits = StatePending;
      state.pending = pending;
    }
    eraseBelow(flow, located.place);
    endBelow(flow, located.place, how, by);
  }

  /// A struct is not moved, passed or left while an owner in it is moved.
  void checkWhole(const Flow &flow, const Place &place, const clang::Expr *at,
                  const std::function<std::string()> &what) {
    if (!emit)
      return;
    for (const auto &[inner, state] : flow.owners) {
      if (!place.covers(inner) || inner == place ||
          (state.bits & StateMoved) == 0 || releasedAbove(flow, inner, place))
        continue;
      const std::string name = text(inner);
      error(at->getExprLoc(), rule::OwnerMoved,
            cat(what(), " while '", name,
                "' was moved out of it and not assigned"),
            locationOf(state.cause),
            cat("'", name, "' was moved here; assign it ('", name,
                " = NULL;') before"));
      return;
    }
  }

  /// Whether an owner between `top` (excluded) and `inner` (excluded) was
  /// moved or released: what is below it no longer matters.
  [[nodiscard]] static bool releasedAbove(const Flow &flow, const Place &inner,
                                          const Place &top) {
    for (std::size_t length = top.path.size() + 1; length < inner.path.size();
         ++length) {
      Place prefix{
          .root = inner.root,
          .path = {inner.path.begin(),
                   inner.path.begin() + static_cast<std::ptrdiff_t>(length)}};
      if (const auto found = flow.owners.find(prefix);
          found != flow.owners.end() && (found->second.bits & StateMoved) != 0)
        return true;
    }
    return false;
  }

  /// `owner-path`: an owner is moved, released or replaced only along the
  /// path that owns it.
  bool checkPath(const Located &located, const clang::Expr *expr, How how) {
    if (located.barrier == Barrier::None)
      return true;
    const std::string name = text(expr);
    const std::string base =
        located.barrierAt != nullptr ? text(located.barrierAt) : name;
    const char *verb = how == How::Replaced ? "replaced" : "moved or released";
    switch (located.barrier) {
    case Barrier::Reference:
      error(expr->getExprLoc(), rule::OwnerPath,
            "'" + name + "' is reached through the reference '" + base +
                "' and cannot be " + verb + " from here",
            {},
            "change an owner from the pointer that owns it: reach it from a "
            "parameter or a local owner through owners, or keep this "
            "function WEAVEC_GUARDED");
      return false;
    case Barrier::Const:
      error(expr->getExprLoc(), rule::OwnerPath,
            "'" + name + "' is reached through a pointer to const ('" + base +
                "') and cannot be " + verb,
            {},
            "take the parameter without 'const' if the function changes "
            "its owners");
      return false;
    case Barrier::Global:
      error(expr->getExprLoc(), rule::OwnerPath,
            "'" + name + "' is below the global '" + base +
                "', read in place, and cannot be " + verb,
            {},
            "move the global into a local owner first ('T *WEAVEC_OWNED s = " +
                base + "; " + base + " = NULL;'), change it, and move it back");
      return false;
    case Barrier::None:
      return true;
    }
    return true;
  }

  /// The statement of a compound that `expression` is part of.
  [[nodiscard]] std::pair<const clang::CompoundStmt *, const clang::Stmt *>
  statementOf(const clang::Stmt *expression) const {
    const clang::Stmt *current = expression;
    while (current != nullptr) {
      const clang::Stmt *parent = parents.getParent(current);
      if (parent == nullptr)
        return {};
      if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(parent))
        return {compound, current};
      current = parent;
    }
    return {};
  }

  /// Whether two lvalues name one slot: the same expression, `--n` or
  /// `n++` in the first standing for `n` in the second.
  [[nodiscard]] bool sameSlot(const clang::Expr *a,
                              const clang::Expr *b) const {
    a = a->IgnoreParenImpCasts();
    b = b->IgnoreParenImpCasts();
    if (const auto *step = llvm::dyn_cast<clang::UnaryOperator>(a);
        step != nullptr && step->isIncrementDecrementOp())
      return sameSlot(step->getSubExpr(), b);
    if (a->getStmtClass() != b->getStmtClass())
      return false;
    if (const auto *x = llvm::dyn_cast<clang::DeclRefExpr>(a))
      return x->getDecl() == llvm::cast<clang::DeclRefExpr>(b)->getDecl();
    if (const auto *x = llvm::dyn_cast<clang::MemberExpr>(a)) {
      const auto *y = llvm::cast<clang::MemberExpr>(b);
      return x->getMemberDecl() == y->getMemberDecl() &&
             x->isArrow() == y->isArrow() &&
             sameSlot(x->getBase(), y->getBase());
    }
    if (const auto *x = llvm::dyn_cast<clang::ArraySubscriptExpr>(a)) {
      const auto *y = llvm::cast<clang::ArraySubscriptExpr>(b);
      return sameSlot(x->getBase(), y->getBase()) &&
             sameSlot(x->getIdx(), y->getIdx());
    }
    if (const auto *x = llvm::dyn_cast<clang::UnaryOperator>(a))
      return x->getOpcode() ==
                 llvm::cast<clang::UnaryOperator>(b)->getOpcode() &&
             sameSlot(x->getSubExpr(),
                      llvm::cast<clang::UnaryOperator>(b)->getSubExpr());
    if (const auto *x = llvm::dyn_cast<clang::IntegerLiteral>(a))
      return x->getValue() == llvm::cast<clang::IntegerLiteral>(b)->getValue();
    if (const auto *x = llvm::dyn_cast<clang::BinaryOperator>(a)) {
      const auto *y = llvm::cast<clang::BinaryOperator>(b);
      return x->getOpcode() == y->getOpcode() &&
             sameSlot(x->getLHS(), y->getLHS()) &&
             sameSlot(x->getRHS(), y->getRHS());
    }
    return false;
  }

  /// `owner-slot`: an owner the function does not name is moved out of only
  /// in a statement followed at once by an assignment to the same place.
  void checkSlot(const clang::Expr *slot, const clang::Stmt *by) {
    if (!emit)
      return;
    const auto [compound, statement] = statementOf(by);
    bool assigned = false;
    if (compound != nullptr) {
      const auto *it = llvm::find(compound->body(), statement);
      if (it != compound->body_end() && std::next(it) != compound->body_end()) {
        const clang::Stmt *next = *std::next(it);
        if (const auto *expression = llvm::dyn_cast<clang::Expr>(next))
          next = expression->IgnoreParenImpCasts();
        if (const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(next);
            assign != nullptr && assign->getOpcode() == clang::BO_Assign)
          assigned = sameSlot(slot, assign->getLHS());
      }
      // `x = a[i]; a[i] = y;` where the move is itself that statement's
      // right-hand side and the next statement is the reassignment: done
      // above. A swap's middle statement assigns the slot it moves to.
      if (!assigned)
        if (const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(
                statement->IgnoreContainers());
            assign != nullptr && assign->getOpcode() == clang::BO_Assign &&
            sameSlot(slot, assign->getLHS()))
          assigned = true;
    }
    if (assigned)
      return;
    const std::string name = text(slot);
    error(slot->getExprLoc(), rule::OwnerSlot,
          "'" + name + "' is moved out and still holds the pointer", {},
          "assign it in the next statement: '" + name + " = NULL;'");
  }

  void dropCheck(const Flow &flow, const Place &place, const clang::Expr *at,
                 const std::string &event) {
    const OwnerState state = stateOf(flow, place);
    if ((state.bits & (StateHolds | StatePending)) == 0)
      return;
    const std::string name = text(place);
    error(at->getExprLoc(), rule::OwnerLeak,
          "'" + name + "' " + event + " while it may still own its block",
          locationOf(state.since),
          (state.bits & StatePending) != 0
              ? "on failure 'realloc' returns null and '" + name +
                    "' still owns its block: assign the result to a new local, "
                    "test it, then assign '" +
                    name + "'"
              : "'" + name +
                    "' owns a block since here; release it first ('free(" +
                    name + ");'), or move it into another owner");
  }

  void ownerSource(const Value &value, const clang::Expr *source,
                   const std::string &destination) {
    const std::string from = text(source);
    std::string why;
    std::string note;
    switch (value.kind) {
    case Value::Kind::Load:
      if (value.place.place.path.empty() &&
          llvm::isa<clang::ParmVarDecl>(value.place.place.root)) {
        why = "'" + from + "' is borrowed for this call";
        note = "store a copy the struct can own (an allocation), or take the "
               "parameter as 'T *WEAVEC_OWNED " +
               value.place.place.root->getName().str() + "'";
      } else if (value.place.place.path.empty() &&
                 borrowLocals.contains(value.place.place.root)) {
        why = "'" + from + "' is a borrow, not an owner";
        note = "store the owner itself, which moves it, or make '" + from +
               "' an owner ('T *WEAVEC_OWNED " + from + "')";
      } else {
        why = "'" + from + "' is a reference, not an owner";
        note = "store the owner itself, which moves it, or declare where it "
               "comes from 'WEAVEC_OWNED'";
      }
      break;
    case Value::Kind::Address:
      why =
          "'" + from + "' is the address of an object, not a block to release";
      note = "allocate the object ('malloc') to give it an owner";
      break;
    case Value::Kind::Literal:
      why = "a string literal is not a heap block";
      note = "store a copy: 'strdup(" + from + ")'";
      break;
    case Value::Kind::Arithmetic:
      why = "'" + from + "' points inside a block, not at its start";
      note = "store the owner itself; step a borrow ('T *q = p;') instead";
      break;
    case Value::Kind::Conditional:
      why = "an owner is moved from one owner, not through '?:'";
      note = "assign in each branch of an 'if' instead";
      break;
    default:
      why = "'" + from + "' is not an owner";
      note = "declare the function's result 'T *WEAVEC_OWNED' if the caller "
             "should release it";
      break;
    }
    error(source->getExprLoc(), rule::OwnerSource,
          destination + " is an owner and " + why, {}, note);
  }

  /// Stores `source` into the owner place `destination`.
  void storeOwner(Flow &flow, const Located &destination,
                  const clang::Expr *target, const clang::Expr *source,
                  const clang::Stmt *by, bool drop) {
    if (!checkPath(destination, target, How::Replaced))
      return;
    const Value value = classify(source);
    OwnerState next;
    next.bits = StateHolds | StateNull;
    next.since = by;
    switch (value.kind) {
    case Value::Kind::Null:
      next.bits = StateNull;
      break;
    case Value::Kind::OwnedCall:
      next.fresh = freshAllocation(*value.call);
      next.since = value.call;
      break;
    case Value::Kind::Load:
      if (value.place.owner) {
        if (destination.named && value.place.named &&
            value.place.place == destination.place)
          return; // `p = p` (the pair of a count that shrank)
        if (value.place.named) {
          next.since = stateOf(flow, value.place.place).since;
          next.took = by;
          next.from = value.place.place;
        }
        if (drop && destination.named)
          dropCheck(flow, destination.place, target, "is assigned over");
        drop = false;
        moveOut(flow, value.place, source, How::Stored, by);
        break;
      }
      [[fallthrough]];
    default:
      ownerSource(value, source, "'" + text(target) + "'");
      break;
    }
    if (destination.named) {
      if (drop)
        dropCheck(flow, destination.place, target, "is assigned over");
      flow.owners[destination.place] = next;
      eraseBelow(flow, destination.place);
      endBelow(flow, destination.place, How::Replaced, by);
    } else {
      endUnnamed(flow, destination, How::Replaced, by);
    }
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void transfer(Flow &flow, const clang::Stmt *statement) {
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(statement)) {
      if (cast->getCastKind() != clang::CK_LValueToRValue)
        return;
      const Located located = placeOf(cast->getSubExpr());
      if (!located.known)
        return;
      if (located.owner)
        useOwner(flow, located, cast);
      else if (located.place.path.empty() &&
               borrowLocals.contains(located.place.root))
        useBorrow(flow, located.place.root, cast);
      recordInPlace(located, cast);
      return;
    }
    if (const auto *call = llvm::dyn_cast<clang::CallExpr>(statement)) {
      handleCall(flow, *call);
      return;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(statement)) {
      if (binary->getOpcode() == clang::BO_Assign)
        handleAssign(flow, binary->getLHS(), binary->getRHS(), binary);
      else if (binary->isCompoundAssignmentOp())
        arithmetic(binary->getLHS());
      return;
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(statement)) {
      if (unary->isIncrementDecrementOp())
        arithmetic(unary->getSubExpr());
      return;
    }
    if (const auto *declaration = llvm::dyn_cast<clang::DeclStmt>(statement)) {
      for (const clang::Decl *decl : declaration->decls())
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl))
          handleDecl(flow, *variable, declaration);
      return;
    }
    if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(statement))
      handleReturn(flow, *ret);
  }

  void recordInPlace(const Located &located, const clang::Expr *at) {
    if (!emit || located.place.root == nullptr ||
        located.place.root->hasLocalStorage() || located.place.path.empty() ||
        located.place.path.front() != StepDeref ||
        !isOwnerType(located.place.root->getType()) ||
        !recordedGlobals.insert(located.place.root).second)
      return;
    const clang::PresumedLoc where = sources.getPresumedLoc(at->getExprLoc());
    unit.inPlace.push_back({.function = function.getName().str(),
                            .global = located.place.root->getName().str(),
                            .file = where.isValid() ? where.getFilename() : "",
                            .line = where.isValid() ? where.getLine() : 0});
  }

  void arithmetic(const clang::Expr *lvalue) {
    const Located located = placeOf(lvalue);
    if (!located.known || !located.owner)
      return;
    const std::string name = text(lvalue);
    error(lvalue->getExprLoc(), rule::OwnerSource,
          "'" + name +
              "' is an owner: arithmetic would leave it pointing inside its "
              "block",
          {},
          "keep the owner as it is and step a borrow: 'T *q = " + name +
              "; q++;'");
  }

  /// Where an owned call's result goes: whether that keeps it.
  void checkKept(const clang::CallExpr &call) {
    const clang::Stmt *child = &call;
    const clang::Stmt *parent = parents.getParent(child);
    while (parent != nullptr) {
      if (llvm::isa<clang::ParenExpr, clang::ImplicitCastExpr>(parent)) {
        child = parent;
        parent = parents.getParent(child);
        continue;
      }
      if (const auto *cast = llvm::dyn_cast<clang::CStyleCastExpr>(parent);
          cast != nullptr && !cast->getType()->isVoidType()) {
        child = parent;
        parent = parents.getParent(child);
        continue;
      }
      break;
    }
    if (parent != nullptr) {
      if (const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(parent);
          assign != nullptr && assign->getOpcode() == clang::BO_Assign &&
          assign->getRHS()->IgnoreParenCasts() == call.IgnoreParenCasts())
        return;
      if (llvm::isa<clang::DeclStmt, clang::InitListExpr>(parent))
        return;
      if (llvm::isa<clang::ReturnStmt>(parent) &&
          isOwnerType(function.getReturnType()))
        return;
      if (const auto *outer = llvm::dyn_cast<clang::CallExpr>(parent))
        for (unsigned i = 0; i < outer->getNumArgs(); ++i)
          if (outer->getArg(i) == child) {
            const Effect effect = effectOf(*outer, i);
            if (effect == Effect::Consume || effect == Effect::Release ||
                effect == Effect::Realloc)
              return;
          }
      if (llvm::isa<clang::AbstractConditionalOperator>(parent)) {
        error(call.getExprLoc(), rule::OwnerSource,
              "the result of '" + calleeName(call) +
                  "' is an owner and goes through '?:'",
              {}, "assign it to an owner in each branch of an 'if' instead");
        return;
      }
    }
    error(call.getExprLoc(), rule::OwnerLeak,
          "the result of '" + calleeName(call) +
              "' owns a block and is not kept in an owner",
          {},
          "store it in an owner ('T *p = " + calleeName(call) +
              "(...);') and release it");
  }

  void handleCall(Flow &flow, const clang::CallExpr &call) {
    if (returnsOwner(call))
      checkKept(call);
    // Byte copies and fills of what holds owners.
    if (const clang::FunctionDecl *callee = call.getDirectCallee()) {
      const unsigned id = callee->getBuiltinID();
      // Its arguments are not evaluated.
      if (id == clang::Builtin::BI__builtin_object_size ||
          id == clang::Builtin::BI__builtin_dynamic_object_size)
        return;
      const bool fill = id == clang::Builtin::BImemset ||
                        id == clang::Builtin::BI__builtin_memset ||
                        id == clang::Builtin::BI__builtin___memset_chk ||
                        id == clang::Builtin::BIbzero ||
                        id == clang::Builtin::BI__builtin_bzero;
      const bool copy = id == clang::Builtin::BImemcpy ||
                        id == clang::Builtin::BI__builtin_memcpy ||
                        id == clang::Builtin::BI__builtin___memcpy_chk ||
                        id == clang::Builtin::BImemmove ||
                        id == clang::Builtin::BI__builtin_memmove ||
                        id == clang::Builtin::BI__builtin___memmove_chk;
      if ((fill || copy) && call.getNumArgs() > 0) {
        const clang::Expr *destination = call.getArg(0)->IgnoreParenCasts();
        const clang::QualType pointee =
            destination->getType()->isPointerType()
                ? destination->getType()->getPointeeType()
                : clang::QualType();
        if (!pointee.isNull() &&
            (holdsOwners(pointee) || isOwnerType(pointee))) {
          if (copy) {
            error(call.getExprLoc(), rule::OwnerSource,
                  "copying '" + pointee.getAsString() +
                      "' as bytes would leave two owners of its blocks",
                  {},
                  "assign it as a struct ('*a = *b'), which moves its owners");
          } else {
            const Located target = pointeeOf(call.getArg(0));
            if (target.known && target.named) {
              llvm::SmallVector<Place, 8> fields;
              ownerFields(pointee, target.place, fields);
              for (const Place &field : fields) {
                dropCheck(flow, field, &call, "is cleared");
                flow.owners[field] = OwnerState{.bits = StateNull};
              }
            }
          }
          // A fill sets its owners to null; a copy was rejected.
          return;
        }
      }
    }
    struct Argument {
      Effect effect = Effect::Borrow;
      Value value;
      llvm::SmallVector<Anchor, 2> reach;
      bool changing = false;
      bool owners = false;
    };
    llvm::SmallVector<Argument, 6> arguments;
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      Argument argument;
      argument.effect = effectOf(call, i);
      argument.value = classify(call.getArg(i));
      argument.reach = reachOf(flow, argument.value);
      clang::QualType type = parameterType(call, i);
      if (type.isNull())
        type = call.getArg(i)->IgnoreParenImpCasts()->getType();
      const clang::QualType pointee =
          type->isPointerType() ? type->getPointeeType() : clang::QualType();
      // Through a `void *`, what the argument is.
      clang::QualType actual = call.getArg(i)->IgnoreParenCasts()->getType();
      const clang::QualType actualPointee = actual->isPointerType()
                                                ? actual->getPointeeType()
                                                : clang::QualType();
      argument.owners =
          !actualPointee.isNull() &&
          (holdsOwners(actualPointee) || isOwnerType(actualPointee));
      argument.changing = argument.effect == Effect::Borrow &&
                          !pointee.isNull() && !pointee.isConstQualified() &&
                          (holdsOwners(pointee) || isOwnerType(pointee));
      arguments.push_back(std::move(argument));
    }
    // `call-alias`: one error per call.
    bool aliased = false;
    for (std::size_t i = 0; i < arguments.size() && !aliased; ++i) {
      if (!arguments[i].changing || arguments[i].reach.empty())
        continue;
      for (std::size_t j = 0; j < arguments.size(); ++j) {
        if (i == j || arguments[j].effect != Effect::Borrow ||
            !arguments[j].owners || arguments[j].reach.empty() ||
            !overlaps(arguments[i].reach, arguments[j].reach))
          continue;
        const clang::Expr *other = call.getArg(static_cast<unsigned>(j));
        const clang::Expr *changed = call.getArg(static_cast<unsigned>(i));
        error(other->getExprLoc(), rule::CallAlias,
              "'" + text(other) + "' and '" + text(changed) +
                  "' overlap, and '" + calleeName(call) +
                  "' may change owners through argument " +
                  std::to_string(i + 1),
              {},
              "pass only the pointer the callee changes, or make the other "
              "parameter 'const'");
        aliased = true;
        break;
      }
    }
    // An owner consumed by the call is not passed to it again.
    bool twice = false;
    for (unsigned i = 0; i < call.getNumArgs() && !twice; ++i) {
      const Argument &consumed = arguments[i];
      if ((consumed.effect != Effect::Consume &&
           consumed.effect != Effect::Release &&
           consumed.effect != Effect::Realloc) ||
          consumed.value.kind != Value::Kind::Load ||
          !consumed.value.place.owner || !consumed.value.place.named)
        continue;
      for (unsigned j = 0; j < call.getNumArgs(); ++j) {
        const Value &other = arguments[j].value;
        if (i == j || !other.place.known ||
            !consumed.value.place.place.covers(other.place.place))
          continue;
        error(call.getArg(j)->getExprLoc(), rule::OwnerMoved,
              "'" + text(call.getArg(i)) + "' is moved to '" +
                  calleeName(call) + "' (argument " + std::to_string(i + 1) +
                  ") and passed to it again (argument " +
                  std::to_string(j + 1) + ")",
              {},
              "pass a copy, or call it with distinct objects: the callee may "
              "release the first before it reads the second");
        twice = true;
        break;
      }
    }
    // A pointer to owners goes only where the parameter says so, or where
    // it is read only (`T *const *`).
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      const clang::QualType parameter = parameterType(call, i);
      const clang::Expr *argument = call.getArg(i)->IgnoreParenImpCasts();
      const clang::QualType given = argument->getType();
      const auto *declared = llvm::dyn_cast<clang::DeclRefExpr>(argument);
      const auto *member = llvm::dyn_cast<clang::MemberExpr>(argument);
      clang::QualType source = given;
      if (declared != nullptr)
        source = declared->getDecl()->getType();
      else if (member != nullptr)
        source = member->getMemberDecl()->getType();
      if (parameter.isNull() || !parameter->isPointerType() ||
          !source->isPointerType() || !isOwnerType(source->getPointeeType()))
        continue;
      const clang::QualType pointee = parameter->getPointeeType();
      if (isOwnerType(pointee) || pointee.isConstQualified() ||
          pointee->isVoidType())
        continue;
      error(argument->getExprLoc(), rule::OwnerSource,
            "'" + text(argument) + "' points to owners and parameter " +
                std::to_string(i + 1) + " of '" + calleeName(call) +
                "' does not say so: the callee could overwrite one",
            {},
            "declare the parameter 'T *WEAVEC_OWNED *', or 'T *const *' if "
            "it only reads them");
    }
    const clang::VarDecl *result = resultLocal(call);
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      const Argument &argument = arguments[i];
      const clang::Expr *expression = call.getArg(i);
      switch (argument.effect) {
      case Effect::Consume:
      case Effect::Release:
      case Effect::Realloc: {
        const Value &value = argument.value;
        if (value.kind == Value::Kind::Null ||
            value.kind == Value::Kind::OwnedCall)
          break;
        if (value.kind == Value::Kind::Load && value.place.owner) {
          How how = How::Moved;
          if (argument.effect == Effect::Release)
            how = How::Released;
          else if (argument.effect == Effect::Realloc)
            how = How::Realloc;
          moveOut(flow, value.place, stripCasts(expression), how, &call,
                  result);
          if (how == How::Realloc && result == nullptr && value.place.named &&
              reallocatesInPlace(call, expression))
            flow.owners[value.place.place].bits = StatePending;
          break;
        }
        if (argument.effect == Effect::Consume) {
          ownerSource(value, expression,
                      "argument " + std::to_string(i + 1) + " of '" +
                          calleeName(call) + "'");
          break;
        }
        const std::string from = text(expression);
        error(expression->getExprLoc(), rule::OwnerSource,
              "'" + from + "' is released by '" + calleeName(call) +
                  "' and is not an owner",
              {},
              value.kind == Value::Kind::Load &&
                      value.place.place.path.empty() &&
                      llvm::isa<clang::ParmVarDecl>(value.place.place.root)
                  ? "take the parameter as 'T *WEAVEC_OWNED " + from +
                        "' if the function releases it"
                  : "release a block through its owner; declare the pointer "
                    "that is responsible for it 'WEAVEC_OWNED'");
        break;
      }
      case Effect::Replace:
      case Effect::Update: {
        const Value &value = argument.value;
        if (value.kind != Value::Kind::Address || !value.place.owner)
          break;
        if (value.place.named) {
          if (argument.effect == Effect::Replace)
            dropCheck(flow, value.place.place, expression,
                      "is replaced by '" + calleeName(call) + "'");
          OwnerState next;
          next.bits = StateHolds | StateNull;
          next.since = &call;
          flow.owners[value.place.place] = next;
          eraseBelow(flow, value.place.place);
          endBelow(flow, value.place.place, How::Replaced, &call);
        } else {
          checkPath(value.place, expression, How::Replaced);
          endUnnamed(flow, value.place, How::Replaced, &call);
        }
        break;
      }
      case Effect::Borrow:
        if (argument.owners && argument.value.kind == Value::Kind::Load &&
            argument.value.place.named)
          checkWhole(flow, argument.value.place.place.with(StepDeref),
                     expression, [&] {
                       return cat("'", text(expression), "' is passed to '",
                                  calleeName(call), "'");
                     });
        else if (argument.owners &&
                 argument.value.kind == Value::Kind::Address &&
                 argument.value.place.named)
          checkWhole(flow, argument.value.place.place, expression, [&] {
            return cat("'", text(expression), "' is passed to '",
                       calleeName(call), "'");
          });
        if (argument.changing) {
          // M6: a callee given a reference cannot change owners through it.
          if (argument.value.kind == Value::Kind::Load &&
              !argument.value.place.owner &&
              (!argument.value.place.place.path.empty() ||
               !argument.value.place.place.root->hasLocalStorage() ||
               (borrowLocals.contains(argument.value.place.place.root) &&
                !pathLocals.contains(argument.value.place.place.root))))
            error(expression->getExprLoc(), rule::OwnerPath,
                  "'" + text(expression) + "' is a reference, and '" +
                      calleeName(call) +
                      "' may change the owners below what it points to",
                  {},
                  "pass the pointer that owns it (a parameter or an owner), "
                  "or declare the parameter 'const' if '" +
                      calleeName(call) + "' changes no owner");
          endReach(flow, argument, expression, call);
          leaveToCallee(flow, argument.value, isDrops(parameterType(call, i)),
                        call);
        }
        break;
      }
    }
  }

  template <typename ArgumentT>
  void endReach(Flow &flow, const ArgumentT &argument,
                const clang::Expr *expression, const clang::CallExpr &call) {
    const clang::VarDecl *self = nullptr;
    if (argument.value.kind == Value::Kind::Load &&
        argument.value.place.place.path.empty())
      self = argument.value.place.place.root;
    Place at = argument.reach.empty() ? Place{} : argument.reach.front().place;
    if (argument.value.kind == Value::Kind::Load && argument.value.place.known)
      at = argument.value.place.place;
    (void)expression;
    for (auto &[variable, borrow] : flow.borrows) {
      if (variable == self)
        continue;
      for (const Anchor &anchor : borrow.anchors)
        if (llvm::any_of(argument.reach, [&](const Anchor &reach) {
              return reach.place.covers(anchor.place) ||
                     (anchor.below && anchor.place.covers(reach.place));
            })) {
          endBorrow(borrow, How::Call, &call, at);
          break;
        }
    }
  }

  /// `p = realloc(p, n)` with `p` not a local: the classic leak.
  [[nodiscard]] bool reallocatesInPlace(const clang::CallExpr &call,
                                        const clang::Expr *argument) const {
    const auto *assign = llvm::dyn_cast_or_null<clang::BinaryOperator>(
        parents.getParentIgnoreParenCasts(&call));
    return assign != nullptr && assign->getOpcode() == clang::BO_Assign &&
           sameSlot(stripCasts(argument), assign->getLHS());
  }

  /// Amendment: the owners below what a call may change are the callee's
  /// to leave (its exit check leaves none moved); the caller does not check
  /// them for leaks after it.
  void leaveToCallee(Flow &flow, const Value &value, bool drops,
                     const clang::CallExpr &call) {
    Place target;
    clang::QualType type;
    if (value.kind == Value::Kind::Address && value.place.named) {
      target = value.place.place;
      type = value.place.type;
    } else if (value.kind == Value::Kind::Load && value.place.named &&
               value.place.type->isPointerType()) {
      target = value.place.place.with(StepDeref);
      type = value.place.type->getPointeeType();
    } else {
      return;
    }
    eraseBelow(flow, target);
    llvm::SmallVector<Place, 8> fields;
    ownerFields(type, target, fields);
    for (const Place &field : fields)
      flow.owners[field] =
          drops ? OwnerState{.bits = StateNull}
                : OwnerState{.bits = StateHolds | StateNull, .since = &call};
  }

  [[nodiscard]] bool isDrops(clang::QualType type) const {
    return !type.isNull() && type->isPointerType() &&
           hasAnnotation(annotationsOf(type, ast), DropsAnnotation);
  }

  /// The local a call's result is assigned to (`d = realloc(p, n)`).
  [[nodiscard]] const clang::VarDecl *
  resultLocal(const clang::CallExpr &call) const {
    const clang::Stmt *parent = parents.getParentIgnoreParenCasts(&call);
    if (const auto *assign =
            llvm::dyn_cast_or_null<clang::BinaryOperator>(parent);
        assign != nullptr && assign->getOpcode() == clang::BO_Assign) {
      const auto *reference = llvm::dyn_cast<clang::DeclRefExpr>(
          assign->getLHS()->IgnoreParenImpCasts());
      return reference != nullptr
                 ? llvm::dyn_cast<clang::VarDecl>(reference->getDecl())
                 : nullptr;
    }
    if (const auto *declaration =
            llvm::dyn_cast_or_null<clang::DeclStmt>(parent);
        declaration != nullptr && declaration->isSingleDecl())
      return llvm::dyn_cast<clang::VarDecl>(declaration->getSingleDecl());
    return nullptr;
  }

  void handleAssign(Flow &flow, const clang::Expr *target,
                    const clang::Expr *source, const clang::Stmt *by) {
    const Located destination = placeOf(target);
    if (target->getType()->isRecordType()) {
      if (holdsOwners(target->getType()))
        copyRecord(flow, destination, target, source, by);
      return;
    }
    if (!destination.known)
      return;
    if (destination.owner) {
      storeOwner(flow, destination, target, source, by, /*drop=*/true);
      return;
    }
    if (destination.place.path.empty() &&
        borrowLocals.contains(destination.place.root)) {
      if (const Value value = classify(source);
          value.kind == Value::Kind::OwnedCall)
        error(value.call->getExprLoc(), rule::OwnerLeak,
              "'" + calleeName(*value.call) + "' returns an owner and '" +
                  text(target) +
                  "' is a borrow (it started as one): the block would have "
                  "no owner",
              {},
              "keep the block in an owner local of its own and point '" +
                  text(target) + "' at it");
      flow.borrows[destination.place.root] = borrowOf(flow, source);
      return;
    }
    const Value value = classify(source);
    if (value.kind == Value::Kind::OwnedCall)
      noOwner(*value.call, target);
    if (value.kind == Value::Kind::Load && value.place.owner &&
        value.place.named) {
      OwnerState &state = flow.owners[value.place.place];
      if (state.bits == 0)
        state = absent(flow, value.place.place);
      state.copied = by;
    }
  }

  void noOwner(const clang::CallExpr &call, const clang::Expr *target) {
    const std::string name = text(target);
    const auto *member =
        llvm::dyn_cast<clang::MemberExpr>(target->IgnoreParenImpCasts());
    error(call.getExprLoc(), rule::OwnerLeak,
          "'" + calleeName(call) + "' returns an owner and '" + name +
              "' is not one: the block would have no owner",
          {},
          member != nullptr
              ? "declare the field '" +
                    member->getMemberDecl()->getName().str() +
                    "' WEAVEC_OWNED, or keep the result in a local owner and "
                    "store a reference to it"
              : "keep the result in an owner, and store a reference to it");
  }

  /// A struct that holds owners, copied as a whole: its owners move.
  void copyRecord(Flow &flow, const Located &destination,
                  const clang::Expr *target, const clang::Expr *source,
                  const clang::Stmt *by) {
    llvm::SmallVector<Place, 8> fields;
    if (destination.known && destination.named)
      ownerFields(target->getType(), destination.place, fields);
    for (const Place &field : fields)
      dropCheck(flow, field, target, "is assigned over");
    const Value value = classify(source);
    if (value.kind == Value::Kind::Load && value.place.known &&
        !checkPath(value.place, source, How::Moved))
      return;
    if (value.kind == Value::Kind::Load && value.place.known &&
        value.place.named) {
      llvm::SmallVector<Place, 8> moved;
      ownerFields(source->getType(), value.place.place, moved);
      for (const Place &field : moved) {
        OwnerState &state = flow.owners[field];
        if (state.bits == 0)
          state = absent(flow, field);
        state.bits = StateMoved;
        state.how = How::Stored;
        state.cause = by;
        endBelow(flow, field, How::Stored, by);
      }
    }
    if (destination.known && destination.named) {
      for (const Place &field : fields) {
        flow.owners[field] =
            OwnerState{.bits = StateHolds | StateNull, .since = by};
        eraseBelow(flow, field);
      }
      flow.owners[destination.place].fresh = false;
    }
  }

  void handleDecl(Flow &flow, const clang::VarDecl &variable,
                  const clang::DeclStmt *by) {
    if (!variable.hasLocalStorage())
      return;
    Located located;
    located.known = true;
    located.named = true;
    located.place.root = &variable;
    located.type = variable.getType();
    located.owner = isOwnerVariable(variable);
    const clang::Expr *init = variable.getInit();
    if (located.owner) {
      flow.owners[located.place] = OwnerState{.bits = StateNull};
      eraseBelow(flow, located.place);
      if (init != nullptr) {
        clang::DeclRefExpr reference(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            ast, const_cast<clang::VarDecl *>(&variable), false,
            variable.getType(), clang::VK_LValue, variable.getLocation());
        storeOwner(flow, located, &reference, init, by, /*drop=*/false);
      }
      return;
    }
    if (borrowLocals.contains(&variable)) {
      flow.borrows[&variable] =
          init != nullptr ? borrowOf(flow, init) : BorrowState{};
      return;
    }
    if (!variable.getType()->isRecordType() || !holdsOwners(variable.getType()))
      return;
    eraseBelow(flow, located.place);
    OwnerState record;
    record.fresh = true;
    if (init == nullptr) {
      flow.owners[located.place] = record;
      return;
    }
    const auto *list =
        llvm::dyn_cast<clang::InitListExpr>(init->IgnoreParens());
    if (list == nullptr) {
      // A declaration drops nothing: its owners take the value's.
      flow.owners[located.place] = record;
      clang::DeclRefExpr reference(
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
          ast, const_cast<clang::VarDecl *>(&variable), false,
          variable.getType(), clang::VK_LValue, variable.getLocation());
      copyRecord(flow, located, &reference, init, by);
      return;
    }
    flow.owners[located.place] = record;
    initRecord(flow, located, *list, by);
  }

  /// A struct's initializer list: each owner field takes its value.
  void initRecord(Flow &flow, const Located &located,
                  const clang::InitListExpr &visited, const clang::Stmt *by) {
    const clang::InitListExpr &list =
        visited.isSyntacticForm() && visited.getSemanticForm() != nullptr
            ? *visited.getSemanticForm()
            : visited;
    const clang::RecordDecl *record = list.getType()->getAsRecordDecl();
    if (record == nullptr || record->isUnion())
      return;
    unsigned index = 0;
    for (const clang::FieldDecl *field : record->fields()) {
      if (index >= list.getNumInits())
        break;
      const clang::Expr *element = list.getInit(index++);
      Located inner = located;
      inner.place.path.push_back(stepOf(field));
      inner.type = field->getType();
      inner.owner = isOwnerType(field->getType());
      if (inner.owner) {
        if (llvm::isa<clang::ImplicitValueInitExpr>(element))
          continue;
        storeOwner(flow, inner, element, element, by, /*drop=*/false);
      } else if (const auto *nested = llvm::dyn_cast<clang::InitListExpr>(
                     element->IgnoreParens())) {
        initRecord(flow, inner, *nested, by);
      }
    }
  }

  void handleReturn(Flow &flow, const clang::ReturnStmt &ret) {
    const clang::Expr *value = ret.getRetValue();
    if (value == nullptr)
      return;
    if (isOwnerType(function.getReturnType())) {
      const Value classified = classify(value);
      if (classified.kind == Value::Kind::Null ||
          classified.kind == Value::Kind::OwnedCall)
        return;
      if (classified.kind == Value::Kind::Load && classified.place.owner) {
        moveOut(flow, classified.place, stripCasts(value), How::Returned, &ret);
        return;
      }
      ownerSource(classified, value,
                  "the result of '" + function.getName().str() + "'");
      return;
    }
    // An owner returned where the result is not one: say so here, not
    // where the local's scope ends.
    if (const Value classified = classify(value);
        classified.kind == Value::Kind::Load && classified.place.owner &&
        classified.place.named && classified.place.place.path.empty() &&
        (stateOf(flow, classified.place.place).bits &
         (StateHolds | StatePending)) != 0) {
      error(value->getExprLoc(), rule::OwnerLeak,
            "'" + text(value) + "' owns a block and '" +
                function.getName().str() + "' does not return an owner",
            {},
            "declare the result 'WEAVEC_OWNED' (after its '*') if the caller "
            "releases it, or return a reference to something an owner keeps");
      OwnerState &state = flow.owners[classified.place.place];
      state.bits = StateNull;
      return;
    }
    if (value->getType()->isRecordType() && holdsOwners(value->getType())) {
      const Value classified = classify(value);
      if (classified.kind == Value::Kind::Load && classified.place.named) {
        llvm::SmallVector<Place, 8> fields;
        ownerFields(value->getType(), classified.place.place, fields);
        for (const Place &field : fields) {
          OwnerState &state = flow.owners[field];
          state.bits = StateMoved;
          state.how = How::Returned;
          state.cause = &ret;
        }
      }
    }
  }

  /// A local's lifetime ends: what it owns must be gone.
  void endLifetime(Flow &flow, const clang::VarDecl &variable,
                   const clang::Stmt *trigger) {
    const bool returning = llvm::isa_and_nonnull<clang::ReturnStmt>(trigger);
    clang::SourceLocation at;
    if (const auto *compound =
            llvm::dyn_cast_or_null<clang::CompoundStmt>(trigger))
      at = compound->getRBracLoc();
    else
      at = locationOf(trigger);
    if (at.isInvalid())
      at = variable.getLocation();
    const Place root{.root = &variable};
    llvm::SmallVector<Place, 8> places;
    if (isOwnerVariable(variable))
      places.push_back(root);
    else if (variable.getType()->isRecordType())
      ownerFields(variable.getType(), root, places);
    for (const Place &place : places) {
      const OwnerState state = stateOf(flow, place);
      if ((state.bits & (StateHolds | StatePending)) == 0)
        continue;
      const std::string name = text(place);
      error(at, rule::OwnerLeak,
            returning
                ? "returning while '" + name + "' may still own its block"
                : "'" + name +
                      "' goes out of scope while it may still own its block",
            state.copied != nullptr ? locationOf(state.copied)
                                    : locationOf(state.since),
            state.copied != nullptr
                ? "'" + name +
                      "' was stored here where it is only a reference: declare "
                      "that place 'WEAVEC_OWNED' to move the block there"
                : "'" + name +
                      "' owns a block since here; release it, return it as a "
                      "'WEAVEC_OWNED' result, or store it in an owner before "
                      "this");
    }
    for (auto it = flow.owners.begin(); it != flow.owners.end();)
      if (it->first.root == &variable)
        it = flow.owners.erase(it);
      else
        ++it;
    flow.borrows.erase(&variable);
  }

  /// At an edge into the exit: the parameters (§3, `owner-leak` and
  /// `owner-moved`).
  void exitChecks(const Flow &flow, clang::SourceLocation at) {
    for (const clang::ParmVarDecl *parameter : function.parameters()) {
      const Place root{.root = parameter};
      llvm::SmallVector<Place, 8> places;
      if (isOwnerType(parameter->getType()))
        places.push_back(root);
      else if (parameter->getType()->isRecordType())
        ownerFields(parameter->getType(), root, places);
      for (const Place &place : places) {
        const OwnerState state = stateOf(flow, place);
        if ((state.bits & (StateHolds | StatePending)) == 0)
          continue;
        const std::string name = text(place);
        error(at, rule::OwnerLeak,
              "returning while '" + name + "' may still own its block", {},
              "release '" + name +
                  "', return it as a 'WEAVEC_OWNED' result, or store it in an "
                  "owner before this");
      }
      // A WEAVEC_DROPS parameter's owners are all null when it returns.
      if (!isDrops(parameter->getType()))
        continue;
      llvm::SmallVector<Place, 8> dropped;
      ownerFields(parameter->getType()->getPointeeType(), root.with(StepDeref),
                  dropped);
      for (const Place &place : dropped) {
        const OwnerState state = stateOf(flow, place);
        if ((state.bits & (StateHolds | StatePending)) == 0)
          continue;
        const std::string name = text(place);
        error(at, rule::OwnerLeak,
              cat("returning while '", name, "' may still own its block, and '",
                  parameter->getName().str(), "' is WEAVEC_DROPS"),
              {},
              cat("release '", name, "' and assign it null ('free(", name,
                  "); ", name, " = NULL;') before this"));
      }
    }
    for (const auto &[place, state] : flow.owners) {
      if (place.path.empty() || (state.bits & StateMoved) == 0 ||
          !llvm::isa<clang::ParmVarDecl>(place.root))
        continue;
      if (const auto found = flow.owners.find(Place{.root = place.root});
          found != flow.owners.end() && (found->second.bits & StateMoved) != 0)
        continue;
      if (releasedAbove(flow, place, Place{.root = place.root}))
        continue;
      const std::string name = text(place);
      error(at, rule::OwnerMoved,
            "'" + name + "' is left moved when '" + function.getName().str() +
                "' returns",
            locationOf(state.cause),
            "it was " + described(state.how, state.cause) +
                " here; assign it before returning: '" + name + " = NULL;'");
    }
  }

  //===--------------------------------------------------------------------===//
  // Branches
  //===--------------------------------------------------------------------===//

  void refine(Flow &flow, const clang::Expr *condition, bool truth) {
    const clang::Expr *e = condition->IgnoreParens();
    while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
      if (cast->getCastKind() == clang::CK_LValueToRValue)
        break;
      e = cast->getSubExpr()->IgnoreParens();
    }
    if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
        unary != nullptr && unary->getOpcode() == clang::UO_LNot) {
      refine(flow, unary->getSubExpr(), !truth);
      return;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      if (binary->getOpcode() == clang::BO_Assign) {
        refinePlace(flow, binary->getLHS(), truth);
        return;
      }
      if (binary->getOpcode() != clang::BO_EQ &&
          binary->getOpcode() != clang::BO_NE)
        return;
      const auto isNull = [&](const clang::Expr *side) {
        return side->isNullPointerConstant(
                   ast, clang::Expr::NPC_ValueDependentIsNotNull) !=
               clang::Expr::NPCK_NotNull;
      };
      const clang::Expr *other = nullptr;
      if (isNull(binary->getRHS()))
        other = binary->getLHS();
      else if (isNull(binary->getLHS()))
        other = binary->getRHS();
      if (other == nullptr || !other->getType()->isPointerType())
        return;
      const bool nonNull = (binary->getOpcode() == clang::BO_NE) == truth;
      refine(flow, other, nonNull);
      return;
    }
    if (!e->getType()->isPointerType())
      return;
    if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
      refinePlace(flow, cast->getSubExpr(), truth);
  }

  void refinePlace(Flow &flow, const clang::Expr *lvalue, bool nonNull) {
    const Located located = placeOf(lvalue);
    if (!located.known || !located.named)
      return;
    if (located.owner) {
      OwnerState state = stateOf(flow, located.place);
      if ((state.bits & (StateMoved | StatePending)) == 0) {
        // A test the state already decides the other way: the edge is
        // never taken.
        if ((nonNull && state.bits == StateNull) ||
            (!nonNull && state.bits == StateHolds))
          infeasible = true;
        state.bits = nonNull ? StateHolds : StateNull;
        flow.owners[located.place] = state;
        if (!nonNull)
          eraseBelow(flow, located.place);
        // What it was moved from held null too.
        if (!nonNull && state.from)
          if (const auto found = flow.owners.find(*state.from);
              found != flow.owners.end() && found->second.cause == state.took &&
              (found->second.bits & StateMoved) != 0)
            found->second.bits = StateNull;
      }
    }
    if (!located.place.path.empty())
      return;
    // `realloc`'s result tested: its argument was released or not.
    for (auto &[place, state] : flow.owners)
      if ((state.bits & StatePending) != 0 &&
          state.pending == located.place.root) {
        state.bits = nonNull ? StateMoved : StateHolds;
        state.pending = nullptr;
        if (nonNull)
          state.how = How::Realloc;
      }
  }

  //===--------------------------------------------------------------------===//
  // The dataflow
  //===--------------------------------------------------------------------===//

  void transferBlock(const clang::CFGBlock &block, Flow &flow) {
    for (const clang::CFGElement &element : block) {
      if (const auto statement = element.getAs<clang::CFGStmt>()) {
        transfer(flow, statement->getStmt());
        continue;
      }
      if (const auto lifetime = element.getAs<clang::CFGLifetimeEnds>())
        endLifetime(flow, *lifetime->getVarDecl(), lifetime->getTriggerStmt());
    }
  }

  [[nodiscard]] Flow entryFlow() const {
    Flow flow;
    flow.reached = true;
    for (const clang::ParmVarDecl *parameter : function.parameters())
      if (isOwnerType(parameter->getType()))
        flow.owners[Place{.root = parameter}] =
            OwnerState{.bits = StateHolds | StateNull};
    // A reassigned parameter starts as the caller's pointer.
    for (const clang::VarDecl *parameter : reassigned)
      flow.borrows[parameter].roots.push_back(parameter);
    return flow;
  }

  /// The flow along an edge: the branch's condition refines it.
  [[nodiscard]] Flow along(const clang::CFGBlock &block, const Flow &out,
                           unsigned successor) {
    Flow flow = out;
    if (block.succ_size() != 2)
      return flow;
    const clang::Stmt *terminator = block.getTerminatorStmt();
    if (terminator == nullptr || llvm::isa<clang::SwitchStmt>(terminator))
      return flow;
    infeasible = false;
    if (const clang::Expr *condition = block.getLastCondition())
      refine(flow, condition, successor == 0);
    if (infeasible)
      flow.reached = false;
    return flow;
  }

  /// Whether the dataflow settled (and its errors were reported).
  bool solve() {
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
    const auto absentOf = [this](const Flow &flow) {
      return [this, &flow](const Place &place) { return absent(flow, place); };
    };
    while (!work.empty()) {
      if (++steps > limit)
        return false;
      const clang::CFGBlock *block = order[*work.begin()];
      work.erase(work.begin());
      Flow out = in[block->getBlockID()];
      transferBlock(*block, out);
      unsigned index = 0;
      for (const clang::CFGBlock::AdjacentBlock &adjacent : block->succs()) {
        const unsigned successor = index++;
        const clang::CFGBlock *next = adjacent.getReachableBlock();
        if (next == nullptr)
          continue;
        const Flow edge = along(*block, out, successor);
        Flow &target = in[next->getBlockID()];
        if (join(target, edge, absentOf(target), absentOf(edge)))
          work.insert(rank[next->getBlockID()]);
      }
    }
    // Report from the converged states.
    emit = true;
    const clang::CFGBlock &exit = graph->getExit();
    for (const clang::CFGBlock *block : order) {
      Flow flow = in[block->getBlockID()];
      if (!flow.reached)
        continue;
      transferBlock(*block, flow);
      unsigned toExit = 0;
      bool exits = false;
      for (const clang::CFGBlock::AdjacentBlock &adjacent : block->succs()) {
        if (adjacent.getReachableBlock() == &exit) {
          exits = true;
          break;
        }
        ++toExit;
      }
      if (!exits || block->hasNoReturnElement() || block == &exit)
        continue;
      // A loop's test that leaves the function refines what it tested.
      flow = along(*block, flow, toExit);
      if (!flow.reached)
        continue;
      clang::SourceLocation at;
      for (const clang::CFGElement &element : *block)
        if (const auto statement = element.getAs<clang::CFGStmt>())
          if (const auto *ret =
                  llvm::dyn_cast<clang::ReturnStmt>(statement->getStmt()))
            at = ret->getBeginLoc();
      if (at.isInvalid())
        if (const auto *compound =
                llvm::dyn_cast<clang::CompoundStmt>(function.getBody()))
          at = compound->getRBracLoc();
      exitChecks(flow, at);
    }
    return true;
  }
};

} // namespace

void checkOwnership(clang::ASTContext &ast, const clang::FunctionDecl &function,
                    SafeUnit &unit, const RuleReport &report) {
  if (!ownershipRules)
    return;
  Ownership(ast, function, unit, report).run();
}

} // namespace weavec::frontend
