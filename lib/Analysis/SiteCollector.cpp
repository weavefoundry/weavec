//===- SiteCollector.cpp - Sites of emitted functions (RFC 0030) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/SiteCollector.h"

#include "weavec/Analysis/Annotations.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/KindInference.h"

#include "clang/AST/Attr.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CheckedArithmetic.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace weavec::analysis {

//===----------------------------------------------------------------------===//
// SiteIndex
//===----------------------------------------------------------------------===//

const SiteIndex::FunctionSites *
SiteIndex::function(const clang::FunctionDecl &decl) const {
  const auto found = byFunction.find(decl.getCanonicalDecl());
  return found == byFunction.end() ? nullptr : &functionList[found->second];
}

const SiteInfo *SiteIndex::info(core::SiteId id) const {
  if (id.function >= functionList.size())
    return nullptr;
  const std::vector<SiteInfo> &sites = functionList[id.function].sites;
  return id.ordinal < sites.size() ? &sites[id.ordinal] : nullptr;
}

llvm::ArrayRef<core::SiteId> SiteIndex::sitesOf(const clang::Stmt &stmt) const {
  const auto found = byStmt.find(&stmt);
  if (found == byStmt.end())
    return {};
  return found->second;
}

static bool isNonReturningCallExit(const SiteInfo &site) {
  return site.boundary == core::Boundary::Exit &&
         llvm::isa<clang::CallExpr>(site.stmt);
}

std::optional<core::SiteId> SiteIndex::find(const clang::Stmt &stmt) const {
  for (const core::SiteId id : sitesOf(stmt)) {
    const SiteInfo *site = info(id);
    if (site != nullptr && !isNonReturningCallExit(*site))
      return id;
  }
  return std::nullopt;
}

std::optional<core::SiteId>
SiteIndex::find(const clang::Stmt &stmt, core::SiteKind kind,
                std::optional<core::Boundary> boundary) const {
  for (const core::SiteId id : sitesOf(stmt)) {
    const SiteInfo *site = info(id);
    if (site != nullptr && site->kind == kind &&
        (!boundary || site->boundary == boundary))
      return id;
  }
  return std::nullopt;
}

std::optional<core::SiteId> SiteIndex::findExit(const clang::Stmt &stmt) const {
  return find(stmt, core::SiteKind::Call, core::Boundary::Exit);
}

static bool contains(clang::SourceLocation begin, clang::SourceLocation end,
                     clang::SourceLocation loc,
                     const clang::SourceManager &sm) {
  if (begin.isInvalid() || end.isInvalid() || loc.isInvalid())
    return false;
  return !sm.isBeforeInTranslationUnit(loc, begin) &&
         !sm.isBeforeInTranslationUnit(end, loc);
}

const SiteIndex::FunctionSites *
SiteIndex::functionAt(clang::SourceLocation loc,
                      const clang::SourceManager &sm) const {
  const clang::SourceLocation at = sm.getExpansionLoc(loc);
  for (const FunctionSites &function : functionList)
    if (contains(function.begin, function.end, at, sm))
      return &function;
  return nullptr;
}

std::optional<core::SiteId>
SiteIndex::innermostAt(clang::SourceLocation loc,
                       std::optional<core::Facet> facet,
                       const clang::SourceManager &sm) const {
  const FunctionSites *function = functionAt(loc, sm);
  if (function == nullptr)
    return std::nullopt;
  const clang::SourceLocation at = sm.getExpansionLoc(loc);
  const core::FunctionLedger &row = rows[function->index];
  const SiteInfo *best = nullptr;
  for (const SiteInfo &site : function->sites) {
    if (!contains(site.begin, site.end, at, sm))
      continue;
    if (facet && !row.sites[site.id.ordinal].hasFacet(*facet))
      continue;
    // Innermost: a range inside the best one so far. Equal ranges keep the
    // later ordinal, which is the inner site (ties sort inner first, but a
    // call's exit comes after the call).
    if (best == nullptr || (contains(best->begin, best->end, site.begin, sm) &&
                            contains(best->begin, best->end, site.end, sm) &&
                            !isNonReturningCallExit(site)))
      best = &site;
  }
  if (best == nullptr)
    return std::nullopt;
  return best->id;
}

//===----------------------------------------------------------------------===//
// Syntactic helpers
//===----------------------------------------------------------------------===//

bool isPlatformDeclaration(const clang::FunctionDecl &function,
                           const core::LibrarySpec &library,
                           const clang::SourceManager &sm) {
  const clang::SourceLocation location =
      sm.getExpansionLoc(function.getCanonicalDecl()->getLocation());
  if (!sm.isInSystemHeader(location))
    return false;
  const llvm::StringRef path = sm.getFilename(location);
  if (path.empty())
    return false;
  if (path.contains("/lib/clang/"))
    return true;
  const bool darwinSdk = path.contains(".sdk/");
  // The include directory that found the header is not known here: every
  // suffix of the path is a candidate name.
  llvm::StringRef rest = path;
  while (!rest.empty()) {
    if (library.isPlatformHeader(std::string_view(rest.data(), rest.size()),
                                 darwinSdk))
      return true;
    const std::size_t slash = rest.find('/');
    if (slash == llvm::StringRef::npos)
      break;
    rest = rest.drop_front(slash + 1);
  }
  return false;
}

bool isReturnsTwice(const clang::FunctionDecl &function,
                    const core::LibrarySpec &library) {
  if (function.hasAttr<clang::ReturnsTwiceAttr>())
    return true;
  if (function.getBuiltinID() == clang::Builtin::BI__builtin_setjmp)
    return true;
  if (function.getIdentifier() == nullptr)
    return false;
  const llvm::StringRef name = function.getName();
  const auto match = library.lookup(std::string_view(name.data(), name.size()));
  return match && match->entry->returnsTwice;
}

std::optional<SourceTerm> argumentTerm(const core::ExtentTerm &term,
                                       const clang::CallExpr &call) {
  if (term.isConstant())
    return SourceTerm::ofConstant(term.offset);
  if (term.path->root != core::ExtentPath::Root::Param ||
      term.path->param >= call.getNumArgs())
    return std::nullopt;
  const clang::Expr &arg = *call.getArg(term.path->param);
  if (!arg.getType()->isIntegerType())
    return std::nullopt;
  SourceTerm out = SourceTerm::ofExpr(arg);
  if (term.scale != 1)
    out = SourceTerm::mul(std::move(out), SourceTerm::ofConstant(term.scale));
  if (term.offset == std::numeric_limits<std::int64_t>::min())
    return std::nullopt;
  if (term.offset > 0)
    out = SourceTerm::add(std::move(out), SourceTerm::ofConstant(term.offset));
  else if (term.offset < 0)
    out = SourceTerm::sub(std::move(out), SourceTerm::ofConstant(-term.offset));
  return out;
}

std::optional<SourceTerm> guardTerm(const RequirementGuard &guard,
                                    const clang::CallExpr &call) {
  // R2's `c < e` over a loop bound: `e - c`, and `c <= e` is `e - c + 1`,
  // one term over the argument (R3's pointer guards have no term).
  if (!guard.lhs.isConstant() || guard.rhs.isConstant() || guard.rhs.scale <= 0)
    return std::nullopt;
  auto offset = llvm::checkedSub(guard.rhs.offset, guard.lhs.offset);
  if (offset && guard.relation == RequirementGuard::Relation::LessEqual)
    offset = llvm::checkedAdd(*offset, std::int64_t{1});
  if (!offset)
    return std::nullopt;
  // The helpers stop a negative signed argument at zero before they add
  // (§10.2): `last + 1` would pass for `last == -1`. Only a term that adds
  // nothing to a signed argument is exact.
  const clang::Expr *arg =
      guard.rhs.path->root == core::ExtentPath::Root::Param &&
              guard.rhs.path->param < call.getNumArgs()
          ? call.getArg(guard.rhs.path->param)
          : nullptr;
  if (arg == nullptr || (*offset > 0 && arg->getType()->isSignedIntegerType()))
    return std::nullopt;
  core::ExtentTerm difference = guard.rhs;
  difference.offset = *offset;
  return argumentTerm(difference, call);
}

bool callDoesNotReturn(const clang::CallExpr &call,
                       const core::LibrarySpec &library) {
  if (const clang::FunctionDecl *callee = call.getDirectCallee()) {
    if (callee->isNoReturn())
      return true;
    if (const auto match = governingLibraryEntry(*callee, library))
      return match->entry->noreturn || match->entry->exits;
    return false;
  }
  clang::QualType type = call.getCallee()->getType();
  if (const auto *pointer = type->getAs<clang::PointerType>())
    type = pointer->getPointeeType();
  if (const auto *function = type->getAs<clang::FunctionType>())
    return function->getNoReturnAttr();
  return false;
}

