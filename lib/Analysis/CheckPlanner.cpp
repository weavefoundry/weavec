//===- CheckPlanner.cpp - Checks from witnesses (RFC 0030) ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/CheckPlanner.h"

#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <string>
#include <utility>

namespace weavec::analysis {

//===----------------------------------------------------------------------===//
// Witness terms
//===----------------------------------------------------------------------===//

WitnessTerm WitnessTerm::ofConstant(std::int64_t value) {
  WitnessTerm term;
  term.kind = Kind::Constant;
  term.constant = value;
  return term;
}

WitnessTerm WitnessTerm::ofPlace(const clang::ValueDecl &decl,
                                 std::vector<core::CheckPathStep> path) {
  WitnessTerm term;
  term.kind = Kind::Place;
  term.decl = &decl;
  term.path = std::move(path);
  return term;
}

WitnessTerm WitnessTerm::sizeOf(clang::QualType type) {
  WitnessTerm term;
  term.kind = Kind::SizeOf;
  term.type = type;
  return term;
}

WitnessTerm WitnessTerm::ofExpr(const clang::Expr &expr) {
  WitnessTerm term;
  term.kind = Kind::Expr;
  term.expr = &expr;
  return term;
}

static WitnessTerm binaryTerm(WitnessTerm::Kind kind, WitnessTerm lhs,
                              WitnessTerm rhs) {
  WitnessTerm term;
  term.kind = kind;
  term.operands.push_back(std::move(lhs));
  term.operands.push_back(std::move(rhs));
  return term;
}

WitnessTerm WitnessTerm::add(WitnessTerm lhs, WitnessTerm rhs) {
  return binaryTerm(Kind::Add, std::move(lhs), std::move(rhs));
}

WitnessTerm WitnessTerm::sub(WitnessTerm lhs, WitnessTerm rhs) {
  return binaryTerm(Kind::Sub, std::move(lhs), std::move(rhs));
}

WitnessTerm WitnessTerm::mul(WitnessTerm lhs, WitnessTerm rhs) {
  return binaryTerm(Kind::Mul, std::move(lhs), std::move(rhs));
}

WitnessTerm WitnessTerm::div(WitnessTerm lhs, std::int64_t divisor) {
  return binaryTerm(Kind::Div, std::move(lhs), ofConstant(divisor));
}

WitnessTerm WitnessTerm::strLen(WitnessTerm pointer) {
  WitnessTerm term;
  term.kind = Kind::StrLen;
  term.operands.push_back(std::move(pointer));
  return term;
}

std::string WitnessTerm::toString() const {
  switch (kind) {
  case Kind::Constant:
    return std::to_string(constant);
  case Kind::Place: {
    std::string text = decl != nullptr ? decl->getNameAsString() : "<null>";
    for (const core::CheckPathStep &step : path) {
      if (step.kind == core::CheckPathStep::Kind::Deref) {
        text.insert(0, "(*");
        text += ')';
      } else {
        text += '.';
        text += step.field;
      }
    }
    return text;
  }
  case Kind::SizeOf:
    return "sizeof(" + type.getAsString() + ")";
  case Kind::Add:
  case Kind::Sub:
  case Kind::Mul:
  case Kind::Div: {
    if (operands.size() != 2)
      return "<malformed>";
    const char *op = " * ";
    if (kind == Kind::Add)
      op = " + ";
    else if (kind == Kind::Sub)
      op = " - ";
    else if (kind == Kind::Div)
      op = " / ";
    std::string text = "(" + operands[0].toString();
    text += op;
    text += operands[1].toString();
    text += ')';
    return text;
  }
  case Kind::StrLen:
    return "strlen(" +
           (operands.empty() ? std::string("<malformed>")
                             : operands.front().toString()) +
           ")";
  case Kind::Expr:
    return "<expr>";
  }
  return "<malformed>";
}

//===----------------------------------------------------------------------===//
// Handles and witnesses
//===----------------------------------------------------------------------===//

std::uint64_t PlaceHandleTable::place(const clang::ValueDecl &decl) {
  const auto [it, inserted] = placeIds.try_emplace(&decl, placeList.size() + 1);
  if (inserted)
    placeList.push_back(&decl);
  return it->second;
}

std::uint64_t PlaceHandleTable::type(clang::QualType type) {
  const auto [it, inserted] =
      typeIds.try_emplace(type.getAsOpaquePtr(), typeList.size() + 1);
  if (inserted)
    typeList.push_back(type);
  return it->second;
}

const clang::ValueDecl *
PlaceHandleTable::resolvePlace(std::uint64_t handle) const {
  return handle == 0 || handle > placeList.size() ? nullptr
                                                  : placeList[handle - 1];
}

clang::QualType PlaceHandleTable::resolveType(std::uint64_t handle) const {
  return handle == 0 || handle > typeList.size() ? clang::QualType()
                                                 : typeList[handle - 1];
}

void WitnessTable::add(core::SiteId site, core::Facet facet,
                       CheckWitness witness) {
  table[{site, facet}].push_back(std::move(witness));
}

llvm::ArrayRef<CheckWitness> WitnessTable::of(core::SiteId site,
                                              core::Facet facet) const {
  const auto found = table.find({site, facet});
  if (found == table.end())
    return {};
  return found->second;
}

void WitnessTable::clear(std::uint32_t function) {
  std::erase_if(table, [function](const auto &entry) {
    return entry.first.first.function == function;
  });
}

//===----------------------------------------------------------------------===//
// Expressibility (§10.3)
//===----------------------------------------------------------------------===//

namespace {

/// The locals whose address a body takes.
class AddressTakenLocals
    : public clang::RecursiveASTVisitor<AddressTakenLocals> {
public:
  llvm::DenseSet<const clang::VarDecl *> taken;

  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitUnaryOperator(clang::UnaryOperator *op) {
    if (op->getOpcode() != clang::UO_AddrOf)
      return true;
    const clang::Expr *object = op->getSubExpr()->IgnoreParens();
    while (const auto *member = llvm::dyn_cast<clang::MemberExpr>(object)) {
      if (member->isArrow())
        return true;
      object = member->getBase()->IgnoreParens();
    }
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(object))
      if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
        taken.insert(variable->getCanonicalDecl());
    return true;
  }
};

/// The places a term names, for rules 4, 5 and 7.
struct TermPlaces {
  llvm::SmallVector<const clang::ValueDecl *, 4> roots;
  /// Some place is reached through a pointer (`->` or `*`).
  bool throughPointer = false;
};

/// Builds the core form of a witness term, checking rules 1, 2, 3 and 6.
class TermBuilder {
public:
  TermBuilder(const clang::ASTContext &ctx,
              const llvm::DenseSet<const clang::VarDecl *> &addressTakenLocals,
              PlaceHandleTable &handleTable)
      : context(ctx), addressTaken(addressTakenLocals), handles(handleTable) {}

