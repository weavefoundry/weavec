//===- CheckPlanner.cpp - Checks from witnesses (RFC 0030, RFC 0032) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/CheckPlanner.h"

#include "weavec/Analysis/KindTable.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/Expr.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
              PlaceHandleTable &handleTable, bool stringsInObject = false)
      : context(ctx), addressTaken(addressTakenLocals), handles(handleTable),
        objectStrings(stringsInObject) {}

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
      if (!have && !objectStrings)
        return fail("the string length has no bound here");
      auto pointer = build(term.operands.front(), have);
      if (!pointer)
        return std::nullopt;
      // RFC 0032 §6: without a have, the string's own object bounds it.
      if (!have) {
        if (pointer->kind != core::CheckTerm::Kind::Place)
          return fail("the string has no name here");
        return core::CheckTerm::objStrLen(std::move(*pointer));
      }
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
  bool objectStrings;

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
    // RFC 0033 *Implementation amendments*: a global array named alone
    // stands for its address, which no store changes; what a term reads
    // through it, it reads at the check (a format argument's `strnlen`).
    if (!variable->hasLocalStorage() &&
        !(variable->getType()->isArrayType() && path.empty()))
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
      case clang::CK_BitCast:
        // A pointer seen as another pointer type (an argument passed as
        // `void *`) is the same address.
        if (cast->getType()->isPointerType() &&
            cast->getSubExpr()->getType()->isPointerType())
          return fromExpr(*cast->getSubExpr(), depth + 1);
        return fail("it converts between kinds of values");
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
      // Pointer arithmetic is not integer arithmetic: `s + 1` on an `int *`
      // advances by four bytes, and the term helpers only add integers.
      if (binary->getType()->isAnyPointerType() ||
          binary->getLHS()->getType()->isAnyPointerType() ||
          binary->getRHS()->getType()->isAnyPointerType())
        return fail("it is pointer arithmetic");
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

/// RFC 0033 §5: a call that writes no memory: a size query that evaluates
/// nothing (the ones a fortified call passes), or a library function whose
/// row only reads (`strlen(p)` as a need).
static bool writesNothing(const clang::CallExpr &call) {
  const unsigned builtin = call.getBuiltinCallee();
  if (builtin == clang::Builtin::BI__builtin_object_size ||
      builtin == clang::Builtin::BI__builtin_dynamic_object_size ||
      builtin == clang::Builtin::BI__builtin_constant_p)
    return true;
  const clang::FunctionDecl *callee = call.getDirectCallee();
  if (callee == nullptr)
    return false;
  const std::optional<core::LibraryMatch> match =
      governingLibraryEntry(*callee, core::LibrarySpec::shipped());
  if (!match || match->entry == nullptr)
    return false;
  const core::LibraryEntry &entry = *match->entry;
  if (entry.variadic || entry.format || !entry.invalidates.empty() ||
      !entry.copies.empty() || !entry.fills.empty() ||
      !entry.writesString.empty() || entry.noreturn || entry.returnsTwice ||
      entry.allocates() || entry.releases() || entry.hasCallback())
    return false;
  return llvm::all_of(entry.params, [](const core::LibraryParam &param) {
    return param.type != core::LibraryParam::Type::Function && !param.out &&
           param.effect == core::LibraryParam::Effect::Borrow &&
           (param.access == core::LibraryParam::Access::None ||
            param.access == core::LibraryParam::Access::Read);
  });
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
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(stmt);
      call != nullptr && stmt != site && places.throughPointer &&
      !writesNothing(*call))
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

/// §10.3 rule 5: every dereference `term` makes checks its pointer for null.
static void checkReadsThrough(core::CheckTerm &term) {
  for (core::CheckPathStep &step : term.path)
    if (step.kind == core::CheckPathStep::Kind::Deref)
      step.checked = true;
  for (core::CheckTerm &operand : term.operands)
    checkReadsThrough(operand);
}

