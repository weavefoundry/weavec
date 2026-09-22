//===- CheckEmitter.cpp - Checks inserted through Sema (RFC 0030) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/CheckEmitter.h"

#include "weavec/Analysis/CheckPlanner.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Frontend/ZeroInit.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/Sema.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace weavec::frontend {

using HelperType = HelperSignature::Type;
using Entry = core::CheckPlanEntry;

/// The analysis hands the emitter `const` AST nodes: it only reads the tree.
/// The emitter owns it (§10.6) and rewrites it in place, so every node it is
/// given has to come back as a mutable one. That is the only place the const
/// is dropped.
template <typename Node>
static Node *mutableNode(const Node *node) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return const_cast<Node *>(node);
}

//===----------------------------------------------------------------------===//
// The helpers' signatures (§10.2, §10.9, §11)
//===----------------------------------------------------------------------===//

namespace {
constexpr HelperType V = HelperType::Void;
constexpr HelperType I = HelperType::Int;
constexpr HelperType LL = HelperType::LongLong;
constexpr HelperType ULL = HelperType::UnsignedLongLong;
constexpr HelperType Z = HelperType::Size;
constexpr HelperType VP = HelperType::VoidPointer;
constexpr HelperType CVP = HelperType::ConstVolatileVoidPointer;
constexpr HelperType CP = HelperType::CharPointer;
constexpr HelperType CCP = HelperType::ConstCharPointer;
constexpr HelperType CPP = HelperType::CharPointerPointer;
constexpr HelperType VPP = HelperType::VoidPointerPointer;
constexpr HelperType FP = HelperType::FunctionPointer;
constexpr HelperType AA = HelperType::AlignedAllocator;
constexpr HelperType PM = HelperType::PosixMemalignFunction;
} // namespace

// The check templates of both families, then the helpers only
// `__weavec_chk_*` has, the term helpers and the zero-initialisation
// wrappers. `reports`: report mode appends (file, line, column).
static constexpr std::array<HelperSignature, 45> Helpers{{
    {.name = "__weavec_chk_nonnull",
     .result = VP,
     .params = {CVP},
     .reports = true},
    {.name = "__weavec_chk_nonnull_n",
     .result = VP,
     .params = {CVP, ULL},
     .reports = true},
    {.name = "__weavec_chk_nonnull_fn",
     .result = FP,
     .params = {FP},
     .reports = true},
    {.name = "__weavec_chk_index",
     .result = ULL,
     .params = {ULL, ULL},
     .reports = true},
    {.name = "__weavec_chk_span",
     .result = VP,
     .params = {CVP, LL, CVP, ULL, ULL},
     .reports = true},
    {.name = "__weavec_chk_len",
     .result = ULL,
     .params = {ULL, ULL},
     .reports = true},
    {.name = "__weavec_chk_len_r",
     .result = I,
     .params = {I, ULL},
     .reports = true},
    {.name = "__weavec_chk_disjoint",
     .result = VP,
     .params = {CVP, CVP, ULL},
     .reports = true},
    {.name = "__weavec_chk_assert",
     .result = V,
     .params = {I},
     .reports = true},
    {.name = "__weavec_prv_nonnull",
     .result = VP,
     .params = {CVP},
     .reports = true},
    {.name = "__weavec_prv_nonnull_n",
     .result = VP,
     .params = {CVP, ULL},
     .reports = true},
    {.name = "__weavec_prv_nonnull_fn",
     .result = FP,
     .params = {FP},
     .reports = true},
    {.name = "__weavec_prv_index",
     .result = ULL,
     .params = {ULL, ULL},
     .reports = true},
    {.name = "__weavec_prv_span",
     .result = VP,
     .params = {CVP, LL, CVP, ULL, ULL},
     .reports = true},
    {.name = "__weavec_prv_len",
     .result = ULL,
     .params = {ULL, ULL},
     .reports = true},
    {.name = "__weavec_prv_len_r",
     .result = I,
     .params = {I, ULL},
     .reports = true},
    {.name = "__weavec_prv_disjoint",
     .result = VP,
     .params = {CVP, CVP, ULL},
     .reports = true},
    {.name = "__weavec_prv_assert",
     .result = V,
     .params = {I},
     .reports = true},
    {.name = "__weavec_chk_violation",
     .result = V,
     .params = {},
     .reports = true},
    {.name = "__weavec_strnlen",
     .result = ULL,
     .params = {CCP, ULL},
     .reports = true},
    {.name = "__weavec_need_s", .result = ULL, .params = {LL}},
    {.name = "__weavec_have_s", .result = ULL, .params = {LL}},
    {.name = "__weavec_need_add", .result = ULL, .params = {ULL, ULL}},
    {.name = "__weavec_need_sub", .result = ULL, .params = {ULL, ULL}},
    {.name = "__weavec_need_mul", .result = ULL, .params = {ULL, ULL}},
    {.name = "__weavec_have_add", .result = ULL, .params = {ULL, ULL}},
    {.name = "__weavec_have_sub", .result = ULL, .params = {ULL, ULL}},
    {.name = "__weavec_have_mul", .result = ULL, .params = {ULL, ULL}},
    {.name = "__weavec_zero_tail", .result = VP, .params = {VP, Z}},
    {.name = "__weavec_malloc_zero", .result = VP, .params = {Z}},
    {.name = "__weavec_calloc_zero", .result = VP, .params = {Z, Z}},
    {.name = "__weavec_realloc_zero", .result = VP, .params = {VP, Z}},
    {.name = "__weavec_reallocarray_zero", .result = VP, .params = {VP, Z, Z}},
    {.name = "__weavec_aligned_zero", .result = VP, .params = {AA, Z, Z}},
    {.name = "__weavec_posix_memalign_zero",
     .result = I,
     .params = {PM, VPP, Z, Z}},
    {.name = "__weavec_zero_string", .result = CP, .params = {CP}},
    {.name = "__weavec_strdup_zero", .result = CP, .params = {CCP}},
    {.name = "__weavec_strndup_zero", .result = CP, .params = {CCP, Z}},
    {.name = "__weavec_zero_line", .result = LL, .params = {LL, CPP}},
    {.name = "__weavec_malloc_zero_fn", .result = VP, .params = {Z}},
    {.name = "__weavec_calloc_zero_fn", .result = VP, .params = {Z, Z}},
    {.name = "__weavec_realloc_zero_fn", .result = VP, .params = {VP, Z}},
    {.name = "__weavec_reallocarray_zero_fn",
     .result = VP,
     .params = {VP, Z, Z}},
    {.name = "__weavec_strdup_zero_fn", .result = CP, .params = {CCP}},
    {.name = "__weavec_strndup_zero_fn", .result = CP, .params = {CCP, Z}},
}};

llvm::ArrayRef<HelperSignature> CheckEmitter::helperSignatures() {
  return Helpers;
}

const HelperSignature *findHelperSignature(llvm::StringRef name) {
  for (const HelperSignature &helper : Helpers)
    if (helper.name == name)
      return &helper;
  return nullptr;
}

// A `const ASTContext &` here would have to travel up through `typeOf` into
// the public `helperFunctionType`, and from there into every caller's
// translation unit.
// NOLINTNEXTLINE(misc-const-correctness)
static clang::QualType prototype(clang::ASTContext &context,
                                 clang::QualType result,
                                 llvm::ArrayRef<clang::QualType> params) {
  const clang::FunctionProtoType::ExtProtoInfo info;
  return context.getFunctionType(result, params, info);
}

static clang::QualType typeOf(clang::ASTContext &context, HelperType type) {
  switch (type) {
  case HelperType::Void:
    return context.VoidTy;
  case HelperType::Int:
    return context.IntTy;
  case HelperType::Unsigned:
    return context.UnsignedIntTy;
  case HelperType::LongLong:
    return context.LongLongTy;
  case HelperType::UnsignedLongLong:
    return context.UnsignedLongLongTy;
  case HelperType::Size:
    return context.getSizeType();
  case HelperType::VoidPointer:
    return context.VoidPtrTy;
  case HelperType::ConstVolatileVoidPointer:
    return context.getPointerType(context.getCVRQualifiedType(
        context.VoidTy,
        clang::Qualifiers::Const | clang::Qualifiers::Volatile));
  case HelperType::CharPointer:
    return context.getPointerType(context.CharTy);
  case HelperType::ConstCharPointer:
    return context.getPointerType(context.CharTy.withConst());
  case HelperType::CharPointerPointer:
    return context.getPointerType(context.getPointerType(context.CharTy));
  case HelperType::VoidPointerPointer:
    return context.getPointerType(context.VoidPtrTy);
  case HelperType::FunctionPointer:
    return context.getPointerType(prototype(context, context.VoidTy, {}));
  case HelperType::AlignedAllocator: {
    const clang::QualType size = context.getSizeType();
    return context.getPointerType(
        prototype(context, context.VoidPtrTy, {size, size}));
  }
  case HelperType::PosixMemalignFunction: {
    const clang::QualType size = context.getSizeType();
    return context.getPointerType(
        prototype(context, context.IntTy,
                  {context.getPointerType(context.VoidPtrTy), size, size}));
  }
  }
  return context.VoidTy;
}

