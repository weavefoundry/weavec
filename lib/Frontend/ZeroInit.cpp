//===- ZeroInit.cpp - Zero-initialisation plan (RFC 0030) -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ZeroInit.h"

#include "weavec/Analysis/KindTable.h"
#include "weavec/Frontend/CheckEmitter.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace weavec::frontend {

namespace {

/// A reference to a function, and the call it is the callee of.
struct Reference {
  const clang::DeclRefExpr *reference = nullptr;
  const clang::CallExpr *call = nullptr;
  const clang::Decl *owner = nullptr;
  /// In a system header or the predefines: never counted.
  bool system = false;
};

/// The places code the code generator emits lives: function and block
/// bodies and variable initialisers, each walked in its semantic form.
struct Code {
  const clang::Decl *owner = nullptr;
  const clang::Stmt *root = nullptr;
  bool system = false;
};

} // namespace

/// The direct callee reference of `call`, through parentheses, implicit
/// conversions and `*`.
static const clang::DeclRefExpr *calleeReference(const clang::CallExpr &call) {
  const clang::Expr *callee = call.getCallee();
  for (unsigned depth = 0; callee != nullptr && depth < 16; ++depth) {
    callee = callee->IgnoreParenImpCasts();
    if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(callee))
      return llvm::isa<clang::FunctionDecl>(ref->getDecl()) ? ref : nullptr;
    const auto *deref = llvm::dyn_cast<clang::UnaryOperator>(callee);
    if (deref == nullptr || (deref->getOpcode() != clang::UO_Deref &&
                             deref->getOpcode() != clang::UO_AddrOf))
      return nullptr;
    callee = deref->getSubExpr();
  }
  return nullptr;
}

/// Every piece of code of the unit, and every function reference in it.
class CodeWalker {
public:
  explicit CodeWalker(clang::ASTContext &ctx) : context(ctx) {}

  void walkUnit() {
    walkDeclContext(context.getTranslationUnitDecl());
    // Blocks append their bodies as they are found.
    for (std::size_t i = 0; i < code.size(); ++i)
      walkCode(Code(code[i]));
  }

  std::vector<Code> code;
  std::vector<Reference> references;
  std::vector<const clang::FunctionDecl *> definitions;

private:
  clang::ASTContext &context;

  /// The predefines (the check prelude) and system headers.
  bool isSystem(clang::SourceLocation loc) const {
    const clang::SourceManager &sm = context.getSourceManager();
    if (loc.isInvalid())
      return true;
    const clang::SourceLocation at = sm.getExpansionLoc(loc);
    return sm.isInSystemHeader(at) || sm.isWrittenInBuiltinFile(at);
  }