CheckPlanner::Expression
CheckPlanner::express(const WitnessTerm &term, const SiteInfo &site,
                      const CheckWitness &witness, PlaceHandleTable &handles,
                      const std::optional<core::CheckTerm> &have,
                      bool objectStrings) const {
  static const llvm::DenseSet<const clang::VarDecl *> None;
  const SiteIndex::FunctionSites *function = nullptr;
  if (site.id.function < sites.functions().size())
    function = &sites.functions()[site.id.function];
  TermBuilder builder(context,
                      function != nullptr && function->decl != nullptr
                          ? addressTakenIn(*function->decl)
                          : None,
                      handles, objectStrings);
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
  // Rule 5: its own accesses are proven or trusted at the site, or checked
  // by the term itself: a read through a pointer not proven non-null checks
  // it first (`nonnull(b)->cap`).
  if (builder.places.throughPointer && !witness.accessesSafe)
    checkReadsThrough(*built);
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

/// RFC 0033 §5: the argument of `call` a witness term is, if it is one.
static std::optional<unsigned> argumentIndex(const WitnessTerm &term,
                                             const clang::CallExpr &call) {
  if (term.kind != WitnessTerm::Kind::Expr || term.expr == nullptr)
    return std::nullopt;
  for (unsigned i = 0; i < call.getNumArgs(); ++i)
    if (call.getArg(i)->IgnoreParenImpCasts() ==
        term.expr->IgnoreParenImpCasts())
      return i;
  return std::nullopt;
}

static bool isCallLike(core::SiteKind kind) {
  return kind == core::SiteKind::LibCall || kind == core::SiteKind::Release ||
         kind == core::SiteKind::Call;
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
    case core::Facet::Spatial: {
      pendingLengthHave.reset();
      if (!spatialChecks(witnesses, out, failure))
        return false;
      // The record is planned: its `len` check is in force, so another
      // record of this site may read a string length bounded by that have.
      if (pendingLengthHave)
        lengthHaves.insert_or_assign(pendingLengthHave->first,
                                     std::move(pendingLengthHave->second));
      pendingLengthHave.reset();
      return true;
    }
    case core::Facet::Temporal:
      // A temporal facet has no static check; RFC 0032's `live` guard is
      // planned by `planGuards`.
      return fail(failure, "a temporal facet has no static check");
    }
    return fail(failure, "the facet has no check");
  }

private:
  const CheckPlanner &planner;
  const SiteInfo &site;
  PlaceHandleTable &handles;
  /// Per argument, the have of a `len` check this site has planned.
  std::map<std::uint8_t, core::CheckTerm> lengthHaves;
  /// The one the record being planned would add, once it is known to hold.
  std::optional<std::pair<std::uint8_t, core::CheckTerm>> pendingLengthHave;

  /// §10.4: the unit declares the bounded writer that lowers `writer`.
  [[nodiscard]] bool
  declaresBoundedWriter(const core::LibraryEntry &writer) const {
    const std::string name = core::boundedWriterName(writer.name);
    if (name.empty())
      return false;
    const clang::ASTContext &context = planner.astContext();
    const clang::DeclarationName declName(&context.Idents.get(name));
    return llvm::any_of(context.getTranslationUnitDecl()->lookup(declName),
                        [](const clang::NamedDecl *decl) {
                          return llvm::isa<clang::FunctionDecl>(decl);
                        });
  }

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
      // §7.5, §10.4: a requirement's guard wraps the check; the guard term
      // saturates at zero, so a guard that fails lets null through.
      if (need.guard) {
        auto guard = planner.express(*need.guard, site, atCall, handles);
        if (!guard.term)
          return fail(failure, guard.failure);
        Entry entry =
            makeEntry(Entry::Template::Nonnull, Entry::Form::Plain,
                      Entry::Placement::WrapArgument, {}, need.argument);
        entry.guard = std::move(*guard.term);
        out.push_back(std::move(entry));
        continue;
      }
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

  /// §10.3 rule 2: the have a `strlen` term of a record other than the
  /// length record itself is read with — the have of the `len` check this
  /// site already planned for the same argument, which runs before the call.
  [[nodiscard]] std::optional<core::CheckTerm>
  lengthHave(std::optional<std::uint8_t> argument) const {
    if (!argument)
      return std::nullopt;
    const auto found = lengthHaves.find(*argument);
    return found == lengthHaves.end() ? std::nullopt
                                      : std::optional(found->second);
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
        // §10.4 (RFC 0030 S5): a writer of the `printf` family has no need
        // term (`fmtlen` never is one); it is lowered to its bounded writer,
        // which the unit must declare.
        if (!witness.need && site.library && site.library->entry != nullptr &&
            site.library->entry->format) {
          if (!declaresBoundedWriter(*site.library->entry))
            return fail(failure, "no bounded writer is declared for it");
          out.push_back(makeEntry(Entry::Template::Len, Entry::Form::Result,
                                  Entry::Placement::ReplaceCall,
                                  {std::move(*have)}));
          break;
        }
        auto need = term(witness.need, witness, failure, have);
        if (!need)
          return false;
        // The check runs before the call, so from here on the copy is known
        // to fit: another record of this site may bound a `strlen` of its
        // own by this have (§10.3 rule 2).
        if (witness.argument && !witness.guard)
          pendingLengthHave = {*witness.argument, *have};
        Entry entry = makeEntry(Entry::Template::Len, Entry::Form::Plain,
                                Entry::Placement::BeforeCall,
                                {std::move(*need), std::move(*have)});
        // §7.5: `(guard ? check : 0, call)`.
        if (witness.guard) {
          auto guard = term(witness.guard, witness, failure);
          if (!guard)
            return false;
          entry.guard = std::move(*guard);
        }
        out.push_back(std::move(entry));
        break;
      }
      case CheckWitness::Shape::Disjoint: {
        if (!isCallLike(site.kind) || !witness.argument)
          return fail(failure, "an overlap check needs a call argument");
        auto other = term(witness.other, witness, failure);
        if (!other)
          return false;
        // The other pointer is passed to the helper as a pointer, so it must
        // be a named place (§10.3 rule 1), not a computed value.
        if (other->kind != core::CheckTerm::Kind::Place)
          return fail(failure, "the other pointer has no name here");
        // §10.3 rule 2: a `strlen` in the length is read as
        // `__weavec_strnlen(p, have)`, and the have of an overlapping copy
        // is the destination's own extent — the `len` witness of the same
        // argument (`strcpy`'s `strlen(a1) + 1` against `sizeof buf`).
        // `lengthHave` answers only once that length check is planned, and
        // it runs before the call, so wherever the overlap check runs the
        // copy is known to fit and the bounded length is the true one.
        auto length =
            term(witness.need, witness, failure, lengthHave(witness.argument));
        if (!length)
          return false;
        out.push_back(makeEntry(Entry::Template::Disjoint, Entry::Form::Plain,
                                Entry::Placement::WrapArgument,
                                {std::move(*other), std::move(*length)},
                                *witness.argument));
        break;
      }
      case CheckWitness::Shape::Object:
        // RFC 0032 §6: what a guard compares; no static check.
        return fail(failure, "only the runtime knows the extent");
      }
    }
    return true;
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Guards (RFC 0032 §6)
//===----------------------------------------------------------------------===//

