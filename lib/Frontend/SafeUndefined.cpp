//===- SafeUndefined.cpp - What a safe function may not assert ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0051 §4. The rules of a safe function, in C and C++, that keep it from
// reaching what the language leaves undefined and no other rule is about:
//
//   assumed         an attribute, a builtin, a qualifier or a pragma that
//                   tells the optimizer what nothing checks ([[assume]],
//                   __builtin_assume and its kin, restrict, pure, const,
//                   malloc, alloc_size, alloc_align, assume_aligned,
//                   returns_nonnull, nonnull, `vectorize(assume_safety)`);
//                   its fix, certain, deletes it
//   pointer-bytes   bytes written into an object that holds a `bool` (a
//                   library call's written argument, a bit cast to it), but
//                   a whole object zeroed or copied from its own type
//   union-pointer   a `bool` member read from a union whose other members
//                   share its bytes
//   signal-handler  a call that registers code the system runs between any
//                   two instructions (the table's `handler` rows), but
//                   `signal` with SIG_IGN or SIG_DFL
//
//===----------------------------------------------------------------------===//

#include "SafeCheckImpl.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/StringRef.h"

#include <cstring>
#include <set>
#include <string>

namespace weavec::frontend {

namespace {

namespace rule {
constexpr llvm::StringLiteral Assumed = "assumed";
constexpr llvm::StringLiteral PointerBytes = "pointer-bytes";
constexpr llvm::StringLiteral UnionPointer = "union-pointer";
constexpr llvm::StringLiteral SignalHandler = "signal-handler";
constexpr llvm::StringLiteral PointerCast = "pointer-cast";
} // namespace rule

} // namespace

bool holdsBool(clang::QualType type, unsigned depth) {
  if (type.isNull() || depth > 8)
    return false;
  type = type.getCanonicalType();
  if (type->isBooleanType())
    return true;
  if (const auto *array = llvm::dyn_cast<clang::ArrayType>(type.getTypePtr()))
    return holdsBool(array->getElementType(), depth + 1);
  const clang::RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr || record->getDefinition() == nullptr)
    return false;
  record = record->getDefinition();
  if (const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record))
    for (const clang::CXXBaseSpecifier &base : cxx->bases())
      if (holdsBool(base.getType(), depth + 1))
        return true;
  return llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
    return holdsBool(field->getType(), depth + 1);
  });
}

/// The member of `type` (a record) that is or holds the `bool`, for the
/// message.
static std::string boolMember(clang::QualType type,
                              const clang::ASTContext &ast) {
  type = type.getCanonicalType();
  if (const auto *array = llvm::dyn_cast<clang::ArrayType>(type.getTypePtr()))
    return boolMember(array->getElementType(), ast);
  const clang::RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr || record->getDefinition() == nullptr)
    return "";
  for (const clang::FieldDecl *field : record->getDefinition()->fields())
    if (holdsBool(field->getType()))
      return field->getName().str();
  return "";
}

namespace {

class Undefined final : public clang::RecursiveASTVisitor<Undefined> {
public:
  using Report = std::function<void(clang::SourceLocation, llvm::StringRef,
                                    llvm::StringRef, llvm::StringRef)>;

  Undefined(clang::ASTContext &ast, const Report &report, FixPlanner *fixes)
      : ast(ast), sources(ast.getSourceManager()), report(report),
        fixes(fixes) {}