clang::QualType helperFunctionType(clang::ASTContext &context,
                                   const HelperSignature &helper, bool report) {
  llvm::SmallVector<clang::QualType, 8> params;
  for (const HelperType param : helper.params) {
    if (param == HelperType::Void)
      break;
    params.push_back(typeOf(context, param));
  }
  if (report && helper.reports) {
    params.push_back(context.getPointerType(context.CharTy.withConst()));
    params.push_back(context.UnsignedIntTy);
    params.push_back(context.UnsignedIntTy);
  }
  return prototype(context, typeOf(context, helper.result), params);
}

//===----------------------------------------------------------------------===//
// The emitter
//===----------------------------------------------------------------------===//

namespace {

/// Which way a term's arithmetic fails closed (§10.2): a need saturates to
/// the maximum, a have (an extent) to zero.
enum class Direction : std::uint8_t { Need, Have };

/// What one site's rewrites share.
struct SiteContext {
  const analysis::SiteInfo *info = nullptr;
  const analysis::PlaceHandleTable *handles = nullptr;
  const clang::FunctionDecl *function = nullptr;
  clang::SourceLocation loc;
  /// Report mode: the site's file, line and column.
  core::SourceLocation where;
  /// The site's text, for the internal error.
  std::string text;
};

} // namespace

static Direction opposite(Direction direction) {
  return direction == Direction::Need ? Direction::Have : Direction::Need;
}

/// §10.4: the order of one site's rewrites. Operand, index and argument
/// wraps come first (on one operand `nonnull` innermost, then `index`),
/// then the replacements of the access or the call, then the checks
/// sequenced before the call.
static int phase(const Entry &entry) {
  switch (entry.placement) {
  case Entry::Placement::WrapOperand:
  case Entry::Placement::WrapIndex:
  case Entry::Placement::WrapArgument:
    return 0;
  case Entry::Placement::ReplaceAccess:
  case Entry::Placement::ReplaceCall:
    return 1;
  case Entry::Placement::BeforeCall:
    return 2;
  }
  return 2;
}

static int nesting(const Entry &entry) {
  if (entry.form == Entry::Form::Violation)
    return 3;
  switch (entry.kind) {
  case Entry::Template::Nonnull:
    return 0;
  case Entry::Template::Index:
    return 1;
  case Entry::Template::Span:
  case Entry::Template::Len:
  case Entry::Template::Disjoint:
  case Entry::Template::Assert:
    return 2;
  }
  return 2;
}

class CheckEmitter::Impl {
public:
  Impl(clang::Sema &s, CheckEmitterOptions o)
      : sema(s), context(s.getASTContext()), diags(s.getDiagnostics()),
        options(o), report(o.mode == core::ChecksMode::Report) {}

  bool emit(const core::CheckPlan &plan, const analysis::SiteIndex &sites,
            const analysis::PlaceHandleTable &handles,
            const core::UnitLedger *unit);
  bool lowerZeroInit(const ZeroInitPlan &plan);

  std::size_t inserted = 0;
  std::size_t lowered = 0;

private:
  clang::Sema &sema;
  clang::ASTContext &context;
  clang::DiagnosticsEngine &diags;
  CheckEmitterOptions options;
  bool report = false;

  //--- Declarations -----------------------------------------------------------

  llvm::StringMap<clang::FunctionDecl *> helperDecls;

  /// A function the unit declares by that name, most recent declaration.
  clang::FunctionDecl *lookupFunction(llvm::StringRef name);
  /// A prelude helper: the unit's definition, or an `extern` declaration
  /// built here (§10.9).
  clang::FunctionDecl *helper(llvm::StringRef name);
  /// A builtin such as `__builtin_memset`.
  clang::FunctionDecl *builtin(llvm::StringRef name, clang::SourceLocation loc);

  //--- The tree ---------------------------------------------------------------

  /// Child to parent, over the semantic form; a root maps to null.
  llvm::DenseMap<clang::Stmt *, clang::Stmt *> parents;
  /// A root to the declaration that holds it.
  llvm::DenseMap<const clang::Stmt *, clang::Decl *> roots;
  llvm::DenseMap<const clang::Decl *, bool> mapped;
  /// A logical position (an original node) to what stands there now, when
  /// it was wrapped or replaced.
  llvm::DenseMap<clang::Stmt *, clang::Stmt *> occupants;

  void mapOwner(const clang::Decl *owner);
  void mapChildren(clang::Stmt *node);
  /// Records the parents below a node just placed in the tree.
  void adopt(clang::Stmt *node);
  /// Puts `replacement` where `node` is now. False when `node` is not in a
  /// known slot.
  bool place(clang::Stmt *node, clang::Stmt *replacement);
  [[nodiscard]] clang::Stmt *occupant(clang::Stmt *logical) const {
    const auto found = occupants.find(logical);
    return found != occupants.end() ? found->second : logical;
  }
  /// Wraps the logical position `logical` with `wrapper`.
  bool wrapAt(clang::Stmt *logical, clang::Stmt *wrapper);
  /// Replaces the node `node` itself (wherever it is) with `replacement`.
  bool replaceNode(clang::Stmt *node, clang::Stmt *replacement);

  //--- Building ---------------------------------------------------------------

  /// Runs `build` as one tentative rewrite (§10.6); null when it failed.
  template <typename Build>
  clang::Stmt *attempt(Build &&build);
  static bool usable(const clang::ExprResult &result) {
    return result.isUsable() && !result.get()->containsErrors();
  }
  clang::Expr *paren(clang::Expr *expr, clang::SourceLocation loc) {
    return new (context) clang::ParenExpr(loc, loc, expr);
  }
  clang::Expr *literal(std::uint64_t value, clang::QualType type,
                       clang::SourceLocation loc);
  clang::Expr *castTo(clang::Expr *expr, clang::QualType type,
                      clang::CastKind kind);
  /// A call of the helper `name`; `site` adds report mode's location.
  clang::ExprResult callHelper(llvm::StringRef name,
                               llvm::MutableArrayRef<clang::Expr *> args,
                               clang::SourceLocation loc,
                               const SiteContext *site);
  clang::ExprResult comma(clang::Expr *lhs, clang::Expr *rhs,
                          clang::SourceLocation loc);

  /// §10.2: an extra operand of a check, as C.
  clang::Expr *term(const core::CheckTerm &term, Direction direction,
                    const SiteContext &site);
  clang::Expr *pointerTerm(const core::CheckTerm &term,
                           const SiteContext &site);
  clang::Expr *placeLvalue(const core::CheckTerm &term,
                           const SiteContext &site);

  //--- Rewrites ---------------------------------------------------------------

  bool apply(const Entry &entry, SiteContext &site);
  /// The check call of `entry` over `wrapped` (the operand, the index or the
  /// argument; null when the check wraps nothing).
  clang::ExprResult checkCall(const Entry &entry, clang::Expr *wrapped,
                              SiteContext &site);
  /// Wraps the logical node `target` with a check: `(T)chk(x)` for helpers
  /// that return their operand, `(chk(...), x)` for the others.
  bool wrapValue(const Entry &entry, clang::Stmt *target, SiteContext &site);
  bool wrapArgument(const Entry &entry, SiteContext &site);
  bool replaceAccess(const Entry &entry, SiteContext &site);
  bool beforeCall(const Entry &entry, SiteContext &site);
  bool replaceCall(const Entry &entry, SiteContext &site);

  //--- Zero-initialisation ----------------------------------------------------