const clang::ParentMap *
CheckPlanner::parentsIn(const clang::FunctionDecl &function) const {
  std::unique_ptr<clang::ParentMap> &map =
      parentMaps[function.getCanonicalDecl()];
  if (!map)
    if (clang::Stmt *body = function.getBody())
      map = std::make_unique<clang::ParentMap>(body);
  return map.get();
}

/// The size of an object type in bytes, when it is a constant.
static std::optional<std::int64_t>
constantSize(const clang::ASTContext &context, clang::QualType type) {
  if (type.isNull() || type->isDependentType() || type->isIncompleteType() ||
      type->isFunctionType() || !type->isConstantSizeType())
    return std::nullopt;
  return context.getTypeSizeInChars(type).getQuantity();
}

std::optional<CheckPlanner::AccessBytes>
CheckPlanner::accessBytes(const SiteInfo &site) const {
  const auto *access = llvm::dyn_cast_or_null<clang::Expr>(site.stmt);
  if (access == nullptr || site.id.function >= sites.functions().size())
    return std::nullopt;
  const clang::FunctionDecl *function =
      sites.functions()[site.id.function].decl;
  const clang::ParentMap *parents =
      function != nullptr ? parentsIn(*function) : nullptr;
  AccessBytes bytes;
  clang::QualType type = access->getType();
  // A bit-field: the bytes its bits lie in.
  const auto bitField = [&](const clang::FieldDecl &field,
                            std::int64_t base) -> AccessBytes {
    const std::uint64_t first = context.getFieldOffset(&field);
    const std::uint64_t bits = field.getBitWidthValue();
    AccessBytes result = bytes;
    result.offset =
        base + static_cast<std::int64_t>(first / context.getCharWidth());
    result.width = static_cast<std::int64_t>(((first % context.getCharWidth()) +
                                              std::max<std::uint64_t>(bits, 1) +
                                              context.getCharWidth() - 1) /
                                             context.getCharWidth());
    return result;
  };
  // The access itself: `p->f` starts at the member; `*p` and `p[i]` at 0.
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(access)) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr || field->getParent()->isInvalidDecl())
      return std::nullopt;
    if (field->isBitField())
      return bitField(*field, 0);
    bytes.offset = static_cast<std::int64_t>(context.getFieldOffset(field) /
                                             context.getCharWidth());
  } else if (site.index != nullptr) {
    const auto element = constantSize(context, type);
    if (!element)
      return std::nullopt;
    bytes.step = *element;
  }
  // The member chain above it narrows the access to the member it names.
  const clang::Stmt *current = access;
  for (unsigned depth = 0; parents != nullptr && depth < 64; ++depth) {
    const clang::Stmt *parent = parents->getParent(current);
    if (parent == nullptr)
      break;
    if (llvm::isa<clang::ParenExpr>(parent)) {
      current = parent;
      continue;
    }
    const auto *member = llvm::dyn_cast<clang::MemberExpr>(parent);
    if (member == nullptr || member->isArrow())
      break;
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr || field->getParent()->isInvalidDecl())
      break;
    if (field->isBitField())
      return bitField(*field, bytes.offset);
    bytes.offset += static_cast<std::int64_t>(context.getFieldOffset(field) /
                                              context.getCharWidth());
    type = member->getType();
    current = parent;
  }
  const auto width = constantSize(context, type);
  if (!width)
    return std::nullopt;
  bytes.width = *width;
  return bytes;
}

