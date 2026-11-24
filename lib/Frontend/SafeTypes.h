//===- SafeTypes.h - One of several types in safe code ----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0054's checker part (SafeTypes.cpp): tagged unions, the checked
// conversions, the layouts of the unit's types, and the marks that carry
// them to the guard passes. Not installed.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_FRONTEND_SAFETYPES_H
#define WEAVEC_LIB_FRONTEND_SAFETYPES_H

#include "SafeCheckImpl.h"
#include "weavec/Frontend/SafeCheck.h"

#include "clang/AST/Type.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clang {
class ASTContext;
class BinaryOperator;
class CastExpr;
class CXXNewExpr;
class CXXRecordDecl;
class CallExpr;
class ClassTemplateDecl;
class Expr;
class FieldDecl;
class FunctionDecl;
class MangleContext;
class MemberExpr;
class RecordDecl;
class SourceLocation;
class TranslationUnitDecl;
class VarDecl;
} // namespace clang

namespace weavec::frontend {

/// §3.1: a union field of a struct with WEAVEC_TAGGED_BY. `problem` is the
/// `bad-annotation` the declaration gets, when it has one.
struct TaggedUnion {
  const clang::FieldDecl *field = nullptr;
  const clang::RecordDecl *record = nullptr;
  const clang::FieldDecl *tag = nullptr;
  std::string tagName;
  std::uint64_t mask = ~std::uint64_t{0};
  std::uint64_t tagOffset = 0;
  unsigned tagWidth = 0;
  std::uint64_t unionOffset = 0;
  std::uint64_t unionSize = 0;
  /// Each member with WEAVEC_WHEN, and its values (masked).
  llvm::DenseMap<const clang::FieldDecl *, std::vector<std::uint64_t>> when;
  std::string problem;
};

/// The tagged union `field` is, if it is one.
[[nodiscard]] std::optional<TaggedUnion>
taggedUnion(const clang::FieldDecl &field);
/// The union field whose tag `field` is, if it is one.
[[nodiscard]] const clang::FieldDecl *tagOf(const clang::FieldDecl &field);
/// Whether `member` uses a member of a tagged union (its base selects one).
[[nodiscard]] bool taggedMember(const clang::MemberExpr &member);

/// §2.1: a conversion a safe function makes as a check.
struct Conversion {
  enum class Form : std::uint8_t {
    /// (a) `void *` to a pointer to a struct, union or class.
    Void,
    /// (b) a base to a derived class (C++), a struct to a struct that
    /// begins with it (C).
    Base,
    /// (c) C's container_of.
    Container,
    /// (d) `void *` to a pointer to a plain scalar.
    Scalar,
  };
  Form form = Form::Void;
  clang::QualType target;
  clang::QualType source;
  /// The base's offset in the target (Base, C++).
  std::uint64_t delta = 0;
  bool polymorphic = false;
  bool reference = false;
  /// (c): the subtraction of the field's offset.
  const clang::BinaryOperator *arithmetic = nullptr;
};
[[nodiscard]] std::optional<Conversion>
checkedConversion(const clang::CastExpr &cast, const clang::ASTContext &ast);
/// §2.1: `D *` to the type of `D`'s first member (C), which needs no check.
[[nodiscard]] bool upcastToFirstMember(clang::QualType from,
                                       clang::QualType to);

/// §4: a function body that only makes a checked conversion and calls (a
/// `void *` adapter, kept guarded before this RFC).
[[nodiscard]] bool convertsAndCalls(const clang::FunctionDecl &function);

/// §1.1: the layouts the unit gives the runtime, in SafeUnit::layouts.
class TypeLayouts {
public:
  TypeLayouts(clang::ASTContext &ast, SafeUnit &unit);
  /// The layout of `type` (an array's: its element's), made once; its key,
  /// or empty when the type has none (void, incomplete, a function).
  std::string layoutOf(clang::QualType type);
  /// The layouts of the unit's allocated types and its globals.
  void collect(clang::TranslationUnitDecl &unitDecl);
  /// One site of `collect`'s walk, for a walk that is made anyway.
  void site(const clang::CXXNewExpr &expression);
  void site(const clang::CastExpr &cast);
  void site(const clang::CallExpr &call);
  void site(const clang::VarDecl &variable);
  /// The symbol of a global, as the code generator names it.
  std::string symbolOf(const clang::VarDecl &variable);
  /// The mangled name of a class's type information (`_ZTI…`).
  std::string typeInformation(clang::QualType type);

