//===- AttributeReader.h - Declared pointer kinds (RFC 0030) ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.2: the kinds programmers declare, read from the annotations
// and attributes that exist today:
//
//   WEAVEC_SIZED_BY(n)           counted(n); sized(n) for void and character
//                                pointees (parameters: a sibling parameter;
//                                fields: a sibling field)
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
// The level of each source decides precedence (§7.2): WEAVEC_* annotations,
// then ecosystem attributes outside system headers, then the `LibrarySpec`
// entry, then attributes in system headers. Shape and nullability are
// resolved separately. A conflict within one level is recorded as a
// `KindProblem` and the weaker kind (their join) is used; so is a
// `WEAVEC_SIZED_BY` name that resolves to nothing, whose kind is dropped.
//
// `WEAVEC_COUNTED_BY`, `WEAVEC_ENDED_BY`, `WEAVEC_STRING` and
// `WEAVEC_REQUIRE_SAFE` arrive with stage S6: each is one more case in
// `readAnnotations`, which already produces every shape of the lattice.
// Ownership attributes (`malloc`, `ownership_*`) are contracts, not kinds;
// the unknown-callee rules read them (§5.1).
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