  void walkDeclContext(const clang::DeclContext *dc) {
    for (const clang::Decl *decl : dc->decls()) {
      if (const auto *nested = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
        walkDeclContext(nested);
        continue;
      }
      if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if (!function->doesThisDeclarationHaveABody())
          continue;
        definitions.push_back(function);
        // The prelude's own helpers call the allocators they wrap.
        const clang::IdentifierInfo *name = function->getIdentifier();
        if (name != nullptr && name->getName().starts_with("__weavec_"))
          continue;
        code.push_back(Code{.owner = function,
                            .root = function->getBody(),
                            .system = isSystem(function->getLocation())});
        continue;
      }
      if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
          variable != nullptr && variable->hasInit() &&
          variable->isFileVarDecl())
        code.push_back(Code{.owner = variable,
                            .root = variable->getInit(),
                            .system = isSystem(variable->getLocation())});
    }
  }

  void walkCode(Code piece) {
    llvm::SmallVector<const clang::Stmt *, 64> stack{piece.root};
    llvm::DenseMap<const clang::DeclRefExpr *, const clang::CallExpr *> callees;
    llvm::DenseSet<const clang::Stmt *> seen;
    while (!stack.empty()) {
      const clang::Stmt *current = stack.pop_back_val();
      if (current == nullptr || !seen.insert(current).second)
        continue;
      if (const auto *block = llvm::dyn_cast<clang::BlockExpr>(current)) {
        code.push_back(Code{.owner = block->getBlockDecl(),
                            .root = block->getBody(),
                            .system = piece.system});
        continue;
      }
      // Unevaluated operands allocate nothing.
      if (const auto *trait =
              llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(current)) {
        if (!trait->isArgumentType() &&
            trait->getArgumentExpr()->getType()->isVariablyModifiedType())
          stack.push_back(trait->getArgumentExpr());
        continue;
      }
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(current))
        if (const clang::DeclRefExpr *ref = calleeReference(*call))
          callees[ref] = call;
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(current);
          ref != nullptr && llvm::isa<clang::FunctionDecl>(ref->getDecl())) {
        const auto callee = callees.find(ref);
        references.push_back(Reference{
            .reference = ref,
            .call = callee != callees.end() ? callee->second : nullptr,
            .owner = piece.owner,
            .system = piece.system || isSystem(ref->getBeginLoc())});
      }
      if (const auto *pseudo =
              llvm::dyn_cast<clang::PseudoObjectExpr>(current)) {
        for (const clang::Expr *semantic : pseudo->semantics())
          stack.push_back(semantic);
        continue;
      }
      // Children in reverse, so that a call is seen before its callee.
      llvm::SmallVector<const clang::Stmt *, 8> children(
          current->children().begin(), current->children().end());
      for (const clang::Stmt *child : llvm::reverse(children))
        stack.push_back(child);
    }
  }
};

//===----------------------------------------------------------------------===//
// Rows
//===----------------------------------------------------------------------===//

static bool inFamily(const core::LibraryResult &result,
                     std::string_view family) {
  return result.family == family;
}

/// The row allocates memory the program reads: a fresh result or `out`
/// value of the heap or stack family, or an `or-fresh` result.
static bool allocatesMemory(const core::LibraryEntry &row) {
  const auto memory = [](const core::LibraryResult &result) {
    return (result.kind == core::LibraryResult::Kind::Fresh ||
            result.kind == core::LibraryResult::Kind::Arg) &&
           (inFamily(result, core::HeapFamily) ||
            inFamily(result, core::StackFamily));
  };
  if (memory(row.result))
    return true;
  return llvm::any_of(row.params, [&](const core::LibraryParam &param) {
    return param.out && memory(*param.out);
  });
}

/// §11: a definition that makes the unit an allocator: a row that
/// allocates or releases the heap family, or has the `zero-init` flag.
static bool definesAllocator(const clang::FunctionDecl &function,
                             const core::LibrarySpec &library) {
  if (function.getIdentifier() == nullptr)
    return false;
  const llvm::StringRef name = function.getName();
  const std::string_view callee(name.data(), name.size());
  std::optional<core::LibraryMatch> match;
  if (const auto signature = analysis::librarySignatureOf(function))
    match = library.lookup(callee, *signature);
  else
    match = library.lookup(callee);
  if (!match || match->entry == nullptr)
    return false;
  const core::LibraryEntry &row = *match->entry;
  if (row.result.zeroInit || allocatesMemory(row))
    return true;
  return llvm::any_of(row.params, [](const core::LibraryParam &param) {
    const bool releases = param.effect == core::LibraryParam::Effect::Release ||
                          param.effect == core::LibraryParam::Effect::Realloc;
    return (releases && param.family == core::HeapFamily) ||
           (param.out && param.out->zeroInit);
  });
}

