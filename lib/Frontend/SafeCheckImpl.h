//===- SafeCheckImpl.h - What Safe C's checkers share ----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The rules of RFC 0041 and RFC 0042 (SafeCheck.cpp) and ownership's (RFC
// 0043, SafeOwnership.cpp) read the same annotations and report the same
// way.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_FRONTEND_SAFECHECKIMPL_H
#define WEAVEC_LIB_FRONTEND_SAFECHECKIMPL_H

#include "weavec/Frontend/SafeCheck.h"

#include "clang/AST/Type.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/ArrayRef.h"

#include <functional>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class FunctionDecl;
} // namespace clang

namespace weavec::frontend {

/// One annotation of a pointer type.
struct TypeAnnotation {
  llvm::StringRef name;
  std::string argument;
};

/// The annotations on `type`'s own pointer level, through its sugar
/// (typedefs, attributes), and Clang's `counted_by` family on a field.
[[nodiscard]] std::vector<TypeAnnotation>
annotationsOf(clang::QualType type, const clang::ASTContext &ast);

[[nodiscard]] bool hasAnnotation(llvm::ArrayRef<TypeAnnotation> annotations,
                                 llvm::StringRef name);

/// An error of a rule, with a note: at `noteAt` when it is valid (where the
/// error's cause is), else at the error.
using RuleReport = std::function<void(
    clang::SourceLocation at, llvm::StringRef rule, llvm::StringRef message,
    clang::SourceLocation noteAt, llvm::StringRef note)>;

/// RFC 0043: ownership's rules (§3) for one safe function's body.
void checkOwnership(clang::ASTContext &ast, const clang::FunctionDecl &function,
                    SafeUnit &unit, const RuleReport &report);

} // namespace weavec::frontend

#endif // WEAVEC_LIB_FRONTEND_SAFECHECKIMPL_H