/// `p + i`, `p - i` (with a pointer result).
static bool isPointerOffset(const clang::Expr *expr) {
  const auto *binary =
      llvm::dyn_cast<clang::BinaryOperator>(expr->IgnoreParens());
  return binary != nullptr &&
         (binary->getOpcode() == clang::BO_Add ||
          binary->getOpcode() == clang::BO_Sub) &&
         binary->getType()->isPointerType();
}

/// §2.1 PtrArith: `p + i`, `p - i`, `++p`, `p++`, `--p`, `p--`, `p += i`,
/// `p -= i`, and `&p[i]` (`&*(p + i)`).
static bool isPointerArithmetic(const clang::Expr *expr) {
  expr = expr->IgnoreParens();
  if (isPointerOffset(expr))
    return true;
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (unary->isIncrementDecrementOp())
      return unary->getType()->isPointerType();
    if (unary->getOpcode() != clang::UO_AddrOf)
      return false;
    const clang::Expr *sub = unary->getSubExpr()->IgnoreParens();
    if (llvm::isa<clang::ArraySubscriptExpr>(sub))
      return true;
    const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(sub);
    return deref != nullptr && deref->getOpcode() == clang::UO_Deref &&
           isPointerOffset(deref->getSubExpr());
  }
  if (const auto *compound =
          llvm::dyn_cast<clang::CompoundAssignOperator>(expr))
    return (compound->getOpcode() == clang::BO_AddAssign ||
            compound->getOpcode() == clang::BO_SubAssign) &&
           compound->getType()->isPointerType();
  return false;
}

/// The array lvalue a pointer operand decays from, through the implicit
/// conversions of an argument (`char *` to `void *`), or null.
static const clang::Expr *decayedArray(const clang::Expr *pointer) {
  const clang::Expr *expr = pointer->IgnoreParens();
  while (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
    if (cast->getCastKind() == clang::CK_ArrayToPointerDecay)
      return cast->getSubExpr();
    if (cast->getCastKind() != clang::CK_NoOp &&
        cast->getCastKind() != clang::CK_BitCast)
      return nullptr;
    expr = cast->getSubExpr()->IgnoreParens();
  }
  return nullptr;
}

/// §2.1: whether the object an array lvalue designates can end its lifetime
/// before the site. Automatic storage in scope and static storage cannot.
static bool arrayStorageCanEnd(const clang::Expr *lvalue) {
  const clang::Expr *expr = lvalue;
  for (unsigned depth = 0; expr != nullptr && depth < 64; ++depth) {
    expr = expr->IgnoreParens();
    if (llvm::isa<clang::DeclRefExpr, clang::StringLiteral,
                  clang::CompoundLiteralExpr, clang::PredefinedExpr,
                  clang::CallExpr>(expr))
      return false;
    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
      if (member->isArrow())
        return true;
      expr = member->getBase();
      continue;
    }
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
      const clang::Expr *array = decayedArray(subscript->getBase());
      if (array == nullptr)
        return true;
      expr = array;
      continue;
    }
    return true;
  }
  return true;
}

/// Whether the statement can complete normally, so that control can reach
/// what follows it (for the end of a body, §2.1). Conservative: true unless
/// the syntax shows otherwise.
static bool canComplete(const clang::Stmt *stmt,
                        const clang::ASTContext &context,
                        const core::LibrarySpec &library, unsigned depth = 0);

/// A `break` that leaves the loop or switch `stmt` belongs to.
static bool hasOwnBreak(const clang::Stmt *stmt) {
  if (stmt == nullptr)
    return false;
  if (llvm::isa<clang::BreakStmt>(stmt))
    return true;
  // A nested loop or switch owns the breaks inside it.
  if (llvm::isa<clang::ForStmt, clang::WhileStmt, clang::DoStmt,
                clang::SwitchStmt>(stmt))
    return false;
  return llvm::any_of(stmt->children(), [](const clang::Stmt *child) {
    return hasOwnBreak(child);
  });
}

static bool isTrueConstant(const clang::Expr *condition,
                           const clang::ASTContext &context) {
  if (condition == nullptr)
    return true;
  const auto value = condition->getIntegerConstantExpr(context);
  return value && !value->isZero();
}

static bool canComplete(const clang::Stmt *stmt,
                        const clang::ASTContext &context,
                        const core::LibrarySpec &library, unsigned depth) {
  if (stmt == nullptr || depth > 256)
    return true;
  if (llvm::isa<clang::ReturnStmt, clang::GotoStmt, clang::IndirectGotoStmt,
                clang::BreakStmt, clang::ContinueStmt>(stmt))
    return false;
  if (const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(stmt))
    return compound->body_empty() ||
           canComplete(compound->body_back(), context, library, depth + 1);
  if (const auto *label = llvm::dyn_cast<clang::LabelStmt>(stmt))
    return canComplete(label->getSubStmt(), context, library, depth + 1);
  if (const auto *attributed = llvm::dyn_cast<clang::AttributedStmt>(stmt))
    return canComplete(attributed->getSubStmt(), context, library, depth + 1);
  if (const auto *branch = llvm::dyn_cast<clang::IfStmt>(stmt))
    return branch->getElse() == nullptr ||
           canComplete(branch->getThen(), context, library, depth + 1) ||
           canComplete(branch->getElse(), context, library, depth + 1);
  // An endless loop completes only through a `break` of its own.
  if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(stmt))
    return !isTrueConstant(loop->getCond(), context) ||
           hasOwnBreak(loop->getBody());
  if (const auto *loop = llvm::dyn_cast<clang::WhileStmt>(stmt))
    return !isTrueConstant(loop->getCond(), context) ||
           hasOwnBreak(loop->getBody());
  if (const auto *loop = llvm::dyn_cast<clang::DoStmt>(stmt))
    return !isTrueConstant(loop->getCond(), context) ||
           hasOwnBreak(loop->getBody());
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt)) {
    const auto *call =
        llvm::dyn_cast<clang::CallExpr>(expr->IgnoreParenImpCasts());
    return call == nullptr || !callDoesNotReturn(*call, library);
  }
  return true;
}

/// §7.5: the null part of a static callee's inferred requirements on
/// argument `index` of `call`: unguarded when one requirement is, else under
/// the first requirement's guard (the body relies on no other, §7.5 and
/// KindSeeding.cpp); none when no requirement is non-null.
static std::optional<ArgumentNeed> inferredNullNeed(const KindEntry &param,
                                                    const clang::CallExpr &call,
                                                    unsigned index) {
  const MustAccessRequirement *first = nullptr;
  for (const MustAccessRequirement &requirement : param.mustAccess) {
    if (requirement.kind.nullability != core::Nullability::Nonnull)
      continue;
    if (!requirement.guard)
      return ArgumentNeed{.argument = static_cast<std::uint8_t>(index),
                          .nonnull = true,
                          .inferred = true};
    if (first == nullptr)
      first = &requirement;
  }
  if (first == nullptr)
    return std::nullopt;
  ArgumentNeed need{.argument = static_cast<std::uint8_t>(index),
                    .nonnull = true,
                    .inferred = true};
  // A guard with no C spelling here allows a null argument.
  need.allowedIfZero = !guardTerm(*first->guard, call).has_value();
  return need;
}

namespace {

/// Where a subexpression is evaluated.
struct Context {
  /// Inside a `WEAVEC_UNSAFE` function or block.
  bool unsafe = false;
};

/// How the parent uses an lvalue: accessed, only its address taken (`&`),
/// or decayed to a pointer (an array).
enum class Use : std::uint8_t { Value, AddressOf, Decay };

/// The facets a site gets.
struct FacetSet {
  bool spatial = false;
  bool null = false;
  bool temporal = false;
  bool assertion = false;
};

/// A site before ordinals are assigned.
struct PendingSite {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SiteInfo info = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  FacetSet facets = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string callee = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string text = {};
  /// Where the ledger reports the site (its begin, or the closing brace).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  clang::SourceLocation location = {};
  std::size_t sequence = 0;
};

/// The variables a body assigns, increments or takes the address of.
class ModifiedVariables : public clang::RecursiveASTVisitor<ModifiedVariables> {
public:
  llvm::DenseSet<const clang::VarDecl *> modified;

  // RecursiveASTVisitor's CRTP hooks are found by name.
  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitUnaryOperator(clang::UnaryOperator *op) {
    if (op->isIncrementDecrementOp() || op->getOpcode() == clang::UO_AddrOf)
      note(op->getSubExpr());
    return true;
  }
  // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method)
  bool VisitBinaryOperator(clang::BinaryOperator *op) {
    if (op->isAssignmentOp())
      note(op->getLHS());
    return true;
  }

private:
  void note(const clang::Expr *expr) {
    if (const auto *ref =
            llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts()))
      if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl()))
        modified.insert(variable->getCanonicalDecl());
  }
};