/// The wrapper with the callee's own signature, when there is one: by the
/// builtin the declaration is, or for a reallocating row by its arity.
static std::string sameSignatureWrapper(const clang::FunctionDecl &function,
                                        const core::LibraryEntry &row) {
  switch (function.getBuiltinID()) {
  case clang::Builtin::BImalloc:
  case clang::Builtin::BI__builtin_malloc:
    return "__weavec_malloc_zero";
  case clang::Builtin::BIcalloc:
  case clang::Builtin::BI__builtin_calloc:
    return "__weavec_calloc_zero";
  case clang::Builtin::BIrealloc:
  case clang::Builtin::BI__builtin_realloc:
    return "__weavec_realloc_zero";
  case clang::Builtin::BIstrdup:
  case clang::Builtin::BI__builtin_strdup:
    return "__weavec_strdup_zero";
  case clang::Builtin::BIstrndup:
  case clang::Builtin::BI__builtin_strndup:
    return "__weavec_strndup_zero";
  default:
    break;
  }
  const bool reallocates =
      !row.params.empty() &&
      row.params.front().effect == core::LibraryParam::Effect::Realloc &&
      row.params.front().family == core::HeapFamily &&
      row.result.kind == core::LibraryResult::Kind::Fresh;
  if (reallocates && row.params.size() == 2)
    return "__weavec_realloc_zero";
  if (reallocates && row.params.size() == 3)
    return "__weavec_reallocarray_zero";
  return {};
}

/// `function` has exactly the type of the helper `name`.
static bool hasHelperType(clang::ASTContext &context,
                          const clang::FunctionDecl &function,
                          llvm::StringRef name) {
  const HelperSignature *signature = findHelperSignature(name);
  return signature != nullptr &&
         context.hasSameType(function.getType(),
                             helperFunctionType(context, *signature, false));
}

/// An operand the lowering evaluates a second time: side-effect free and
/// made only of what `CheckEmitter` can rebuild (constants, variables that
/// are not volatile, `&`, `.`, `+`, `-`, `*`, conversions and `sizeof`).
static bool isRebuildable(const clang::Expr *expr,
                          const clang::ASTContext &context,
                          unsigned depth = 0) {
  if (expr == nullptr || depth > 16 ||
      expr->HasSideEffects(context, /*IncludePossibleEffects=*/true))
    return false;
  const clang::Expr *e = expr->IgnoreParens();
  const clang::QualType type = e->getType();
  if (type.isVolatileQualified())
    return false;
  if (!e->isValueDependent() && type->isIntegerType() &&
      !type->isEnumeralType() && !type->isBooleanType() &&
      e->getIntegerConstantExpr(context))
    return true;
  if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(e)) {
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(ref->getDecl());
    return variable != nullptr && !variable->getType().isVolatileQualified();
  }
  if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(e))
    return isRebuildable(cast->getSubExpr(), context, depth + 1);
  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(e))
    return unary->getOpcode() == clang::UO_AddrOf &&
           isRebuildable(unary->getSubExpr(), context, depth + 1);
  if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(e))
    return !member->isArrow() &&
           isRebuildable(member->getBase(), context, depth + 1);
  if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(e))
    return !binary->isCompoundAssignmentOp() &&
           (binary->getOpcode() == clang::BO_Add ||
            binary->getOpcode() == clang::BO_Sub ||
            binary->getOpcode() == clang::BO_Mul) &&
           isRebuildable(binary->getLHS(), context, depth + 1) &&
           isRebuildable(binary->getRHS(), context, depth + 1);
  if (const auto *trait = llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(e))
    return trait->isArgumentType();
  return false;
}

/// `__weavec_zero_line`'s slot is read again after the call: only
/// `&object`, whose value no call can change.
static bool isStableSlot(const clang::Expr *slot,
                         const clang::ASTContext &context) {
  const auto *address =
      llvm::dyn_cast<clang::UnaryOperator>(slot->IgnoreParenImpCasts());
  return address != nullptr && address->getOpcode() == clang::UO_AddrOf &&
         isRebuildable(slot, context);
}

static bool isNarrowCharPointer(clang::QualType type) {
  return type->isPointerType() && type->getPointeeType()->isCharType();
}

//===----------------------------------------------------------------------===//
// Declarations a jump can bypass (A5)
//===----------------------------------------------------------------------===//