/// RFC 0032 §6: the reasons a guard does not answer: they are not about one
/// object's bounds or liveness at the site.
static bool guardableReason(core::UnresolvedReason reason) {
  switch (reason) {
  case core::UnresolvedReason::NoZeroInit:
  case core::UnresolvedReason::Lowered:
  case core::UnresolvedReason::SecondOwner:
  case core::UnresolvedReason::MayConflict:
  case core::UnresolvedReason::MayDangle:
    return false;
  default:
    return true;
  }
}

static bool isUnresolvedForGuard(const core::FacetDecision &decision) {
  return decision.outcome == core::SiteOutcome::Unresolved &&
         decision.unresolved && guardableReason(*decision.unresolved);
}

/// `*p`, `p->f`, `p[i]` and `*(p + i)`: what `CheckEmitter` can wrap or
/// replace in place.
static bool isReplaceableIndex(const clang::Stmt *stmt) {
  if (llvm::isa_and_nonnull<clang::ArraySubscriptExpr>(stmt))
    return true;
  const auto *deref = llvm::dyn_cast_or_null<clang::UnaryOperator>(stmt);
  if (deref == nullptr || deref->getOpcode() != clang::UO_Deref)
    return false;
  const auto *sum = llvm::dyn_cast<clang::BinaryOperator>(
      deref->getSubExpr()->IgnoreParens());
  return sum != nullptr && sum->getOpcode() == clang::BO_Add;
}

/// The argument of a heap-family release or reallocation (RFC 0032 §6):
/// what the runtime's allocator can validate.
static bool releasesHeap(const SiteInfo &site) {
  if (!site.library || site.library->entry == nullptr)
    return false;
  return llvm::any_of(
      site.library->entry->params, [](const core::LibraryParam &param) {
        return (param.effect == core::LibraryParam::Effect::Release ||
                param.effect == core::LibraryParam::Effect::Realloc) &&
               param.family == core::HeapFamily;
      });
}

/// RFC 0034 §5.2: the check the library row's wrapper makes for a spatial
/// requirement record of a call that is still to be guarded, when the
/// wrapper computes it and no guard of the record's own (`guarded`) states
/// it: `object` for the bytes behind argument 0 (also when the only guard
/// would check a format's least output, a lower bound), `disjoint` for the
/// row's overlap clause.
static std::optional<core::CheckPlanEntry::Template>
wrapperCheck(const SiteInfo &site, const core::Requirement &requirement,
             const CheckWitness *witness, bool guarded) {
  using Template = core::CheckPlanEntry::Template;
  if (site.kind != core::SiteKind::LibCall || !site.library ||
      site.library->entry == nullptr || !site.library->entry->wrapper ||
      !requirement.argument)
    return std::nullopt;
  const core::LibraryEntry &entry = *site.library->entry;
  if (witness != nullptr && witness->shape == CheckWitness::Shape::Disjoint)
    return entry.wrapper->disjoint && !guarded
               ? std::optional(Template::Disjoint)
               : std::nullopt;
  if ((witness != nullptr && witness->string) || (guarded && !entry.format))
    return std::nullopt;
  const int destination = site.library->callArgument(0);
  const int source = site.library->callArgument(1);
  if ((entry.wrapper->room && destination >= 0 &&
       std::cmp_equal(*requirement.argument, destination)) ||
      (entry.wrapper->source && source >= 0 &&
       std::cmp_equal(*requirement.argument, source)))
    return Template::Object;
  return std::nullopt;
}