/// Walks one emitted function's body and records its sites.
class Walker {
public:
  Walker(clang::ASTContext &ctx, const KindTable &kindTable,
         const core::LibrarySpec &spec, const clang::FunctionDecl &fn,
         const KindInferenceResult *inference, const SlotCollection *unitSlots,
         const core::SlotSolution *slotSolution)
      : context(ctx), sm(ctx.getSourceManager()), kinds(kindTable),
        library(spec), function(fn), signature(collectAnnotations(fn)),
        inferred(inference), slots(unitSlots), solution(slotSolution) {}

  /// Walks the body; the sites are then in `sites` in walk order.
  void walkBody();

  std::vector<PendingSite> sites;
  bool callsSetjmp = false;

private:
  clang::ASTContext &context;
  const clang::SourceManager &sm;
  const KindTable &kinds;
  const core::LibrarySpec &library;
  const clang::FunctionDecl &function;
  SignatureAnnotations signature;
  const KindInferenceResult *inferred;
  const SlotCollection *slots;
  const core::SlotSolution *solution;
  /// RFC 0030 §9.3: the callee of a call, following a slot that is closed
  /// on one target which a `LibrarySpec` row governs (`fp = memcpy;
  /// fp(dst, src, n)` applies memcpy's row, as a direct call would). Every
  /// other indirect call has no callee here.
  [[nodiscard]] const clang::FunctionDecl *
  calleeOf(const clang::CallExpr &call) const;
  llvm::DenseSet<const clang::VarDecl *> modified;
  llvm::DenseSet<const clang::Stmt *> visited;
  std::size_t sequence = 0;

  void walkStmt(const clang::Stmt *stmt, Context ctx);
  void walkExpr(const clang::Expr *expr, Context ctx, Use use = Use::Value,
                const KindEntry *required = nullptr);
  void walkDeref(const clang::UnaryOperator &deref, Context ctx, Use use);
  void walkUnary(const clang::UnaryOperator &unary, Context ctx,
                 const KindEntry *required);
  void walkBinary(const clang::BinaryOperator &binary, Context ctx, Use use,
                  const KindEntry *required);
  void walkSubscript(const clang::ArraySubscriptExpr &subscript, Context ctx,
                     Use use);
  void walkMember(const clang::MemberExpr &member, Context ctx, Use use);
  void walkCast(const clang::CastExpr &cast, Context ctx, Use use,
                const KindEntry *required);
  void walkCall(const clang::CallExpr &call, Context ctx);
  void walkInitList(const clang::InitListExpr &list, Context ctx);
  void walkDecl(const clang::Decl &decl, Context ctx);
  void walkVariablyModified(clang::QualType type, Context ctx);

  PendingSite &addSite(core::SiteKind kind, const clang::Stmt &stmt,
                       FacetSet facets, Context ctx);
  void addStore(const clang::Stmt &store, const clang::Expr &value,
                const KindEntry &slot, Context ctx);
  void addCallSites(const clang::CallExpr &call, Context ctx);

  /// The declared kind of the slot `lvalue` designates, when it has a shape.
  [[nodiscard]] const KindEntry *slotKind(const clang::Expr *lvalue) const;
  /// §2.1 Raw: a raw origin visible in the syntax.
  [[nodiscard]] bool hasRawOrigin(const clang::Expr *pointer,
                                  unsigned depth = 0) const;
  [[nodiscard]] bool isWidening(clang::QualType from, clang::QualType to) const;
  /// Whether a stored or passed value carries its own PtrArith or Cast
  /// obligation, so that the store needs no Cast site of its own.
  [[nodiscard]] bool carriesOwnObligation(const clang::Expr *value) const;
  [[nodiscard]] bool isNullConstant(const clang::Expr *value) const;
  [[nodiscard]] bool isFlexible(const clang::Expr &array) const;
  [[nodiscard]] std::optional<std::int64_t>
  constantBound(const clang::Expr &array) const;
  [[nodiscard]] std::optional<SourceTerm>
  libraryTerm(const core::LibTerm &term, const clang::CallExpr &call,
              const core::LibraryMatch &match) const;
};

} // namespace

//===----------------------------------------------------------------------===//
// The walk
//===----------------------------------------------------------------------===//

PendingSite &Walker::addSite(core::SiteKind kind, const clang::Stmt &stmt,
                             FacetSet facets, Context ctx) {
  PendingSite &site = sites.emplace_back();
  site.info.kind = kind;
  site.info.stmt = &stmt;
  const clang::CharSourceRange range =
      sm.getExpansionRange(stmt.getSourceRange());
  site.info.begin = range.getBegin();
  site.info.end = range.getEnd();
  site.info.inUnsafe = ctx.unsafe;
  site.facets = facets;
  site.location = sm.getExpansionLoc(stmt.getBeginLoc());
  // The text of an operation inside a macro argument is the argument's own
  // text; inside a macro body it is the whole invocation.
  clang::CharSourceRange written = clang::Lexer::makeFileCharRange(
      clang::CharSourceRange::getTokenRange(stmt.getSourceRange()), sm,
      context.getLangOpts());
  if (written.isInvalid())
    written = range;
  const llvm::StringRef text =
      clang::Lexer::getSourceText(written, sm, context.getLangOpts());
  site.text = core::siteText(std::string_view(text.data(), text.size()));
  site.sequence = sequence++;
  return site;
}

void Walker::walkBody() {
  clang::Stmt *body = function.getBody();
  if (body == nullptr)
    return;
  ModifiedVariables finder;
  finder.TraverseStmt(body);
  modified = std::move(finder.modified);
  Context ctx;
  ctx.unsafe = signature.unsafe;
  walkStmt(body, ctx);
  if (!canComplete(body, context, library))
    return;
  // The end of the body is an exit when control can reach it (§2.1).
  PendingSite &end =
      addSite(core::SiteKind::Call, *body, FacetSet{.temporal = true}, ctx);
  end.info.boundary = core::Boundary::Exit;
  const auto *compound = llvm::dyn_cast<clang::CompoundStmt>(body);
  const clang::SourceLocation brace = sm.getExpansionLoc(
      compound != nullptr ? compound->getRBracLoc() : body->getEndLoc());
  end.info.begin = brace;
  end.info.end = brace;
  end.location = brace;
  end.text = "}";
}

void Walker::walkStmt(const clang::Stmt *stmt, Context ctx) {
  if (stmt == nullptr)
    return;
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(stmt)) {
    walkExpr(expr, ctx);
    return;
  }
  if (!visited.insert(stmt).second)
    return;
  if (const auto *decls = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
    for (const clang::Decl *decl : decls->decls())
      walkDecl(*decl, ctx);
    return;
  }
  if (const auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
    const KindEntry *result = kinds.result(function);
    walkExpr(ret->getRetValue(), ctx, Use::Value,
             result != nullptr && result->hasDeclaredShape() ? result
                                                             : nullptr);
    PendingSite &exit =
        addSite(core::SiteKind::Call, *ret, FacetSet{.temporal = true}, ctx);
    exit.info.boundary = core::Boundary::Exit;
    return;
  }
  if (const auto *attributed = llvm::dyn_cast<clang::AttributedStmt>(stmt)) {
    if (isUnsafeBlock(*attributed))
      ctx.unsafe = true;
    walkStmt(attributed->getSubStmt(), ctx);
    return;
  }
  if (const auto *label = llvm::dyn_cast<clang::CaseStmt>(stmt)) {
    walkExpr(label->getLHS(), ctx);
    walkExpr(label->getRHS(), ctx);
    walkStmt(label->getSubStmt(), ctx);
    return;
  }
  if (const auto *assembly = llvm::dyn_cast<clang::GCCAsmStmt>(stmt)) {
    // RFC 0033 *Implementation amendments*: an operand the assembly takes
    // in memory only (`"+m" (*(uint64_t (*)[16])d)`) is passed by address,
    // and which of its bytes the assembly touches C does not say: its
    // lvalue is no access of its whole type. Register operands are loaded.
    const auto inMemory = [](llvm::StringRef constraint) {
      return constraint.find_first_of("mQoV") != llvm::StringRef::npos &&
             constraint.find_first_of("rgiX") == llvm::StringRef::npos;
    };
    for (unsigned i = 0; i < assembly->getNumOutputs(); ++i)
      walkExpr(assembly->getOutputExpr(i), ctx,
               inMemory(assembly->getOutputConstraint(i)) ? Use::AddressOf
                                                          : Use::Value);
    for (unsigned i = 0; i < assembly->getNumInputs(); ++i)
      walkExpr(assembly->getInputExpr(i), ctx,
               inMemory(assembly->getInputConstraint(i)) ? Use::AddressOf
                                                         : Use::Value);
    return;
  }
  for (const clang::Stmt *child : stmt->children())
    walkStmt(child, ctx);
}

