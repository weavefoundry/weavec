//===- Prelude.h - The check prelude ---------------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030, sections 10.2 and 11: the helpers the check rewrites call.
//
// The inline form is C text for the predefines buffer (or `-include`): only
// `static __inline__ __attribute__((always_inline, nodebug, unused))`
// functions, under a pragma that silences -Weverything. It uses no macros,
// because `__SIZE_TYPE__` and friends are not expanded in preprocessed `.i`
// input; sizes are `__typeof__(sizeof 0)` and 64-bit values `unsigned long
// long` or `long long`; it calls only builtins and, for zero-initialisation,
// declares the target's usable-size query. It compiles under
// `-std=c89 -pedantic-errors -Weverything -Werror` and every later standard.
//
// Helper families, by mode:
//
//   trap    __weavec_chk_*: trap with category "weavec" and the template
//           name as reason (`__builtin_verbose_trap`, or `__builtin_trap`).
//   report  the same names with three trailing arguments
//           (const char *file, unsigned line, unsigned column); a failure
//           calls __weavec_rt_report (runtime/weavec_rt.c) and returns.
//   verify  trap's family plus __weavec_prv_*, identical but for the
//           category "weavec.proven", for checks of proven facets.
//   none    no helpers and no zero-initialisation: an empty prelude.
//
// Every mode but none also has the term helpers (need/have arithmetic over
// 64 bits, saturating toward failure) and, with zero-initialisation on and a
// usable-size query, the section 11 allocation wrappers.
//
// The out-of-line form is the same helpers as external definitions, for
// runtime/weavec_chk*.c (section 10.9, precompiled headers and modules).
// There one archive holds every mode, so the report helpers are named with a
// `_report` suffix; the trap is the `WEAVEC_CHK_TRAP(category, reason)` macro
// and the usable-size query the `WEAVEC_CHK_USABLE(p)` macro, both defined by
// the including file.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_PRELUDE_H
#define WEAVEC_FRONTEND_PRELUDE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <optional>
#include <string>

namespace weavec::frontend {

/// `-fweavec-checks=` (RFC 0030, section 10.7).
enum class CheckMode : std::uint8_t { Trap, Report, Verify, None };

/// A target C library's usable-size query (RFC 0030, section 11).
enum class UsableSizeQuery : std::uint8_t {
  /// None known: the allocation family is not zero-initialised.
  None,
  /// Darwin: `size_t malloc_size(const void *)`.
  MallocSize,
  /// glibc and musl: `size_t malloc_usable_size(void *)`.
  MallocUsableSize,
  /// Bionic and FreeBSD: `size_t malloc_usable_size(const void *)`.
  MallocUsableSizeConst,
};

enum class PreludeForm : std::uint8_t {
  /// Static always-inline helpers, for the predefines buffer.
  Inline,
  /// External definitions, for runtime/weavec_chk*.c.
  OutOfLine,
};

struct PreludeOptions {
  CheckMode mode = CheckMode::Trap;
  /// Section 11; ignored in mode none, which never zero-initialises.
  bool zeroInit = true;
  /// The inline form's usable-size query (see `usableSizeQueryFor`).
  UsableSizeQuery usableSize = UsableSizeQuery::None;
  PreludeForm form = PreludeForm::Inline;
  /// `__builtin_verbose_trap`; false for compilers that lack it, which get
  /// `__builtin_trap()`. The out-of-line form always uses WEAVEC_CHK_TRAP.
  bool verboseTrap = true;
};

/// The prelude text for `options`.
std::string buildCheckPrelude(const PreludeOptions &options);

/// The usable-size query of `triple`'s C library.
UsableSizeQuery usableSizeQueryFor(const llvm::Triple &triple);

std::optional<CheckMode> parseCheckMode(llvm::StringRef name);
llvm::StringRef checkModeName(CheckMode mode);

/// Every template a trap reason or a report can name: `nonnull`, `index`,
/// `span`, `len`, `disjoint`, `assert` and `violation`.
llvm::ArrayRef<llvm::StringLiteral> checkTemplates();

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_PRELUDE_H