void CheckPlanner::planGuards(const SiteInfo &site, core::Site &row,
                              const WitnessTable &witnesses,
                              PlaceHandleTable &handles,
                              std::vector<Entry> &planned,
                              const std::set<core::Facet> &lowered) const {
  // RFC 0030 §2.1: no check can serve these sites.
  if (site.constantExpression || site.sharedOperand ||
      site.nonDefaultAddressSpace || site.stmt == nullptr)
    return;
  const bool verify = options.checks == core::ChecksMode::Verify;
  // What a facet wants: a guard (it is unresolved), a verify guard (it is
  // proven and nothing checks it yet), or nothing.
  enum class Want : std::uint8_t { None, Guard, Verify };
  // RFC 0033 (V): a lowered violation no check guards.
  const auto loweredViolation = [&](const core::FacetRecord *record) {
    return record != nullptr && record->decided &&
           record->outcome() == core::SiteOutcome::Violation &&
           !record->check &&
           llvm::any_of(core::AllFacets, [&](core::Facet facet) {
             return lowered.contains(facet) && row.facet(facet) == record;
           });
  };
  const auto wantOf = [&](const core::FacetRecord *record) {
    if (record == nullptr || !record->decided)
      return Want::None;
    if (isUnresolvedForGuard(record->decision) || loweredViolation(record))
      return Want::Guard;
    if (verify && record->outcome() == core::SiteOutcome::Proven &&
        !record->check)
      return Want::Verify;
    return Want::None;
  };
  const auto add = [&](Entry entry, core::Facet facet, bool proven,
                       std::uint16_t requirement = 0) {
    entry.site = site.id;
    entry.facet = facet;
    entry.requirement = requirement;
    entry.proven = proven;
    assert(core::isWellFormed(entry) && "a malformed guard");
    planned.push_back(std::move(entry));
    return core::facetCheck(planned.back());
  };
  const auto guard = [](core::FacetRecord &record, core::FacetCheck check) {
    if (record.outcome() == core::SiteOutcome::Unresolved)
      record.decision = core::FacetDecision::guardedFor(
          *record.decision.unresolved, std::move(record.decision.detail));
    record.check = check;
  };
  core::FacetRecord *spatial = row.facet(core::Facet::Spatial);
  core::FacetRecord *temporal = row.facet(core::Facet::Temporal);

  const bool access = site.kind == core::SiteKind::Deref ||
                      site.kind == core::SiteKind::Index ||
                      (site.kind == core::SiteKind::Raw && !site.library);
  if (access) {
    Want wantSpatial = wantOf(spatial);
    // The types alone prove these; nothing a guard could add.
    if (wantSpatial == Want::Verify && site.provenByType)
      wantSpatial = Want::None;
    const Want wantTemporal =
        site.operand != nullptr && site.operand->getType()->isPointerType()
            ? wantOf(temporal)
            : Want::None;
    std::optional<core::FacetCheck> object;
    if (wantSpatial != Want::None) {
      const auto bytes = accessBytes(site);
      const bool proven = wantSpatial == Want::Verify;
      if (bytes && site.index != nullptr && isReplaceableIndex(site.stmt)) {
        object = add(makeEntry(Entry::Template::Object, Entry::Form::Plain,
                               Entry::Placement::ReplaceAccess,
                               {core::CheckTerm::ofConstant(bytes->step),
                                core::CheckTerm::ofConstant(bytes->offset),
                                core::CheckTerm::ofConstant(bytes->width)}),
                     core::Facet::Spatial, proven);
      } else if (bytes && site.index == nullptr && site.operand != nullptr &&
                 site.operand->getType()->isPointerType()) {
        object = add(makeEntry(Entry::Template::Object, Entry::Form::Plain,
                               Entry::Placement::WrapOperand,
                               {core::CheckTerm::ofConstant(bytes->offset),
                                core::CheckTerm::ofConstant(bytes->width)}),
                     core::Facet::Spatial, proven);
      }
      if (object)
        guard(*spatial, *object);
    }
    if (wantTemporal != Want::None) {
      const bool proven = wantTemporal == Want::Verify;
      // An `object` guard of the same family already fails on a dead
      // object; a verify guard and a real one must not stand in for each
      // other, or a false proof would look like an ordinary trap.
      // RFC 0033 §4: a subscripted pointer may lie outside the object the
      // access reads (`base + index`), so a verify `live` of the pointer
      // would ask about other bytes; a real `object` guard of the accessed
      // bytes already fails on a dead object.
      const bool coveredByRealObject =
          object && proven && site.index != nullptr;
      if (object && object->proven == proven)
        guard(*temporal, *object);
      else if (!coveredByRealObject)
        guard(*temporal,
              add(makeEntry(Entry::Template::Live, Entry::Form::Plain,
                            Entry::Placement::WrapOperand),
                  core::Facet::Temporal, proven));
    }
    return;
  }

  // A heap release: the allocator's own validation, at the site.
  if ((site.kind == core::SiteKind::Release ||
       (site.kind == core::SiteKind::Raw && site.library)) &&
      site.operand != nullptr && releasesHeap(site)) {
    const Want wantSpatial = wantOf(spatial);
    const Want wantTemporal = wantOf(temporal);
    for (const Want want : {Want::Verify, Want::Guard}) {
      if (wantSpatial != want && wantTemporal != want)
        continue;
      const core::FacetCheck check = add(
          makeEntry(Entry::Template::Release, Entry::Form::Plain,
                    Entry::Placement::WrapOperand),
          wantTemporal == want ? core::Facet::Temporal : core::Facet::Spatial,
          want == Want::Verify);
      for (core::FacetRecord *record : {spatial, temporal}) {
        if (record == nullptr || wantOf(record) != want)
          continue;
        for (core::Requirement &requirement : record->requirements)
          if (isUnresolvedForGuard(requirement.decision)) {
            requirement.decision = core::FacetDecision::guardedFor(
                *requirement.decision.unresolved);
            requirement.check = check;
          }
        guard(*record, check);
      }
    }
    return;
  }

  const auto *call = llvm::dyn_cast<clang::CallExpr>(site.stmt);
  if (call == nullptr || !isCallLike(site.kind) ||
      (site.kind == core::SiteKind::Call &&
       site.boundary != core::Boundary::Call))
    return;
  // Arguments an `object` guard already covers, by whether it is a verify
  // guard.
  std::set<std::pair<unsigned, bool>> covered;
  if (spatial != nullptr && spatial->decided &&
      !spatial->requirements.empty()) {
    const llvm::ArrayRef<CheckWitness> all =
        witnesses.of(site.id, core::Facet::Spatial);
    std::optional<core::UnresolvedReason> reason;
    bool left = false;
    for (std::size_t i = 0; i < spatial->requirements.size(); ++i) {
      core::Requirement &requirement = spatial->requirements[i];
      // RFC 0033 (V): a lowered violation of the requirement is guarded
      // like an unresolved one, and stays a violation.
      const bool loweredRequirement =
          requirement.decision.outcome == core::SiteOutcome::Violation &&
          !requirement.check && lowered.contains(core::Facet::Spatial);
      if (requirement.decision.outcome != core::SiteOutcome::Unresolved &&
          !loweredRequirement)
        continue;
      const auto index = static_cast<std::uint16_t>(i);
      const CheckWitness *witness = nullptr;
      for (const CheckWitness &candidate : all)
        if (candidate.requirement == std::optional(index) &&
            (candidate.shape == CheckWitness::Shape::Object ||
             candidate.shape == CheckWitness::Shape::Disjoint))
          witness = &candidate;
      // RFC 0033 §5: a static length check that could not be planned (its
      // have is only a lower bound, a flexible array member's) still names
      // the need a guard can check against the object.
      if (witness == nullptr)
        for (const CheckWitness &candidate : all)
          if (candidate.requirement == std::optional(index) &&
              candidate.shape == CheckWitness::Shape::Length &&
              candidate.need && candidate.argument && !candidate.guard)
            witness = &candidate;
      // A requirement that binds only under a guard term has no guard yet:
      // the helpers take no condition.
      const bool guardable =
          (isUnresolvedForGuard(requirement.decision) || loweredRequirement) &&
          (witness == nullptr || !witness->guard);
      std::optional<Entry> entry;
      if (!guardable || witness == nullptr || !witness->argument ||
          *witness->argument >= call->getNumArgs() ||
          !call->getArg(*witness->argument)->getType()->isPointerType()) {
        // Nothing to guard here; the row's wrapper may still compute it.
      } else if (witness->shape == CheckWitness::Shape::Disjoint) {
        // An overlap check whose length is a string's: the static check
        // had no bound to read it with; the guard reads it inside the
        // string's own object.
        if (witness->need && witness->other) {
          auto other = express(*witness->other, site, *witness, handles);
          auto length = express(*witness->need, site, *witness, handles,
                                std::nullopt, /*objectStrings=*/true);
          if (other.term && length.term &&
              other.term->kind == core::CheckTerm::Kind::Place)
            entry = makeEntry(Entry::Template::Disjoint, Entry::Form::Plain,
                              Entry::Placement::WrapArgument,
                              {std::move(*other.term), std::move(*length.term)},
                              *witness->argument);
        }
      } else if (witness->string) {
        entry =
            makeEntry(Entry::Template::Object, Entry::Form::String,
                      Entry::Placement::WrapArgument, {}, *witness->argument);
      } else if (witness->need) {
        auto need = express(*witness->need, site, *witness, handles,
                            std::nullopt, /*objectStrings=*/true);
        if (need.term) {
          entry = makeEntry(Entry::Template::Object, Entry::Form::Need,
                            Entry::Placement::WrapArgument,
                            {std::move(*need.term)}, *witness->argument);
        } else if (const std::optional<unsigned> length =
                       argumentIndex(*witness->need, *call)) {
          // RFC 0033 §5: a need that is a call argument no term can
          // repeat (`strlen(p)`) is checked where the call evaluates it,
          // against the pointer argument, which a term can repeat. The
          // pointer is read at the call that reads it (rule 4 holds by
          // construction; rule 7 still applies).
          CheckWitness atCall = *witness;
          atCall.unmodified = true;
          auto pointer =
              express(WitnessTerm::ofExpr(*call->getArg(*witness->argument)),
                      site, atCall, handles);
          if (pointer.term &&
              pointer.term->kind == core::CheckTerm::Kind::Place &&
              *length != *witness->argument)
            entry = makeEntry(Entry::Template::Object, Entry::Form::Length,
                              Entry::Placement::WrapArgument,
                              {std::move(*pointer.term)},
                              static_cast<std::uint8_t>(*length));
        }
      }
      // RFC 0034 §5.2: what no guard states, or states only as a lower
      // bound, the row's checked wrapper computes at run time. A lowered
      // violation keeps what RFC 0033 (V) gives it.
      const std::optional<Entry::Template> wrapped =
          guardable && !loweredRequirement
              ? wrapperCheck(site, requirement, witness, entry.has_value())
              : std::nullopt;
      // (A wrapper's `object` names the row's argument it checks: 0 the
      // destination's room, 1 a copy's source.)
      if (wrapped)
        entry = makeEntry(
            *wrapped, Entry::Form::Wrapper, Entry::Placement::ReplaceCall, {},
            static_cast<std::uint8_t>(
                *wrapped == Entry::Template::Object && requirement.argument &&
                        std::cmp_equal(*requirement.argument,
                                       site.library->callArgument(1))
                    ? 1
                    : 0));
      if (!entry) {
        left = true;
        continue;
      }
      requirement.check =
          add(std::move(*entry), core::Facet::Spatial, false, index);
      if (!loweredRequirement) {
        reason = requirement.decision.unresolved;
        requirement.decision = core::FacetDecision::guardedFor(
            *reason, std::move(requirement.decision.detail));
      }
      if (wrapped == Entry::Template::Object)
        covered.insert({*requirement.argument, false});
      else if (!wrapped && witness->shape != CheckWitness::Shape::Disjoint)
        covered.insert({*witness->argument, false});
    }
    // The merged facet follows its records once none is left unresolved.
    if (reason && !left &&
        spatial->outcome() == core::SiteOutcome::Unresolved) {
      spatial->decision = core::FacetDecision::guardedFor(
          *spatial->decision.unresolved, std::move(spatial->decision.detail));
      for (const core::Requirement &requirement : spatial->requirements)
        if (requirement.check &&
            requirement.decision.outcome == core::SiteOutcome::Guarded) {
          spatial->check = requirement.check;
          break;
        }
    }
  }

  // The temporal facet of a library call: every pointer the row reads or
  // writes through is live. Of a variadic row's arguments (the `printf`
  // family, RFC 0033 §5) only the strings a literal format reads are; their
  // `object` guards cover them, and the others it does not dereference. A
  // variadic row with no format, or a format that is no literal, names no
  // such list, and stays unresolved.
  const Want wantTemporal = wantOf(temporal);
  if (site.kind != core::SiteKind::LibCall || wantTemporal == Want::None ||
      !site.library || site.library->entry == nullptr ||
      !temporal->requirements.empty())
    return;
  const core::LibraryEntry &entry = *site.library->entry;
  if (entry.variadic) {
    if (!entry.format)
      return;
    const int format = site.library->callArgument(entry.format->format);
    if (format < 0 || static_cast<unsigned>(format) >= call->getNumArgs() ||
        !llvm::isa<clang::StringLiteral>(
            call->getArg(static_cast<unsigned>(format))->IgnoreParenImpCasts()))
      return;
  }
  const bool proven = wantTemporal == Want::Verify;
  std::vector<unsigned> arguments;
  for (unsigned i = 0; i < call->getNumArgs(); ++i) {
    const core::LibraryParam *param = site.library->param(i);
    if (param == nullptr || param->type != core::LibraryParam::Type::Pointer ||
        !call->getArg(i)->getType()->isPointerType())
      continue;
    const bool accessed = param->access != core::LibraryParam::Access::None ||
                          param->string || param->bytes || param->count;
    if (accessed && param->effect == core::LibraryParam::Effect::Borrow)
      arguments.push_back(i);
  }
  // RFC 0033 §5: the variadic strings the format reads (each has a spatial
  // requirement record).
  if (entry.variadic && spatial != nullptr)
    for (const core::Requirement &requirement : spatial->requirements)
      if (requirement.argument && *requirement.argument < call->getNumArgs() &&
          site.library->param(*requirement.argument) == nullptr &&
          call->getArg(*requirement.argument)->getType()->isPointerType() &&
          !llvm::is_contained(arguments, *requirement.argument))
        arguments.push_back(*requirement.argument);
  if (arguments.empty() || arguments.size() > 255)
    return;
  std::optional<core::FacetCheck> check;
  for (const unsigned argument : arguments) {
    if (covered.contains({argument, proven})) {
      if (!check)
        check = core::FacetCheck{.kind = core::CheckTemplate::Object,
                                 .proven = proven};
      continue;
    }
    check = add(makeEntry(Entry::Template::Live, Entry::Form::Plain,
                          Entry::Placement::WrapArgument, {},
                          static_cast<std::uint8_t>(argument)),
                core::Facet::Temporal, proven);
  }
  guard(*temporal, *check);
}

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
      std::set<core::Facet> loweredFacets;
      // RFC 0034 §6.4: a lowered violation gets the check its witness
      // states, else the guard its facet would have as a possible finding,
      // else nothing (`unresolved(lowered)`); never an unconditional trap.

      // Plans one record; returns false when it became unresolved.
      const auto planRecord = [&](core::Facet facet,
                                  core::FacetDecision &decision,
                                  std::optional<core::FacetCheck> &check,
                                  const core::Requirement *requirement,
                                  std::optional<std::uint16_t> index) {
        const bool lowered = decision.outcome == core::SiteOutcome::Violation &&
                             options.lowered && options.lowered(site.id, facet);
        const auto selected = witnessesFor(witnesses.of(site.id, facet), index);
        // §10.7: verify mode checks a proven facet wherever the check is
        // expressible. A spatial one needs a witness to compare against; a
        // null one wraps the operand in place, which always is (§10.3).
        const bool verify =
            decision.outcome == core::SiteOutcome::Proven &&
            options.checks == core::ChecksMode::Verify &&
            ((facet == core::Facet::Spatial && !selected.empty()) ||
             facet == core::Facet::Null);
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
          // The guard pass guards it if it can; below, what it cannot is
          // unresolved.
          loweredFacets.insert(facet);
          return true;
        }
        const bool downgraded =
            options.setjmpDowngraded.contains({site.id, facet});
        if (options.concurrencyDowngraded.contains({site.id, facet}))
          decision =
              core::FacetDecision::trustedFor(core::TrustReason::Concurrency);
        else
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
        // A violation of the facet itself (a definite error linked to it)
        // that no requirement check guards, lowered: the guard pass, below.
        if (record->outcome() == core::SiteOutcome::Violation &&
            options.lowered && options.lowered(site.id, facet) &&
            llvm::none_of(record->requirements,
                          [](const core::Requirement &requirement) {
                            return requirement.check.has_value();
                          })) {
          loweredFacets.insert(facet);
          continue;
        }
        if (record->outcome() == core::SiteOutcome::Checked && !record->check)
          for (const core::Requirement &requirement : record->requirements)
            if (requirement.check) {
              record->check = requirement.check;
              break;
            }
      }

      // RFC 0032 §6: what is still unresolved is guarded, where the site has
      // a pointer operand.
      if (options.runtime)
        planGuards(site, *ledgerSite, witnesses, handles, planned,
                   loweredFacets);
      // RFC 0034 §6.4: a lowered violation no check or guard serves is
      // unresolved, and nothing is inserted for it.
      for (const core::Facet facet : loweredFacets) {
        core::FacetRecord *record = ledgerSite->facet(facet);
        if (record == nullptr)
          continue;
        const bool unguardedRequirement = llvm::any_of(
            record->requirements, [](const core::Requirement &requirement) {
              return requirement.decision.outcome ==
                         core::SiteOutcome::Violation &&
                     !requirement.check;
            });
        const bool unguardedFacet =
            record->outcome() == core::SiteOutcome::Violation &&
            !record->check &&
            llvm::none_of(record->requirements,
                          [](const core::Requirement &requirement) {
                            return requirement.check.has_value();
                          });
        if (!unguardedRequirement && !unguardedFacet)
          continue;
        const auto unresolved =
            core::FacetDecision::unresolvedFor(core::UnresolvedReason::Lowered);
        for (core::Requirement &requirement : record->requirements)
          if (requirement.decision.outcome == core::SiteOutcome::Violation &&
              !requirement.check)
            requirement.decision = unresolved;
        // (Assigned: a violation outranks every other decision.)
        record->decision = unresolved;
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