void Walker::walkExpr(const clang::Expr *expr, Context ctx, Use use,
                      const KindEntry *required) {
  if (expr == nullptr || !visited.insert(expr).second)
    return;
  if (const auto *paren = llvm::dyn_cast<clang::ParenExpr>(expr)) {
    walkExpr(paren->getSubExpr(), ctx, use, required);
    return;
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (unary->getOpcode() == clang::UO_Deref)
      walkDeref(*unary, ctx, use);
    else
      walkUnary(*unary, ctx, required);
    return;
  }
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
    walkBinary(*binary, ctx, use, required);
    return;
  }
  if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
    walkExpr(choice->getCond(), ctx);
    walkExpr(choice->getTrueExpr(), ctx, Use::Value, required);
    walkExpr(choice->getFalseExpr(), ctx, Use::Value, required);
    return;
  }
  if (const auto *elvis =
          llvm::dyn_cast<clang::BinaryConditionalOperator>(expr)) {
    // `a ?: b`: `a` is evaluated once and read through opaque values.
    walkExpr(elvis->getCommon(), ctx);
    walkExpr(elvis->getCond(), ctx);
    walkExpr(elvis->getTrueExpr(), ctx, Use::Value, required);
    walkExpr(elvis->getFalseExpr(), ctx, Use::Value, required);
    return;
  }
  if (const auto *opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expr)) {
    walkExpr(opaque->getSourceExpr(), ctx);
    return;
  }
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expr)) {
    walkSubscript(*subscript, ctx, use);
    return;
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
    walkMember(*member, ctx, use);
    return;
  }
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr)) {
    walkCall(*call, ctx);
    return;
  }
  if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr)) {
    walkCast(*cast, ctx, use, required);
    return;
  }
  if (const auto *trait =
          llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(expr)) {
    // Unevaluated, except `sizeof` of a variable-length array, which C
    // evaluates (C11 6.5.3.4p2).
    if (trait->getKind() != clang::UETT_SizeOf)
      return;
    if (trait->isArgumentType()) {
      if (context.getAsVariableArrayType(trait->getArgumentType()) != nullptr)
        walkVariablyModified(trait->getArgumentType(), ctx);
      return;
    }
    // The operand is evaluated for its address and size, not read.
    if (context.getAsVariableArrayType(trait->getArgumentExpr()->getType()) !=
        nullptr)
      walkExpr(trait->getArgumentExpr(), ctx, Use::Decay);
    return;
  }
  if (const auto *generic = llvm::dyn_cast<clang::GenericSelectionExpr>(expr)) {
    // Only the selected association is evaluated.
    if (!generic->isResultDependent())
      walkExpr(generic->getResultExpr(), ctx, use, required);
    return;
  }
  if (const auto *choose = llvm::dyn_cast<clang::ChooseExpr>(expr)) {
    walkExpr(choose->getChosenSubExpr(), ctx, use, required);
    return;
  }
  if (const auto *statement = llvm::dyn_cast<clang::StmtExpr>(expr)) {
    walkStmt(statement->getSubStmt(), ctx);
    return;
  }
  if (const auto *literal = llvm::dyn_cast<clang::CompoundLiteralExpr>(expr)) {
    walkExpr(literal->getInitializer(), ctx);
    return;
  }
  if (const auto *list = llvm::dyn_cast<clang::InitListExpr>(expr)) {
    walkInitList(*list, ctx);
    return;
  }
  if (const auto *vaArg = llvm::dyn_cast<clang::VAArgExpr>(expr)) {
    walkExpr(vaArg->getSubExpr(), ctx);
    if (vaArg->getType()->isPointerType()) {
      PendingSite &site =
          addSite(core::SiteKind::IntToPtr, *vaArg,
                  FacetSet{.spatial = true, .temporal = true}, ctx);
      site.info.operand = vaArg;
    }
    return;
  }
  if (const auto *pseudo = llvm::dyn_cast<clang::PseudoObjectExpr>(expr)) {
    // CodeGen evaluates the semantic form.
    for (const clang::Expr *semantic : pseudo->semantics())
      walkExpr(semantic, ctx);
    return;
  }
  // A block literal's body is a function of its own.
  if (llvm::isa<clang::BlockExpr>(expr))
    return;
  if (const auto *constant = llvm::dyn_cast<clang::ConstantExpr>(expr)) {
    walkExpr(constant->getSubExpr(), ctx, use, required);
    return;
  }
  for (const clang::Stmt *child : expr->children())
    walkStmt(child, ctx);
}

static bool isZeroConstant(const clang::Expr *expr,
                           const clang::ASTContext &context) {
  const auto value = expr->getIntegerConstantExpr(context);
  return value && value->isZero();
}

/// §2.1: the null facet exists only for a pointer value, not an array
/// lvalue; the temporal facet only when the object's lifetime can end.
static void applyOperandRules(FacetSet &facets, const clang::Expr *pointer) {
  const clang::Expr *array = decayedArray(pointer);
  if (array == nullptr)
    return;
  facets.null = false;
  if (!arrayStorageCanEnd(array))
    facets.temporal = false;
}

/// `&object`: a pointer to one whole object, whose width the types give.
static bool isAddressOfObject(const clang::Expr *pointer) {
  const auto *address =
      llvm::dyn_cast<clang::UnaryOperator>(pointer->IgnoreParenImpCasts());
  if (address == nullptr || address->getOpcode() != clang::UO_AddrOf)
    return false;
  const clang::Expr *object = address->getSubExpr()->IgnoreParens();
  return !llvm::isa<clang::ArraySubscriptExpr, clang::UnaryOperator>(object);
}

void Walker::walkUnary(const clang::UnaryOperator &unary, Context ctx,
                       const KindEntry *required) {
  const clang::Expr *sub = unary.getSubExpr();
  if (unary.getOpcode() == clang::UO_AddrOf) {
    walkExpr(sub, ctx, Use::AddressOf);
    if (required == nullptr || !isPointerArithmetic(&unary))
      return;
    // `&p[i]` and `&*(p + i)` in a required position (§7.4).
    const clang::Expr *object = sub->IgnoreParens();
    const clang::Expr *pointer = nullptr;
    if (const auto *subscript =
            llvm::dyn_cast<clang::ArraySubscriptExpr>(object))
      pointer = subscript->getBase();
    else if (const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(object))
      pointer = deref->getSubExpr();
    PendingSite &site = addSite(core::SiteKind::PtrArith, unary,
                                FacetSet{.spatial = true}, ctx);
    site.info.operand = pointer;
    site.info.spatialSystemApi = required->shapeFromSystemHeader();
    return;
  }
  if (unary.isIncrementDecrementOp() && unary.getType()->isPointerType()) {
    // The new value is stored back into the operand, and the result flows
    // into the parent: either can be a required position.
    const KindEntry *slot = slotKind(sub);
    const KindEntry *kind = slot != nullptr ? slot : required;
    walkExpr(sub, ctx);
    if (kind == nullptr)
      return;
    PendingSite &site = addSite(core::SiteKind::PtrArith, unary,
                                FacetSet{.spatial = true}, ctx);
    site.info.operand = sub;
    site.info.spatialSystemApi = kind->shapeFromSystemHeader();
    return;
  }
  walkExpr(sub, ctx);
}

void Walker::walkBinary(const clang::BinaryOperator &binary, Context ctx,
                        Use use, const KindEntry *required) {
  const clang::Expr *lhs = binary.getLHS();
  const clang::Expr *rhs = binary.getRHS();
  const bool pointer = binary.getType()->isPointerType();
  switch (binary.getOpcode()) {
  case clang::BO_Assign: {
    const KindEntry *slot = slotKind(lhs);
    walkExpr(lhs, ctx);
    walkExpr(rhs, ctx, Use::Value, slot);
    if (slot != nullptr)
      addStore(binary, *rhs, *slot, ctx);
    return;
  }
  case clang::BO_AddAssign:
  case clang::BO_SubAssign:
  case clang::BO_Add:
  case clang::BO_Sub: {
    walkExpr(lhs, ctx);
    walkExpr(rhs, ctx);
    if (!pointer)
      return;
    const KindEntry *slot =
        binary.isCompoundAssignmentOp() ? slotKind(lhs) : nullptr;
    const KindEntry *kind = slot != nullptr ? slot : required;
    if (kind == nullptr)
      return;
    const clang::Expr *operand = lhs->getType()->isPointerType() ? lhs : rhs;
    PendingSite &site = addSite(core::SiteKind::PtrArith, binary,
                                FacetSet{.spatial = true}, ctx);
    site.info.operand = operand;
    site.info.spatialSystemApi = kind->shapeFromSystemHeader();
    return;
  }
  case clang::BO_Comma:
    walkExpr(lhs, ctx);
    walkExpr(rhs, ctx, use, required);
    return;
  default:
    walkExpr(lhs, ctx);
    walkExpr(rhs, ctx);
    return;
  }
}