/// Whether an object of `type` can hold a pointer: -ftrivial-auto-var-init
/// does not initialise a bypassed one.
static bool mayHoldPointer(clang::QualType type, unsigned depth = 0) {
  if (depth > 8 || type.isNull())
    return false;
  type = type.getCanonicalType();
  if (type->isPointerType() || type->isBlockPointerType())
    return true;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return mayHoldPointer(array->getElementType(), depth + 1);
  if (const auto *record = type->getAsRecordDecl()) {
    const clang::RecordDecl *definition = record->getDefinition();
    return definition != nullptr &&
           llvm::any_of(definition->fields(),
                        [depth](const clang::FieldDecl *field) {
                          return mayHoldPointer(field->getType(), depth + 1);
                        });
  }
  return false;
}

namespace {

/// §11, A5: the pointer locals of one body whose declaration a `goto`, a
/// computed `goto` or a `switch` can jump past. A jump from J to label L
/// bypasses the declaration D when L follows D within D's parent and J is
/// not in that part of the tree.
class BypassCounter {
public:
  explicit BypassCounter(const clang::Stmt &body) { number(&body, nullptr); }

  std::uint64_t count() const {
    std::uint64_t bypassed = 0;
    for (const Declaration &declaration : declarations) {
      const auto inRegion = [&](unsigned at) {
        return at >= declaration.begin && at <= declaration.end;
      };
      if (llvm::any_of(jumps, [&](const std::pair<unsigned, unsigned> &jump) {
            return inRegion(jump.second) && !inRegion(jump.first);
          }))
        bypassed += declaration.variables;
    }
    return bypassed;
  }

private:
  struct Declaration {
    /// The pre-order indices after the declaration within its parent.
    unsigned begin = 0;
    unsigned end = 0;
    std::uint64_t variables = 0;
  };

  unsigned next = 0;
  llvm::DenseMap<const clang::Stmt *, unsigned> order;
  llvm::DenseMap<const clang::LabelDecl *, unsigned> labels;
  llvm::DenseSet<const clang::LabelDecl *> addressTaken;
  std::vector<std::pair<unsigned, const clang::LabelDecl *>> gotos;
  std::vector<unsigned> indirectGotos;
  std::vector<std::pair<unsigned, unsigned>> jumps;
  std::vector<Declaration> declarations;

