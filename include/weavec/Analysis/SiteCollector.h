//===- SiteCollector.h - Sites of emitted functions (RFC 0030) --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §2.1, §2.6: before any dataflow runs, `SiteCollector` walks the
// body of every *emitted* function and enumerates its sites: one operation,
// its kind, its 0-based ordinal in source order, and the facets that apply
// to it. The result is the `SiteIndex` (statement to site, per function)
// and the unit's initial ledger rows, in which every facet is undecided.
// `LedgerAdapter::finish` later fills whatever the engine did not decide
// with the §2.6 defaults, so the ledger is complete by construction.
//
// Emitted functions are the definitions outside system headers whose body
// CodeGen may emit: those `ASTContext::DeclMustBeEmitted` accepts, used
// internal-linkage definitions, and C99 `inline` and `gnu_inline`
// definitions (`available_externally`).
//
// Site kinds, and what makes a site (§2.1):
//
//   deref       *p, p->f, p[0] (a pointer base, a zero constant index)
//   index       e[i] otherwise, *(p + i), *(p - i)
//   ptr-arith   p + i, p - i, ++p, p++, p += i, &p[i] flowing directly into
//               a required position (§7.4)
//   cast        a widening pointer conversion flowing directly into a
//               required position; a store into a slot with a declared kind
//   int-to-ptr  a conversion from an integer (not a null pointer constant);
//               va_arg of a pointer type
//   lib-call    a call a LibrarySpec row governs, without a release effect
//   release     a call whose row releases or reallocates an argument
//   call        every other call (boundary call); every return, the end of
//               the body when control can reach it, and every call that
//               does not return (boundary exit)
//   assume      WEAVEC_ASSUME(e)
//   raw         a dereference, subscript or release through a pointer with
//               a raw origin visible in the syntax: declared WEAVEC_RAW,
//               converted from an integer, loaded through such a pointer,
//               or returned by a function whose result is WEAVEC_RAW
//
// Exclusions: `&*p`, `&((T *)0)->f`, unevaluated operands (`sizeof` and
// `_Alignof` of anything but a variable-length array, unselected `_Generic`
// and `__builtin_choose_expr` branches, the operands of
// `__builtin_constant_p` and `__builtin_object_size`), and the bodies of
// blocks. Initialiser lists are walked in their semantic form, and sites are
// deduplicated by statement, so shared subtrees count once. Sites in
// constant expressions (static local initialisers, case labels), in the
// shared operand of `a ?: b` or another `OpaqueValueExpr` source, and on
// pointers into a non-default address space are marked, because no check
// can serve them.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_SITECOLLECTOR_H
#define WEAVEC_ANALYSIS_SITECOLLECTOR_H

#include "weavec/Analysis/CheckWitness.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace weavec::analysis {

/// A requirement of a call-like site on one argument, from the callee's
/// declared kinds or its `LibrarySpec` row.
struct ArgumentNeed {
  std::uint8_t argument = 0;
  /// The null facet: the argument must not be null ...
  bool nonnull = false;
  /// ... unless a length is zero (`null-if-zero`, §8.3) ...
  bool allowedIfZero = false;
  /// ... namely this term over the call's arguments; none when the row's
  /// term has no C spelling at the call (the check is then inexpressible).
  std::optional<WitnessTerm> unlessZero = std::nullopt;
  /// The requirement rests only on system-header attributes (§7.2 level
  /// 4): its facet is `trusted(system-api)`.
  bool systemApi = false;
};

/// What `SiteCollector` knows about one site before the engine runs.
struct SiteInfo {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::SiteId id = {};
  core::SiteKind kind = core::SiteKind::Deref;
  /// Call sites only.
  std::optional<core::Boundary> boundary = std::nullopt;
  /// The statement the site stands for: the dereference, subscript, member
  /// access, arithmetic, conversion, store or call expression; the `return`
  /// statement; or the function body (the end of the body).
  const clang::Stmt *stmt = nullptr;
  /// The pointer the null and spatial facets are about: the dereferenced
  /// pointer, the subscript's base, the released argument or the callee
  /// operand of an indirect call. Null when there is none.
  const clang::Expr *operand = nullptr;
  /// Index sites: the subscript.
  const clang::Expr *index = nullptr;
  /// Call-like sites: the direct callee.
  const clang::FunctionDecl *callee = nullptr;
  /// LibCall, Release (and a Raw release): the row that governs the call.
  std::optional<core::LibraryMatch> library = std::nullopt;
  /// PtrArith and Cast sites: the kind of the required position.
  std::optional<KindEntry> required = std::nullopt;
  /// Call-like sites: the arguments with a null requirement.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<ArgumentNeed> arguments = {};
  /// Call sites: the arguments whose parameter has a declared shape (§7.2),
  /// with the kind and the parameter's pointee.
  struct DeclaredShape {
    std::uint8_t argument = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    core::PointerKind kind = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    clang::QualType pointee = {};
  };
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<DeclaredShape> declaredShapes = {};
  /// The expansion range of the site's source text.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  clang::SourceLocation begin = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  clang::SourceLocation end = {};
  /// Inside a `WEAVEC_UNSAFE` function or block (§6.1).
  bool inUnsafe = false;
  /// Inside the shared operand of `a ?: b` or another `OpaqueValueExpr`
  /// source: no check can replace the operand in place (§2.1).
  bool sharedOperand = false;
  /// The pointer points into a non-default address space (§2.1).
  bool nonDefaultAddressSpace = false;
  /// In a constant expression: decided statically, never checked (§2.1).
  bool constantExpression = false;
  /// The null facet rests only on system-header attributes (§5.2).
  bool nullSystemApi = false;
  /// The spatial facet rests only on system-header attributes (§5.2).
  bool spatialSystemApi = false;
  /// The spatial facet is proven by the types alone: a constant index
  /// inside a non-flexible array, or a dereference of an object's address
  /// (§5.3, §5.4 keep these proven).
  bool provenByType = false;
  /// §2.6: the spatial facet defaults to `checked` against these witnesses
  /// (extents exact from the type, or declared over constants and
  /// parameters never assigned or address-taken, one per requirement of a
  /// call); empty when it defaults to `unresolved`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<CheckWitness> spatialDefaults = {};