void Walker::walkDeref(const clang::UnaryOperator &deref, Context ctx,
                       Use use) {
  const clang::Expr *sub = deref.getSubExpr();
  // `(*fp)(...)` belongs to the call; `&*p` is `p` (§2.1).
  if (deref.getType()->isFunctionType() || use == Use::AddressOf) {
    walkExpr(sub, ctx);
    return;
  }
  const clang::Expr *pointer = sub;
  const clang::Expr *index = nullptr;
  const bool offset = isPointerOffset(sub);
  if (offset) {
    const auto *binary = llvm::cast<clang::BinaryOperator>(sub->IgnoreParens());
    const bool lhsPointer = binary->getLHS()->getType()->isPointerType();
    pointer = lhsPointer ? binary->getLHS() : binary->getRHS();
    if (binary->getOpcode() == clang::BO_Add)
      index = lhsPointer ? binary->getRHS() : binary->getLHS();
  }
  core::SiteKind kind = offset ? core::SiteKind::Index : core::SiteKind::Deref;
  FacetSet facets{.spatial = true, .null = true, .temporal = true};
  if (!offset && use == Use::Decay)
    facets = FacetSet{.null = true};
  if (hasRawOrigin(pointer))
    kind = core::SiteKind::Raw;
  applyOperandRules(facets, pointer);
  walkExpr(sub, ctx);
  PendingSite &site = addSite(kind, deref, facets, ctx);
  site.info.operand = pointer;
  site.info.index = index;
  if (!facets.spatial || (offset && index == nullptr))
    return;
  if (const clang::Expr *array = decayedArray(pointer))
    site.info.provenByType = index == nullptr && !isFlexible(*array) &&
                             constantBound(*array).value_or(0) >= 1;
  else if (!offset && isAddressOfObject(pointer))
    site.info.provenByType = true;
}

void Walker::walkSubscript(const clang::ArraySubscriptExpr &subscript,
                           Context ctx, Use use) {
  const clang::Expr *base = subscript.getBase();
  const clang::Expr *index = subscript.getIdx();
  const clang::Expr *array = decayedArray(base);
  const auto walkOperands = [&] {
    if (array != nullptr)
      walkExpr(array, ctx, Use::Decay);
    else
      walkExpr(base, ctx);
    walkExpr(index, ctx);
  };
  // `&p[i]` is `p + i`: a PtrArith site, and only in a required position.
  if (use == Use::AddressOf) {
    walkOperands();
    return;
  }
  const bool zero = array == nullptr && isZeroConstant(index, context);
  core::SiteKind kind = zero ? core::SiteKind::Deref : core::SiteKind::Index;
  FacetSet facets{.spatial = true, .null = true, .temporal = true};
  if (zero && use == Use::Decay)
    facets = FacetSet{.null = true};
  if (array != nullptr)
    applyOperandRules(facets, base);
  else if (hasRawOrigin(base))
    kind = core::SiteKind::Raw;
  walkOperands();
  PendingSite &site = addSite(kind, subscript, facets, ctx);
  site.info.operand = base;
  site.info.index = zero ? nullptr : index;
  if (!facets.spatial)
    return;
  if (array != nullptr) {
    const auto bound =
        isFlexible(*array) ? std::nullopt : constantBound(*array);
    const auto at = index->getIntegerConstantExpr(context);
    site.info.provenByType = bound && at && at->isNonNegative() &&
                             at->getActiveBits() < 63 &&
                             at->getExtValue() < *bound;
  }
}

void Walker::walkMember(const clang::MemberExpr &member, Context ctx, Use use) {
  const clang::Expr *base = member.getBase();
  if (!member.isArrow()) {
    walkExpr(base, ctx, use);
    return;
  }
  // `&((T *)0)->f`, the old `offsetof` idiom, accesses nothing (§2.1).
  if (use != Use::Value && isNullConstant(base->IgnoreParenCasts())) {
    walkExpr(base, ctx);
    return;
  }
  FacetSet facets =
      use == Use::Value
          ? FacetSet{.spatial = true, .null = true, .temporal = true}
          : FacetSet{.null = true};
  applyOperandRules(facets, base);
  const core::SiteKind kind =
      hasRawOrigin(base) ? core::SiteKind::Raw : core::SiteKind::Deref;
  walkExpr(base, ctx);
  PendingSite &site = addSite(kind, member, facets, ctx);
  site.info.operand = base;
  if (!facets.spatial)
    return;
  if (const clang::Expr *array = decayedArray(base))
    site.info.provenByType =
        !isFlexible(*array) && constantBound(*array).value_or(0) >= 1;
  else if (isAddressOfObject(base))
    site.info.provenByType = true;
}

void Walker::walkCast(const clang::CastExpr &cast, Context ctx, Use use,
                      const KindEntry *required) {
  const clang::Expr *sub = cast.getSubExpr();
  // A cast to a variably modified type evaluates the type's sizes.
  if (const auto *written = llvm::dyn_cast<clang::ExplicitCastExpr>(&cast))
    walkVariablyModified(written->getTypeAsWritten(), ctx);
  switch (cast.getCastKind()) {
  case clang::CK_ArrayToPointerDecay:
    walkExpr(sub, ctx, Use::Decay);
    return;
  case clang::CK_IntegralToPointer: {
    // A null pointer constant converts with CK_NullToPointer instead.
    walkExpr(sub, ctx);
    PendingSite &site =
        addSite(core::SiteKind::IntToPtr, cast,
                FacetSet{.spatial = true, .temporal = true}, ctx);
    site.info.operand = &cast;
    return;
  }
  case clang::CK_BitCast:
  case clang::CK_NoOp:
    if (required != nullptr && cast.getType()->isPointerType() &&
        sub->getType()->isPointerType() &&
        isWidening(sub->getType(), cast.getType())) {
      walkExpr(sub, ctx);
      PendingSite &site =
          addSite(core::SiteKind::Cast, cast, FacetSet{.spatial = true}, ctx);
      site.info.operand = sub;
      site.info.spatialSystemApi = required->shapeFromSystemHeader();
      return;
    }
    // A conversion that is not widening passes the value, and its required
    // position, through.
    walkExpr(sub, ctx, cast.getCastKind() == clang::CK_NoOp ? use : Use::Value,
             required);
    return;
  default:
    walkExpr(sub, ctx);
    return;
  }
}

/// A compiler builtin that is not a library function (`__builtin_clz`,
/// `__builtin_constant_p`). The `LibrarySpec` lists every builtin whose
/// arguments are pointers or which changes control flow; one without a row
/// is lowered to plain instructions and calls nothing, so it is no site.
static bool isCompilerBuiltin(const clang::FunctionDecl &callee,
                              const clang::ASTContext &context) {
  const unsigned id = callee.getBuiltinID();
  return id != 0 && !context.BuiltinInfo.isLibFunction(id);
}

/// Builtins whose arguments are not evaluated.
static bool hasUnevaluatedArguments(const clang::FunctionDecl &callee) {
  const unsigned id = callee.getBuiltinID();
  return id == clang::Builtin::BI__builtin_constant_p ||
         id == clang::Builtin::BI__builtin_object_size ||
         id == clang::Builtin::BI__builtin_dynamic_object_size ||
         id == clang::Builtin::BI__builtin_classify_type;
}

const clang::FunctionDecl *Walker::calleeOf(const clang::CallExpr &call) const {
  if (const clang::FunctionDecl *direct = call.getDirectCallee())
    return direct;
  if (slots == nullptr || solution == nullptr)
    return nullptr;
  const auto slot = slots->calleeSlot(call);
  if (!slot)
    return nullptr;
  const core::CallResolution resolution = solution->resolveCall(*slot);
  // Closed on that one target, or open with it as its only known one: the
  // row says which arguments the call needs, and the engine decides those
  // needs `unresolved(callback)` when the slot is open (§9.3).
  if ((resolution.kind != core::IndirectCallKind::ClosedSingle &&
       resolution.kind != core::IndirectCallKind::OpenKnown) ||
      resolution.targets.size() != 1)
    return nullptr;
  const clang::FunctionDecl *target =
      slots->function(resolution.targets.front());
  if (target == nullptr)
    return nullptr;
  const auto match = governingLibraryEntry(*target, library);
  // Only what the row needs behind its arguments follows the slot. What it
  // allocates or releases decides ownership and §11's zero-initialisation,
  // which are the syntactic call's business (gate G8's oracle takes the
  // address of an allocator and expects its wrapper).
  if (!match || match->entry->allocates() || match->entry->releases())
    return nullptr;
  return target;
}