  std::optional<core::CheckTerm>
  build(const WitnessTerm &term, const std::optional<core::CheckTerm> &have) {
    switch (term.kind) {
    case WitnessTerm::Kind::Constant:
      return core::CheckTerm::ofConstant(term.constant);
    case WitnessTerm::Kind::Place:
      if (term.decl == nullptr)
        return fail("it names nothing");
      return place(*term.decl, term.path);
    case WitnessTerm::Kind::SizeOf:
      if (term.type.isNull())
        return fail("it takes the size of nothing");
      return core::CheckTerm::sizeOf(handles.type(term.type));
    case WitnessTerm::Kind::Add:
    case WitnessTerm::Kind::Sub:
    case WitnessTerm::Kind::Mul: {
      if (term.operands.size() != 2)
        return fail("it is malformed");
      auto lhs = build(term.operands[0], have);
      auto rhs = build(term.operands[1], have);
      if (!lhs || !rhs)
        return std::nullopt;
      if (term.kind == WitnessTerm::Kind::Add)
        return core::CheckTerm::add(std::move(*lhs), std::move(*rhs));
      if (term.kind == WitnessTerm::Kind::Sub)
        return core::CheckTerm::sub(std::move(*lhs), std::move(*rhs));
      return core::CheckTerm::mul(std::move(*lhs), std::move(*rhs));
    }
    case WitnessTerm::Kind::Div: {
      // Rounding down is safe only for an extent (a have), which is all
      // the engine divides (§7.4).
      if (term.operands.size() != 2 ||
          term.operands[1].kind != WitnessTerm::Kind::Constant ||
          term.operands[1].constant <= 0)
        return fail("it divides by something other than a positive constant");
      auto lhs = build(term.operands[0], have);
      if (!lhs)
        return std::nullopt;
      return core::CheckTerm::div(std::move(*lhs), term.operands[1].constant);
    }
    case WitnessTerm::Kind::StrLen: {
      // Rule 2: `strlen(p)` is `__weavec_strnlen(p, have)`.
      if (term.operands.empty())
        return fail("it is malformed");
      if (!have)
        return fail("the string length has no bound here");
      auto pointer = build(term.operands.front(), have);
      if (!pointer)
        return std::nullopt;
      return core::CheckTerm::strnlen(std::move(*pointer), *have);
    }
    case WitnessTerm::Kind::Expr:
      if (term.expr == nullptr)
        return fail("it names nothing");
      return fromExpr(*term.expr, 0);
    }
    return fail("it is malformed");
  }