  [[nodiscard]] bool spatialCheckable() const noexcept {
    return !spatialDefaults.empty();
  }
};

/// Every site of the unit, by function and by statement.
class SiteIndex {
public:
  struct FunctionSites {
    const clang::FunctionDecl *decl = nullptr;
    /// The function's index in `UnitLedger::functions`.
    std::uint32_t index = 0;
    /// `WEAVEC_UNSAFE` on the function (§6.1).
    bool unsafe = false;
    /// The function calls a returns-twice function (§5.4).
    bool callsSetjmp = false;
    /// The expansion range of the definition.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    clang::SourceLocation begin = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    clang::SourceLocation end = {};
    /// By ordinal.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::vector<SiteInfo> sites = {};
  };

  [[nodiscard]] const std::vector<FunctionSites> &functions() const noexcept {
    return functionList;
  }
  /// The emitted function `decl` (any redeclaration), or null.
  [[nodiscard]] const FunctionSites *
  function(const clang::FunctionDecl &decl) const;
  [[nodiscard]] const SiteInfo *info(core::SiteId id) const;

  /// Every site `stmt` stands for, in ordinal order: usually one; a call to
  /// a function that does not return also has its exit site; a value
  /// stored into a slot with a declared kind by an initialiser list may
  /// also be a Cast site.
  [[nodiscard]] llvm::ArrayRef<core::SiteId>
  sitesOf(const clang::Stmt &stmt) const;
  /// The first site of `stmt` that is not the exit of a call that does not
  /// return; for a `return` statement or a function body, its exit site.
  [[nodiscard]] std::optional<core::SiteId> find(const clang::Stmt &stmt) const;
  /// The site of `stmt` of kind `kind` (and, for Call sites, boundary).
  [[nodiscard]] std::optional<core::SiteId>
  find(const clang::Stmt &stmt, core::SiteKind kind,
       std::optional<core::Boundary> boundary = std::nullopt) const;
  /// The exit site of a `return` statement, a function body or a call that
  /// does not return.
  [[nodiscard]] std::optional<core::SiteId>
  findExit(const clang::Stmt &stmt) const;

  /// The innermost site of the function that contains `loc`, whose range
  /// contains `loc` and to which `facet` applies (when given).
  [[nodiscard]] std::optional<core::SiteId>
  innermostAt(clang::SourceLocation loc, std::optional<core::Facet> facet,
              const clang::SourceManager &sm) const;
  /// The emitted function whose definition contains `loc`, or null.
  [[nodiscard]] const FunctionSites *
  functionAt(clang::SourceLocation loc, const clang::SourceManager &sm) const;

  /// The unit's ledger rows before any decision: every site with the
  /// facets that apply to it, all undecided.
  [[nodiscard]] const std::vector<core::FunctionLedger> &
  ledgers() const noexcept {
    return rows;
  }
  [[nodiscard]] std::size_t siteCount() const noexcept;

private:
  friend class SiteCollector;
  std::vector<FunctionSites> functionList;
  std::vector<core::FunctionLedger> rows;
  llvm::DenseMap<const clang::Stmt *, llvm::SmallVector<core::SiteId, 1>>
      byStmt;
  llvm::DenseMap<const clang::FunctionDecl *, std::uint32_t> byFunction;
};

/// Enumerates the sites of every emitted function of a unit.
class SiteCollector {
public:
  SiteCollector(clang::ASTContext &ctx, const KindTable &kindTable,
                const core::LibrarySpec &spec)
      : context(ctx), kinds(kindTable), library(spec) {}

  /// §2.6: CodeGen may emit `function`'s body in some form. `function`
  /// must be a definition.
  [[nodiscard]] bool isEmitted(const clang::FunctionDecl &function);
  /// Every emitted function of the unit, in source order.
  [[nodiscard]] std::vector<const clang::FunctionDecl *> emittedFunctions();
  /// The sites of every emitted function.
  [[nodiscard]] SiteIndex collect();

private:
  clang::ASTContext &context;
  const KindTable &kinds;
  const core::LibrarySpec &library;
};

/// Whether `call` is to a function that does not return: declared
/// `noreturn` (or `_Noreturn`), through a `noreturn` function type, or a
/// `LibrarySpec` row that is `noreturn` or `exits`.
[[nodiscard]] bool callDoesNotReturn(const clang::CallExpr &call,
                                     const core::LibrarySpec &library);

/// §5.2: whether `function` is declared in a C library, POSIX or platform
/// header: a system header under the toolchain's resource directory, or one
/// the `LibrarySpec` header list names (on Darwin, every header of the SDK).
[[nodiscard]] bool isPlatformDeclaration(const clang::FunctionDecl &function,
                                         const core::LibrarySpec &library,
                                         const clang::SourceManager &sm);

/// Whether `function` is a returns-twice function (§5.4): `returns_twice`,
/// a `LibrarySpec` row marked `returns-twice`, or `__builtin_setjmp`.
[[nodiscard]] bool isReturnsTwice(const clang::FunctionDecl &function,
                                  const core::LibrarySpec &library);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_SITECOLLECTOR_H