  /// The function's own declarations: its attributes, and its parameters'
  /// and its locals' `restrict`.
  void declaration(const clang::FunctionDecl &function) {
    attributes(function, true);
    for (const clang::ParmVarDecl *parameter : function.parameters()) {
      attributes(*parameter, true);
      restricted(*parameter);
      // C's `a[static n]`: the argument is said to have n elements, which
      // Clang passes on as `dereferenceable`.
      if (const auto *array = llvm::dyn_cast<clang::ArrayType>(
              parameter->getOriginalType().getCanonicalType().getTypePtr());
          array != nullptr &&
          array->getSizeModifier() == clang::ArraySizeModifier::Static)
        report(parameter->getLocation(), rule::Assumed,
               "'static' in an array parameter tells the optimizer what "
               "nothing checks",
               "delete it, and give the parameter its bound: '" +
                   std::string("T *WEAVEC_COUNTED_BY(n) ") +
                   parameter->getName().str() + "'");
    }
  }

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitAttributedStmt(clang::AttributedStmt *statement) {
    for (const clang::Attr *attribute : statement->getAttrs()) {
      if (llvm::isa<clang::CXXAssumeAttr>(attribute)) {
        const bool alone = statement->getAttrs().size() == 1 &&
                           llvm::isa<clang::NullStmt>(statement->getSubStmt());
        remove(attribute->getLocation(),
               enclosing(
                   clang::CharSourceRange::getTokenRange(attribute->getRange()),
                   alone),
               "", "assume");
      } else if (const auto *hint =
                     llvm::dyn_cast<clang::LoopHintAttr>(attribute);
                 hint != nullptr &&
                 hint->getState() == clang::LoopHintAttr::AssumeSafety) {
        assumeSafety(*hint);
      }
    }
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *call) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee == nullptr)
      return true;
    switch (callee->getBuiltinID()) {
    case clang::Builtin::BI__builtin_assume:
    case clang::Builtin::BI__builtin_assume_separate_storage:
      remove(call->getExprLoc(),
             clang::CharSourceRange::getTokenRange(call->getSourceRange()), "",
             callee->getName());
      return true;
    case clang::Builtin::BI__builtin_assume_aligned:
      // The pointer, without what it asserts of it.
      remove(call->getExprLoc(),
             clang::CharSourceRange::getTokenRange(call->getSourceRange()),
             text(call->getArg(0)->getSourceRange()), callee->getName());
      return true;
    default:
      break;
    }
    if (!sources.isInSystemHeader(callee->getLocation()))
      attributes(*callee, false);
    library(*call, *callee);
    bitCast(*call, *callee);
    return true;
  }

  /// A member pointer converted to a base class's: applied to a base
  /// object that is not the derived one, it names a member that is not
  /// there.
  bool VisitExplicitCastExpr(clang::ExplicitCastExpr *cast) {
    if (cast->getCastKind() == clang::CK_DerivedToBaseMemberPointer)
      report(cast->getExprLoc(), rule::PointerCast,
             "a member pointer is converted to a base class's without a "
             "check",
             "keep the member pointer of the class that has the member, or "
             "a 'std::function' that reads it");
    return true;
  }

  /// RFC 0051 (C++'s core list, a delete through a base): an owner of a
  /// base made from an owner of a derived object, when the base's
  /// destructor is not virtual, destroys the object as the base.
  bool VisitCXXConstructExpr(clang::CXXConstructExpr *construct) {
    if (construct->getNumArgs() >= 1)
      ownerToBase(construct->getType(), construct->getArg(0)->getType(),
                  construct->getExprLoc());
    return true;
  }
  bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *call) {
    if (call->getOperator() == clang::OO_Equal && call->getNumArgs() == 2)
      ownerToBase(call->getArg(0)->getType(), call->getArg(1)->getType(),
                  call->getOperatorLoc());
    return true;
  }

  bool VisitBuiltinBitCastExpr(clang::BuiltinBitCastExpr *cast) {
    if (holdsBool(cast->getType()) &&
        !clang::ASTContext::hasSameUnqualifiedType(
            cast->getType(), cast->getSubExpr()->getType()))
      boolBytes(cast->getExprLoc(), cast->getType());
    return true;
  }

  bool VisitVarDecl(clang::VarDecl *variable) {
    if (!llvm::isa<clang::ParmVarDecl>(variable))
      restricted(*variable);
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *member) {
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr || !field->getParent()->isUnion() ||
        !holdsBool(field->getType()) || assigned.contains(member))
      return true;
    for (const clang::FieldDecl *other : field->getParent()->fields())
      if (other != field && !clang::ASTContext::hasSameUnqualifiedType(
                                other->getType(), field->getType())) {
        report(member->getMemberLoc(), rule::UnionPointer,
               "'" + field->getName().str() +
                   "' is a 'bool' that shares its "
                   "bytes with '" +
                   other->getName().str() + "'",
               ast.getLangOpts().CPlusPlus
                   ? "a 'bool' is 0 or 1: 'std::variant' keeps which "
                     "alternative is live; or give the member the type "
                     "'std::uint8_t'"
                   : "a 'bool' is 0 or 1: keep a tag beside the union, or "
                     "give the member the type 'uint8_t'");
        break;
      }
    return true;
  }

  /// A union member being assigned is written, not read.
  bool VisitBinaryOperator(clang::BinaryOperator *op) {
    if (op->isAssignmentOp())
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(
              op->getLHS()->IgnoreParenImpCasts()))
        assigned.insert(member);
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)