  TermPlaces places;
  std::string failure;

private:
  const clang::ASTContext &context;
  const llvm::DenseSet<const clang::VarDecl *> &addressTaken;
  PlaceHandleTable &handles;

  std::nullopt_t fail(std::string why) {
    if (failure.empty())
      failure = std::move(why);
    return std::nullopt;
  }

  /// Rule 6: a leaf of at most 64 bits.
  [[nodiscard]] bool fits(clang::QualType type) const {
    return !type.isNull() && !type->isDependentType() &&
           type->isConstantSizeType() && !type->isIncompleteType() &&
           context.getTypeSize(type) <= 64;
  }

  /// Rule 1: parameters, non-volatile locals whose address is not taken,
  /// `const` locals, and fields reached from them through `.` and `->`.
  std::optional<core::CheckTerm> place(const clang::ValueDecl &decl,
                                       std::vector<core::CheckPathStep> path) {
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(&decl);
    if (variable == nullptr)
      return fail("it names neither a parameter nor a local");
    if (!variable->hasLocalStorage())
      return fail("it names a global");
    const clang::QualType type = variable->getType();
    if (type.isVolatileQualified())
      return fail("it reads a volatile object");
    if (!llvm::isa<clang::ParmVarDecl>(variable) &&
        addressTaken.contains(variable->getCanonicalDecl()) &&
        !type.isConstQualified())
      return fail("it names a local whose address is taken");
    places.roots.push_back(&decl);
    if (llvm::any_of(path, [](const core::CheckPathStep &step) {
          return step.kind == core::CheckPathStep::Kind::Deref;
        }))
      places.throughPointer = true;
    return core::CheckTerm::ofPlace(handles.place(decl), std::move(path));
  }