  bool lowerOne(const ZeroInitRewrite &rewrite);
  /// A copy of a side-effect-free operand that `ZeroInit.cpp` found can be
  /// rebuilt.
  clang::Expr *rebuild(const clang::Expr *expr, clang::SourceLocation loc,
                       unsigned depth = 0);

  //--- Failure ----------------------------------------------------------------

  void internalError(clang::SourceLocation loc, llvm::StringRef text,
                     bool zeroInit);
};

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

clang::FunctionDecl *CheckEmitter::Impl::lookupFunction(llvm::StringRef name) {
  const clang::TranslationUnitDecl *unit = context.getTranslationUnitDecl();
  const clang::DeclarationName declName(&context.Idents.get(name));
  clang::FunctionDecl *found = nullptr;
  for (clang::NamedDecl *decl : unit->lookup(declName))
    if (auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl))
      found = function->getMostRecentDecl();
  return found;
}

clang::FunctionDecl *CheckEmitter::Impl::helper(llvm::StringRef name) {
  // Out of line, report helpers carry a `_report` suffix (§10.9).
  const HelperSignature *signature = findHelperSignature(name);
  std::string spelled = name.str();
  if (options.externalHelpers && report && signature != nullptr &&
      signature->reports)
    spelled += "_report";
  if (const auto cached = helperDecls.find(spelled);
      cached != helperDecls.end())
    return cached->second;
  clang::FunctionDecl *function = nullptr;
  if (!options.externalHelpers) {
    function = lookupFunction(spelled);
  } else if (signature != nullptr) {
    // `extern` declarations without bodies; libweavec_chk.a defines them.
    clang::TranslationUnitDecl *unit = context.getTranslationUnitDecl();
    const clang::QualType type =
        helperFunctionType(context, *signature, report);
    const clang::SourceLocation none;
    function = clang::FunctionDecl::Create(
        context, unit, none, none,
        clang::DeclarationName(&context.Idents.get(spelled)), type,
        context.getTrivialTypeSourceInfo(type), clang::SC_Extern);
    const auto *proto = type->castAs<clang::FunctionProtoType>();
    llvm::SmallVector<clang::ParmVarDecl *, 8> params;
    for (unsigned i = 0; i < proto->getNumParams(); ++i) {
      auto *param = clang::ParmVarDecl::Create(
          context, function, none, none, nullptr, proto->getParamType(i),
          context.getTrivialTypeSourceInfo(proto->getParamType(i)),
          clang::SC_None, nullptr);
      param->setScopeInfo(0, i);
      params.push_back(param);
    }
    function->setParams(params);
    function->setImplicit(true);
    unit->addDecl(function);
  }
  helperDecls[spelled] = function;
  return function;
}

clang::FunctionDecl *CheckEmitter::Impl::builtin(llvm::StringRef name,
                                                 clang::SourceLocation loc) {
  if (clang::FunctionDecl *declared = lookupFunction(name))
    return declared;
  // Declared as Sema declares a builtin on first use; Sema's own entry point
  // needs the translation unit's scope, which is gone at the end of the unit.
  const clang::IdentifierInfo &identifier = context.Idents.get(name);
  const unsigned id = identifier.getBuiltinID();
  if (id == 0)
    return nullptr;
  clang::ASTContext::GetBuiltinTypeError error = clang::ASTContext::GE_None;
  const clang::QualType type = context.GetBuiltinType(id, error);
  if (error != clang::ASTContext::GE_None || type.isNull())
    return nullptr;
  clang::TranslationUnitDecl *unit = context.getTranslationUnitDecl();
  auto *function = clang::FunctionDecl::Create(
      context, unit, loc, loc, clang::DeclarationName(&identifier), type,
      /*TInfo=*/nullptr, clang::SC_Extern);
  if (const auto *proto = type->getAs<clang::FunctionProtoType>()) {
    llvm::SmallVector<clang::ParmVarDecl *, 4> params;
    for (unsigned i = 0; i < proto->getNumParams(); ++i) {
      auto *param = clang::ParmVarDecl::Create(
          context, function, loc, loc, nullptr, proto->getParamType(i),
          /*TInfo=*/nullptr, clang::SC_None, nullptr);
      param->setScopeInfo(0, i);
      params.push_back(param);
    }
    function->setParams(params);
  }
  function->setImplicit(true);
  function->addAttr(clang::BuiltinAttr::CreateImplicit(context, id));
  unit->addDecl(function);
  return function;
}

//===----------------------------------------------------------------------===//
// The tree
//===----------------------------------------------------------------------===//

void CheckEmitter::Impl::mapOwner(const clang::Decl *owner) {
  if (owner == nullptr || mapped.contains(owner))
    return;
  mapped[owner] = true;
  auto *decl = mutableNode(owner);
  clang::Stmt *root = nullptr;
  if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
    const clang::FunctionDecl *definition = nullptr;
    root = function->getBody(definition);
    decl = mutableNode(definition);
  } else if (const auto *block = llvm::dyn_cast<clang::BlockDecl>(decl)) {
    root = block->getBody();
  } else if (auto *variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
    root = variable->getInit();
  }
  if (root == nullptr || decl == nullptr)
    return;
  parents[root] = nullptr;
  roots[root] = decl;
  mapChildren(root);
}

void CheckEmitter::Impl::mapChildren(clang::Stmt *node) {
  // The first parent found wins; semantic forms are visited first, since
  // they are what the code generator reads.
  llvm::SmallVector<clang::Stmt *, 64> stack{node};
  const auto visit = [&](clang::Stmt *parent, clang::Stmt *child) {
    if (child == nullptr || parents.contains(child))
      return;
    parents[child] = parent;
    stack.push_back(child);
  };
  while (!stack.empty()) {
    clang::Stmt *current = stack.pop_back_val();
    if (auto *pseudo = llvm::dyn_cast<clang::PseudoObjectExpr>(current)) {
      for (clang::Expr *semantic : pseudo->semantics())
        visit(current, semantic);
      visit(current, pseudo->getSyntacticForm());
      continue;
    }
    for (clang::Stmt *child : current->children())
      visit(current, child);
  }
}

void CheckEmitter::Impl::adopt(clang::Stmt *node) {
  llvm::SmallVector<clang::Stmt *, 16> stack{node};
  while (!stack.empty()) {
    clang::Stmt *current = stack.pop_back_val();
    for (clang::Stmt *child : current->children()) {
      if (child == nullptr)
        continue;
      const bool known = parents.contains(child);
      parents[child] = current;
      if (!known)
        stack.push_back(child);
    }
  }
}

/// The variable whose initialiser `node` is, when `parent` is its
/// declaration statement.
static clang::VarDecl *initialised(clang::Stmt *parent,
                                   const clang::Stmt *node) {
  auto *statement = llvm::dyn_cast_or_null<clang::DeclStmt>(parent);
  if (statement == nullptr)
    return nullptr;
  for (clang::Decl *decl : statement->decls())
    if (auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
        variable != nullptr && variable->getInit() == node)
      return variable;
  return nullptr;
}

bool CheckEmitter::Impl::place(clang::Stmt *node, clang::Stmt *replacement) {
  const auto found = parents.find(node);
  if (found == parents.end())
    return false;
  clang::Stmt *parent = found->second;
  if (parent == nullptr) {
    const auto owner = roots.find(node);
    if (owner == roots.end())
      return false;
    clang::Decl *decl = owner->second;
    if (auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
      function->setBody(replacement);
    } else if (auto *block = llvm::dyn_cast<clang::BlockDecl>(decl)) {
      auto *body = llvm::dyn_cast<clang::CompoundStmt>(replacement);
      if (body == nullptr)
        return false;
      block->setBody(body);
    } else if (auto *variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
      auto *init = llvm::dyn_cast<clang::Expr>(replacement);
      if (init == nullptr)
        return false;
      // Also forgets a constant evaluation of the old initialiser.
      variable->setInit(init);
    } else {
      return false;
    }
    roots.erase(owner);
    roots[replacement] = decl;
    parents[replacement] = nullptr;
  } else if (clang::VarDecl *variable = initialised(parent, node)) {
    auto *init = llvm::dyn_cast<clang::Expr>(replacement);
    if (init == nullptr)
      return false;
    variable->setInit(init);
    parents[replacement] = parent;
  } else {
    bool replaced = false;
    for (clang::Stmt *&child : parent->children()) {
      if (child == node) {
        child = replacement;
        replaced = true;
        break;
      }
    }
    if (!replaced)
      return false;
    parents[replacement] = parent;
  }
  adopt(replacement);
  // A variable whose initialiser changed below its top must not keep a
  // constant evaluation of the old one.
  const clang::Stmt *below = replacement;
  for (clang::Stmt *at = parents.lookup(replacement);;
       below = at, at = parents.lookup(at)) {
    if (at == nullptr) {
      if (auto *variable =
              llvm::dyn_cast_or_null<clang::VarDecl>(roots.lookup(below)))
        variable->setInit(variable->getInit());
      break;
    }
    if (clang::VarDecl *variable = initialised(at, below))
      variable->setInit(variable->getInit());
  }
  return true;
}

