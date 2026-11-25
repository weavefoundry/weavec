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

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class CXXRecordDecl;
class Decl;
class FieldDecl;
class FunctionDecl;
class Preprocessor;
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

/// RFC 0049 §1: whose code the errors reported until the next owner are in.
struct ErrorOwner {
  std::string name;
  std::string file;
  unsigned line = 0;
  bool global = false;
  /// Code that is not safe yet, under the survey: recorded, not printed.
  bool surveyed = false;
  /// The function, for the fixes that read its body.
  const clang::FunctionDecl *function = nullptr;
};

/// RFC 0049 §3: what an error wants of a declaration, said just before the
/// error is reported (at the same place); its fixes come from it.
struct Want {
  enum class Kind : std::uint8_t {
    /// A bound on a pointer: `declaration`, or the one `expression` (a
    /// local's value) comes from (index-single, call-args).
    Bound,
    /// The value is the end of its own bound: `WEAVEC_COUNTED_BY(0)` on the
    /// place it goes, `declaration` (C3).
    End,
    /// A parameter, or `this` (`declaration` a member function), is kept
    /// (C1); `keeper`, the member it is stored in (D5).
    Escapes,
    /// A global that never changes (C4).
    Const,
    /// A class changed through pointers other objects keep: a choice.
    Aliased,
    /// What a function's result borrows from: its parameter (E7).
    Lifetime,
    /// The declarations of a function, `declaration` and `subjects[0]`
    /// (an earlier one, or the function an override overrides), disagree:
    /// what each states is written on the other (C2).
    Agree,
    /// No fix: the declaration (or the one `expression` comes from) is the
    /// error's subject, for the fixer to withdraw a fix by (§6).
    Subject,
    /// RFC 0051 §4.1: the text of `removal`, which only asserts, taken out
    /// (certain).
    Remove,
    /// RFC 0054 §4: a fix the rule spells itself: `insertions`, and the text
    /// of `removal` taken out when it is valid, at `level`, on
    /// `declaration` (`aspect`, `fact` and `description` say what it is).
    Write,
  };
  Kind kind = Kind::Bound;
  const clang::Decl *declaration = nullptr;
  const clang::Expr *expression = nullptr;
  const clang::FieldDecl *keeper = nullptr;
  /// Other declarations the rule read.
  std::vector<const clang::Decl *> subjects;
  /// Remove: the characters to take out, and what goes in their place.
  // A designated initializer may leave it out.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  clang::CharSourceRange removal = {};
  // A designated initializer may leave it out.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::string replacement = {};
  /// Write: what goes where; whether the text spells WeaveC's macros.
  // NOLINTBEGIN(readability-redundant-member-init)
  std::vector<std::pair<clang::SourceLocation, std::string>> insertions = {};
  std::string level = {};
  std::string aspect = {};
  std::string fact = {};
  std::string description = {};
  // NOLINTEND(readability-redundant-member-init)
  bool macros = false;
};

class FixPlanner;

/// RFC 0047 §6: whether `at` lies in a file that said WEAVEC_SAFE_FILE,
/// after it said so.
[[nodiscard]] bool inSafeFile(const SafeUnit &unit,
                              const clang::SourceManager &sources,
                              clang::SourceLocation at);

/// RFC 0051 §4 (SafeUndefined.cpp): `assumed`, `signal-handler`, and the
/// `bool` cases of `pointer-bytes` and `union-pointer`, for one safe
/// function, C or C++.
void checkUndefined(
    clang::ASTContext &ast, const clang::FunctionDecl &function,
    const std::function<void(clang::SourceLocation, llvm::StringRef,
                             llvm::StringRef, llvm::StringRef)> &report,
    FixPlanner *fixes);

/// RFC 0051 §4.2: whether `type` holds a `bool` at any depth of its own
/// bytes (not behind a pointer).
[[nodiscard]] bool holdsBool(clang::QualType type, unsigned depth = 0);

/// RFC 0051 §8: the calls of `function` that register a signal handler
/// (the table's `handler` rows), for the audit of code that is not safe.
[[nodiscard]] unsigned countHandlers(const clang::FunctionDecl &function);

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
  /// RFC 0049 §1 (RFC 0047 §7): whose code the errors reported until the
  /// next call are in; null ends that.
  std::function<void(const ErrorOwner *)> owner;
  /// RFC 0049 §3: the unit's fix planner.
  FixPlanner *fixes = nullptr;
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
                   bool safe, const RuleReport *report,
                   FixPlanner *fixes = nullptr);

/// RFC 0044: Safe C++'s rules for a C++ unit, and its functions and
/// classes' members for the ledger.
void checkCxx(clang::ASTContext &ast, bool unitSafe, SafeUnit &unit,
              const RuleReport &report, const BoundsRules &bounds);

/// RFC 0049: the fixes of a unit's errors. A rule says what it wants just
/// before it reports; the report takes the want, and the planner turns it
/// into fixes, from what the declarations and the functions that use them
/// show (§3), and into the note's text and its fix-its (§2).
class FixPlanner {
public:
  FixPlanner(clang::ASTContext &ast, const clang::Preprocessor *preprocessor);
  ~FixPlanner();
  FixPlanner(const FixPlanner &) = delete;
  FixPlanner &operator=(const FixPlanner &) = delete;

  /// What the error reported next at `at` wants.
  void want(clang::SourceLocation at, Want wanted);
  /// A note with its fix-its: where, the text, and the insertions.
  struct Note {
    clang::SourceLocation at;
    std::string text;
    std::vector<std::pair<clang::SourceLocation, std::string>> insertions;
    /// RFC 0051 §4.1: the characters a removal takes out.
    // A designated initializer may leave it out.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<clang::CharSourceRange> removals = {};
  };
  /// Fills the fixes and subjects of the error reported at `at` from the
  /// want said for it (else, in `function`, from the error's message); the
  /// note to print with it, if any fix was found.
  std::optional<Note> attach(clang::SourceLocation at, RuleError &error,
                             const clang::FunctionDecl *function);
  /// C2: the bounds `other` gives that `first` (an earlier declaration of
  /// the same function) does not, written on `other`, and back.
  std::optional<Note> agree(const clang::FunctionDecl &first,
                            const clang::FunctionDecl &other, RuleError &error);
  /// §5: what this unit's evidence says of the wanted declarations.
  void evidence(const std::vector<std::string> &wants,
                std::vector<RuleFix> &out);
  /// §6: the project's files the unit read.
  void files(std::vector<ReadFile> &out) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace weavec::frontend

#endif // WEAVEC_LIB_FRONTEND_SAFECHECKIMPL_H
