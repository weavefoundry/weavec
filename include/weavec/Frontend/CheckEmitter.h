//===- CheckEmitter.h - Checks inserted through Sema (RFC 0030) -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §10.6: `CheckEmitter` applies a unit's `core::CheckPlan` to the
// AST before the code generator sees any of it (`DeferredCodeGenConsumer`,
// §10.5), and lowers the §11 zero-initialisation rows. Every rewrite is
// built through Sema:
//
//   - a `DeclRefExpr` to the prelude helper (§10.2), or to an `extern`
//     declaration of it when the prelude was not injected (§10.9);
//   - `Sema::BuildCallExpr(nullptr, ...)`, which converts the arguments;
//   - `Sema::ImpCastExprToType(..., CK_BitCast)` back to the operand's
//     pointer type, so that a dereference of the result is still an lvalue;
//   - `Sema::BuildBinOp(nullptr, ..., BO_Comma, ...)` for `BeforeCall`.
//
// Placements (§10.4), for an operand `p`, a subscript `i`, a call `f(a)`:
//
//   WrapOperand    p        -> (T *)chk(p, ...)   or (chk(...), p)
//   WrapIndex      i        -> chk(i, n)
//   WrapArgument   f(a)     -> f((A)chk(a, ...))
//   ReplaceAccess  p[i]     -> *(T *)__weavec_chk_span(p, i, base, bytes, w)
//   BeforeCall     f(a)     -> (chk(...), f(a)), or (g ? chk(...) : 0, f(a))
//   ReplaceCall    assume   -> __weavec_chk_assert(c)
//                  sprintf  -> __weavec_chk_len_r(snprintf(d, have, ...), have)
//
// On one operand `nonnull` is innermost, then `index`. The unconditional
// trap of a lowered violation (§3.4) is `__weavec_chk_violation()`, before
// the operation. In report mode every check helper also receives the site's
// file, line and column; verify-mode checks of proven facets call the
// `__weavec_prv_*` family.
//
// Each rewrite runs inside a `Sema::TentativeAnalysisScope`, with an SFINAE
// trap and a `DiagnosticErrorTrap`. It is rejected when the result is
// invalid or contains errors, or when an error was diagnosed; the tree is
// then left as it was (a rewrite is built completely before it replaces
// anything, and moved operands are wrapped in parentheses so that Sema's
// conversions never modify them in place), and the internal error of §10.6
// fails the compile. A replacement goes back into the tree through the
// parent's child slot on the semantic form, which is what the code
// generator reads.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_CHECKEMITTER_H
#define WEAVEC_FRONTEND_CHECKEMITTER_H

#include "weavec/Core/CheckPlan.h"
#include "weavec/Core/Ledger.h"

#include "clang/AST/Type.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace clang {
class ASTContext;
class Sema;
} // namespace clang

namespace weavec::analysis {
class PlaceHandleTable;
class SiteIndex;
struct PlannedLedger;
} // namespace weavec::analysis

namespace weavec::frontend {

struct ZeroInitPlan;

struct CheckEmitterOptions {
  /// `-fweavec-checks`: trap, report or verify (none emits nothing).
  core::ChecksMode mode = core::ChecksMode::Trap;
  /// §10.9: the prelude was not injected (precompiled header or modules).
  /// The helpers are then declared `extern` in the AST and come from
  /// `libweavec_chk.a`, whose report helpers carry a `_report` suffix.
  bool externalHelpers = false;
};

/// The C type of one prelude helper, for its `extern` declaration (§10.9).
struct HelperSignature {
  enum class Type : std::uint8_t {
    Void,
    Int,
    Unsigned,
    LongLong,
    UnsignedLongLong,
    Size,
    VoidPointer,
    ConstVolatileVoidPointer,
    CharPointer,
    ConstCharPointer,
    CharPointerPointer,
    VoidPointerPointer,
    /// `void (*)(void)`.
    FunctionPointer,
    /// `void *(*)(size_t, size_t)`.
    AlignedAllocator,
    /// `int (*)(void **, size_t, size_t)`.
    PosixMemalignFunction,
  };
  llvm::StringLiteral name;
  Type result = Type::Void;
  /// At most five parameters; `Type::Void` ends the list.
  Type params[5] = {Type::Void, Type::Void, Type::Void, Type::Void, Type::Void};
  /// A check helper: report mode appends (file, line, column).
  bool reports = false;
};

class CheckEmitter {
public:
  CheckEmitter(clang::Sema &sema, CheckEmitterOptions options = {});
  ~CheckEmitter();
  CheckEmitter(const CheckEmitter &) = delete;
  CheckEmitter &operator=(const CheckEmitter &) = delete;
  CheckEmitter(CheckEmitter &&) = delete;
  CheckEmitter &operator=(CheckEmitter &&) = delete;

  /// Applies every entry of the unit's plan. False when a rewrite was
  /// rejected: the internal error has been reported and the compile fails.
  bool emit(const analysis::PlannedLedger &planned);
  /// The same over the parts of a planned ledger; `unit` gives the sites'
  /// locations and texts, and may be null (report mode then uses the
  /// statements' locations).
  bool emit(const core::CheckPlan &plan, const analysis::SiteIndex &sites,
            const analysis::PlaceHandleTable &handles,
            const core::UnitLedger *unit);
  /// Applies the §11 zero-initialisation rewrites. False after an internal
  /// error.
  bool lowerZeroInit(const ZeroInitPlan &plan);

  /// Plan entries applied so far.
  [[nodiscard]] std::size_t checksInserted() const noexcept;
  /// Zero-initialisation rewrites applied so far.
  [[nodiscard]] std::size_t zeroInitRewrites() const noexcept;

  /// Every helper the rewrites can call, with its C signature: what the
  /// `extern` declarations of §10.9 are built from. A unit test compares
  /// it with the prelude.
  [[nodiscard]] static llvm::ArrayRef<HelperSignature> helperSignatures();

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

/// The signature of the helper `name` (without a `_report` suffix), or
/// null.
[[nodiscard]] const HelperSignature *findHelperSignature(llvm::StringRef name);

/// The function type of `helper` in `context`; with `report`, a check
/// helper also takes `(const char *file, unsigned line, unsigned column)`.
[[nodiscard]] clang::QualType helperFunctionType(clang::ASTContext &context,
                                                 const HelperSignature &helper,
                                                 bool report);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_CHECKEMITTER_H