bool CheckEmitter::Impl::wrapAt(clang::Stmt *logical, clang::Stmt *wrapper) {
  if (!place(occupant(logical), wrapper))
    return false;
  occupants[logical] = wrapper;
  return true;
}

bool CheckEmitter::Impl::replaceNode(clang::Stmt *node,
                                     clang::Stmt *replacement) {
  if (!place(node, replacement))
    return false;
  // A later wrap of the same position wraps the replacement.
  if (!occupants.contains(node))
    occupants[node] = replacement;
  return true;
}

//===----------------------------------------------------------------------===//
// Building
//===----------------------------------------------------------------------===//

template <typename Build>
clang::Stmt *CheckEmitter::Impl::attempt(Build &&build) {
  // §10.6: provisional analysis, whose diagnostics are suppressed; the
  // traps say whether one of them was an error.
  const clang::Sema::TentativeAnalysisScope tentative(sema);
  const clang::Sema::SFINAETrap trap(sema, /*WithAccessChecking=*/true);
  const clang::DiagnosticErrorTrap errors(diags);
  const bool before = diags.hasErrorOccurred();
  clang::Stmt *built = std::forward<Build>(build)();
  if (built == nullptr || trap.hasErrorOccurred() ||
      errors.hasErrorOccurred() || diags.hasErrorOccurred() != before)
    return nullptr;
  if (const auto *expr = llvm::dyn_cast<clang::Expr>(built);
      expr != nullptr && expr->containsErrors())
    return nullptr;
  return built;
}

clang::Expr *CheckEmitter::Impl::literal(std::uint64_t value,
                                         clang::QualType type,
                                         clang::SourceLocation loc) {
  return clang::IntegerLiteral::Create(
      context,
      llvm::APInt(static_cast<unsigned>(context.getTypeSize(type)), value),
      type, loc);
}

clang::Expr *CheckEmitter::Impl::castTo(clang::Expr *expr, clang::QualType type,
                                        clang::CastKind kind) {
  if (expr == nullptr)
    return nullptr;
  if (clang::ASTContext::hasSameType(expr->getType(), type))
    return expr;
  // `expr` is a helper call, never an implicit cast Sema could merge into.
  const clang::ExprResult cast = sema.ImpCastExprToType(expr, type, kind);
  return usable(cast) ? cast.get() : nullptr;
}

clang::ExprResult CheckEmitter::Impl::callHelper(
    llvm::StringRef name, llvm::MutableArrayRef<clang::Expr *> args,
    clang::SourceLocation loc, const SiteContext *site) {
  clang::FunctionDecl *function = helper(name);
  if (function == nullptr || llvm::is_contained(args, nullptr))
    return clang::ExprError();
  llvm::SmallVector<clang::Expr *, 8> all(args.begin(), args.end());
  const HelperSignature *signature = findHelperSignature(name);
  if (report && signature != nullptr && signature->reports) {
    // §10.2, report mode: the site's file, line and column.
    const std::string file = site != nullptr ? site->where.file : "";
    const clang::QualType array = context.getStringLiteralArrayType(
        context.CharTy, static_cast<unsigned>(file.size()));
    all.push_back(clang::StringLiteral::Create(
        context, file, clang::StringLiteralKind::Ordinary, false, array, loc));
    all.push_back(literal(site != nullptr ? site->where.line : 0,
                          context.UnsignedIntTy, loc));
    all.push_back(literal(site != nullptr ? site->where.column : 0,
                          context.UnsignedIntTy, loc));
  }
  clang::Expr *callee = sema.BuildDeclRefExpr(function, function->getType(),
                                              clang::VK_LValue, loc);
  return sema.BuildCallExpr(nullptr, callee, loc, all, loc);
}

clang::ExprResult CheckEmitter::Impl::comma(clang::Expr *lhs, clang::Expr *rhs,
                                            clang::SourceLocation loc) {
  if (lhs == nullptr || rhs == nullptr)
    return clang::ExprError();
  return sema.BuildBinOp(nullptr, loc, clang::BO_Comma, lhs, rhs);
}

//===----------------------------------------------------------------------===//
// Terms (§10.2, §10.3)
//===----------------------------------------------------------------------===//

clang::Expr *CheckEmitter::Impl::placeLvalue(const core::CheckTerm &term,
                                             const SiteContext &site) {
  auto *decl = mutableNode(site.handles != nullptr
                               ? site.handles->resolvePlace(term.handle)
                               : nullptr);
  if (decl == nullptr)
    return nullptr;
  // The place is a parameter or a local of the function being rewritten
  // (§10.3 rule 1), already referenced there: a plain reference, with no
  // capture analysis from the translation unit's context.
  clang::Expr *expr = clang::DeclRefExpr::Create(
      context, clang::NestedNameSpecifierLoc(), clang::SourceLocation(), decl,
      /*RefersToEnclosingVariableOrCapture=*/false, site.loc,
      decl->getType().getNonReferenceType(), clang::VK_LValue);
  for (const core::CheckPathStep &step : term.path) {
    if (step.kind == core::CheckPathStep::Kind::Deref) {
      clang::ExprResult pointer =
          sema.DefaultFunctionArrayLvalueConversion(expr);
      if (!usable(pointer))
        return nullptr;
      // §10.3 rule 5: a read through a pointer the term checks itself.
      if (step.checked) {
        const clang::QualType type = pointer.get()->getType();
        std::array<clang::Expr *, 1> args = {pointer.get()};
        const clang::ExprResult checked =
            callHelper("__weavec_chk_nonnull", args, site.loc, &site);
        clang::Expr *typed =
            usable(checked) ? castTo(checked.get(), type, clang::CK_BitCast)
                            : nullptr;
        if (typed == nullptr)
          return nullptr;
        pointer = typed;
      }
      const clang::ExprResult object =
          sema.CreateBuiltinUnaryOp(site.loc, clang::UO_Deref, pointer.get());
      if (!usable(object))
        return nullptr;
      expr = object.get();
      continue;
    }
    // An anonymous member: the next named step is found through it.
    if (step.field.empty())
      continue;
    clang::CXXScopeSpec scope;
    const clang::DeclarationNameInfo name(&context.Idents.get(step.field),
                                          site.loc);
    const clang::ExprResult member = sema.BuildMemberReferenceExpr(
        expr, expr->getType(), site.loc, /*IsArrow=*/false, scope,
        clang::SourceLocation(), nullptr, name, nullptr, nullptr);
    if (!usable(member))
      return nullptr;
    expr = member.get();
  }
  return expr;
}

clang::Expr *CheckEmitter::Impl::pointerTerm(const core::CheckTerm &term,
                                             const SiteContext &site) {
  if (term.kind != core::CheckTerm::Kind::Place)
    return nullptr;
  clang::Expr *lvalue = placeLvalue(term, site);
  if (lvalue == nullptr)
    return nullptr;
  const clang::ExprResult value =
      sema.DefaultFunctionArrayLvalueConversion(lvalue);
  return usable(value) ? value.get() : nullptr;
}