  clang::ASTContext &ast;
  SafeUnit &unit;

private:
  [[nodiscard]] std::string keyOf(clang::QualType type) const;
  std::string shape(clang::QualType type, unsigned depth);
  std::uint64_t identityOf(clang::QualType type);
  void flatten(clang::QualType type, std::uint64_t offset, std::uint32_t stride,
               std::uint32_t elements, std::uint32_t flags, unsigned depth,
               SafeUnit::TypeLayout &out);
  llvm::DenseMap<const clang::Type *, std::string> shapes;
  llvm::DenseMap<const clang::Type *, std::uint64_t> identities;
  mutable llvm::DenseMap<const clang::Type *, std::string> keys;
  std::unique_ptr<clang::MangleContext> mangler;
};

/// §2.3: the marks of the unit's safe functions, which can be taken out
/// while the rules read the code and put back after.
class TypeMarks {
public:
  TypeMarks(clang::ASTContext &ast, SafeUnit &unit);
  ~TypeMarks();
  TypeMarks(const TypeMarks &) = delete;
  TypeMarks &operator=(const TypeMarks &) = delete;

  /// Marks one safe function's checked forms (once).
  void mark(clang::FunctionDecl &function);
  void undo();
  void redo();
  /// The layouts of the unit's allocated types and globals.
  void collect();
  /// The functions a walk the checker makes anyway found, marked after the
  /// rules (in place of a walk of `finish`'s own).
  std::vector<clang::FunctionDecl *> found;
  bool walked = false;

  [[nodiscard]] bool seen(const clang::Expr *node) const;
  /// The classes some class the unit has parsed derives from (canonical).
  const llvm::DenseSet<const clang::CXXRecordDecl *> &bases();
  void add(clang::Expr *node, clang::Expr *original, clang::Expr *replacement);
  clang::Expr *wrap(llvm::StringRef name, clang::Expr *expression,
                    std::uint64_t index);

  clang::ASTContext &ast;
  SafeUnit &unit;
  TypeLayouts layouts;
  /// §2.2: asks for a class's vtable, and so its type information, in the
  /// unit (Sema's `MarkVTableUsed`), while the unit is parsed.
  std::function<void(const clang::CXXRecordDecl &, clang::SourceLocation)>
      markVTable;

private:
  struct Rewrite {
    clang::Expr *node = nullptr;
    clang::Expr *original = nullptr;
    clang::Expr *replacement = nullptr;
    void apply() const;
    void undo() const;
  };
  clang::FunctionDecl *marker(llvm::StringRef name);
  clang::Expr *call(llvm::StringRef name, clang::Expr *pointer,
                    std::uint64_t index, clang::SourceLocation at);
  llvm::StringMap<clang::FunctionDecl *> markers;
  std::vector<Rewrite> rewrites;
  llvm::DenseSet<const clang::Expr *> rewritten;
  llvm::DenseSet<const clang::FunctionDecl *> marked;
  llvm::DenseSet<const clang::CXXRecordDecl *> knownBases;
  /// The last of the unit's top-level declarations `bases` has read.
  const clang::Decl *basesRead = nullptr;
  /// The class templates read, whose instances `bases` reads as they are
  /// made, and the instances read.
  std::vector<const clang::ClassTemplateDecl *> classTemplates;
  llvm::DenseSet<const clang::CXXRecordDecl *> instancesRead;
};

/// §3.2: what `union-pointer` and `bad-annotation` report.
using TypeRuleReport =
    std::function<void(clang::SourceLocation, llvm::StringRef rule,
                       const std::string &message, const std::string &note)>;
/// The rules of tagged unions over one safe function: no member's address,
/// no union apart from its tag, no member stored before its tag.
void checkTaggedUnions(clang::ASTContext &ast,
                       const clang::FunctionDecl &function,
                       const TypeRuleReport &report, FixPlanner *fixes);
/// §4, E9: the fix of a `union-pointer` error on the member `member` of a
/// union that has no tag: `WEAVEC_TAGGED_BY` and `WEAVEC_WHEN`, from the
/// tests of one of the struct's fields that every read of the union's
/// members in the unit is under (likely); the field's name in `tag`.
[[nodiscard]] std::optional<Want> tagWant(const clang::MemberExpr &member,
                                          clang::ASTContext &ast,
                                          std::string *tag);
/// The same rules over a global's initializer.
void checkTaggedExpression(clang::ASTContext &ast, const clang::Stmt &statement,
                           const TypeRuleReport &report);
/// `bad-annotation` for a struct's tagged unions.
void checkTaggedRecord(const clang::RecordDecl &record,
                       const TypeRuleReport &report);

} // namespace weavec::frontend

#endif // WEAVEC_LIB_FRONTEND_SAFETYPES_H