  /// A place read through an lvalue: a declaration and `.`, `->` and `*`
  /// steps below it.
  std::optional<core::CheckTerm> fromLvalue(const clang::Expr &lvalue) {
    if (!fits(lvalue.getType()) && !lvalue.getType()->isArrayType())
      return fail("it does not fit in 64 bits");
    std::vector<core::CheckPathStep> steps;
    const clang::Expr *expr = lvalue.IgnoreParens();
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (expr->getType().isVolatileQualified())
        return fail("it reads a volatile object");
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
        std::ranges::reverse(steps);
        return place(*ref->getDecl(), std::move(steps));
      }
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
        steps.push_back(core::CheckPathStep::member(
            member->getMemberDecl()->getNameAsString()));
        if (member->isArrow()) {
          steps.push_back(core::CheckPathStep::deref());
          expr = member->getBase()->IgnoreParenImpCasts();
        } else {
          expr = member->getBase()->IgnoreParens();
        }
        continue;
      }
      if (const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(expr);
          deref != nullptr && deref->getOpcode() == clang::UO_Deref) {
        steps.push_back(core::CheckPathStep::deref());
        expr = deref->getSubExpr()->IgnoreParenImpCasts();
        continue;
      }
      break;
    }
    return fail("it is not a parameter, a local or a field of one");
  }

  std::optional<core::CheckTerm> fromExpr(const clang::Expr &expr,
                                          unsigned depth) {
    if (depth > 32)
      return fail("it is too deep");
    // Rule 3.
    if (expr.HasSideEffects(context, /*IncludePossibleEffects=*/true))
      return fail("it has side effects");
    const clang::Expr *e = expr.IgnoreParens();
    if (!e->isValueDependent()) {
      clang::Expr::EvalResult folded;
      if (e->EvaluateAsInt(folded, context) && !folded.HasSideEffects) {
        const llvm::APSInt &value = folded.Val.getInt();
        if (value.getSignificantBits() > 64 ||
            (value.isUnsigned() && value.getActiveBits() > 63))
          return fail("it does not fit in 64 bits");
        return core::CheckTerm::ofConstant(value.getExtValue());
      }
    }
    if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(e)) {
      switch (cast->getCastKind()) {
      case clang::CK_LValueToRValue:
        return fromLvalue(*cast->getSubExpr());
      case clang::CK_IntegralCast:
      case clang::CK_NoOp:
        if (!fits(cast->getSubExpr()->getType()))
          return fail("it does not fit in 64 bits");
        return fromExpr(*cast->getSubExpr(), depth + 1);
      case clang::CK_ArrayToPointerDecay:
        return fromLvalue(*cast->getSubExpr());
      default:
        return fail("it converts between kinds of values");
      }
    }
    if (llvm::isa<clang::DeclRefExpr, clang::MemberExpr>(e))
      return fromLvalue(*e);
    if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(e);
        trait != nullptr && trait->getKind() == clang::UETT_SizeOf)
      return core::CheckTerm::sizeOf(handles.type(trait->getTypeOfArgument()));
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e)) {
      const clang::BinaryOperatorKind op = binary->getOpcode();
      if (op != clang::BO_Add && op != clang::BO_Sub && op != clang::BO_Mul)
        return fail("it uses an operator the term helpers do not have");
      auto lhs = fromExpr(*binary->getLHS(), depth + 1);
      auto rhs = fromExpr(*binary->getRHS(), depth + 1);
      if (!lhs || !rhs)
        return std::nullopt;
      // Term helpers, never C arithmetic (§10.2).
      if (op == clang::BO_Add)
        return core::CheckTerm::add(std::move(*lhs), std::move(*rhs));
      if (op == clang::BO_Sub)
        return core::CheckTerm::sub(std::move(*lhs), std::move(*rhs));
      return core::CheckTerm::mul(std::move(*lhs), std::move(*rhs));
    }
    if (llvm::isa<clang::CallExpr>(e))
      return fail("it calls a function"); // Rule 2.
    return fail("it is not a constant, a parameter, a local or a field");
  }
};

} // namespace

/// Rule 7: whether writing through `lvalue` may change a place `places`
/// names.
static bool writesPlace(const clang::Expr *lvalue, const TermPlaces &places) {
  const clang::Expr *expr = lvalue->IgnoreParenImpCasts();
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
      expr = member->getBase()->IgnoreParenImpCasts();
      continue;
    }
    const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(expr);
    if (deref != nullptr && deref->getOpcode() == clang::UO_Deref) {
      // A write through a pointer may reach any place read through one.
      if (places.throughPointer)
        return true;
      expr = deref->getSubExpr()->IgnoreParenImpCasts();
      continue;
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
      if (places.throughPointer)
        return true;
      expr = subscript->getBase()->IgnoreParenImpCasts();
      continue;
    }
    break;
  }
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr);
  return ref != nullptr && llvm::is_contained(places.roots, ref->getDecl());
}

/// Rule 7: whether evaluating `stmt` may write a place `places` names. The
/// site's own call runs after its check, so only calls inside it count; a
/// callee can write what is reached through a pointer, but not parameters
/// or locals whose address is not taken.
static bool mayWrite(const clang::Stmt *stmt, const TermPlaces &places,
                     const clang::Stmt *site, unsigned depth = 0) {
  if (stmt == nullptr || depth > 256)
    return false;
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt);
      binary != nullptr && binary->isAssignmentOp() &&
      writesPlace(binary->getLHS(), places))
    return true;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt);
      unary != nullptr &&
      (unary->isIncrementDecrementOp() ||
       unary->getOpcode() == clang::UO_AddrOf) &&
      writesPlace(unary->getSubExpr(), places))
    return true;
  if (llvm::isa<clang::CallExpr>(stmt) && stmt != site && places.throughPointer)
    return true;
  return llvm::any_of(stmt->children(), [&](const clang::Stmt *child) {
    return mayWrite(child, places, site, depth + 1);
  });
}