clang::Expr *CheckEmitter::Impl::term(const core::CheckTerm &term,
                                      Direction direction,
                                      const SiteContext &site) {
  const clang::SourceLocation loc = site.loc;
  const bool need = direction == Direction::Need;
  switch (term.kind) {
  case core::CheckTerm::Kind::Constant: {
    std::uint64_t value = 0;
    if (term.constant >= 0)
      value = static_cast<std::uint64_t>(term.constant);
    else if (need)
      value = ~std::uint64_t{0};
    return literal(value, context.UnsignedLongLongTy, loc);
  }
  case core::CheckTerm::Kind::Place: {
    clang::Expr *lvalue = placeLvalue(term, site);
    if (lvalue == nullptr)
      return nullptr;
    const clang::QualType type = lvalue->getType();
    const clang::ExprResult value =
        sema.DefaultFunctionArrayLvalueConversion(lvalue);
    if (!usable(value))
      return nullptr;
    if (!type->isSignedIntegerOrEnumerationType())
      return value.get();
    // A signed leaf enters through need_s or have_s: a negative count is
    // the maximum as a need and 0 as a have.
    std::array<clang::Expr *, 1> args = {value.get()};
    const clang::ExprResult entered = callHelper(
        need ? "__weavec_need_s" : "__weavec_have_s", args, loc, nullptr);
    return usable(entered) ? entered.get() : nullptr;
  }
  case core::CheckTerm::Kind::SizeOf: {
    const clang::QualType type = site.handles != nullptr
                                     ? site.handles->resolveType(term.handle)
                                     : clang::QualType();
    if (type.isNull())
      return nullptr;
    const clang::ExprResult size = sema.CreateUnaryExprOrTypeTraitExpr(
        context.getTrivialTypeSourceInfo(type, loc), loc, clang::UETT_SizeOf,
        clang::SourceRange(loc, loc));
    return usable(size) ? size.get() : nullptr;
  }
  case core::CheckTerm::Kind::Add:
  case core::CheckTerm::Kind::Sub:
  case core::CheckTerm::Kind::Mul: {
    if (term.operands.size() != 2)
      return nullptr;
    // The right operand of a subtraction goes the other way.
    const Direction right = term.kind == core::CheckTerm::Kind::Sub
                                ? opposite(direction)
                                : direction;
    std::array<clang::Expr *, 2> args = {
        this->term(term.operands[0], direction, site),
        this->term(term.operands[1], right, site)};
    std::string name = need ? "__weavec_need_" : "__weavec_have_";
    if (term.kind == core::CheckTerm::Kind::Add)
      name += "add";
    else if (term.kind == core::CheckTerm::Kind::Sub)
      name += "sub";
    else
      name += "mul";
    const clang::ExprResult result = callHelper(name, args, loc, nullptr);
    return usable(result) ? result.get() : nullptr;
  }
  case core::CheckTerm::Kind::Div: {
    // RFC 0030 §10.1 (amended in S3): floor division by a positive
    // constant, which only an extent (a have) uses; the dividend is never
    // negative there, so C's unsigned division is exact.
    if (term.operands.size() != 2 || need)
      return nullptr;
    const core::CheckTerm &divisor = term.operands[1];
    if (divisor.kind != core::CheckTerm::Kind::Constant ||
        divisor.constant <= 0)
      return nullptr;
    clang::Expr *dividend = this->term(term.operands[0], direction, site);
    if (dividend == nullptr)
      return nullptr;
    clang::Expr *constant =
        literal(static_cast<std::uint64_t>(divisor.constant),
                context.UnsignedLongLongTy, loc);
    const clang::ExprResult quotient =
        sema.BuildBinOp(nullptr, loc, clang::BO_Div, dividend, constant);
    return usable(quotient) ? quotient.get() : nullptr;
  }
  case core::CheckTerm::Kind::StrNLen: {
    if (term.operands.size() != 2)
      return nullptr;
    std::array<clang::Expr *, 2> args = {
        pointerTerm(term.operands[0], site),
        this->term(term.operands[1], Direction::Have, site)};
    const clang::ExprResult result =
        callHelper("__weavec_strnlen", args, loc, &site);
    return usable(result) ? result.get() : nullptr;
  }
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Checks
//===----------------------------------------------------------------------===//

clang::ExprResult CheckEmitter::Impl::checkCall(const Entry &entry,
                                                clang::Expr *wrapped,
                                                SiteContext &site) {
  const clang::SourceLocation loc = site.loc;
  if (entry.form == Entry::Form::Violation)
    return callHelper("__weavec_chk_violation", {}, loc, &site);
  const std::string name = core::helperName(entry);
  const auto operand = [&](std::size_t i, Direction direction) {
    return i < entry.operands.size() ? term(entry.operands[i], direction, site)
                                     : nullptr;
  };
  const auto pointer = [&](std::size_t i) {
    return i < entry.operands.size() ? pointerTerm(entry.operands[i], site)
                                     : nullptr;
  };
  clang::Expr *moved = wrapped != nullptr ? paren(wrapped, loc) : nullptr;
  switch (entry.kind) {
  case Entry::Template::Nonnull: {
    if (moved == nullptr)
      return clang::ExprError();
    if (entry.form == Entry::Form::Function) {
      // The callee operand, as `void (*)(void)` and back (§10.2).
      std::array<clang::Expr *, 1> args = {
          castTo(moved, typeOf(context, HelperType::FunctionPointer),
                 clang::CK_BitCast)};
      return callHelper(name, args, loc, &site);
    }
    if (entry.form == Entry::Form::IfNonZero) {
      std::array<clang::Expr *, 2> args = {moved, operand(0, Direction::Need)};
      return callHelper(name, args, loc, &site);
    }
    // §7.5, §10.4: a requirement's guard is the zero-length form's length,
    // computed as a have so that a guard that fails saturates at zero.
    if (entry.guard && entry.placement == Entry::Placement::WrapArgument) {
      std::array<clang::Expr *, 2> args = {
          moved, term(*entry.guard, Direction::Have, site)};
      return callHelper(name + "_n", args, loc, &site);
    }
    std::array<clang::Expr *, 1> args = {moved};
    return callHelper(name, args, loc, &site);
  }
  case Entry::Template::Index: {
    // WrapIndex wraps the subscript; WrapOperand checks the element at 0.
    clang::Expr *index =
        moved != nullptr ? moved : literal(0, context.UnsignedLongLongTy, loc);
    std::array<clang::Expr *, 2> args = {index, operand(0, Direction::Have)};
    return callHelper(name, args, loc, &site);
  }
  case Entry::Template::Span: {
    if (moved == nullptr)
      return clang::ExprError();
    std::array<clang::Expr *, 5> args = {
        moved, literal(0, context.LongLongTy, loc), pointer(0),
        operand(1, Direction::Have), operand(2, Direction::Need)};
    return callHelper(name, args, loc, &site);
  }
  case Entry::Template::Len: {
    if (entry.placement == Entry::Placement::WrapArgument) {
      std::array<clang::Expr *, 2> args = {moved, operand(0, Direction::Have)};
      return callHelper(name, args, loc, &site);
    }
    // This form does not take the operand, so `moved` is left unused; the
    // ASTContext owns the node and releases its arena.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
    std::array<clang::Expr *, 2> args = {operand(0, Direction::Need),
                                         operand(1, Direction::Have)};
    return callHelper(name, args, loc, &site);
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
  }
  case Entry::Template::Disjoint: {
    std::array<clang::Expr *, 3> args = {moved, pointer(0),
                                         operand(1, Direction::Need)};
    return callHelper(name, args, loc, &site);
  }
  case Entry::Template::Assert: {
    std::array<clang::Expr *, 1> args = {moved};
    return callHelper(name, args, loc, &site);
  }
  }
  return clang::ExprError();
}

//===----------------------------------------------------------------------===//
// Placements (§10.4)
//===----------------------------------------------------------------------===//

/// The logical node a `WrapOperand` entry wraps: the callee of an indirect
/// call for the function-pointer form, the value a PtrArith or Cast site
/// creates for `span`, else the site's operand.
static clang::Stmt *operandOf(const Entry &entry,
                              const analysis::SiteInfo &info) {
  auto *stmt = mutableNode(info.stmt);
  if (entry.kind == Entry::Template::Nonnull &&
      entry.form == Entry::Form::Function) {
    auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(stmt);
    return call != nullptr ? call->getCallee() : nullptr;
  }
  if (entry.kind == Entry::Template::Span &&
      (info.kind == core::SiteKind::PtrArith ||
       info.kind == core::SiteKind::Cast))
    return stmt;
  return mutableNode(info.operand);
}

bool CheckEmitter::Impl::wrapValue(const Entry &entry, clang::Stmt *target,
                                   SiteContext &site) {
  auto *current = llvm::dyn_cast_or_null<clang::Expr>(
      target != nullptr ? occupant(target) : nullptr);
  if (current == nullptr)
    return false;
  const clang::QualType type = current->getType();
  const bool returnsOperand = entry.form != Entry::Form::Violation &&
                              (entry.kind == Entry::Template::Nonnull ||
                               entry.kind == Entry::Template::Span);
  const bool wrapsIndex = entry.placement == Entry::Placement::WrapIndex;
  clang::Stmt *wrapper = attempt([&]() -> clang::Stmt * {
    if (wrapsIndex) {
      // The subscript: `i` becomes `chk_index(i, n)`.
      const clang::ExprResult check = checkCall(entry, current, site);
      return usable(check) ? check.get() : nullptr;
    }
    if (returnsOperand) {
      // `p` becomes `(T *)chk(p, ...)`: still the operand's type, so an
      // access through it is still an lvalue.
      const clang::ExprResult check = checkCall(entry, current, site);
      if (!usable(check))
        return nullptr;
      return castTo(check.get(), type, clang::CK_BitCast);
    }
    // `index` of the element at 0, or a lowered violation: `(chk(...), p)`.
    const clang::ExprResult check = checkCall(entry, nullptr, site);
    if (!usable(check))
      return nullptr;
    // ASTContext owns the nodes a rejected rewrite leaves behind (§10.6).
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
    const clang::ExprResult sequenced =
        comma(check.get(), paren(current, site.loc), site.loc);
    return usable(sequenced) ? sequenced.get() : nullptr;
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
  });
  return wrapper != nullptr && wrapAt(target, wrapper);
}

bool CheckEmitter::Impl::wrapArgument(const Entry &entry, SiteContext &site) {
  auto *call =
      llvm::dyn_cast_or_null<clang::CallExpr>(mutableNode(site.info->stmt));
  if (call == nullptr || entry.argument >= call->getNumArgs())
    return false;
  clang::Expr *current = call->getArg(entry.argument);
  const clang::QualType type = current->getType();
  clang::Stmt *wrapper = attempt([&]() -> clang::Stmt * {
    const clang::ExprResult check = checkCall(entry, current, site);
    if (!usable(check))
      return nullptr;
    // `len` returns the need it wraps, the others the pointer.
    return castTo(check.get(), type,
                  type->isIntegerType() ? clang::CK_IntegralCast
                                        : clang::CK_BitCast);
  });
  return wrapper != nullptr && place(current, wrapper);
}

bool CheckEmitter::Impl::replaceAccess(const Entry &entry, SiteContext &site) {
  const analysis::SiteInfo &info = *site.info;
  // `*p`, `p->f` and `p[0]`: the element at 0, so the operand is checked in
  // place and the access itself stays.
  if (info.kind != core::SiteKind::Index || info.index == nullptr)
    return wrapValue(entry, operandOf(entry, info), site);
  auto *access = mutableNode(info.stmt);
  clang::Expr *base = nullptr;
  clang::Expr *index = nullptr;
  if (auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(access)) {
    base = subscript->getBase();
    index = subscript->getIdx();
  } else if (const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(access);
             deref != nullptr && deref->getOpcode() == clang::UO_Deref) {
    const auto *sum = llvm::dyn_cast<clang::BinaryOperator>(
        deref->getSubExpr()->IgnoreParens());
    if (sum == nullptr || sum->getOpcode() != clang::BO_Add)
      return false;
    const bool lhsPointer = sum->getLHS()->getType()->isPointerType();
    base = lhsPointer ? sum->getLHS() : sum->getRHS();
    index = lhsPointer ? sum->getRHS() : sum->getLHS();
  }
  if (base == nullptr || index == nullptr || entry.operands.size() != 3)
    return false;
  const clang::QualType pointerType = base->getType();
  clang::Stmt *replacement = attempt([&]() -> clang::Stmt * {
    // `p[i]` becomes `*(T *)__weavec_chk_span(p, i, base, bytes, width)`:
    // `p` and `i` are evaluated once and the address is formed only after
    // the check.
    std::array<clang::Expr *, 5> args = {
        paren(base, site.loc), paren(index, site.loc),
        pointerTerm(entry.operands[0], site),
        term(entry.operands[1], Direction::Have, site),
        term(entry.operands[2], Direction::Need, site)};
    const clang::ExprResult check =
        callHelper(core::helperName(entry), args, site.loc, &site);
    if (!usable(check))
      return nullptr;
    clang::Expr *typed = castTo(check.get(), pointerType, clang::CK_BitCast);
    if (typed == nullptr)
      return nullptr;
    const clang::ExprResult object =
        sema.CreateBuiltinUnaryOp(site.loc, clang::UO_Deref, typed);
    return usable(object) ? object.get() : nullptr;
  });
  return replacement != nullptr && replaceNode(access, replacement);
}

bool CheckEmitter::Impl::beforeCall(const Entry &entry, SiteContext &site) {
  auto *stmt = mutableNode(site.info->stmt);
  const clang::SourceLocation loc = site.loc;
  // The check, and its §7.5 guard: `(guard ? check : 0)`.
  const auto guarded = [&]() -> clang::Expr * {
    const clang::ExprResult check = checkCall(entry, nullptr, site);
    if (!usable(check))
      return nullptr;
    if (!entry.guard)
      return check.get();
    clang::Expr *guard = term(*entry.guard, Direction::Have, site);
    clang::Expr *otherwise = literal(0, context.IntTy, loc);
    if (check.get()->getType()->isVoidType())
      otherwise =
          sema.ImpCastExprToType(otherwise, context.VoidTy, clang::CK_ToVoid)
              .get();
    if (guard == nullptr || otherwise == nullptr)
      return nullptr;
    const clang::ExprResult choice =
        sema.ActOnConditionalOp(loc, loc, guard, check.get(), otherwise);
    return usable(choice) ? choice.get() : nullptr;
  };
  if (auto *ret = llvm::dyn_cast<clang::ReturnStmt>(stmt)) {
    if (clang::Expr *value = ret->getRetValue()) {
      // `return v;` becomes `return (check, v);`.
      clang::Stmt *wrapper = attempt([&]() -> clang::Stmt * {
        // ASTContext owns the nodes a rejected rewrite leaves behind (§10.6).
        // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
        const clang::ExprResult sequenced =
            comma(guarded(), paren(value, loc), loc);
        return usable(sequenced) ? sequenced.get() : nullptr;
        // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
      });
      return wrapper != nullptr && place(value, wrapper);
    }
    // `return;` becomes `{ check; return; }`.
    clang::Stmt *current = occupant(ret);
    clang::Stmt *wrapper = attempt([&]() -> clang::Stmt * {
      clang::Expr *check = guarded();
      if (check == nullptr)
        return nullptr;
      return clang::CompoundStmt::Create(context, {check, current},
                                         clang::FPOptionsOverride(), loc, loc);
    });
    return wrapper != nullptr && wrapAt(ret, wrapper);
  }
  if (auto *body = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
    // The end of the body: the check follows its last statement.
    auto *current = llvm::dyn_cast<clang::CompoundStmt>(occupant(body));
    if (current == nullptr)
      return false;
    clang::Stmt *wrapper = attempt([&]() -> clang::Stmt * {
      clang::Expr *check = guarded();
      if (check == nullptr)
        return nullptr;
      llvm::SmallVector<clang::Stmt *, 16> statements(current->body());
      statements.push_back(check);
      return clang::CompoundStmt::Create(
          context, statements, clang::FPOptionsOverride(),
          current->getLBracLoc(), current->getRBracLoc());
    });
    return wrapper != nullptr && wrapAt(body, wrapper);
  }
  auto *call = llvm::dyn_cast<clang::Expr>(stmt);
  auto *current =
      call != nullptr ? llvm::dyn_cast<clang::Expr>(occupant(call)) : nullptr;
  if (current == nullptr)
    return false;
  // `f(a)` becomes `(check, f(a))`, of the call's type and category.
  clang::Stmt *wrapper = attempt([&]() -> clang::Stmt * {
    // ASTContext owns the nodes a rejected rewrite leaves behind (§10.6).
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
    const clang::ExprResult sequenced =
        comma(guarded(), paren(current, loc), loc);
    return usable(sequenced) ? sequenced.get() : nullptr;
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
  });
  return wrapper != nullptr && wrapAt(call, wrapper);
}

bool CheckEmitter::Impl::replaceCall(const Entry &entry, SiteContext &site) {
  auto *call =
      llvm::dyn_cast_or_null<clang::CallExpr>(mutableNode(site.info->stmt));
  if (call == nullptr)
    return false;
  const clang::SourceLocation loc = site.loc;
  clang::Stmt *replacement = nullptr;
  if (entry.form == Entry::Form::Violation ||
      entry.kind == Entry::Template::Assert) {
    // `WEAVEC_ASSUME(e)` becomes `__weavec_chk_assert(e)`; a contradicted
    // one that was lowered, the unconditional trap.
    if (entry.form != Entry::Form::Violation && call->getNumArgs() != 1)
      return false;
    replacement = attempt([&]() -> clang::Stmt * {
      const clang::ExprResult check = checkCall(
          entry,
          entry.form == Entry::Form::Violation ? nullptr : call->getArg(0),
          site);
      return usable(check) ? check.get() : nullptr;
    });
  } else if (entry.kind == Entry::Template::Len &&
             entry.form == Entry::Form::Result) {
    // `sprintf(d, fmt, ...)` becomes
    // `__weavec_chk_len_r(snprintf(d, have, fmt, ...), have)`: the write
    // stays in bounds and the arguments are evaluated once.
    const std::optional<core::LibraryMatch> &match = site.info->library;
    if (!match || match->entry == nullptr || entry.operands.size() != 1)
      return false;
    clang::FunctionDecl *writer =
        lookupFunction(core::boundedWriterName(match->entry->name));
    if (writer == nullptr)
      return false;
    // The row's arguments in row order; a fortified alias's own arguments
    // (flag, object size) are dropped.
    std::vector<clang::Expr *> rowArguments;
    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
      const int row = match->rowArgument(i);
      if (row < 0)
        continue;
      if (static_cast<std::size_t>(row) >= rowArguments.size())
        rowArguments.resize(static_cast<std::size_t>(row) + 1, nullptr);
      rowArguments[static_cast<std::size_t>(row)] = call->getArg(i);
    }
    if (rowArguments.size() < 2 || llvm::is_contained(rowArguments, nullptr))
      return false;
    replacement = attempt([&]() -> clang::Stmt * {
      llvm::SmallVector<clang::Expr *, 8> args;
      args.push_back(paren(rowArguments[0], loc));
      args.push_back(term(entry.operands[0], Direction::Have, site));
      for (std::size_t i = 1; i < rowArguments.size(); ++i)
        args.push_back(paren(rowArguments[i], loc));
      if (llvm::is_contained(args, nullptr))
        return nullptr;
      clang::Expr *callee = sema.BuildDeclRefExpr(writer, writer->getType(),
                                                  clang::VK_LValue, loc);
      const clang::ExprResult bounded =
          sema.BuildCallExpr(nullptr, callee, loc, args, loc);
      if (!usable(bounded))
        return nullptr;
      std::array<clang::Expr *, 2> checkArgs = {
          bounded.get(), term(entry.operands[0], Direction::Have, site)};
      const clang::ExprResult check =
          callHelper(core::helperName(entry), checkArgs, loc, &site);
      if (!usable(check))
        return nullptr;
      return castTo(check.get(), call->getType(), clang::CK_IntegralCast);
    });
  } else {
    return false;
  }
  return replacement != nullptr && replaceNode(call, replacement);
}

bool CheckEmitter::Impl::apply(const Entry &entry, SiteContext &site) {
  switch (entry.placement) {
  case Entry::Placement::WrapOperand:
    return wrapValue(entry, operandOf(entry, *site.info), site);
  case Entry::Placement::WrapIndex:
    return wrapValue(entry, mutableNode(site.info->index), site);
  case Entry::Placement::WrapArgument:
    return wrapArgument(entry, site);
  case Entry::Placement::ReplaceAccess:
    return replaceAccess(entry, site);
  case Entry::Placement::BeforeCall:
    return beforeCall(entry, site);
  case Entry::Placement::ReplaceCall:
    return replaceCall(entry, site);
  }
  return false;
}

void CheckEmitter::Impl::internalError(clang::SourceLocation loc,
                                       llvm::StringRef text, bool zeroInit) {
  // §10.6: the ledger was decided from the plan, so a check that cannot be
  // inserted fails the compile rather than change the ledger.
  const unsigned id =
      zeroInit ? diags.getCustomDiagID(
                     clang::DiagnosticsEngine::Error,
                     "WeaveC internal error: could not zero-initialise the "
                     "allocation '%0'; build with -fno-weavec-zero-init to "
                     "bypass")
               : diags.getCustomDiagID(
                     clang::DiagnosticsEngine::Error,
                     "WeaveC internal error: could not insert the check for "
                     "'%0'; build with -fweavec-checks=none to bypass");
  diags.Report(loc, id) << text;
}

/// The source text of `stmt` with its whitespace removed, as the ledger
/// spells a site.
static std::string textOf(const clang::Stmt &stmt,
                          const clang::ASTContext &context) {
  const clang::SourceManager &sm = context.getSourceManager();
  const llvm::StringRef text = clang::Lexer::getSourceText(
      sm.getExpansionRange(stmt.getSourceRange()), sm, context.getLangOpts());
  return core::siteText(std::string_view(text.data(), text.size()));
}

bool CheckEmitter::Impl::emit(const core::CheckPlan &plan,
                              const analysis::SiteIndex &sites,
                              const analysis::PlaceHandleTable &handles,
                              const core::UnitLedger *unit) {
  std::map<core::SiteId, std::vector<const Entry *>> bySite;
  for (const Entry &entry : plan.entries)
    bySite[entry.site].push_back(&entry);
  bool ok = true;
  for (auto &[id, entries] : bySite) {
    const analysis::SiteInfo *info = sites.info(id);
    if (info == nullptr || info->stmt == nullptr ||
        id.function >= sites.functions().size())
      continue;
    const analysis::SiteIndex::FunctionSites &function =
        sites.functions()[id.function];
    if (function.decl == nullptr)
      continue;
    mapOwner(function.decl);
    SiteContext site;
    site.info = info;
    site.handles = &handles;
    site.function = function.decl;
    site.loc = info->stmt->getBeginLoc();
    const core::Site *row = unit != nullptr ? unit->site(id) : nullptr;
    site.where =
        row != nullptr && !row->location.file.empty()
            ? row->location
            : analysis::toCoreLocation(context.getSourceManager(), site.loc);
    site.text = row != nullptr ? row->text : textOf(*info->stmt, context);
    std::ranges::stable_sort(entries, [](const Entry *a, const Entry *b) {
      return std::make_pair(phase(*a), nesting(*a)) <
             std::make_pair(phase(*b), nesting(*b));
    });
    // Sema builds the rewrites as if inside the function.
    clang::Sema::ContextRAII inFunction(sema, mutableNode(function.decl));
    for (const Entry *entry : entries) {
      if (!apply(*entry, site)) {
        internalError(site.loc, site.text, /*zeroInit=*/false);
        ok = false;
        break;
      }
      ++inserted;
    }
  }
  return ok;
}

//===----------------------------------------------------------------------===//
// Zero-initialisation (§11)
//===----------------------------------------------------------------------===//

clang::Expr *CheckEmitter::Impl::rebuild(const clang::Expr *expr,
                                         clang::SourceLocation loc,
                                         unsigned depth) {
  if (expr == nullptr || depth > 16)
    return nullptr;
  const clang::Expr *e = expr->IgnoreParens();
  const clang::QualType type = e->getType();
  if (!e->isValueDependent() && type->isIntegerType() &&
      !type->isEnumeralType() && !type->isBooleanType())
    if (const std::optional<llvm::APSInt> value =
            e->getIntegerConstantExpr(context))
      return clang::IntegerLiteral::Create(
          context, value->extOrTrunc(context.getIntWidth(type)), type, loc);
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
    auto *variable =
        mutableNode(llvm::dyn_cast<clang::VarDecl>(ref->getDecl()));
    if (variable == nullptr || variable->getType().isVolatileQualified())
      return nullptr;
    return clang::DeclRefExpr::Create(context, clang::NestedNameSpecifierLoc(),
                                      clang::SourceLocation(), variable,
                                      ref->refersToEnclosingVariableOrCapture(),
                                      loc, type, ref->getValueKind());
  }
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e)) {
    clang::Expr *sub = rebuild(cast->getSubExpr(), loc, depth + 1);
    if (sub == nullptr)
      return nullptr;
    return clang::ImplicitCastExpr::Create(
        context, type, cast->getCastKind(), sub, nullptr, cast->getValueKind(),
        cast->getStoredFPFeaturesOrDefault());
  }
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e);
      unary != nullptr && unary->getOpcode() == clang::UO_AddrOf) {
    clang::Expr *sub = rebuild(unary->getSubExpr(), loc, depth + 1);
    if (sub == nullptr)
      return nullptr;
    const clang::ExprResult address =
        sema.CreateBuiltinUnaryOp(loc, clang::UO_AddrOf, sub);
    return usable(address) ? address.get() : nullptr;
  }
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e);
      member != nullptr && !member->isArrow()) {
    clang::Expr *base = rebuild(member->getBase(), loc, depth + 1);
    if (base == nullptr)
      return nullptr;
    return clang::MemberExpr::CreateImplicit(
        context, base, /*IsArrow=*/false, member->getMemberDecl(), type,
        member->getValueKind(), member->getObjectKind());
  }
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e);
      binary != nullptr && !binary->isCompoundAssignmentOp() &&
      (binary->getOpcode() == clang::BO_Add ||
       binary->getOpcode() == clang::BO_Sub ||
       binary->getOpcode() == clang::BO_Mul)) {
    clang::Expr *lhs = rebuild(binary->getLHS(), loc, depth + 1);
    clang::Expr *rhs = rebuild(binary->getRHS(), loc, depth + 1);
    if (lhs == nullptr || rhs == nullptr)
      return nullptr;
    return clang::BinaryOperator::Create(
        context, lhs, rhs, binary->getOpcode(), type, binary->getValueKind(),
        binary->getObjectKind(), loc, binary->getStoredFPFeaturesOrDefault());
  }
  if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(e);
      trait != nullptr && trait->isArgumentType())
    return new (context) clang::UnaryExprOrTypeTraitExpr(
        trait->getKind(), trait->getArgumentTypeInfo(), type, loc, loc);
  return nullptr;
}