void Walker::walkCall(const clang::CallExpr &call, Context ctx) {
  const clang::FunctionDecl *callee = calleeOf(call);
  if (callee != nullptr && isReturnsTwice(*callee, library))
    callsSetjmp = true;
  const bool governed =
      callee != nullptr && governingLibraryEntry(*callee, library).has_value();
  walkExpr(call.getCallee(), ctx);
  if (callee == nullptr || !hasUnevaluatedArguments(*callee)) {
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      // §7.4: an argument for a parameter with a declared kind is a
      // required position. A LibrarySpec row checks its own arguments.
      // Inferred and default kinds are not: `hasDeclaredShape`.
      const KindEntry *param =
          callee != nullptr && !governed ? kinds.param(*callee, i) : nullptr;
      walkExpr(call.getArg(i), ctx, Use::Value,
               param != nullptr && param->hasDeclaredShape() ? param : nullptr);
    }
  }
  addCallSites(call, ctx);
}

std::optional<SourceTerm>
Walker::libraryTerm(const core::LibTerm &term, const clang::CallExpr &call,
                    const core::LibraryMatch &match) const {
  const auto argument = [&](unsigned rowArg) -> const clang::Expr * {
    const int index = match.callArgument(rowArg);
    if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
      return nullptr;
    return call.getArg(static_cast<unsigned>(index));
  };
  const auto operand = [&](std::size_t i) -> std::optional<SourceTerm> {
    if (i >= term.operands.size())
      return std::nullopt;
    return libraryTerm(term.operands[i], call, match);
  };
  switch (term.kind) {
  case core::LibTerm::Kind::Constant:
    return SourceTerm::ofConstant(term.value);
  case core::LibTerm::Kind::Argument:
    if (const clang::Expr *arg = argument(term.arg))
      return SourceTerm::ofExpr(*arg);
    return std::nullopt;
  case core::LibTerm::Kind::StringLength:
    if (const clang::Expr *arg = argument(term.arg))
      return SourceTerm::strLen(SourceTerm::ofExpr(*arg));
    return std::nullopt;
  case core::LibTerm::Kind::Product:
  case core::LibTerm::Kind::Sum: {
    auto lhs = operand(0);
    auto rhs = operand(1);
    if (!lhs || !rhs)
      return std::nullopt;
    return term.kind == core::LibTerm::Kind::Product
               ? SourceTerm::mul(std::move(*lhs), std::move(*rhs))
               : SourceTerm::add(std::move(*lhs), std::move(*rhs));
  }
  case core::LibTerm::Kind::Difference: {
    auto lhs = operand(0);
    if (!lhs)
      return std::nullopt;
    return SourceTerm::sub(std::move(*lhs), SourceTerm::ofConstant(term.value));
  }
  case core::LibTerm::Kind::Quotient: {
    auto lhs = operand(0);
    if (!lhs)
      return std::nullopt;
    return SourceTerm::div(std::move(*lhs), term.value);
  }
  // `fmtlen` is never a check term (§8.1); a macro's value and `min` have
  // no C spelling at the call.
  case core::LibTerm::Kind::FormatLength:
  case core::LibTerm::Kind::Macro:
  case core::LibTerm::Kind::Min:
    return std::nullopt;
  }
  return std::nullopt;
}

/// The function a `WEAVEC_ASSUME` calls (RFC 0012).
static bool isAssumeFunction(const clang::FunctionDecl &callee) {
  return llvm::any_of(callee.redecls(), [](const clang::FunctionDecl *redecl) {
    return getAnnotations(*redecl).assume;
  });
}

/// The callee operand of an indirect call, through `*` and conversions.
static const clang::Expr *indirectCallee(const clang::CallExpr &call) {
  const clang::Expr *callee = call.getCallee();
  for (unsigned depth = 0; depth < 16; ++depth) {
    callee = callee->IgnoreParenImpCasts();
    const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(callee);
    if (deref == nullptr || deref->getOpcode() != clang::UO_Deref)
      break;
    callee = deref->getSubExpr();
  }
  return callee;
}

void Walker::addCallSites(const clang::CallExpr &call, Context ctx) {
  const clang::FunctionDecl *callee = calleeOf(call);
  const std::string name =
      callee != nullptr ? callee->getNameAsString() : std::string();
  if (callee != nullptr && isAssumeFunction(*callee)) {
    PendingSite &site =
        addSite(core::SiteKind::Assume, call, FacetSet{.assertion = true}, ctx);
    site.info.callee = callee;
    site.callee = name;
    return;
  }
  const auto match = callee != nullptr ? governingLibraryEntry(*callee, library)
                                       : std::nullopt;
  if (!match && callee != nullptr && isCompilerBuiltin(*callee, context))
    return;
  std::optional<unsigned> released;
  if (match && match->entry->releases()) {
    for (unsigned i = 0; i < call.getNumArgs() && !released; ++i) {
      const core::LibraryParam *param = match->param(i);
      if (param != nullptr &&
          (param->effect == core::LibraryParam::Effect::Release ||
           param->effect == core::LibraryParam::Effect::Realloc))
        released = i;
    }
  }

  FacetSet facets;
  core::SiteKind kind = core::SiteKind::Call;
  std::vector<ArgumentNeed> needs;
  std::vector<SiteInfo::DeclaredShape> shapes;
  std::vector<std::uint8_t> reliance;
  const clang::Expr *operand = nullptr;
  bool nullSystemApi = false;
  bool spatialSystemApi = false;
  if (released) {
    // §2.1 Release: the start of an allocation, non-null when the row says
    // so, and live (of the releasing family).
    const clang::Expr *arg = call.getArg(*released);
    const core::LibraryParam &param = *match->param(*released);
    kind = hasRawOrigin(arg) ? core::SiteKind::Raw : core::SiteKind::Release;
    facets = FacetSet{.spatial = true,
                      .null = param.null == core::LibraryParam::Null::Forbidden,
                      .temporal = true};
    applyOperandRules(facets, arg);
    if (facets.null)
      needs.push_back(ArgumentNeed{
          .argument = static_cast<std::uint8_t>(*released), .nonnull = true});
    operand = arg;
  } else if (match) {
    // §2.1 LibCall: the facets the row's requirements name.
    kind = core::SiteKind::LibCall;
    for (unsigned i = 0; i < call.getNumArgs(); ++i) {
      const clang::Expr *arg = call.getArg(i);
      const core::LibraryParam *param = match->param(i);
      if (param == nullptr) {
        // Variadic arguments are borrowed for the call (§8).
        if (match->entry->format && arg->getType()->isPointerType()) {
          FacetSet borrowed{.temporal = true};
          applyOperandRules(borrowed, arg);
          facets.temporal = facets.temporal || borrowed.temporal;
        }
        continue;
      }
      if (param->callback)
        facets.temporal = true;
      if (param->type != core::LibraryParam::Type::Pointer)
        continue;
      const bool accessed = param->access != core::LibraryParam::Access::None ||
                            param->bytes || param->count || param->string;
      FacetSet argument{
          .spatial = accessed,
          .null = param->null != core::LibraryParam::Null::Allowed,
          .temporal = accessed ||
                      param->effect != core::LibraryParam::Effect::Borrow ||
                      param->out.has_value()};
      applyOperandRules(argument, arg);
      facets.spatial = facets.spatial || argument.spatial;
      facets.temporal = facets.temporal || argument.temporal;
      if (!argument.null)
        continue;
      facets.null = true;
      ArgumentNeed need{.argument = static_cast<std::uint8_t>(i),
                        .nonnull = true};
      if (param->null == core::LibraryParam::Null::AllowedIfZero) {
        need.allowedIfZero = true;
        if (param->zeroTerm)
          need.unlessZero = libraryTerm(*param->zeroTerm, call, *match);
      }
      needs.push_back(std::move(need));
    }
    if (!match->entry->disjoint.empty())
      facets.spatial = true;
    if (match->entry->hasCallback())
      facets.temporal = true;
  } else {
    // §2.1 Call: the boundary, the callee operand of an indirect call, and
    // each argument with a declared requirement.
    facets.temporal = true;
    if (callee == nullptr) {
      facets.null = true;
      operand = indirectCallee(call);
    }
    bool spatialOnlySystem = true;
    bool nullOnlySystem = callee != nullptr;
    for (unsigned i = 0; callee != nullptr && i < call.getNumArgs(); ++i) {
      const KindEntry *param = kinds.param(*callee, i);
      if (param == nullptr)
        continue;
      const clang::Expr *arg = call.getArg(i);
      if (param->hasDeclaredShape()) {
        facets.spatial = true;
        spatialOnlySystem = spatialOnlySystem && param->shapeFromSystemHeader();
        if (!param->shapeFromSystemHeader())
          shapes.push_back(SiteInfo::DeclaredShape{
              .argument = static_cast<std::uint8_t>(i),
              .kind = param->kind,
              .pointee =
                  i < callee->getNumParams()
                      ? callee->getParamDecl(i)->getType()->getPointeeType()
                      : clang::QualType()});
      } else if (param->hasEnforcedRequirement()) {
        // §7.5: a static callee's inferred requirement is checked at every
        // direct call, under its guard.
        facets.spatial = true;
        spatialOnlySystem = false;
        if (auto need = inferredNullNeed(*param, call, i);
            need && !param->declaresNonnull() && decayedArray(arg) == nullptr) {
          facets.null = true;
          nullOnlySystem = false;
          needs.push_back(std::move(*need));
        }
      } else if (param->enforcement == RequirementEnforcement::CallerContract &&
                 llvm::any_of(param->mustAccess,
                              [](const MustAccessRequirement &requirement) {
                                return requirement.kind.shape !=
                                       core::PointerShape::Single;
                              })) {
        // §7.5: an exported callee's extent beyond Single is its callers'
        // contract, verified at each call the unit makes (the link step
        // verifies the others). Its nullability stays the body's.
        facets.spatial = true;
        spatialOnlySystem = false;
      }
      if (!param->hasDeclaredShape() && !param->hasEnforcedRequirement() &&
          param->reliesOnSingle && inferred != nullptr &&
          arg->getType()->isPointerType() &&
          !inferred->argumentIsSingleValid(call, i)) {
        // §7.3: a value that may be a cursor, passed where the callee relies
        // on the Single default, gets the Call site's row.
        facets.spatial = true;
        spatialOnlySystem = false;
        reliance.push_back(static_cast<std::uint8_t>(i));
      }
      if (!param->declaresNonnull() || !arg->getType()->isPointerType() ||
          decayedArray(arg) != nullptr)
        continue;
      facets.null = true;
      nullOnlySystem = nullOnlySystem && param->nullabilityFromSystemHeader();
      needs.push_back(
          ArgumentNeed{.argument = static_cast<std::uint8_t>(i),
                       .nonnull = true,
                       .systemApi = param->nullabilityFromSystemHeader()});
    }
    nullSystemApi = facets.null && nullOnlySystem;
    spatialSystemApi = facets.spatial && spatialOnlySystem;
  }

  PendingSite &site = addSite(kind, call, facets, ctx);
  if (kind == core::SiteKind::Call)
    site.info.boundary = core::Boundary::Call;
  site.info.callee = callee;
  site.info.library = match;
  site.info.operand = operand;
  site.info.arguments = std::move(needs);
  site.info.declaredShapes = std::move(shapes);
  site.info.reliance = std::move(reliance);
  site.info.nullSystemApi = nullSystemApi;
  site.info.spatialSystemApi = spatialSystemApi;
  site.callee = name;

  if (callDoesNotReturn(call, library)) {
    PendingSite &exit =
        addSite(core::SiteKind::Call, call, FacetSet{.temporal = true}, ctx);
    exit.info.boundary = core::Boundary::Exit;
    exit.info.callee = callee;
    exit.callee = name;
  }
}