const llvm::DenseSet<const clang::VarDecl *> &
CheckPlanner::addressTakenIn(const clang::FunctionDecl &function) const {
  const auto found = addressTaken.find(function.getCanonicalDecl());
  if (found != addressTaken.end())
    return found->second;
  AddressTakenLocals visitor;
  if (clang::Stmt *body = function.getBody())
    visitor.TraverseStmt(body);
  return addressTaken[function.getCanonicalDecl()] = std::move(visitor.taken);
}

CheckPlanner::Expression
CheckPlanner::express(const WitnessTerm &term, const SiteInfo &site,
                      const CheckWitness &witness, PlaceHandleTable &handles,
                      const std::optional<core::CheckTerm> &have) const {
  static const llvm::DenseSet<const clang::VarDecl *> None;
  const SiteIndex::FunctionSites *function = nullptr;
  if (site.id.function < sites.functions().size())
    function = &sites.functions()[site.id.function];
  TermBuilder builder(context,
                      function != nullptr && function->decl != nullptr
                          ? addressTakenIn(*function->decl)
                          : None,
                      handles);
  Expression result;
  auto built = builder.build(term, have);
  if (!built) {
    result.failure = builder.failure;
    return result;
  }
  // Rule 4: the places still hold the values the extent was derived from.
  if (!builder.places.roots.empty() && !witness.unmodified) {
    result.failure = "its places may have changed since the extent was derived";
    return result;
  }
  // Rule 5: its own accesses are proven or trusted at the site.
  if (builder.places.throughPointer && !witness.accessesSafe) {
    result.failure = "reading it is not proven safe here";
    return result;
  }
  // Rule 7: nothing the operation evaluates writes a place it names.
  if (!builder.places.roots.empty() &&
      mayWrite(site.stmt, builder.places, site.stmt)) {
    result.failure = "the operation may write a place it names";
    return result;
  }
  result.term = std::move(built);
  return result;
}

//===----------------------------------------------------------------------===//
// Planning (§10.1, §10.4)
//===----------------------------------------------------------------------===//

using Entry = core::CheckPlanEntry;

static Entry makeEntry(Entry::Template kind, Entry::Form form,
                       Entry::Placement placement,
                       std::vector<core::CheckTerm> operands = {},
                       std::uint8_t argument = 0) {
  Entry entry;
  entry.kind = kind;
  entry.form = form;
  entry.placement = placement;
  entry.argument = argument;
  entry.operands = std::move(operands);
  return entry;
}

static bool isCallLike(core::SiteKind kind) {
  return kind == core::SiteKind::LibCall || kind == core::SiteKind::Release ||
         kind == core::SiteKind::Call;
}

/// The unconditional trap of a lowered violation (§3.4): before the
/// operation.
static Entry violationGuard(const SiteInfo &site, core::Facet facet) {
  Entry::Template kind = Entry::Template::Assert;
  if (facet == core::Facet::Null)
    kind = Entry::Template::Nonnull;
  else if (facet == core::Facet::Spatial)
    kind = Entry::Template::Index;
  Entry::Placement placement = Entry::Placement::WrapOperand;
  if (site.kind == core::SiteKind::Assume)
    placement = Entry::Placement::ReplaceCall;
  else if (isCallLike(site.kind) ||
           (site.kind == core::SiteKind::Raw && site.library.has_value()))
    placement = Entry::Placement::BeforeCall;
  return makeEntry(kind, Entry::Form::Violation, placement);
}

namespace {

/// Plans the checks of one site.
class SitePlanner {
public:
  SitePlanner(const CheckPlanner &checkPlanner, const SiteInfo &siteInfo,
              PlaceHandleTable &handleTable)
      : planner(checkPlanner), site(siteInfo), handles(handleTable) {}