  /// Numbers `node`'s subtree in pre-order; returns its last index.
  unsigned number(const clang::Stmt *node, const clang::Stmt *parent) {
    const unsigned at = next++;
    order[node] = at;
    if (const auto *label = llvm::dyn_cast<clang::LabelStmt>(node))
      labels[label->getDecl()] = at;
    else if (const auto *jump = llvm::dyn_cast<clang::GotoStmt>(node))
      gotos.emplace_back(at, jump->getLabel());
    else if (llvm::isa<clang::IndirectGotoStmt>(node))
      indirectGotos.push_back(at);
    else if (const auto *address = llvm::dyn_cast<clang::AddrLabelExpr>(node))
      addressTaken.insert(address->getLabel());
    unsigned last = at;
    std::vector<std::pair<const clang::DeclStmt *, unsigned>> pending;
    for (const clang::Stmt *child : node->children()) {
      if (child == nullptr || llvm::isa<clang::BlockExpr>(child))
        continue;
      last = number(child, node);
      if (const auto *statement = llvm::dyn_cast<clang::DeclStmt>(child))
        pending.emplace_back(statement, last);
    }
    for (const auto &[statement, end] : pending) {
      std::uint64_t variables = 0;
      for (const clang::Decl *decl : statement->decls())
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
            variable != nullptr && variable->hasLocalStorage() &&
            !variable->getType()->isVariablyModifiedType() &&
            mayHoldPointer(variable->getType()))
          ++variables;
      if (variables != 0 && end < last)
        declarations.push_back(
            Declaration{.begin = end + 1, .end = last, .variables = variables});
    }
    if (const auto *switchStmt = llvm::dyn_cast<clang::SwitchStmt>(node))
      for (const clang::SwitchCase *label = switchStmt->getSwitchCaseList();
           label != nullptr; label = label->getNextSwitchCase())
        if (const auto found = order.find(label); found != order.end())
          jumps.emplace_back(at, found->second);
    if (parent == nullptr) {
      for (const auto &[from, label] : gotos)
        if (const auto target = labels.find(label); target != labels.end())
          jumps.emplace_back(from, target->second);
      for (const unsigned from : indirectGotos)
        for (const clang::LabelDecl *label : addressTaken)
          if (const auto target = labels.find(label); target != labels.end())
            jumps.emplace_back(from, target->second);
    }
    return last;
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// The plan
//===----------------------------------------------------------------------===//

/// The call argument that carries row argument `row`, when it exists.
static const clang::Expr *rowArgument(const clang::CallExpr &call,
                                      const core::LibraryMatch &match,
                                      unsigned row) {
  const int index = match.callArgument(row);
  if (index < 0 || static_cast<unsigned>(index) >= call.getNumArgs())
    return nullptr;
  return call.getArg(static_cast<unsigned>(index));
}

/// The lowering of one reference to a `zero-init` row, if any.
static std::optional<ZeroInitRewrite>
lowering(clang::ASTContext &context, const Reference &found,
         const clang::FunctionDecl &function, const core::LibraryMatch &match,
         const ZeroInitOptions &options) {
  const core::LibraryEntry &row = *match.entry;
  // Only rows with the `zero-init` flag are lowered.
  if (!row.result.zeroInit &&
      llvm::none_of(row.params, [](const core::LibraryParam &param) {
        return param.out && param.out->zeroInit;
      }))
    return std::nullopt;
  ZeroInitRewrite rewrite{
      .reference = found.reference, .call = found.call, .owner = found.owner};
  const std::string same = sameSignatureWrapper(function, row);
  const bool sameType = !same.empty() && hasHelperType(context, function, same);
  const core::LibraryResult &result = row.result;
  const bool stack = result.kind == core::LibraryResult::Kind::Fresh &&
                     inFamily(result, core::StackFamily) && result.zeroInit;
  if (found.call == nullptr) {
    // `p = malloc`: a static wrapper with the same signature.
    if (!options.heap || stack || !sameType)
      return std::nullopt;
    rewrite.kind = ZeroInitRewrite::Kind::AddressOf;
    rewrite.helper = same + "_fn";
    return rewrite;
  }
  const clang::CallExpr &call = *found.call;
  if (stack) {
    // `alloca(n)`, with `n` evaluated a second time.
    const int size = match.callArgument(0);
    if (!options.stack || size < 0 ||
        static_cast<unsigned>(size) >= call.getNumArgs() ||
        !isRebuildable(call.getArg(static_cast<unsigned>(size)), context))
      return std::nullopt;
    rewrite.kind = ZeroInitRewrite::Kind::Alloca;
    rewrite.argument = static_cast<unsigned>(size);
    rewrite.operand = call.getArg(static_cast<unsigned>(size));
    return rewrite;
  }
  if (!options.heap)
    return std::nullopt;
  if (sameType) {
    rewrite.kind = ZeroInitRewrite::Kind::SwapCallee;
    rewrite.helper = same;
    return rewrite;
  }
  const bool reallocates =
      llvm::any_of(row.params, [](const core::LibraryParam &param) {
        return param.effect == core::LibraryParam::Effect::Realloc;
      });
  const clang::QualType type = call.getType();
  if (result.zeroInit && inFamily(result, core::HeapFamily) && !reallocates) {
    if (result.kind == core::LibraryResult::Kind::Arg) {
      // `realpath(p, NULL)`, `getcwd(NULL, n)`: fresh only when the
      // argument is null.
      const clang::Expr *arg = rowArgument(call, match, result.arg);
      if (arg == nullptr ||
          arg->IgnoreParenImpCasts()->isNullPointerConstant(
              context, clang::Expr::NPC_ValueDependentIsNotNull) ==
              clang::Expr::NPCK_NotNull)
        return std::nullopt;
    } else if (result.kind != core::LibraryResult::Kind::Fresh) {
      return std::nullopt;
    }
    if (result.string) {
      // Zero from after the terminator; a wide string is left alone.
      if (!isNarrowCharPointer(type))
        return std::nullopt;
      rewrite.kind = ZeroInitRewrite::Kind::WrapResult;
      rewrite.helper = "__weavec_zero_string";
      return rewrite;
    }
    if (!type->isPointerType())
      return std::nullopt;
    rewrite.kind = ZeroInitRewrite::Kind::WrapResult;
    rewrite.helper = "__weavec_zero_tail";
    return rewrite;
  }
  // A fresh `out` value: `getline`, `asprintf`, `posix_memalign`.
  for (unsigned i = 0; i < row.params.size(); ++i) {
    const core::LibraryParam &param = row.params[i];
    if (!param.out || !param.out->zeroInit ||
        !inFamily(*param.out, core::HeapFamily))
      continue;
    const clang::Expr *slot = rowArgument(call, match, i);
    if (slot == nullptr)
      return std::nullopt;
    if (param.out->string) {
      if (!type->isIntegerType() || !isStableSlot(slot, context) ||
          !isNarrowCharPointer(
              slot->IgnoreParenImpCasts()->getType()->getPointeeType()))
        return std::nullopt;
      rewrite.kind = ZeroInitRewrite::Kind::ZeroLine;
      rewrite.helper = "__weavec_zero_line";
      rewrite.argument = static_cast<unsigned>(match.callArgument(i));
      rewrite.operand = slot;
      return rewrite;
    }
    // The helper calls the program's own function through a pointer of
    // exactly its type.
    const HelperSignature *signature =
        findHelperSignature("__weavec_posix_memalign_zero");
    if (signature == nullptr || match.alias != nullptr ||
        call.getNumArgs() != row.params.size() ||
        !context.hasSameType(context.getPointerType(function.getType()),
                             helperFunctionType(context, *signature, false)
                                 ->castAs<clang::FunctionProtoType>()
                                 ->getParamType(0)))
      return std::nullopt;
    rewrite.kind = ZeroInitRewrite::Kind::PassCallee;
    rewrite.helper = "__weavec_posix_memalign_zero";
    return rewrite;
  }
  return std::nullopt;
}

ZeroInitPlan planZeroInit(clang::ASTContext &context,
                          const core::LibrarySpec &library, bool enabled,
                          const ZeroInitOptions &options) {
  ZeroInitPlan plan;
  CodeWalker walker(context);
  walker.walkUnit();

  // A unit that defines an allocator lowers nothing (§11).
  for (const clang::FunctionDecl *definition : walker.definitions)
    if (definesAllocator(*definition, library)) {
      plan.allocator = definition;
      break;
    }
  const bool lowers =
      enabled && plan.allocator == nullptr && (options.heap || options.stack);

  for (const Reference &found : walker.references) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(found.reference->getDecl());
    if (function == nullptr)
      continue;
    const std::optional<core::LibraryMatch> match =
        analysis::governingLibraryEntry(*function, library);
    if (!match || match->entry == nullptr || !allocatesMemory(*match->entry))
      continue;
    std::optional<ZeroInitRewrite> rewrite;
    if (lowers)
      rewrite = lowering(context, found, *function, *match, options);
    if (rewrite)
      plan.rewrites.push_back(std::move(*rewrite));
    else if (!found.system)
      ++plan.a5.nonLoweredAllocations;
  }

  for (const Code &piece : walker.code)
    if (!piece.system && piece.root != nullptr &&
        llvm::isa<clang::FunctionDecl, clang::BlockDecl>(piece.owner))
      plan.a5.bypassedDeclarations += BypassCounter(*piece.root).count();
  return plan;
}

} // namespace weavec::frontend