private:
  clang::ASTContext &ast;
  const clang::SourceManager &sources;
  const Report &report;
  FixPlanner *fixes;
  std::set<const clang::Decl *> attributed;
  llvm::DenseSet<const clang::MemberExpr *> assigned;

  /// The file's text of `range`.
  std::string text(clang::SourceRange range) const {
    return clang::Lexer::getSourceText(
               clang::CharSourceRange::getTokenRange(range), sources,
               ast.getLangOpts())
        .str();
  }

  /// An attribute's range widened to its brackets, `[[a]]` or
  /// `__attribute__((a))`, and the space after them, when it is the only
  /// one inside; with `statement`, to the `;` of the empty statement it
  /// stands on.
  clang::CharSourceRange enclosing(clang::CharSourceRange range,
                                   bool statement) const {
    if (range.isInvalid() || range.getBegin().isMacroID() ||
        range.getEnd().isMacroID())
      return range;
    const clang::FileID file = sources.getFileID(range.getBegin());
    bool invalid = false;
    const llvm::StringRef buffer = sources.getBufferData(file, &invalid);
    if (invalid)
      return range;
    const unsigned begin = sources.getFileOffset(range.getBegin());
    const clang::SourceLocation last = clang::Lexer::getLocForEndOfToken(
        range.getEnd(), 0, sources, ast.getLangOpts());
    if (last.isInvalid())
      return range;
    const unsigned end = sources.getFileOffset(last);
    const llvm::StringRef before = buffer.substr(0, begin).rtrim(" \t");
    const llvm::StringRef after = buffer.substr(end).ltrim(" \t");
    std::size_t from = 0;
    std::size_t to = 0;
    if (before.ends_with("[[") && after.starts_with("]]")) {
      from = before.size() - 2;
      to = buffer.size() - after.size() + 2;
    } else if (before.ends_with("__attribute__((") && after.starts_with("))")) {
      from = before.size() - std::strlen("__attribute__((");
      to = buffer.size() - after.size() + 2;
    } else {
      return range;
    }
    llvm::StringRef rest = buffer.substr(to);
    if (statement) {
      rest = rest.ltrim(" \t");
      if (!rest.starts_with(";"))
        return range;
      to = buffer.size() - rest.size() + 1;
    } else if (rest.starts_with(" ")) {
      ++to;
    }
    return clang::CharSourceRange::getCharRange(
        sources.getComposedLoc(file, static_cast<unsigned>(from)),
        sources.getComposedLoc(file, static_cast<unsigned>(to)));
  }

  /// `assumed`, at `at`, with the certain fix that replaces `range` by
  /// `replacement`.
  void remove(clang::SourceLocation at, clang::CharSourceRange range,
              const std::string &replacement, llvm::StringRef what) {
    if (fixes != nullptr) {
      Want wanted;
      wanted.kind = Want::Kind::Remove;
      wanted.removal = range;
      wanted.replacement = replacement;
      fixes->want(at, std::move(wanted));
    }
    report(at, rule::Assumed,
           "'" + what.str() + "' tells the optimizer what nothing checks",
           what == "assume" ? "delete it; 'if (!(e)) " +
                                  std::string(ast.getLangOpts().CPlusPlus
                                                  ? "std::unreachable()"
                                                  : "__builtin_unreachable()") +
                                  ";' states the same, and is checked"
                            : "delete it: nothing checks what it says");
  }

  /// An asserting attribute of `declaration`, once each: of the safe
  /// function's own (`own`), or of a function it calls. An allocation
  /// attribute of a function that is not safe (a project's allocator) says
  /// what it returns, which Safe C types the allocation by, and the runtime
  /// tracks the block whatever the attribute says: it is that code's
  /// promise, as the rest of its code is.
  void attributes(const clang::Decl &declaration, bool own) {
    if (!attributed.insert(&declaration).second)
      return;
    for (const clang::Attr *attribute : declaration.attrs()) {
      if (attribute->isImplicit() || attribute->isInherited() ||
          attribute->getLocation().isInvalid() ||
          sources.isInSystemHeader(attribute->getLocation()))
        continue;
      if (!own && llvm::isa<clang::RestrictAttr, clang::AllocSizeAttr,
                            clang::AllocAlignAttr>(attribute))
        continue;
      if (!llvm::isa<clang::PureAttr, clang::ConstAttr, clang::RestrictAttr,
                     clang::AllocSizeAttr, clang::AllocAlignAttr,
                     clang::AssumeAlignedAttr, clang::ReturnsNonNullAttr,
                     clang::NonNullAttr>(attribute))
        continue;
      const std::string name = attribute->getSpelling();
      remove(attribute->getLocation(),
             enclosing(
                 clang::CharSourceRange::getTokenRange(attribute->getRange()),
                 false),
             "", name);
    }
  }

  /// `restrict` on a declaration's type, at the qualifier's token.
  void restricted(const clang::DeclaratorDecl &declaration) {
    if (!declaration.getType().isRestrictQualified() ||
        sources.isInSystemHeader(declaration.getLocation()))
      return;
    // The qualifier's token, between the declaration's start and its name.
    const clang::SourceLocation begin =
        sources.getExpansionLoc(declaration.getBeginLoc());
    const clang::SourceLocation end =
        sources.getExpansionLoc(declaration.getLocation());
    clang::SourceLocation at = begin;
    clang::Token token{};
    for (unsigned guard = 0; guard < 64 && at.isValid() &&
                             sources.isBeforeInTranslationUnit(at, end);
         ++guard) {
      if (clang::Lexer::getRawToken(at, token, sources, ast.getLangOpts(),
                                    /*IgnoreWhiteSpace=*/true))
        break;
      const std::string spelling = text(token.getLocation());
      if (spelling == "restrict" || spelling == "__restrict" ||
          spelling == "__restrict__") {
        remove(token.getLocation(),
               clang::CharSourceRange::getTokenRange(token.getLocation()), "",
               spelling);
        return;
      }
      at = token.getEndLoc();
    }
    // A qualifier a typedef or a macro spells: no edit the fixer can make.
    report(declaration.getLocation(), rule::Assumed,
           "'restrict' tells the optimizer what nothing checks",
           "delete it: nothing checks what it says");
  }

  /// `vectorize(assume_safety)`, `interleave(assume_safety)`: `enable`
  /// states the same and asserts nothing.
  void assumeSafety(const clang::LoopHintAttr &hint) {
    const clang::SourceLocation from =
        sources.getExpansionLoc(hint.getRange().getBegin());
    bool invalid = false;
    const llvm::StringRef buffer =
        sources.getBufferData(sources.getFileID(from), &invalid);
    if (invalid)
      return;
    const unsigned offset = sources.getFileOffset(from);
    const std::size_t line = buffer.find('\n', offset);
    const std::size_t found =
        buffer.substr(0, line).find("assume_safety", offset);
    if (found == llvm::StringRef::npos) {
      report(from, rule::Assumed,
             "'assume_safety' tells the optimizer what nothing checks",
             "write 'enable', which asserts nothing");
      return;
    }
    const clang::SourceLocation at = sources.getComposedLoc(
        sources.getFileID(from), static_cast<unsigned>(found));
    remove(at,
           clang::CharSourceRange::getCharRange(
               at, at.getLocWithOffset(sizeof("assume_safety") - 1)),
           "enable", "assume_safety");
  }

  void boolBytes(clang::SourceLocation at, clang::QualType type) {
    const std::string member = boolMember(type, ast);
    const std::string name =
        type.getUnqualifiedType().getAsString(ast.getPrintingPolicy());
    report(at, rule::PointerBytes,
           type->isBooleanType()
               ? "bytes are written into a 'bool'"
               : "bytes are written into a '" + name +
                     "', which holds a 'bool'" +
                     (member.empty() ? std::string() : " ('" + member + "')"),
           ast.getLangOpts().CPlusPlus
               ? "a 'bool' is 0 or 1: give the member the type "
                 "'std::uint8_t', or read the bytes and assign each member"
               : "a 'bool' is 0 or 1: give the member the type 'uint8_t', "
                 "or read the bytes and assign each member");
  }

  /// A library call that writes bytes into an object holding a `bool`, and
  /// one that registers a signal handler.
  void library(const clang::CallExpr &call, const clang::FunctionDecl &callee) {
    if (callee.getIdentifier() == nullptr)
      return;
    const auto match = core::LibrarySpec::shipped().lookup(
        callee.getName().str(), call.getNumArgs());
    if (!match.has_value())
      return;
    if (match->entry->handler && registers(call)) {
      report(call.getExprLoc(), rule::SignalHandler,
             "'" + callee.getName().str() +
                 "' registers a handler that runs between any two "
                 "instructions of the program",
             ast.getLangOpts().CPlusPlus
                 ? "register it in a WEAVEC_GUARDED function and let it only "
                   "set a 'std::atomic' or write to a pipe; or block the "
                   "signal and wait for it with 'sigwait' on a thread"
                 : "register it in a WEAVEC_GUARDED function and let it only "
                   "set a 'volatile sig_atomic_t' or write to a pipe; or "
                   "block the signal and wait for it with 'sigwait' on a "
                   "thread");
      return;
    }
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      const int row = match->rowArgument(i);
      if (row < 0 ||
          static_cast<std::size_t>(row) >= match->entry->params.size())
        continue;
      const core::LibraryParam &parameter =
          match->entry->params[static_cast<std::size_t>(row)];
      if (parameter.type != core::LibraryParam::Type::Pointer ||
          parameter.effect != core::LibraryParam::Effect::Borrow ||
          (parameter.access != core::LibraryParam::Access::Write &&
           parameter.access != core::LibraryParam::Access::ReadWrite))
        continue;
      const clang::QualType given =
          call.getArg(i)->IgnoreParenImpCasts()->getType();
      if (!given->isPointerType() || !holdsBool(given->getPointeeType()) ||
          zeroOrCopy(call, i, given->getPointeeType(), *match))
        continue;
      boolBytes(call.getArg(i)->getExprLoc(), given->getPointeeType());
      return;
    }
  }

  /// A fill with zero, or a copy whose source has the destination's type.
  bool zeroOrCopy(const clang::CallExpr &call, unsigned index,
                  clang::QualType type, const core::LibraryMatch &match) const {
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      if (i == index)
        continue;
      const clang::Expr *argument = call.getArg(i)->IgnoreParenImpCasts();
      const int row = match.rowArgument(i);
      if (row < 0 ||
          static_cast<std::size_t>(row) >= match.entry->params.size())
        continue;
      const core::LibraryParam &parameter =
          match.entry->params[static_cast<std::size_t>(row)];
      if (parameter.type == core::LibraryParam::Type::Pointer &&
          parameter.access == core::LibraryParam::Access::Read &&
          argument->getType()->isPointerType() &&
          clang::ASTContext::hasSameUnqualifiedType(
              argument->getType()->getPointeeType(), type))
        return true;
      clang::Expr::EvalResult value;
      if (parameter.type == core::LibraryParam::Type::Int && i == index + 1 &&
          !argument->isValueDependent() &&
          argument->EvaluateAsInt(value, ast) && value.Val.getInt().isZero())
        return true;
    }
    return false;
  }

  /// Whether a `handler` call registers code: a function pointer that is
  /// not a constant (SIG_IGN, SIG_DFL), or a description that is not null.
  bool registers(const clang::CallExpr &call) const {
    return llvm::any_of(call.arguments(), [&](const clang::Expr *argument) {
      const clang::QualType type = argument->getType();
      const bool null = argument->isNullPointerConstant(
                            ast, clang::Expr::NPC_ValueDependentIsNotNull) !=
                        clang::Expr::NPCK_NotNull;
      if (type->isFunctionPointerType())
        return !llvm::isa<clang::IntegerLiteral>(
                   argument->IgnoreParenCasts()) &&
               !null;
      // The old action, written, registers nothing.
      if (type->isPointerType() && !type->getPointeeType().isConstQualified() &&
          type->getPointeeType()->isRecordType())
        return false;
      return type->isPointerType() && !null;
    });
  }

  /// `to` and `from` are specializations of one class template that owns
  /// (it has a destructor and the table does not call it shared), of a
  /// base and of a class derived from it, and the base's destructor is
  /// not virtual.
  void ownerToBase(clang::QualType to, clang::QualType from,
                   clang::SourceLocation at) {
    const auto *target =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            to.getNonReferenceType()->getAsCXXRecordDecl());
    const auto *source =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            from.getNonReferenceType()->getAsCXXRecordDecl());
    if (target == nullptr || source == nullptr || target == source ||
        target->getSpecializedTemplate() != source->getSpecializedTemplate() ||
        target->getTemplateArgs().size() == 0 ||
        source->getTemplateArgs().size() == 0 ||
        target->getTemplateArgs()[0].getKind() !=
            clang::TemplateArgument::Type ||
        source->getTemplateArgs()[0].getKind() !=
            clang::TemplateArgument::Type ||
        !target->hasNonTrivialDestructor() ||
        core::LibrarySpec::shipped().classIs(
            target->getSpecializedTemplate()->getQualifiedNameAsString(),
            "shared"))
      return;
    const clang::CXXRecordDecl *base =
        target->getTemplateArgs()[0].getAsType()->getAsCXXRecordDecl();
    const clang::CXXRecordDecl *derived =
        source->getTemplateArgs()[0].getAsType()->getAsCXXRecordDecl();
    if (base == nullptr || derived == nullptr || !base->hasDefinition() ||
        !derived->hasDefinition() || !derived->isDerivedFrom(base))
      return;
    const clang::CXXDestructorDecl *destructor = base->getDestructor();
    if (destructor != nullptr && destructor->isVirtual())
      return;
    report(at, rule::PointerCast,
           "'" + base->getName().str() + "' owns a '" +
               derived->getName().str() +
               "', and its destructor is not virtual: the object would be "
               "destroyed as a '" +
               base->getName().str() + "'",
           "give '" + base->getName().str() +
               "' a virtual destructor, or keep the owner's type '" +
               derived->getName().str() + "'");
  }

  /// `std::bit_cast`: a library function whose body is one bit cast.
  void bitCast(const clang::CallExpr &call, const clang::FunctionDecl &callee) {
    if (!sources.isInSystemHeader(callee.getLocation()) ||
        !holdsBool(call.getType()))
      return;
    const clang::FunctionDecl *definition = callee.getDefinition();
    const auto *body = llvm::dyn_cast_or_null<clang::CompoundStmt>(
        definition == nullptr ? nullptr : definition->getBody());
    if (body == nullptr || body->size() != 1)
      return;
    const auto *returned =
        llvm::dyn_cast<clang::ReturnStmt>(body->body_front());
    if (returned == nullptr || returned->getRetValue() == nullptr ||
        !llvm::isa<clang::BuiltinBitCastExpr>(
            returned->getRetValue()->IgnoreImplicit()))
      return;
    if (call.getNumArgs() == 1 &&
        clang::ASTContext::hasSameUnqualifiedType(call.getArg(0)->getType(),
                                                  call.getType()))
      return;
    boolBytes(call.getExprLoc(), call.getType());
  }
};

} // namespace

unsigned countHandlers(const clang::FunctionDecl &function) {
  struct Handlers final : clang::RecursiveASTVisitor<Handlers> {
    unsigned count = 0;
    // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
    bool VisitCallExpr(clang::CallExpr *call) {
      const clang::FunctionDecl *callee = call->getDirectCallee();
      if (callee != nullptr && callee->getIdentifier() != nullptr)
        if (const auto match = core::LibrarySpec::shipped().lookup(
                callee->getName().str(), call->getNumArgs());
            match.has_value() && match->entry->handler)
          ++count;
      return true;
    }
  } handlers;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  handlers.TraverseStmt(const_cast<clang::Stmt *>(function.getBody()));
  return handlers.count;
}

void checkUndefined(
    clang::ASTContext &ast, const clang::FunctionDecl &function,
    const std::function<void(clang::SourceLocation, llvm::StringRef,
                             llvm::StringRef, llvm::StringRef)> &report,
    FixPlanner *fixes) {
  Undefined rules(ast, report, fixes);
  rules.declaration(function);
  // The visitor takes what it walks as changeable; it changes nothing.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  rules.TraverseStmt(const_cast<clang::Stmt *>(function.getBody()));
}

} // namespace weavec::frontend