  /// The checks of one record of `facet` (the facet as a whole when
  /// `requirement` is none): true with the entries in `out`, or false with
  /// the reason in `failure`.
  bool checksFor(core::Facet facet, const core::Requirement *requirement,
                 llvm::ArrayRef<CheckWitness> witnesses,
                 std::vector<Entry> &out, std::string &failure) {
    // §2.1: no check can serve these sites.
    if (site.constantExpression)
      return fail(failure, "the site is in a constant expression");
    if (site.sharedOperand)
      return fail(failure, "the operand is shared by `?:` and cannot be "
                           "replaced in place");
    if (site.nonDefaultAddressSpace)
      return fail(failure, "the pointer is in a non-default address space");
    switch (facet) {
    case core::Facet::Assertion:
      out.push_back(makeEntry(Entry::Template::Assert, Entry::Form::Plain,
                              Entry::Placement::ReplaceCall));
      return true;
    case core::Facet::Null:
      return nullChecks(requirement, out, failure);
    case core::Facet::Spatial:
      return spatialChecks(witnesses, out, failure);
    case core::Facet::Temporal:
      return fail(failure, "temporal facets are never checked");
    }
    return fail(failure, "the facet has no check");
  }

private:
  const CheckPlanner &planner;
  const SiteInfo &site;
  PlaceHandleTable &handles;

  static bool fail(std::string &failure, std::string why) {
    failure = std::move(why);
    return false;
  }

  bool nullChecks(const core::Requirement *requirement, std::vector<Entry> &out,
                  std::string &failure) {
    const bool release = site.library.has_value();
    if (!isCallLike(site.kind) && !release) {
      // The pointer the access dereferences, wrapped in place: always
      // expressible (§10.3).
      if (site.kind == core::SiteKind::Deref ||
          site.kind == core::SiteKind::Index ||
          site.kind == core::SiteKind::Raw) {
        out.push_back(makeEntry(Entry::Template::Nonnull, Entry::Form::Plain,
                                Entry::Placement::WrapOperand));
        return true;
      }
      return fail(failure, "the site has no pointer operand");
    }
    if (site.kind == core::SiteKind::Call && site.callee == nullptr &&
        (requirement == nullptr || !requirement->argument))
      out.push_back(makeEntry(Entry::Template::Nonnull, Entry::Form::Function,
                              Entry::Placement::WrapOperand));
    // A null witness for the zero-length form: the argument is evaluated
    // at the call, so it is unmodified; reading through a pointer is not
    // known to be safe.
    const CheckWitness atCall{.unmodified = true};
    for (const ArgumentNeed &need : site.arguments) {
      if (!need.nonnull || need.systemApi)
        continue;
      if (requirement != nullptr && requirement->argument &&
          *requirement->argument != need.argument)
        continue;
      if (!need.allowedIfZero) {
        out.push_back(makeEntry(Entry::Template::Nonnull, Entry::Form::Plain,
                                Entry::Placement::WrapArgument, {},
                                need.argument));
        continue;
      }
      if (!need.unlessZero)
        return fail(failure, "the length that allows null has no name here");
      auto length = planner.express(*need.unlessZero, site, atCall, handles);
      if (!length.term)
        return fail(failure, length.failure);
      out.push_back(makeEntry(Entry::Template::Nonnull, Entry::Form::IfNonZero,
                              Entry::Placement::WrapArgument,
                              {std::move(*length.term)}, need.argument));
    }
    if (out.empty())
      return fail(failure, "no argument of the call can be checked");
    return true;
  }

  std::optional<core::CheckTerm>
  term(const std::optional<WitnessTerm> &from, const CheckWitness &witness,
       std::string &failure,
       const std::optional<core::CheckTerm> &have = std::nullopt) {
    if (!from) {
      failure = "the witness lacks a term";
      return std::nullopt;
    }
    auto expressed = planner.express(*from, site, witness, handles, have);
    if (!expressed.term)
      failure = expressed.failure;
    return std::move(expressed.term);
  }