bool CheckEmitter::Impl::lowerOne(const ZeroInitRewrite &rewrite) {
  auto *reference = mutableNode(rewrite.reference);
  auto *call = mutableNode(rewrite.call);
  const clang::SourceLocation loc =
      call != nullptr ? call->getBeginLoc() : reference->getBeginLoc();
  switch (rewrite.kind) {
  case ZeroInitRewrite::Kind::SwapCallee:
  case ZeroInitRewrite::Kind::AddressOf: {
    // Out of line the wrappers are real functions: an address needs no
    // second one.
    llvm::StringRef name = rewrite.helper;
    if (options.externalHelpers)
      name.consume_back("_fn");
    clang::FunctionDecl *wrapper = helper(name);
    if (reference == nullptr || wrapper == nullptr ||
        !clang::ASTContext::hasSameType(wrapper->getType(),
                                        reference->getType()))
      return false;
    clang::Stmt *replacement = attempt([&]() -> clang::Stmt * {
      return sema.BuildDeclRefExpr(wrapper, wrapper->getType(),
                                   clang::VK_LValue, loc);
    });
    return replacement != nullptr && place(reference, replacement);
  }
  case ZeroInitRewrite::Kind::WrapResult:
  case ZeroInitRewrite::Kind::ZeroLine:
  case ZeroInitRewrite::Kind::Alloca: {
    auto *current =
        call != nullptr ? llvm::dyn_cast<clang::Expr>(occupant(call)) : nullptr;
    if (current == nullptr)
      return false;
    const clang::QualType type = current->getType();
    clang::Stmt *wrapper = attempt([&]() -> clang::Stmt * {
      if (rewrite.kind == ZeroInitRewrite::Kind::Alloca) {
        // `alloca(n)` becomes `__builtin_memset(alloca(n), 0, n)`; in a
        // helper the block would end with the inlined call.
        clang::FunctionDecl *memset = builtin("__builtin_memset", loc);
        clang::Expr *size = rebuild(rewrite.operand, loc);
        if (memset == nullptr || size == nullptr)
          return nullptr;
        std::array<clang::Expr *, 3> args = {
            paren(current, loc), literal(0, context.IntTy, loc), size};
        clang::Expr *callee = sema.BuildDeclRefExpr(memset, memset->getType(),
                                                    clang::VK_LValue, loc);
        const clang::ExprResult zeroed =
            sema.BuildCallExpr(nullptr, callee, loc, args, loc);
        return usable(zeroed) ? castTo(zeroed.get(), type, clang::CK_BitCast)
                              : nullptr;
      }
      llvm::SmallVector<clang::Expr *, 2> args{paren(current, loc)};
      if (rewrite.kind == ZeroInitRewrite::Kind::ZeroLine)
        args.push_back(rebuild(rewrite.operand, loc));
      else if (rewrite.helper == "__weavec_zero_tail")
        args.push_back(literal(0, context.getSizeType(), loc));
      const clang::ExprResult zeroed =
          callHelper(rewrite.helper, args, loc, nullptr);
      if (!usable(zeroed))
        return nullptr;
      return castTo(zeroed.get(), type,
                    type->isIntegerType() ? clang::CK_IntegralCast
                                          : clang::CK_BitCast);
    });
    return wrapper != nullptr && wrapAt(call, wrapper);
  }
  case ZeroInitRewrite::Kind::PassCallee: {
    if (call == nullptr)
      return false;
    clang::Stmt *replacement = attempt([&]() -> clang::Stmt * {
      llvm::SmallVector<clang::Expr *, 6> args{paren(call->getCallee(), loc)};
      for (clang::Expr *arg : call->arguments())
        args.push_back(paren(arg, loc));
      const clang::ExprResult zeroed =
          callHelper(rewrite.helper, args, loc, nullptr);
      return usable(zeroed)
                 ? castTo(zeroed.get(), call->getType(), clang::CK_IntegralCast)
                 : nullptr;
    });
    return replacement != nullptr && replaceNode(call, replacement);
  }
  }
  return false;
}