void Walker::addStore(const clang::Stmt &store, const clang::Expr &value,
                      const KindEntry &slot, Context ctx) {
  // §2.1 Cast: a pointer stored into a slot with a declared kind, unless
  // the value carries the obligation itself (a PtrArith or widening Cast
  // site) or is null, which satisfies every shape.
  if (!value.getType()->isPointerType() || carriesOwnObligation(&value) ||
      isNullConstant(&value))
    return;
  PendingSite &site =
      addSite(core::SiteKind::Cast, store, FacetSet{.spatial = true}, ctx);
  site.info.operand = &value;
  site.info.spatialSystemApi = slot.shapeFromSystemHeader();
}

void Walker::walkInitList(const clang::InitListExpr &list, Context ctx) {
  // CodeGen reads the semantic form (§2.1).
  const clang::InitListExpr *semantic =
      list.isSemanticForm() ? &list : list.getSemanticForm();
  if (semantic == nullptr)
    semantic = &list;
  if (semantic != &list && !visited.insert(semantic).second)
    return;
  const clang::RecordDecl *record = semantic->getType()->getAsRecordDecl();
  const auto store = [&](const clang::FieldDecl *field,
                         const clang::Expr *init) {
    const KindEntry *slot = field != nullptr ? kinds.field(*field) : nullptr;
    if (slot != nullptr && !slot->hasDeclaredShape())
      slot = nullptr;
    walkExpr(init, ctx, Use::Value, slot);
    if (slot != nullptr && init != nullptr)
      addStore(*init, *init, *slot, ctx);
  };
  if (record == nullptr) {
    for (const clang::Expr *init : semantic->inits())
      walkExpr(init, ctx);
    return;
  }
  if (record->isUnion()) {
    for (const clang::Expr *init : semantic->inits())
      store(semantic->getInitializedFieldInUnion(), init);
    return;
  }
  auto field = record->field_begin();
  for (const clang::Expr *init : semantic->inits()) {
    while (field != record->field_end() && field->isUnnamedBitField())
      ++field;
    const clang::FieldDecl *current =
        field != record->field_end() ? *field : nullptr;
    if (field != record->field_end())
      ++field;
    store(current, init);
  }
}

void Walker::walkDecl(const clang::Decl &decl, Context ctx) {
  if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(&decl)) {
    walkVariablyModified(variable->getType(), ctx);
    if (const clang::Expr *init = variable->getInit())
      walkExpr(init, ctx);
    return;
  }
  if (const auto *alias = llvm::dyn_cast<clang::TypedefNameDecl>(&decl))
    walkVariablyModified(alias->getUnderlyingType(), ctx);
}

void Walker::walkVariablyModified(clang::QualType type, Context ctx) {
  // The size expressions of a variably modified type are evaluated where
  // the type is declared, cast to, or taken the `sizeof` of.
  for (unsigned depth = 0;
       depth < 32 && !type.isNull() && type->isVariablyModifiedType();
       ++depth) {
    if (const auto *variable = context.getAsVariableArrayType(type)) {
      walkExpr(variable->getSizeExpr(), ctx);
      type = variable->getElementType();
    } else if (const auto *array = context.getAsArrayType(type)) {
      type = array->getElementType();
    } else if (const auto *pointer = type->getAs<clang::PointerType>()) {
      type = pointer->getPointeeType();
    } else {
      return;
    }
  }
}

const KindEntry *Walker::slotKind(const clang::Expr *lvalue) const {
  const clang::Expr *expr = lvalue->IgnoreParenImpCasts();
  const KindEntry *entry = nullptr;
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl()))
      entry = kinds.field(*field);
  } else if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
        variable != nullptr && variable->hasGlobalStorage())
      entry = kinds.variable(*variable);
  }
  return entry != nullptr && entry->hasDeclaredShape() ? entry : nullptr;
}