  bool spatialChecks(llvm::ArrayRef<CheckWitness> witnesses,
                     std::vector<Entry> &out, std::string &failure) {
    if (witnesses.empty())
      return fail(failure, "nothing says what the check compares");
    const bool access = site.kind == core::SiteKind::Deref ||
                        site.kind == core::SiteKind::Index ||
                        (site.kind == core::SiteKind::Raw && !site.library);
    for (const CheckWitness &witness : witnesses) {
      // Rule 8: never against a lower bound (§7.1).
      if (!core::isCheckOperand(witness.extentClass))
        return fail(failure, "its extent is only a lower bound");
      switch (witness.shape) {
      case CheckWitness::Shape::Index: {
        if (!access)
          return fail(failure, "an index check does not fit the site");
        auto count = term(witness.extent, witness, failure);
        if (!count)
          return false;
        out.push_back(makeEntry(Entry::Template::Index, Entry::Form::Plain,
                                site.kind == core::SiteKind::Index &&
                                        site.index != nullptr
                                    ? Entry::Placement::WrapIndex
                                    : Entry::Placement::WrapOperand,
                                {std::move(*count)}));
        break;
      }
      case CheckWitness::Shape::Span: {
        auto base = term(witness.base, witness, failure);
        if (!base)
          return false;
        auto bytes = term(witness.extent, witness, failure);
        if (!bytes)
          return false;
        auto width = term(witness.width, witness, failure);
        if (!width)
          return false;
        Entry::Placement placement = Entry::Placement::WrapOperand;
        if (access) {
          placement = Entry::Placement::ReplaceAccess;
        } else if (isCallLike(site.kind)) {
          if (!witness.argument)
            return fail(failure, "the witness names no argument");
          placement = Entry::Placement::WrapArgument;
        }
        out.push_back(
            makeEntry(Entry::Template::Span, Entry::Form::Plain, placement,
                      {std::move(*base), std::move(*bytes), std::move(*width)},
                      witness.argument.value_or(0)));
        break;
      }
      case CheckWitness::Shape::Length: {
        if (!isCallLike(site.kind))
          return fail(failure, "a length check needs a call");
        auto have = term(witness.extent, witness, failure);
        if (!have)
          return false;
        auto need = term(witness.need, witness, failure, have);
        if (!need)
          return false;
        out.push_back(makeEntry(Entry::Template::Len, Entry::Form::Plain,
                                Entry::Placement::BeforeCall,
                                {std::move(*need), std::move(*have)}));
        break;
      }
      case CheckWitness::Shape::Disjoint: {
        if (!isCallLike(site.kind) || !witness.argument)
          return fail(failure, "an overlap check needs a call argument");
        auto other = term(witness.other, witness, failure);
        if (!other)
          return false;
        auto length = term(witness.need, witness, failure);
        if (!length)
          return false;
        out.push_back(makeEntry(Entry::Template::Disjoint, Entry::Form::Plain,
                                Entry::Placement::WrapArgument,
                                {std::move(*other), std::move(*length)},
                                *witness.argument));
        break;
      }
      }
    }
    return true;
  }
};

} // namespace

/// The witnesses of one record: those naming its requirement, or, for the
/// facet as a whole, those naming none.
static std::vector<CheckWitness>
witnessesFor(llvm::ArrayRef<CheckWitness> all,
             std::optional<std::uint16_t> requirement) {
  std::vector<CheckWitness> selected;
  for (const CheckWitness &witness : all)
    if (witness.requirement == requirement)
      selected.push_back(witness);
  return selected;
}

