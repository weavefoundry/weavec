//===- AttributeReader.h - Declared pointer kinds (RFC 0030) ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.2: the kinds programmers declare, from every source of the
// table:
//
//   WEAVEC_COUNTED_BY(n),        counted(n); sized(n) for void and character
//   WEAVEC_SIZED_BY(n)           pointees (synonyms; parameters and fields)
//   WEAVEC_ENDED_BY(q)           ended-by(q) (parameters and fields)
//   WEAVEC_STRING                nul-terminated (parameters, fields, results)
//   WEAVEC_NONNULL/_NULLABLE     nullability (parameter, field, result,
//                                variable)
//   counted_by, sized_by,        counted/sized(n) on fields (and parameters
//   ..._or_null                  where Clang accepts them), nonnull unless
//                                _or_null
//   alloc_size(i[, j])           result sized(param i [* param j])
//   nonnull, returns_nonnull,    nullability
//   _Nonnull, _Nullable
//   T p[static N]                counted(N) nonnull
//   T p[n] (a VLA parameter)     counted(n) nullable
//
// The `WEAVEC_*` extent macros are resolved by name: the argument names any
// sibling parameter (in any position) or sibling field, an integer for a
// count and a pointer for an end. A name that resolves to nothing, or to
// the wrong type, is a `KindProblem` (an `invalid-annotation` warning) and
// the kind is dropped. A constant `T p[N]` without `static` is no
// requirement; it yields a `KindSuggestion` (a fix-it adding `static`).
//
// The level of each source decides precedence (§7.2): WEAVEC_* annotations,
// then ecosystem attributes outside system headers, then the `LibrarySpec`
// entry, then attributes in system headers. Shape and nullability are
// resolved separately. A conflict within one level is recorded as a
// `KindProblem` and the weaker kind (their join) is used.
//
// `WEAVEC_REQUIRE_SAFE` on any declaration of a function marks it in the
// table (§6.3); on anything else it is a `KindProblem`. The ownership
// attributes (`malloc`, `ownership_returns`, `ownership_takes`,
// `ownership_holds`) are contracts, not kinds: they become the function's
// `OwnershipContract`, which the unknown-callee rules (§5.1) read.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_ATTRIBUTEREADER_H
#define WEAVEC_ANALYSIS_ATTRIBUTEREADER_H

#include "weavec/Analysis/KindTable.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

namespace weavec::analysis {

class AttributeReader {
public:
  AttributeReader(clang::ASTContext &ctx, const core::LibrarySpec &spec)
      : context(ctx), library(spec) {}

  /// Reads every function the unit defines or references, every field of a
  /// record the unit declares, and every pointer variable, into a new table.
  [[nodiscard]] KindTable read();

  /// The declared kinds of `function`'s parameters and result, over all its
  /// redeclarations, and whether a `LibrarySpec` entry governs it.
  void readFunction(const clang::FunctionDecl &function, KindTable &table);
  /// The declared kind of a pointer (or array) field.
  void readField(const clang::FieldDecl &field, KindTable &table);
  /// The declared nullability of a pointer variable.
  void readVariable(const clang::VarDecl &variable, KindTable &table);

private:
  clang::ASTContext &context;
  const core::LibrarySpec &library;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_ATTRIBUTEREADER_H