bool Walker::hasRawOrigin(const clang::Expr *pointer, unsigned depth) const {
  if (pointer == nullptr || depth > 16)
    return false;
  const clang::Expr *expr = pointer->IgnoreParens();
  // A pointer converted from an integer is not raw (RFC 0033 §2): only a
  // declaration makes one.
  while (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr)) {
    if (cast->getCastKind() != clang::CK_LValueToRValue &&
        cast->getCastKind() != clang::CK_NoOp &&
        cast->getCastKind() != clang::CK_BitCast)
      return false;
    expr = cast->getSubExpr()->IgnoreParens();
  }
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (const auto *param = llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl());
        param != nullptr && param->getDeclContext() == &function) {
      const unsigned index = param->getFunctionScopeIndex();
      return index < signature.params.size() && signature.params[index].raw;
    }
    return getAnnotations(*ref->getDecl()).raw;
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
    if (getAnnotations(*member->getMemberDecl()).raw)
      return true;
    // Loaded through a raw pointer.
    return member->isArrow() && hasRawOrigin(member->getBase(), depth + 1);
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr))
    return (unary->getOpcode() == clang::UO_Deref ||
            unary->isIncrementDecrementOp()) &&
           hasRawOrigin(unary->getSubExpr(), depth + 1);
  if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expr))
    return decayedArray(subscript->getBase()) == nullptr &&
           hasRawOrigin(subscript->getBase(), depth + 1);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(expr)) {
    if (isPointerOffset(binary))
      return hasRawOrigin(binary->getLHS()->getType()->isPointerType()
                              ? binary->getLHS()
                              : binary->getRHS(),
                          depth + 1);
    if (binary->getOpcode() == clang::BO_Comma ||
        binary->getOpcode() == clang::BO_Assign)
      return hasRawOrigin(binary->getRHS(), depth + 1);
    if (binary->isCompoundAssignmentOp())
      return hasRawOrigin(binary->getLHS(), depth + 1);
    return false;
  }
  if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(expr))
    return hasRawOrigin(choice->getTrueExpr(), depth + 1) &&
           hasRawOrigin(choice->getFalseExpr(), depth + 1);
  // Handed out as raw by a callee.
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr))
    if (const clang::FunctionDecl *callee = call->getDirectCallee())
      return collectAnnotations(*callee).result.raw;
  return false;
}

bool Walker::isWidening(clang::QualType from, clang::QualType to) const {
  // §2.1 Cast: `U` is a complete object type wider than `T`; `void` and
  // character types count as one byte.
  const clang::QualType source = from->getPointeeType();
  const clang::QualType target = to->getPointeeType();
  if (source.isNull() || target.isNull() || source->isFunctionType() ||
      target->isFunctionType() || target->isVoidType() ||
      target->isIncompleteType() || !target->isConstantSizeType())
    return false;
  const auto width = [this](clang::QualType type) -> std::int64_t {
    if (type->isVoidType() || type->isCharType() || type->isIncompleteType() ||
        !type->isConstantSizeType())
      return 1;
    return context.getTypeSizeInChars(type).getQuantity();
  };
  return width(target) > width(source);
}

bool Walker::carriesOwnObligation(const clang::Expr *value) const {
  const clang::Expr *expr = value;
  for (unsigned depth = 0; expr != nullptr && depth < 32; ++depth) {
    expr = expr->IgnoreParens();
    if (isPointerArithmetic(expr))
      return true;
    const auto *cast = llvm::dyn_cast<clang::CastExpr>(expr);
    if (cast == nullptr || (cast->getCastKind() != clang::CK_BitCast &&
                            cast->getCastKind() != clang::CK_NoOp))
      return false;
    const clang::Expr *sub = cast->getSubExpr();
    if (cast->getType()->isPointerType() && sub->getType()->isPointerType() &&
        isWidening(sub->getType(), cast->getType()))
      return true;
    expr = sub;
  }
  return false;
}

bool Walker::isNullConstant(const clang::Expr *value) const {
  if (llvm::isa<clang::ImplicitValueInitExpr>(value->IgnoreParens()))
    return true;
  return value->isNullPointerConstant(
             context, clang::Expr::NPC_ValueDependentIsNotNull) !=
         clang::Expr::NPCK_NotNull;
}

bool Walker::isFlexible(const clang::Expr &array) const {
  // §7.4: at the default -fstrict-flex-arrays level every trailing array
  // member is flexible, whatever its declared bound.
  return array.isFlexibleArrayMemberLike(
      context, context.getLangOpts().getStrictFlexArraysLevel());
}

std::optional<std::int64_t>
Walker::constantBound(const clang::Expr &array) const {
  const auto *constant = context.getAsConstantArrayType(array.getType());
  if (constant == nullptr)
    return std::nullopt;
  const auto size = constant->getSize().tryZExtValue();
  if (!size || *size > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max()))
    return std::nullopt;
  return static_cast<std::int64_t>(*size);
}

//===----------------------------------------------------------------------===//
// SiteCollector
//===----------------------------------------------------------------------===//

bool SiteCollector::isEmitted(const clang::FunctionDecl &function) {
  if (!function.doesThisDeclarationHaveABody() || function.isInvalidDecl())
    return false;
  // Functions defined in system headers are never analysed or instrumented
  // (§5.6), and are not in the ledger (§2.6); nor is the check prelude,
  // which `weavec-cc` appends to the predefines (§10.2).
  const clang::SourceManager &sm = context.getSourceManager();
  if (sm.isInSystemHeader(function.getLocation()) ||
      sm.isWrittenInBuiltinFile(sm.getExpansionLoc(function.getLocation())))
    return false;
  if (context.DeclMustBeEmitted(&function))
    return true;
  // C99 `inline` and `gnu_inline` definitions: `available_externally`,
  // inlined at -O1 and above.
  if (context.GetGVALinkageForFunction(&function) ==
      clang::GVA_AvailableExternally)
    return true;
  // Internal-linkage definitions that are used.
  return !function.isExternallyVisible() && function.isUsed();
}

/// Function definitions in declaration (source) order.
static void collectDefinitions(const clang::DeclContext &dc,
                               std::vector<const clang::FunctionDecl *> &out) {
  for (const clang::Decl *decl : dc.decls()) {
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      if (function->doesThisDeclarationHaveABody())
        out.push_back(function);
      continue;
    }
    if (llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(decl))
      collectDefinitions(*llvm::cast<clang::DeclContext>(decl), out);
  }
}

std::vector<const clang::FunctionDecl *> SiteCollector::emittedFunctions() {
  std::vector<const clang::FunctionDecl *> definitions;
  collectDefinitions(*context.getTranslationUnitDecl(), definitions);
  std::vector<const clang::FunctionDecl *> emitted;
  for (const clang::FunctionDecl *function : definitions)
    if (isEmitted(*function))
      emitted.push_back(function);
  return emitted;
}

SiteIndex SiteCollector::collect() {
  const clang::SourceManager &sm = context.getSourceManager();
  SiteIndex index;
  for (const clang::FunctionDecl *function : emittedFunctions()) {
    const auto functionIndex =
        static_cast<std::uint32_t>(index.functionList.size());
    Walker walker(context, kinds, library, *function, inferred, slots,
                  solution);
    walker.walkBody();
    std::vector<PendingSite> &pending = walker.sites;
    // §2.1: ordinals in source order; sites that begin at the same place
    // keep walk order, which puts inner operations first and a call's exit
    // after the call.
    std::ranges::stable_sort(
        pending, [&sm](const PendingSite &a, const PendingSite &b) {
          if (a.location != b.location &&
              sm.isBeforeInTranslationUnit(a.location, b.location))
            return true;
          if (a.location != b.location &&
              sm.isBeforeInTranslationUnit(b.location, a.location))
            return false;
          return a.sequence < b.sequence;
        });

    SiteIndex::FunctionSites sites;
    sites.decl = function;
    sites.index = functionIndex;
    sites.unsafe = collectAnnotations(*function).unsafe;
    sites.callsSetjmp = walker.callsSetjmp;
    const clang::CharSourceRange range =
        sm.getExpansionRange(function->getSourceRange());
    sites.begin = range.getBegin();
    sites.end = range.getEnd();

    core::FunctionLedger row;
    row.name = function->getNameAsString();
    const core::SourceLocation where =
        toCoreLocation(sm, function->getLocation());
    row.file = where.file;
    row.line = where.line;
    row.sites.reserve(pending.size());
    sites.sites.reserve(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
      PendingSite &site = pending[i];
      const core::SiteId id{.function = functionIndex,
                            .ordinal = static_cast<std::uint32_t>(i)};
      site.info.id = id;
      core::Site &ledgerSite = row.sites.emplace_back();
      ledgerSite.ordinal = id.ordinal;
      ledgerSite.kind = site.info.kind;
      ledgerSite.location = toCoreLocation(sm, site.location);
      ledgerSite.text = std::move(site.text);
      ledgerSite.boundary = site.info.boundary;
      ledgerSite.callee = std::move(site.callee);
      if (site.facets.spatial)
        ledgerSite.addFacet(core::Facet::Spatial);
      if (site.facets.null)
        ledgerSite.addFacet(core::Facet::Null);
      if (site.facets.temporal)
        ledgerSite.addFacet(core::Facet::Temporal);
      if (site.facets.assertion)
        ledgerSite.addFacet(core::Facet::Assertion);
      index.byStmt[site.info.stmt].push_back(id);
      sites.sites.push_back(std::move(site.info));
    }
    index.byFunction[function->getCanonicalDecl()] = functionIndex;
    index.functionList.push_back(std::move(sites));
    index.rows.push_back(std::move(row));
  }
  return index;
}

} // namespace weavec::analysis