core::CheckPlan CheckPlanner::plan(core::UnitLedger &unit,
                                   const WitnessTable &witnesses,
                                   PlaceHandleTable &handles) const {
  core::CheckPlan result;
  for (const SiteIndex::FunctionSites &function : sites.functions()) {
    if (function.index >= unit.functions.size())
      continue;
    core::FunctionLedger &row = unit.functions[function.index];
    for (const SiteInfo &site : function.sites) {
      core::Site *ledgerSite = row.site(site.id.ordinal);
      if (ledgerSite == nullptr)
        continue;
      SitePlanner planner(*this, site, handles);
      std::vector<Entry> planned;

      // Plans one record; returns false when it became unresolved.
      const auto planRecord = [&](core::Facet facet,
                                  core::FacetDecision &decision,
                                  std::optional<core::FacetCheck> &check,
                                  const core::Requirement *requirement,
                                  std::optional<std::uint16_t> index) {
        const bool lowered = decision.outcome == core::SiteOutcome::Violation &&
                             options.lowered && options.lowered(site.id, facet);
        const auto selected = witnessesFor(witnesses.of(site.id, facet), index);
        const bool verify = decision.outcome == core::SiteOutcome::Proven &&
                            options.checks == core::ChecksMode::Verify &&
                            facet == core::Facet::Spatial && !selected.empty();
        if (decision.outcome != core::SiteOutcome::Checked && !lowered &&
            !verify)
          return true;
        std::vector<Entry> entries;
        std::string failure;
        if (planner.checksFor(facet, requirement, selected, entries, failure)) {
          for (Entry &entry : entries) {
            entry.site = site.id;
            entry.facet = facet;
            entry.requirement = index.value_or(0);
            entry.proven = verify;
            assert(core::isWellFormed(entry) && "a malformed check plan");
          }
          check = core::facetCheck(entries.front());
          planned.insert(planned.end(),
                         std::make_move_iterator(entries.begin()),
                         std::make_move_iterator(entries.end()));
          return true;
        }
        // A proven facet without an expressible verify check stays
        // proven.
        if (verify)
          return true;
        if (lowered) {
          Entry guard = violationGuard(site, facet);
          guard.site = site.id;
          guard.facet = facet;
          guard.requirement = index.value_or(0);
          check = core::facetCheck(guard);
          planned.push_back(std::move(guard));
          return true;
        }
        const bool downgraded =
            options.setjmpDowngraded.contains({site.id, facet});
        decision = core::FacetDecision::unresolvedFor(
            downgraded ? core::UnresolvedReason::Setjmp
                       : core::UnresolvedReason::Inexpressible,
            std::move(failure));
        check = std::nullopt;
        return false;
      };

      for (const core::Facet facet : core::AllFacets) {
        core::FacetRecord *record = ledgerSite->facet(facet);
        if (record == nullptr || !record->decided)
          continue;
        if (record->requirements.empty()) {
          planRecord(facet, record->decision, record->check, nullptr,
                     std::nullopt);
          continue;
        }
        // Checks are planned per requirement record, whatever the merged
        // outcome (§2.5); an inexpressible one merges back in.
        std::optional<core::FacetDecision> worse;
        for (std::size_t i = 0; i < record->requirements.size(); ++i) {
          core::Requirement &requirement = record->requirements[i];
          if (!planRecord(facet, requirement.decision, requirement.check,
                          &requirement, static_cast<std::uint16_t>(i)))
            worse = requirement.decision;
        }
        if (worse)
          record->decide(*worse);
        if (record->outcome() == core::SiteOutcome::Checked && !record->check)
          for (const core::Requirement &requirement : record->requirements)
            if (requirement.check) {
              record->check = requirement.check;
              break;
            }
      }

      // §10.4: a span check traps on null, so it replaces the nonnull check
      // of the same operand.
      const bool hasSpan = llvm::any_of(planned, [](const Entry &entry) {
        return entry.kind == Entry::Template::Span &&
               (entry.placement == Entry::Placement::ReplaceAccess ||
                entry.placement == Entry::Placement::WrapOperand);
      });
      if (hasSpan) {
        const auto subsumed = [](const Entry &entry) {
          return entry.kind == Entry::Template::Nonnull &&
                 entry.form == Entry::Form::Plain &&
                 entry.placement == Entry::Placement::WrapOperand;
        };
        if (llvm::any_of(planned, subsumed)) {
          std::erase_if(planned, subsumed);
          if (core::FacetRecord *null = ledgerSite->facet(core::Facet::Null);
              null != nullptr && null->check)
            null->check = core::FacetCheck{.kind = core::CheckTemplate::Span,
                                           .proven = null->check->proven};
        }
      }
      for (Entry &entry : planned)
        result.add(std::move(entry));
    }
  }
  result.sort();
  return result;
}

} // namespace weavec::analysis
