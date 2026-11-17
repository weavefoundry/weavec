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
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class FunctionDecl;
class RecordDecl;
class SourceManager;
class Stmt;
class VarDecl;
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

/// RFC 0047 §6: whether `at` lies in a file that said WEAVEC_SAFE_FILE,
/// after it said so.
[[nodiscard]] bool inSafeFile(const SafeUnit &unit,
                              const clang::SourceManager &sources,
                              clang::SourceLocation at);

/// RFC 0043: ownership's rules (§3) for one safe function's body.
void checkOwnership(clang::ASTContext &ast, const clang::FunctionDecl &function,
                    SafeUnit &unit, const RuleReport &report);

/// RFC 0047 §1: what Safe C's bounds give Safe C++ (SafeCheck.cpp).
struct BoundsRules {
  /// The bounds a function's declarations give its parameters (without
  /// `this`) and its result.
  std::function<FunctionBounds(const clang::FunctionDecl &)> tableOf;
  /// §1.2: the rules of bounds over one safe C++ function; whether it uses
  /// bounds (indexes or steps a pointer, passes an array as one, names a
  /// member with a bound).
  std::function<bool(const clang::FunctionDecl &)> check;
  /// The bounds of a class's members name what they should.
  std::function<void(const clang::RecordDecl &)> checkRecord;
  /// RFC 0047 §7: the errors reported until the next call are counted in
  /// `counts` (a function's or a global's that is not safe), not printed;
  /// null ends that.
  std::function<void(std::map<std::string, unsigned> *)> survey;
};

/// RFC 0048 §2.2, §4: the local pointer an expression names; the values
/// each local pointer of a statement is given, and those stepped in place.
using LocalValues =
    llvm::DenseMap<const clang::VarDecl *, std::vector<const clang::Expr *>>;
const clang::VarDecl *localPointer(const clang::Expr *expression);
void localValues(const clang::Stmt *statement, LocalValues &values,
                 llvm::DenseSet<const clang::VarDecl *> *stepped = nullptr);

/// RFC 0048 §4, §5.1: marks a C++ function's kept locals with the implicit
/// `annotate("weavec.placed <type>")` the passes place them by and, in a
/// safe function, reports `escape` for any other kept address (`report`
/// null: marks only).
void keptAddresses(clang::ASTContext &ast, const clang::FunctionDecl &function,
                   bool safe, const RuleReport *report);

/// RFC 0044: Safe C++'s rules for a C++ unit, and its functions and
/// classes' members for the ledger.
void checkCxx(clang::ASTContext &ast, bool unitSafe, SafeUnit &unit,
              const RuleReport &report, const BoundsRules &bounds);

} // namespace weavec::frontend

#endif // WEAVEC_LIB_FRONTEND_SAFECHECKIMPL_H