bool CheckEmitter::Impl::lowerZeroInit(const ZeroInitPlan &plan) {
  bool ok = true;
  for (const ZeroInitRewrite &rewrite : plan.rewrites) {
    if (rewrite.reference == nullptr || rewrite.owner == nullptr)
      continue;
    mapOwner(rewrite.owner);
    std::optional<clang::Sema::ContextRAII> inFunction;
    const clang::Decl *owner = rewrite.owner;
    if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(owner))
      owner = llvm::dyn_cast<clang::Decl>(variable->getDeclContext());
    if (const auto *function =
            llvm::dyn_cast_or_null<clang::FunctionDecl>(owner))
      inFunction.emplace(sema, mutableNode(function));
    if (!lowerOne(rewrite)) {
      const clang::Stmt &at =
          rewrite.call != nullptr
              ? static_cast<const clang::Stmt &>(*rewrite.call)
              : *rewrite.reference;
      internalError(at.getBeginLoc(), textOf(at, context), /*zeroInit=*/true);
      ok = false;
      continue;
    }
    ++lowered;
  }
  return ok;
}

//===----------------------------------------------------------------------===//
// CheckEmitter
//===----------------------------------------------------------------------===//

CheckEmitter::CheckEmitter(clang::Sema &sema, CheckEmitterOptions options)
    : impl(std::make_unique<Impl>(sema, options)) {}

CheckEmitter::~CheckEmitter() = default;

bool CheckEmitter::emit(const analysis::PlannedLedger &planned) {
  if (!planned.sites)
    return true;
  const core::UnitLedger *unit =
      planned.ledger.units.empty() ? nullptr : &planned.ledger.units.front();
  return emit(planned.plan, *planned.sites, planned.handles, unit);
}

bool CheckEmitter::emit(const core::CheckPlan &plan,
                        const analysis::SiteIndex &sites,
                        const analysis::PlaceHandleTable &handles,
                        const core::UnitLedger *unit) {
  return impl->emit(plan, sites, handles, unit);
}

bool CheckEmitter::lowerZeroInit(const ZeroInitPlan &plan) {
  return impl->lowerZeroInit(plan);
}

std::size_t CheckEmitter::checksInserted() const noexcept {
  return impl->inserted;
}

std::size_t CheckEmitter::zeroInitRewrites() const noexcept {
  return impl->lowered;
}

} // namespace weavec::frontend
