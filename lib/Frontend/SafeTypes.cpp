//===- SafeTypes.cpp - One of several types in safe code -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0054, the checker's part. Three forms that pick one of several types
// are checked forms in a safe function, each checked where the type is
// chosen:
//
//   - a tagged union: a union in a struct says which of the struct's fields
//     is its tag (WEAVEC_TAGGED_BY) and each member which values of the tag
//     it is for (WEAVEC_WHEN); a member is used after a comparison of the
//     tag, a store of the tag clears the union, and no address of a member
//     is taken (§3);
//   - a conversion from `void *`, from a struct to a struct that begins
//     with it (C), from a base to a derived class (C++), and C's
//     `container_of`: a question to the runtime, is an object of the target
//     type there (§2);
//   - the layouts the runtime answers from, of the unit's allocated types,
//     its globals and the locals of its safe functions whose address leaves
//     them (§1).
//
// The checker marks each checked form in the function's code before the
// code is made, with a call of a marker GuardInsert expands
// (`__weavec.cast`, `__weavec.member`, `__weavec.tag`). The marks are taken
// out again while the rules read the function, which see the code as it
// was written, and put back after them, for the functions whose code is
// made at the unit's end.
//
//===----------------------------------------------------------------------===//

#include "SafeTypes.h"

#include "SafeCheckImpl.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/InferAlloc.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/xxhash.h"

#include <memory>
#include <optional>
#include <string>

namespace weavec::frontend {

/// An annotation's arguments, without the implicit wrappers Sema keeps.
static const clang::Expr *bare(const clang::Expr *expression) {
  while (expression != nullptr) {
    if (const auto *constant = llvm::dyn_cast<clang::ConstantExpr>(expression))
      expression = constant->getSubExpr();
    else if (const auto *cast =
                 llvm::dyn_cast<clang::ImplicitCastExpr>(expression))
      expression = cast->getSubExpr();
    else if (const auto *paren = llvm::dyn_cast<clang::ParenExpr>(expression))
      expression = paren->getSubExpr();
    else
      break;
  }
  return expression;
}

static const clang::AnnotateAttr *annotation(const clang::Decl &declaration,
                                             llvm::StringRef name) {
  for (const auto *attribute :
       declaration.specific_attrs<clang::AnnotateAttr>())
    if (attribute->getAnnotation() == name)
      return attribute;
  return nullptr;
}

/// An integer literal as C writes one (with a base prefix and suffixes).
static std::optional<std::uint64_t> literal(llvm::StringRef text) {
  text = text.trim();
  while (!text.empty() && (text.back() == 'u' || text.back() == 'U' ||
                           text.back() == 'l' || text.back() == 'L'))
    text = text.drop_back();
  std::uint64_t value = 0;
  if (text.getAsInteger(0, value))
    return std::nullopt;
  return value;
}

/// Whether `type` holds a pointer, a reference, a member pointer or a
/// `bool` in its own bytes, at any depth.
static bool notPlain(clang::QualType type, unsigned depth = 0) {
  const clang::Type *canonical = type.getCanonicalType().getTypePtr();
  if (depth > 64)
    return true;
  if (canonical->isAnyPointerType() || canonical->isBlockPointerType() ||
      canonical->isMemberPointerType() || canonical->isReferenceType() ||
      canonical->isBooleanType())
    return true;
  if (const auto *array = llvm::dyn_cast<clang::ArrayType>(canonical))
    return notPlain(array->getElementType(), depth + 1);
  if (const auto *atomic = llvm::dyn_cast<clang::AtomicType>(canonical))
    return notPlain(atomic->getValueType(), depth + 1);
  if (const auto *record = canonical->getAsRecordDecl()) {
    const clang::RecordDecl *definition = record->getDefinition();
    if (definition == nullptr)
      return true;
    if (const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(definition)) {
      if (cxx->isDynamicClass())
        return true;
      for (const clang::CXXBaseSpecifier &base : cxx->bases())
        if (notPlain(base.getType(), depth + 1))
          return true;
    }
    return llvm::any_of(definition->fields(), [&](const clang::FieldDecl *f) {
      return notPlain(f->getType(), depth + 1);
    });
  }
  return false;
}

/// A one-byte type a pointer to which views bytes.
static bool byteType(clang::QualType type) {
  const clang::QualType canonical =
      type.getCanonicalType().getUnqualifiedType();
  return canonical->isCharType() ||
         canonical->isSpecificBuiltinType(clang::BuiltinType::UChar) ||
         canonical->isSpecificBuiltinType(clang::BuiltinType::SChar);
}

static const clang::RecordDecl *completeRecord(clang::QualType type) {
  const clang::RecordDecl *record = type->getAsRecordDecl();
  if (record == nullptr || type->isIncompleteType() || type->isDependentType())
    return nullptr;
  return record->getDefinition();
}

static bool allocationCall(const clang::Expr &expression) {
  const auto *call =
      llvm::dyn_cast<clang::CallExpr>(expression.IgnoreParenImpCasts());
  if (call == nullptr)
    return false;
  const clang::FunctionDecl *callee = call->getDirectCallee();
  if (callee == nullptr)
    return false;
  return callee->hasAttr<clang::AllocSizeAttr>() ||
         callee->getBuiltinID() == clang::Builtin::BI__builtin_operator_new;
}

//===-- Tagged unions (§3) ------------------------------------------------===//

std::optional<TaggedUnion> taggedUnion(const clang::FieldDecl &field) {
  const clang::QualType type = field.getType();
  const clang::RecordDecl *self = completeRecord(type);
  if (self == nullptr || !self->isUnion())
    return std::nullopt;
  const clang::AnnotateAttr *tagged = annotation(field, TaggedByAnnotation);
  if (tagged == nullptr)
    tagged = annotation(*self, TaggedByAnnotation);
  if (tagged == nullptr || tagged->args_size() == 0)
    return std::nullopt;
  TaggedUnion out;
  out.field = &field;
  out.record = field.getParent();
  const auto *text =
      llvm::dyn_cast<clang::StringLiteral>(bare(*tagged->args_begin()));
  if (text == nullptr) {
    out.problem = "'WEAVEC_TAGGED_BY' names no field";
    return out;
  }
  const auto [name, rest] = text->getString().split(',');
  out.tagName = name.trim().str();
  if (!rest.trim().empty()) {
    const auto mask = literal(rest);
    if (!mask) {
      out.problem = "the mask of 'WEAVEC_TAGGED_BY(" + text->getString().str() +
                    ")' is not an integer literal";
      return out;
    }
    out.mask = *mask;
  }
  const clang::ASTContext &ast = field.getASTContext();
  for (const clang::FieldDecl *other : out.record->fields())
    if (other->getName() == out.tagName)
      out.tag = other;
  if (out.tag == nullptr || out.tag->isBitField() ||
      !(out.tag->getType()->isIntegerType() ||
        out.tag->getType()->isEnumeralType())) {
    out.problem = "'WEAVEC_TAGGED_BY(" + out.tagName +
                  ")' names no integer field of '" +
                  out.record->getNameAsString() + "'";
    out.tag = nullptr;
    return out;
  }
  out.tagOffset = ast.getFieldOffset(out.tag) / 8;
  out.tagWidth = static_cast<unsigned>(
      ast.getTypeSizeInChars(out.tag->getType()).getQuantity());
  if (out.tagWidth > 8) {
    out.problem = "'" + out.tagName + "' is wider than 64 bits";
    return out;
  }
  // One tag, one union: its store clears the union it names.
  for (const clang::FieldDecl *other : out.record->fields())
    if (other != &field && other->getType()->getAsRecordDecl() != nullptr &&
        other->getType()->getAsRecordDecl()->isUnion())
      if (const clang::AnnotateAttr *again =
              annotation(*other, TaggedByAnnotation)) {
        const auto *otherText =
            llvm::dyn_cast<clang::StringLiteral>(bare(*again->args_begin()));
        if (otherText != nullptr &&
            otherText->getString().split(',').first.trim() == out.tagName) {
          out.problem = "'" + out.tagName + "' is the tag of two unions";
          return out;
        }
      }
  if (out.tagWidth < 8)
    out.mask &= (std::uint64_t{1} << (out.tagWidth * 8)) - 1;
  out.unionOffset = ast.getFieldOffset(&field) / 8;
  out.unionSize =
      static_cast<std::uint64_t>(ast.getTypeSizeInChars(type).getQuantity());
  for (const clang::FieldDecl *member : self->fields()) {
    const clang::AnnotateAttr *when = annotation(*member, WhenAnnotation);
    if (when == nullptr) {
      if (notPlain(member->getType()) && out.problem.empty())
        out.problem = "'" + member->getNameAsString() +
                      "' holds a pointer and has no 'WEAVEC_WHEN'";
      continue;
    }
    std::vector<std::uint64_t> &values = out.when[member];
    for (const clang::Expr *argument : when->args()) {
      clang::Expr::EvalResult result;
      if (!bare(argument)->EvaluateAsInt(result, ast)) {
        out.problem = "the values of 'WEAVEC_WHEN' on '" +
                      member->getNameAsString() + "' are not constants";
        continue;
      }
      values.push_back(
          static_cast<std::uint64_t>(result.Val.getInt().getExtValue()) &
          out.mask);
    }
  }
  // Two members for one value: only if neither can forge the other's
  // pointers or bools, or they are of one type.
  for (const auto &[a, valuesA] : out.when)
    for (const auto &[b, valuesB] : out.when) {
      if (a >= b || !out.problem.empty())
        continue;
      for (const std::uint64_t value : valuesA)
        if (llvm::is_contained(valuesB, value) &&
            (notPlain(a->getType()) || notPlain(b->getType())) &&
            !clang::ASTContext::hasSameUnqualifiedType(a->getType(),
                                                       b->getType()))
          out.problem = "'" + a->getNameAsString() + "' and '" +
                        b->getNameAsString() + "' are both for " +
                        std::to_string(value) + " and one holds a pointer";
    }
  return out;
}

const clang::FieldDecl *tagOf(const clang::FieldDecl &field) {
  for (const clang::FieldDecl *other : field.getParent()->fields())
    if (other != &field)
      if (const auto tagged = taggedUnion(*other);
          tagged && tagged->tag == &field && tagged->problem.empty())
        return other;
  return nullptr;
}

/// The member access `member` is of a tagged union's member: the union's
/// field, when `member`'s base selects one.
static const clang::MemberExpr *unionAccess(const clang::MemberExpr &member) {
  const auto *base = llvm::dyn_cast<clang::MemberExpr>(
      member.getBase()->IgnoreParenImpCasts());
  if (base == nullptr)
    return nullptr;
  const auto *field = llvm::dyn_cast<clang::FieldDecl>(base->getMemberDecl());
  if (field == nullptr)
    return nullptr;
  const auto tagged = taggedUnion(*field);
  return tagged && tagged->problem.empty() ? base : nullptr;
}

//===-- Checked conversions (§2.1)
//-----------------------------------------===//

/// Whether a `from` begins `to`: its first member, or its first member's
/// first member (C).
static bool beginsWith(clang::QualType to, clang::QualType from) {
  for (unsigned depth = 0; depth < 16; ++depth) {
    const clang::RecordDecl *record = completeRecord(to);
    if (record == nullptr || record->isUnion() || record->field_empty())
      return false;
    const clang::FieldDecl *first = *record->field_begin();
    if (first->isBitField())
      return false;
    if (clang::ASTContext::hasSameUnqualifiedType(first->getType(), from))
      return true;
    to = first->getType();
  }
  return false;
}

std::optional<Conversion> checkedConversion(const clang::CastExpr &cast,
                                            const clang::ASTContext &ast) {
  const bool cxx = ast.getLangOpts().CPlusPlus;
  const clang::Expr *sub = cast.getSubExpr();
  if (cast.getCastKind() == clang::CK_BaseToDerived && cxx) {
    const bool reference = cast.isGLValue();
    const clang::QualType target =
        reference ? cast.getType() : cast.getType()->getPointeeType();
    const clang::QualType source =
        reference ? sub->getType() : sub->getType()->getPointeeType();
    const auto *derived = target->getAsCXXRecordDecl();
    if (derived == nullptr || completeRecord(target) == nullptr)
      return std::nullopt;
    Conversion out;
    out.form = Conversion::Form::Base;
    out.target = target.getUnqualifiedType();
    out.source = source.getUnqualifiedType();
    out.reference = reference;
    // Read through the source's vtable (amendment 3), which only a
    // source with virtual functions has; a downcast from a class without
    // them asks the type records, as C's conversions do.
    const auto *from = source->getAsCXXRecordDecl();
    out.polymorphic = from != nullptr && from->hasDefinition() &&
                      from->getDefinition()->isPolymorphic();
    // The base's offset in the derived class, along the cast's path (from
    // the derived class down).
    const clang::CXXRecordDecl *at = derived->getDefinition();
    std::uint64_t delta = 0;
    for (const clang::CXXBaseSpecifier *base : cast.path()) {
      const auto *next = base->getType()->getAsCXXRecordDecl();
      if (next == nullptr || at == nullptr || base->isVirtual())
        return std::nullopt;
      delta += static_cast<std::uint64_t>(
          ast.getASTRecordLayout(at).getBaseClassOffset(next).getQuantity());
      at = next->getDefinition();
    }
    out.delta = delta;
    return out;
  }
  if (cast.getCastKind() != clang::CK_BitCast ||
      !cast.getType()->isPointerType() || !sub->getType()->isPointerType() ||
      // (Clang's test reads the context, by a reference that is not
      // `const`.)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      sub->isNullPointerConstant(const_cast<clang::ASTContext &>(ast),
                                 clang::Expr::NPC_ValueDependentIsNotNull))
    return std::nullopt;
  // C++: a `static_cast` or a C cast (not `reinterpret_cast`) from `void *`.
  if (cxx && !llvm::isa<clang::CXXStaticCastExpr, clang::CStyleCastExpr>(cast))
    return std::nullopt;
  const clang::QualType from = sub->getType()->getPointeeType();
  const clang::QualType to = cast.getType()->getPointeeType();
  if (to->isFunctionType() || from->isFunctionType())
    return std::nullopt;
  Conversion out;
  out.target = to.getUnqualifiedType();
  out.source = from.getUnqualifiedType();
  if (from->isVoidType()) {
    if (allocationCall(*sub))
      return std::nullopt;
    if (completeRecord(to) != nullptr) {
      out.form = Conversion::Form::Void;
      return out;
    }
    // (d): a plain scalar (not a pointer, not a `bool`); `const` bytes need
    // no check at all.
    if ((to->isArithmeticType() || to->isEnumeralType()) &&
        !to->isBooleanType() && !to->isIncompleteType() &&
        !(byteType(to) && to.isConstQualified())) {
      out.form = Conversion::Form::Scalar;
      return out;
    }
    return std::nullopt;
  }
  if (cxx)
    return std::nullopt;
  // (b): a struct to a struct that begins with it.
  if (completeRecord(from) != nullptr && completeRecord(to) != nullptr &&
      beginsWith(to, from)) {
    out.form = Conversion::Form::Base;
    return out;
  }
  // (c): container_of, `(D *)((char *)m - offsetof(D, f))` with `f` an `M`
  // and `m` an `M *`.
  if (byteType(from) && completeRecord(to) != nullptr) {
    const auto *minus =
        llvm::dyn_cast<clang::BinaryOperator>(sub->IgnoreParenImpCasts());
    if (minus == nullptr || minus->getOpcode() != clang::BO_Sub)
      return std::nullopt;
    const auto *offset = llvm::dyn_cast<clang::OffsetOfExpr>(
        minus->getRHS()->IgnoreParenImpCasts());
    if (offset == nullptr || offset->getNumComponents() != 1 ||
        offset->getComponent(0).getKind() != clang::OffsetOfNode::Field ||
        !clang::ASTContext::hasSameUnqualifiedType(
            offset->getTypeSourceInfo()->getType(), to))
      return std::nullopt;
    const clang::FieldDecl *member = offset->getComponent(0).getField();
    const clang::Expr *pointer = minus->getLHS()->IgnoreParenCasts();
    if (!pointer->getType()->isPointerType() ||
        !clang::ASTContext::hasSameUnqualifiedType(
            pointer->getType()->getPointeeType(), member->getType()))
      return std::nullopt;
    out.form = Conversion::Form::Container;
    out.arithmetic = minus;
    return out;
  }
  return std::nullopt;
}

bool convertsAndCalls(const clang::FunctionDecl &function) {
  const auto *body =
      llvm::dyn_cast_or_null<clang::CompoundStmt>(function.getBody());
  if (body == nullptr || body->size() == 0 || body->size() > 4)
    return false;
  const clang::ASTContext &ast = function.getASTContext();
  bool converts = false;
  bool calls = false;
  const std::function<bool(const clang::Stmt *)> plain =
      [&](const clang::Stmt *statement) {
        if (statement == nullptr)
          return true;
        if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(statement))
          converts = converts || checkedConversion(*cast, ast).has_value();
        if (llvm::isa<clang::CallExpr>(statement))
          calls = true;
        if (llvm::isa<clang::IfStmt, clang::ForStmt, clang::WhileStmt,
                      clang::DoStmt, clang::SwitchStmt, clang::GotoStmt>(
                statement))
          return false;
        return llvm::all_of(statement->children(), plain);
      };
  return plain(body) && converts && calls;
}

bool upcastToFirstMember(clang::QualType from, clang::QualType to) {
  return completeRecord(from) != nullptr && completeRecord(to) != nullptr &&
         beginsWith(from, to);
}

//===-- Layouts (§1.1) ----------------------------------------------------===//

TypeLayouts::TypeLayouts(clang::ASTContext &ast, SafeUnit &unit)
    : ast(ast), unit(unit) {}

std::string TypeLayouts::keyOf(clang::QualType type) const {
  type = type.getCanonicalType().getUnqualifiedType();
  std::string &key = keys[type.getTypePtr()];
  if (!key.empty())
    return key;
  if (const auto metadata =
          clang::infer_alloc::getAllocTokenMetadata(type, ast))
    key = std::string(metadata->TypeName);
  else
    key = type.getAsString(ast.getPrintingPolicy());
  return key;
}

/// What a type is made of, as text: the identity's input.
std::string TypeLayouts::shape(clang::QualType type, unsigned depth) {
  type = type.getCanonicalType().getUnqualifiedType();
  const clang::Type *canonical = type.getTypePtr();
  if (depth > 64)
    return "?";
  // A pointer by what it points to (its name: what a view of it reads).
  if (canonical->isAnyPointerType() || canonical->isReferenceType())
    return "p(" + keyOf(canonical->getPointeeType()) + ")";
  if (canonical->isBlockPointerType() || canonical->isMemberPointerType())
    return "p";
  if (canonical->isBooleanType())
    return "b";
  if (const auto *array = ast.getAsConstantArrayType(type))
    return "[" + std::to_string(array->getZExtSize()) + "]" +
           shape(array->getElementType(), depth + 1);
  const clang::RecordDecl *record = completeRecord(type);
  if (record == nullptr || type->isIncompleteType())
    return "s" +
           std::to_string(type->isIncompleteType()
                              ? 0
                              : ast.getTypeSizeInChars(type).getQuantity());
  if (const auto found = shapes.find(canonical); found != shapes.end())
    return found->second;
  const clang::ASTRecordLayout &layout = ast.getASTRecordLayout(record);
  std::string out = record->isUnion() ? "u{" : "{";
  if (const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
    if (cxx->isDynamicClass())
      out += 'v';
    for (const clang::CXXBaseSpecifier &base : cxx->bases())
      if (const auto *decl = base.getType()->getAsCXXRecordDecl();
          decl != nullptr && !base.isVirtual())
        out += std::to_string(layout.getBaseClassOffset(decl).getQuantity()) +
               ":" + shape(base.getType(), depth + 1) + ",";
  }
  for (const clang::FieldDecl *field : record->fields())
    out += std::to_string(ast.getFieldOffset(field) / 8) + ":" +
           (field->isBitField() ? "s0" : shape(field->getType(), depth + 1)) +
           ",";
  out += "}" + std::to_string(ast.getTypeSizeInChars(type).getQuantity());
  shapes[canonical] = out;
  return out;
}

std::uint64_t TypeLayouts::identityOf(clang::QualType type) {
  const clang::Type *canonical =
      type.getCanonicalType().getUnqualifiedType().getTypePtr();
  if (const auto found = identities.find(canonical); found != identities.end())
    return found->second;
  std::string text = keyOf(type) + '\0' + shape(type, 0);
  // A type declared in a function is named without it: its place tells two
  // such types of one name apart.
  if (const clang::RecordDecl *record = type->getAsRecordDecl();
      record != nullptr && record->getParentFunctionOrMethod() != nullptr)
    text += '\0' + record->getLocation().printToString(ast.getSourceManager());
  const std::uint64_t identity = llvm::xxh3_64bits(text);
  return identities[canonical] = identity != 0 ? identity : 1;
}

void TypeLayouts::flatten(clang::QualType type, std::uint64_t offset,
                          std::uint32_t stride, std::uint32_t elements,
                          std::uint32_t flags, unsigned depth,
                          SafeUnit::TypeLayout &out) {
  const clang::RecordDecl *record = completeRecord(type);
  if (record == nullptr || depth > 16 || out.parts.size() > 4096)
    return;
  const clang::ASTRecordLayout &layout = ast.getASTRecordLayout(record);
  const std::uint32_t inside = record->isUnion() ? 1 : 0;
  const auto add = [&](clang::QualType part, std::uint64_t at,
                       std::uint32_t partStride, std::uint32_t count,
                       std::uint32_t partFlags) {
    out.parts.push_back({.identity = identityOf(part),
                         .offset = static_cast<std::uint32_t>(at),
                         .stride = partStride,
                         .elements = count,
                         .flags = partFlags});
    flatten(part, at, partStride, count, partFlags, depth + 1, out);
  };
  if (const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record))
    for (const clang::CXXBaseSpecifier &base : cxx->bases())
      if (const auto *decl = base.getType()->getAsCXXRecordDecl();
          decl != nullptr && !base.isVirtual())
        add(base.getType(),
            offset + static_cast<std::uint64_t>(
                         layout.getBaseClassOffset(decl).getQuantity()),
            stride, elements, flags | inside);
  for (const clang::FieldDecl *field : record->fields()) {
    if (field->isBitField())
      continue;
    const std::uint64_t at = offset + (ast.getFieldOffset(field) / 8);
    const clang::QualType fieldType = field->getType();
    // A pointer or `bool` of the type's own bytes: no scalar view covers it.
    clang::QualType element = fieldType;
    std::uint32_t count = 1;
    if (const auto *array = ast.getAsConstantArrayType(fieldType)) {
      element = array->getElementType();
      count = static_cast<std::uint32_t>(array->getZExtSize());
    }
    if (element->isAnyPointerType() || element->isReferenceType() ||
        element->isBooleanType() || element->isMemberPointerType() ||
        element->isBlockPointerType()) {
      const auto width = static_cast<std::uint32_t>(
          ast.getTypeSizeInChars(element).getQuantity());
      if (stride == 0)
        out.parts.push_back({.identity = 0,
                             .offset = static_cast<std::uint32_t>(at),
                             .stride = count > 1 ? width : 0,
                             .elements = count,
                             .flags = flags | inside | 2U});
      else
        out.parts.push_back({.identity = 0,
                             .offset = static_cast<std::uint32_t>(at),
                             .stride = stride,
                             .elements = elements,
                             .flags = flags | inside | 2U});
      continue;
    }
    if (completeRecord(fieldType) != nullptr) {
      add(fieldType, at, stride, elements, flags | inside);
    } else if (const auto *array = ast.getAsConstantArrayType(fieldType);
               array != nullptr && stride == 0 &&
               completeRecord(array->getElementType()) != nullptr) {
      add(array->getElementType(), at,
          static_cast<std::uint32_t>(
              ast.getTypeSizeInChars(array->getElementType()).getQuantity()),
          static_cast<std::uint32_t>(array->getZExtSize()), flags | inside);
    }
  }
}

std::string TypeLayouts::layoutOf(clang::QualType type) {
  type = type.getCanonicalType().getUnqualifiedType();
  // An array's layout is its element's: a block holds an array of its type.
  while (const auto *array = ast.getAsArrayType(type))
    type = array->getElementType().getCanonicalType().getUnqualifiedType();
  if (type->isVoidType() || type->isIncompleteType() ||
      type->isDependentType() || type->isFunctionType())
    return {};
  std::string key = keyOf(type);
  if (unit.layouts.contains(key))
    return key;
  SafeUnit::TypeLayout layout;
  layout.identity = identityOf(type);
  layout.size =
      static_cast<std::uint64_t>(ast.getTypeSizeInChars(type).getQuantity());
  layout.display = type.getAsString(ast.getPrintingPolicy());
  if (!notPlain(type))
    layout.flags |= 1U;
  if (completeRecord(type) == nullptr)
    layout.flags |= 2U;
  flatten(type, 0, 0, 1, 0, 0, layout);
  llvm::stable_sort(layout.parts, [](const auto &a, const auto &b) {
    return a.offset < b.offset;
  });
  unit.layouts[key] = std::move(layout);
  return key;
}

void TypeLayouts::site(const clang::CXXNewExpr &expression) {
  const std::string key = layoutOf(expression.getAllocatedType());
  if (!key.empty() && expression.isArray() &&
      (expression.doesUsualArrayDeleteWantSize() ||
       expression.getAllocatedType().isDestructedType()))
    unit.cookieTypes.insert(key);
}

void TypeLayouts::site(const clang::CastExpr &cast) {
  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(
          cast.getSubExpr()->IgnoreParenImpCasts());
      call != nullptr && allocationCall(*call)) {
    const clang::QualType type =
        clang::infer_alloc::inferPossibleType(call, ast, &cast);
    if (!type.isNull())
      (void)layoutOf(type);
  }
}

void TypeLayouts::site(const clang::CallExpr &call) {
  if (allocationCall(call)) {
    const clang::QualType type =
        clang::infer_alloc::inferPossibleType(&call, ast, nullptr);
    if (!type.isNull())
      (void)layoutOf(type);
  }
}

void TypeLayouts::site(const clang::VarDecl &variable) {
  // A local placed on the heap (RFC 0048 §5.1) is named by its mark.
  for (const auto *attribute : variable.specific_attrs<clang::AnnotateAttr>())
    if (attribute->getAnnotation().starts_with(PlacedAnnotation)) {
      const std::string key = layoutOf(variable.getType());
      const std::string printed = attribute->getAnnotation()
                                      .drop_front(PlacedAnnotation.size())
                                      .trim()
                                      .str();
      if (!key.empty() && !printed.empty() && printed != key &&
          !unit.layouts.contains(printed))
        unit.layouts[printed] = unit.layouts[key];
    }
  if (!variable.hasGlobalStorage() || variable.isStaticDataMember() ||
      variable.getType()->isDependentType() ||
      variable.getType()->isReferenceType() ||
      !variable.isThisDeclarationADefinition())
    return;
  const std::string key = layoutOf(variable.getType());
  if (key.empty())
    return;
  const std::string symbol = symbolOf(variable);
  if (!symbol.empty())
    unit.globalTypes[symbol] = key;
}

void TypeLayouts::collect(clang::TranslationUnitDecl &unitDecl) {
  struct Sites final : clang::RecursiveASTVisitor<Sites> {
    TypeLayouts *self = nullptr;
    // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
    [[nodiscard]] static bool shouldVisitTemplateInstantiations() {
      return true;
    }
    bool VisitCXXNewExpr(clang::CXXNewExpr *expression) {
      self->site(*expression);
      return true;
    }
    bool VisitCastExpr(clang::CastExpr *cast) {
      self->site(*cast);
      return true;
    }
    bool VisitCallExpr(clang::CallExpr *call) {
      self->site(*call);
      return true;
    }
    bool VisitVarDecl(clang::VarDecl *variable) {
      self->site(*variable);
      return true;
    }
    // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
  } sites;
  sites.self = this;
  sites.TraverseDecl(&unitDecl);
}

std::string TypeLayouts::symbolOf(const clang::VarDecl &variable) {
  if (!mangler)
    mangler.reset(ast.createMangleContext());
  // A C function's static local is named `<function>.<local>`.
  if (!ast.getLangOpts().CPlusPlus && variable.isStaticLocal()) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(variable.getDeclContext());
    return function != nullptr
               ? function->getName().str() + "." + variable.getName().str()
               : std::string();
  }
  if (!mangler->shouldMangleDeclName(&variable))
    return variable.getName().str();
  std::string name;
  llvm::raw_string_ostream text(name);
  mangler->mangleName(clang::GlobalDecl(&variable), text);
  return name;
}

std::string TypeLayouts::typeInformation(clang::QualType type) {
  if (!mangler)
    mangler.reset(ast.createMangleContext());
  std::string name;
  llvm::raw_string_ostream text(name);
  mangler->mangleCXXRTTI(type.getCanonicalType().getUnqualifiedType(), text);
  return name;
}

//===-- The marks (§2.3)
//---------------------------------------------------===//

TypeMarks::TypeMarks(clang::ASTContext &ast, SafeUnit &unit)
    : ast(ast), unit(unit), layouts(ast, unit) {}

TypeMarks::~TypeMarks() = default;

clang::FunctionDecl *TypeMarks::marker(llvm::StringRef name) {
  clang::FunctionDecl *&declared = markers[name];
  if (declared != nullptr)
    return declared;
  const clang::QualType pointer = ast.VoidPtrTy;
  clang::FunctionProtoType::ExtProtoInfo info;
  info.ExceptionSpec.Type = clang::EST_BasicNoexcept;
  const clang::QualType type =
      ast.getFunctionType(pointer, {pointer, ast.LongTy}, info);
  clang::TranslationUnitDecl *unitDecl = ast.getTranslationUnitDecl();
  declared = clang::FunctionDecl::Create(
      ast, unitDecl, {}, {},
      clang::DeclarationName(&ast.Idents.get(
          (llvm::Twine("__weavec_marker_") + name.drop_front(9)).str())),
      type, ast.getTrivialTypeSourceInfo(type), clang::SC_Extern);
  llvm::SmallVector<clang::ParmVarDecl *, 2> parameters;
  for (const clang::QualType each : {pointer, ast.LongTy.withConst()})
    parameters.push_back(clang::ParmVarDecl::Create(
        ast, declared, {}, {}, nullptr, each.getUnqualifiedType(),
        ast.getTrivialTypeSourceInfo(each.getUnqualifiedType()), clang::SC_None,
        nullptr));
  declared->setParams(parameters);
  declared->addAttr(clang::AsmLabelAttr::CreateImplicit(ast, name));
  declared->addAttr(clang::NoThrowAttr::CreateImplicit(ast));
  declared->setImplicit();
  return declared;
}

/// `marker((void *)pointer, index)`, of the pointer's own type.
clang::Expr *TypeMarks::call(llvm::StringRef name, clang::Expr *pointer,
                             std::uint64_t index, clang::SourceLocation at) {
  clang::FunctionDecl *function = marker(name);
  auto *reference = clang::DeclRefExpr::Create(
      ast, clang::NestedNameSpecifierLoc(), {}, function, false, at,
      function->getType(), clang::VK_LValue);
  auto *callee = clang::ImplicitCastExpr::Create(
      ast, ast.getPointerType(function->getType()),
      clang::CK_FunctionToPointerDecay, reference, nullptr, clang::VK_PRValue,
      clang::FPOptionsOverride());
  auto *argument = clang::ImplicitCastExpr::Create(
      ast, ast.VoidPtrTy, clang::CK_BitCast, pointer, nullptr,
      clang::VK_PRValue, clang::FPOptionsOverride());
  auto *count = clang::IntegerLiteral::Create(
      ast,
      llvm::APInt(static_cast<unsigned>(ast.getTypeSize(ast.LongTy)), index),
      ast.LongTy, at);
  auto *made = clang::CallExpr::Create(ast, callee, {argument, count},
                                       ast.VoidPtrTy, clang::VK_PRValue, at,
                                       clang::FPOptionsOverride());
  auto *back = clang::ImplicitCastExpr::Create(
      ast, pointer->getType(), clang::CK_BitCast, made, nullptr,
      clang::VK_PRValue, clang::FPOptionsOverride());
  // (The marks' own conversions are not the program's.)
  rewritten.insert(argument);
  rewritten.insert(back);
  return back;
}

/// The marked form of `expression`: a pointer, or an lvalue (through its
/// address).
clang::Expr *TypeMarks::wrap(llvm::StringRef name, clang::Expr *expression,
                             std::uint64_t index) {
  const clang::SourceLocation at = expression->getExprLoc();
  if (!expression->isGLValue())
    return call(name, expression, index, at);
  const clang::QualType type = expression->getType();
  auto *address = clang::UnaryOperator::Create(
      ast, expression, clang::UO_AddrOf, ast.getPointerType(type),
      clang::VK_PRValue, clang::OK_Ordinary, at, false,
      clang::FPOptionsOverride());
  return clang::UnaryOperator::Create(ast, call(name, address, index, at),
                                      clang::UO_Deref, type, clang::VK_LValue,
                                      clang::OK_Ordinary, at, false,
                                      clang::FPOptionsOverride());
}

void TypeMarks::Rewrite::apply() const {
  if (auto *cast = llvm::dyn_cast<clang::CastExpr>(node))
    cast->setSubExpr(replacement);
  else if (auto *member = llvm::dyn_cast<clang::MemberExpr>(node))
    member->setBase(replacement);
}

void TypeMarks::Rewrite::undo() const {
  if (auto *cast = llvm::dyn_cast<clang::CastExpr>(node))
    cast->setSubExpr(original);
  else if (auto *member = llvm::dyn_cast<clang::MemberExpr>(node))
    member->setBase(original);
}

void TypeMarks::undo() {
  for (const Rewrite &rewrite : llvm::reverse(rewrites))
    rewrite.undo();
}

void TypeMarks::redo() {
  for (const Rewrite &rewrite : rewrites)
    rewrite.apply();
}

namespace {

/// Finds the forms one safe function's body marks.
class Marker final : public clang::RecursiveASTVisitor<Marker> {
public:
  Marker(TypeMarks &marks, clang::ASTContext &ast) : marks(marks), ast(ast) {}

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
  bool VisitCastExpr(clang::CastExpr *cast) {
    if (marks.seen(cast))
      return true;
    const auto conversion = checkedConversion(*cast, ast);
    if (!conversion)
      return recordsVoid(*cast);
    SafeUnit::CheckedCast record;
    record.type = marks.layouts.layoutOf(conversion->target);
    if (record.type.empty())
      return true;
    record.delta = conversion->delta;
    if (conversion->polymorphic && ast.getLangOpts().RTTI) {
      record.polymorphic = true;
      record.derivedInfo = marks.layouts.typeInformation(conversion->target);
      record.baseInfo = marks.layouts.typeInformation(conversion->source);
      // The classes' type information, which the check names, in the unit.
      if (marks.markVTable)
        for (const clang::QualType each :
             {conversion->target, conversion->source})
          if (const auto *named = each->getAsCXXRecordDecl())
            marks.markVTable(*named, cast->getExprLoc());
    }
    const std::uint64_t index = marks.unit.casts.size();
    marks.unit.casts.push_back(std::move(record));
    clang::Expr *original = cast->getSubExpr();
    marks.add(cast, original, marks.wrap(CastMarker, original, index));
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *member) {
    if (unionAccess(*member) != nullptr)
      markAccess(*member, *llvm::cast<clang::MemberExpr>(
                              member->getBase()->IgnoreParenImpCasts()));
    return true;
  }

  bool VisitBinaryOperator(clang::BinaryOperator *op) {
    if (op->isAssignmentOp()) {
      writes.insert(op->getLHS()->IgnoreParens());
      markTagStore(*op->getLHS());
    }
    return true;
  }
  bool VisitCompoundAssignOperator(clang::CompoundAssignOperator *op) {
    writes.insert(op->getLHS()->IgnoreParens());
    markTagStore(*op->getLHS());
    return true;
  }
  bool VisitUnaryOperator(clang::UnaryOperator *op) {
    if (op->isIncrementDecrementOp()) {
      writes.insert(op->getSubExpr()->IgnoreParens());
      markTagStore(*op->getSubExpr());
    }
    // §1.2: any address of a local struct or array may come back to a
    // conversion through a pointer variable.
    // (In C++ only an address that becomes a `void *`, or a local seen as
    // its base: a typed pointer converts down only by a class's rules.)
    if (op->getOpcode() == clang::UO_AddrOf && !ast.getLangOpts().CPlusPlus)
      record(*op->getSubExpr());
    return true;
  }
  bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *subscript) {
    // (Visited before its base: an element read in place is no address.)
    subscripted.insert(subscript->getBase()->IgnoreParens());
    return true;
  }
  bool VisitImplicitCastExpr(clang::ImplicitCastExpr *cast) {
    switch (cast->getCastKind()) {
    case clang::CK_ArrayToPointerDecay:
      // (In C++ an array given as a typed pointer stays in its plain frame:
      // one that becomes a `void *` is recorded there.)
      if (!subscripted.contains(cast) && !ast.getLangOpts().CPlusPlus)
        record(*cast->getSubExpr());
      break;
    case clang::CK_DerivedToBase:
    case clang::CK_UncheckedDerivedToBase:
      // A local seen as its base, which a downcast may convert back.
      if (cast->getSubExpr()->isGLValue())
        record(*cast->getSubExpr());
      break;
    default:
      break;
    }
    return true;
  }
  /// C++: a local, or a member of one, of a class this unit derives a class
  /// from (without virtual functions: those are checked by their vtable),
  /// bound to a reference or given as a pointer, may be converted down.
  bool VisitCallExpr(clang::CallExpr *call) {
    // (In C every address taken is recorded where it is taken.)
    if (!ast.getLangOpts().CPlusPlus)
      return true;
    const clang::FunctionDecl *callee = call->getDirectCallee();
    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
      const clang::Expr *argument = call->getArg(i)->IgnoreParenImpCasts();
      if (argument->isGLValue() && callee != nullptr &&
          i < callee->getNumParams() &&
          callee->getParamDecl(i)->getType()->isReferenceType() &&
          derivedFrom(argument->getType()))
        record(*argument);
    }
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *variable) {
    if (ast.getLangOpts().CPlusPlus && variable->getType()->isReferenceType() &&
        variable->getInit() != nullptr) {
      const clang::Expr *bound = variable->getInit()->IgnoreParenImpCasts();
      if (bound->isGLValue() && derivedFrom(bound->getType()))
        record(*bound);
    }
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)

private:
  TypeMarks &marks;
  clang::ASTContext &ast;
  llvm::DenseSet<const clang::Expr *> writes;
  llvm::DenseSet<const clang::Expr *> subscripted;

  /// Whether a class the unit has parsed derives from `type`, a class
  /// without virtual functions (a downcast's source).
  bool derivedFrom(clang::QualType type) {
    const auto *base = type->getAsCXXRecordDecl();
    if (base == nullptr || base->getDefinition() == nullptr ||
        base->isPolymorphic())
      return false;
    return marks.bases().contains(base->getCanonicalDecl());
  }

  /// The struct a tagged union's member is reached from: a pointer the
  /// access goes through, or an lvalue of the struct.
  void markAccess(clang::MemberExpr &member, clang::MemberExpr &field) {
    if (marks.seen(&field))
      return;
    const auto tagged =
        taggedUnion(*llvm::cast<clang::FieldDecl>(field.getMemberDecl()));
    const auto *chosen =
        llvm::dyn_cast<clang::FieldDecl>(member.getMemberDecl());
    if (!tagged || chosen == nullptr)
      return;
    clang::Expr *base = field.getBase();
    // (A struct that is a temporary has no address to mark.)
    if (!field.isArrow() && !base->isGLValue())
      return;
    SafeUnit::TaggedAccess access;
    access.tagOffset = tagged->tagOffset;
    access.tagWidth = tagged->tagWidth;
    access.mask = tagged->mask;
    if (const auto found = tagged->when.find(chosen);
        found != tagged->when.end())
      access.values = found->second;
    access.member = chosen->getNameAsString();
    access.tag = tagged->tagName;
    access.write = writes.contains(&member);
    const std::uint64_t index = marks.unit.taggedAccesses.size();
    marks.unit.taggedAccesses.push_back(std::move(access));
    marks.add(&field, base, marks.wrap(MemberMarker, base, index));
  }

  void markTagStore(clang::Expr &target) {
    auto *member = llvm::dyn_cast<clang::MemberExpr>(target.IgnoreParens());
    if (member == nullptr || marks.seen(member))
      return;
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr)
      return;
    const clang::FieldDecl *unionField = tagOf(*field);
    if (unionField == nullptr)
      return;
    const auto tagged = taggedUnion(*unionField);
    clang::Expr *base = member->getBase();
    if (!member->isArrow() && !base->isGLValue())
      return;
    const std::uint64_t index = marks.unit.tagStores.size();
    marks.unit.tagStores.push_back({.tagOffset = tagged->tagOffset,
                                    .tagWidth = tagged->tagWidth,
                                    .mask = tagged->mask,
                                    .unionOffset = tagged->unionOffset,
                                    .unionSize = tagged->unionSize});
    marks.add(member, base, marks.wrap(TagMarker, base, index));
  }

  /// §1.2: a local whose address becomes a `void *`: `&local`, or an
  /// array local that decays.
  bool recordsVoid(const clang::CastExpr &cast) {
    if (cast.getCastKind() == clang::CK_BitCast &&
        cast.getType()->isVoidPointerType())
      addressTaken(*cast.getSubExpr(), /*toVoid=*/true);
    return true;
  }

  void addressTaken(const clang::Expr &expression, bool toVoid) {
    if (!toVoid)
      return;
    const clang::Expr *at = expression.IgnoreParens();
    if (const auto *address = llvm::dyn_cast<clang::UnaryOperator>(at);
        address != nullptr && address->getOpcode() == clang::UO_AddrOf) {
      record(*address->getSubExpr());
      return;
    }
    if (const auto *decay = llvm::dyn_cast<clang::ImplicitCastExpr>(at);
        decay != nullptr &&
        decay->getCastKind() == clang::CK_ArrayToPointerDecay)
      record(*decay->getSubExpr());
  }

  /// The local an lvalue (a local, a member of one, an array element of
  /// one) is part of, recorded for its scope.
  void record(const clang::Expr &lvalue) {
    const clang::Expr *at = lvalue.IgnoreParenImpCasts();
    while (true) {
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(at);
          member != nullptr && !member->isArrow()) {
        at = member->getBase()->IgnoreParenImpCasts();
        continue;
      }
      if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(at);
          subscript != nullptr && subscript->getBase()
                                      ->IgnoreParenImpCasts()
                                      ->getType()
                                      ->isArrayType()) {
        at = subscript->getBase()->IgnoreParenImpCasts();
        continue;
      }
      break;
    }
    const auto *named = llvm::dyn_cast<clang::DeclRefExpr>(at);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast): the local is
    // marked (an implicit attribute), and its uses reach it as `const`.
    auto *variable = named != nullptr
                         ? llvm::dyn_cast<clang::VarDecl>(
                               const_cast<clang::ValueDecl *>(named->getDecl()))
                         : nullptr;
    // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
    // Unresolved question 3: structs, unions, classes and arrays; a
    // scalar local's address converted back is believed.
    if (variable == nullptr || !variable->hasLocalStorage() ||
        variable->getType()->isReferenceType() ||
        variable->getType()->isVariableArrayType() ||
        annotation(*variable, RecordedAnnotation) != nullptr)
      return;
    const std::string key = marks.layouts.layoutOf(variable->getType());
    if (key.empty())
      return;
    const auto index =
        static_cast<std::int64_t>(marks.unit.recordedLocals.size());
    marks.unit.recordedLocals.push_back(key);
    const llvm::APSInt integer(
        llvm::APInt(64, static_cast<std::uint64_t>(index)),
        /*isUnsigned=*/false);
    auto *literal =
        clang::IntegerLiteral::Create(ast, integer, ast.LongLongTy, {});
    clang::Expr *argument =
        clang::ConstantExpr::Create(ast, literal, clang::APValue(integer));
    variable->addAttr(clang::AnnotateAttr::CreateImplicit(
        ast, RecordedAnnotation, &argument, 1));
  }
};

} // namespace

const llvm::DenseSet<const clang::CXXRecordDecl *> &TypeMarks::bases() {
  // The declarations parsed since the last question, namespaces included.
  std::vector<const clang::Decl *> fresh;
  const clang::Decl *each = basesRead != nullptr
                                ? basesRead->getNextDeclInContext()
                                : *ast.getTranslationUnitDecl()->decls_begin();
  for (; each != nullptr; each = each->getNextDeclInContext()) {
    fresh.push_back(each);
    basesRead = each;
  }
  const auto basesOf = [&](const clang::CXXRecordDecl &record) {
    if (!record.hasDefinition())
      return;
    for (const clang::CXXBaseSpecifier &base : record.getDefinition()->bases())
      if (const auto *decl = base.getType()->getAsCXXRecordDecl())
        knownBases.insert(decl->getCanonicalDecl());
  };
  // Every class: nested, local to a function, in a linkage block, a
  // template's pattern (its bases that do not depend on a parameter) and
  // the instances made so far.
  struct Reader final : clang::RecursiveASTVisitor<Reader> {
    TypeMarks *self = nullptr;
    const std::function<void(const clang::CXXRecordDecl &)> *basesOf = nullptr;
    // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
    [[nodiscard]] static bool shouldVisitTemplateInstantiations() {
      return true;
    }
    bool VisitCXXRecordDecl(clang::CXXRecordDecl *record) {
      (*basesOf)(*record);
      return true;
    }
    bool VisitClassTemplateDecl(clang::ClassTemplateDecl *pattern) {
      self->classTemplates.push_back(pattern);
      return true;
    }
    // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
  };
  const std::function<void(const clang::CXXRecordDecl &)> reads = basesOf;
  Reader reader;
  reader.self = this;
  reader.basesOf = &reads;
  for (const clang::Decl *declaration : fresh)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): read only.
    reader.TraverseDecl(const_cast<clang::Decl *>(declaration));
  // A template's instances made since it was read (a base that depends on
  // a parameter, `template <class T> struct D : T`).
  for (const clang::ClassTemplateDecl *pattern : classTemplates)
    for (const clang::ClassTemplateSpecializationDecl *instance :
         pattern->specializations())
      if (instance->hasDefinition() && instancesRead.insert(instance).second)
        basesOf(*instance);
  return knownBases;
}

bool TypeMarks::seen(const clang::Expr *node) const {
  return rewritten.contains(node);
}

void TypeMarks::add(clang::Expr *node, clang::Expr *original,
                    clang::Expr *replacement) {
  rewritten.insert(node);
  rewrites.push_back(
      {.node = node, .original = original, .replacement = replacement});
  rewrites.back().apply();
}

void TypeMarks::mark(clang::FunctionDecl &function) {
  if (!function.doesThisDeclarationHaveABody() ||
      function.isDependentContext() || function.getBody() == nullptr ||
      !marked.insert(&function).second)
    return;
  Marker visitor(*this, ast);
  visitor.TraverseStmt(function.getBody());
  // A constructor's member initializers are its code too.
  if (const auto *constructor =
          llvm::dyn_cast<clang::CXXConstructorDecl>(&function))
    for (const clang::CXXCtorInitializer *initializer : constructor->inits())
      if (initializer->getInit() != nullptr)
        visitor.TraverseStmt(initializer->getInit());
}

void TypeMarks::collect() {
  layouts.collect(*ast.getTranslationUnitDecl());
}

//===-- E9: a tag from the tests the code makes (§4)
//-----------------------===//

static std::string sourceText(const clang::Expr &expression,
                              const clang::ASTContext &ast) {
  const clang::SourceManager &sources = ast.getSourceManager();
  return clang::Lexer::getSourceText(
             sources.getExpansionRange(expression.getSourceRange()), sources,
             ast.getLangOpts())
      .str();
}

namespace {

/// What one read of a union's member was tested under: the struct's field
/// and the values (their text and their value), or nothing.
struct Guard {
  const clang::FieldDecl *field = nullptr;
  std::vector<std::pair<std::string, std::uint64_t>> values;
};

class Guards {
public:
  explicit Guards(clang::ASTContext &ast) : ast(ast) {}

  /// The comparison `base->f == C` (or `C == base->f`) in `condition`, of
  /// an integer field `f` of `record`, for a struct `base` spelled `text`.
  std::optional<Guard> compared(const clang::Expr *condition,
                                const clang::RecordDecl &record,
                                const std::string &text) {
    condition = condition->IgnoreParenImpCasts();
    const auto *op = llvm::dyn_cast<clang::BinaryOperator>(condition);
    if (op == nullptr)
      return std::nullopt;
    if (op->getOpcode() == clang::BO_LAnd) {
      if (auto found = compared(op->getLHS(), record, text))
        return found;
      return compared(op->getRHS(), record, text);
    }
    if (op->getOpcode() != clang::BO_EQ)
      return std::nullopt;
    for (const auto &[side, constant] :
         {std::pair{op->getLHS(), op->getRHS()},
          std::pair{op->getRHS(), op->getLHS()}}) {
      Guard guard;
      if (!fieldOf(side, record, text, guard))
        continue;
      clang::Expr::EvalResult result;
      if (!constant->EvaluateAsInt(result, ast))
        continue;
      guard.values.emplace_back(
          sourceText(*constant->IgnoreParenImpCasts(), ast),
          result.Val.getInt().getZExtValue());
      return guard;
    }
    return std::nullopt;
  }

  /// Whether `expression` reads an integer field of `record` from a struct
  /// spelled `text`.
  bool fieldOf(const clang::Expr *expression, const clang::RecordDecl &record,
               const std::string &text, Guard &guard) {
    const auto *member =
        llvm::dyn_cast<clang::MemberExpr>(expression->IgnoreParenImpCasts());
    if (member == nullptr)
      return false;
    const auto *field =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (field == nullptr || field->getParent() != &record ||
        field->isBitField() ||
        !(field->getType()->isIntegerType() ||
          field->getType()->isEnumeralType()) ||
        sourceText(*member->getBase(), ast) != text)
      return false;
    guard.field = field;
    return true;
  }

  /// The guard of the read `member` of a union field of `record`, whose
  /// struct is spelled `text`.
  std::optional<Guard> of(const clang::MemberExpr &member,
                          const clang::RecordDecl &record,
                          const std::string &text) {
    const clang::Stmt *child = &member;
    for (unsigned depth = 0; depth < 64; ++depth) {
      const auto parents = ast.getParents(*child);
      if (parents.empty())
        return std::nullopt;
      const auto *parent = parents[0].get<clang::Stmt>();
      if (parent == nullptr)
        return std::nullopt;
      if (const auto *branch = llvm::dyn_cast<clang::IfStmt>(parent);
          branch != nullptr && branch->getThen() == child)
        if (auto guard = compared(branch->getCond(), record, text))
          return guard;
      if (const auto *choice =
              llvm::dyn_cast<clang::ConditionalOperator>(parent);
          choice != nullptr && choice->getTrueExpr() == child)
        if (auto guard = compared(choice->getCond(), record, text))
          return guard;
      if (const auto *block = llvm::dyn_cast<clang::CompoundStmt>(parent))
        if (const auto *switched = switchOf(*block))
          return labels(*switched, *block, child, record, text);
      if (const auto *label = llvm::dyn_cast<clang::SwitchCase>(parent)) {
        // The first of a chain of labels (`case A: case B:`).
        const clang::SwitchCase *first = label;
        for (unsigned up = 0; up < 64; ++up) {
          const auto above = ast.getParents(*first);
          const auto *outer =
              above.empty() ? nullptr : above[0].get<clang::SwitchCase>();
          if (outer == nullptr || outer->getSubStmt() != first)
            break;
          first = outer;
        }
        if (const auto *switched = enclosingSwitch(*first))
          return chain(*switched, *first, record, text);
      }
      child = parent;
    }
    return std::nullopt;
  }

private:
  clang::ASTContext &ast;

  const clang::SwitchStmt *switchOf(const clang::CompoundStmt &block) {
    const auto parents = ast.getParents(block);
    if (parents.empty())
      return nullptr;
    const auto *switched = parents[0].get<clang::SwitchStmt>();
    return switched != nullptr && switched->getBody() == &block ? switched
                                                                : nullptr;
  }

  const clang::SwitchStmt *enclosingSwitch(const clang::Stmt &statement) {
    const clang::Stmt *at = &statement;
    for (unsigned depth = 0; depth < 64; ++depth) {
      const auto parents = ast.getParents(*at);
      if (parents.empty())
        return nullptr;
      at = parents[0].get<clang::Stmt>();
      if (at == nullptr)
        return nullptr;
      if (const auto *switched = llvm::dyn_cast<clang::SwitchStmt>(at))
        return switched;
    }
    return nullptr;
  }

  /// The values of a chain of labels (`case A: case B:`), ending at the
  /// statement; none with a `default`.
  std::optional<Guard> chain(const clang::SwitchStmt &switched,
                             const clang::SwitchCase &first,
                             const clang::RecordDecl &record,
                             const std::string &text) {
    Guard guard;
    if (!fieldOf(switched.getCond(), record, text, guard))
      return std::nullopt;
    for (const clang::Stmt *at = &first;;) {
      const auto *label = llvm::dyn_cast<clang::CaseStmt>(at);
      if (llvm::isa<clang::DefaultStmt>(at))
        return std::nullopt;
      if (label == nullptr)
        break;
      clang::Expr::EvalResult result;
      if (label->getRHS() != nullptr ||
          !label->getLHS()->EvaluateAsInt(result, ast))
        return std::nullopt;
      guard.values.emplace_back(sourceText(*label->getLHS(), ast),
                                result.Val.getInt().getZExtValue());
      at = label->getSubStmt();
    }
    return guard;
  }

  /// A statement of a switch's body: the labels before it, back to the
  /// last `break` or `return`.
  std::optional<Guard> labels(const clang::SwitchStmt &switched,
                              const clang::CompoundStmt &block,
                              const clang::Stmt *child,
                              const clang::RecordDecl &record,
                              const std::string &text) {
    const clang::SwitchCase *label = nullptr;
    for (const clang::Stmt *each : block.body()) {
      if (each == child)
        break;
      if (const auto *found = llvm::dyn_cast<clang::SwitchCase>(each))
        label = found;
      else if (llvm::isa<clang::BreakStmt, clang::ReturnStmt>(each))
        label = nullptr;
    }
    if (label == nullptr)
      return std::nullopt;
    return chain(switched, *label, record, text);
  }
};

/// What E9 found of one union field: the field, each member's values.
struct Evidence {
  bool known = false;
  bool consistent = true;
  const clang::FieldDecl *tag = nullptr;
  llvm::DenseMap<const clang::FieldDecl *,
                 std::vector<std::pair<std::string, std::uint64_t>>>
      values;
};

} // namespace

static Evidence gather(const clang::FieldDecl &unionField,
                       clang::ASTContext &ast) {
  struct Reads final : clang::RecursiveASTVisitor<Reads> {
    const clang::FieldDecl *unionField = nullptr;
    clang::ASTContext *ast = nullptr;
    Guards *guards = nullptr;
    Evidence evidence;
    llvm::DenseSet<const clang::Expr *> written;
    // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
    [[nodiscard]] static bool shouldVisitTemplateInstantiations() {
      return true;
    }
    bool VisitBinaryOperator(clang::BinaryOperator *op) {
      if (op->isAssignmentOp())
        written.insert(op->getLHS()->IgnoreParens());
      return true;
    }
    bool VisitMemberExpr(clang::MemberExpr *member) {
      const auto *base = llvm::dyn_cast<clang::MemberExpr>(
          member->getBase()->IgnoreParenImpCasts());
      const auto *chosen =
          llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      if (base == nullptr || base->getMemberDecl() != unionField ||
          chosen == nullptr || written.contains(member))
        return true;
      if (ast->getSourceManager().isInSystemHeader(member->getExprLoc()))
        return true;
      const auto guard = guards->of(*member, *unionField->getParent(),
                                    sourceText(*base->getBase(), *ast));
      // A read under no test is checked when it runs, under the tag the
      // others say; a read under another field's test leaves the choice to
      // the program.
      if (!guard)
        return true;
      if (evidence.tag != nullptr && evidence.tag != guard->field) {
        evidence.consistent = false;
        return true;
      }
      evidence.known = true;
      evidence.tag = guard->field;
      auto &values = evidence.values[chosen];
      for (const auto &value : guard->values)
        if (llvm::none_of(values, [&](const auto &each) {
              return each.second == value.second;
            }))
          values.push_back(value);
      return true;
    }
    // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
  } reads;
  Guards guards(ast);
  reads.unionField = &unionField;
  reads.ast = &ast;
  reads.guards = &guards;
  reads.TraverseDecl(ast.getTranslationUnitDecl());
  return reads.evidence;
}

std::optional<Want> tagWant(const clang::MemberExpr &member,
                            clang::ASTContext &ast, std::string *tag) {
  const auto *base = llvm::dyn_cast<clang::MemberExpr>(
      member.getBase()->IgnoreParenImpCasts());
  const auto *unionField =
      base != nullptr ? llvm::dyn_cast<clang::FieldDecl>(base->getMemberDecl())
                      : nullptr;
  if (unionField == nullptr || unionField->getParent()->isUnion() ||
      completeRecord(unionField->getType()) == nullptr ||
      taggedUnion(*unionField))
    return std::nullopt;
  static llvm::DenseMap<const clang::FieldDecl *, Evidence> cache;
  auto found = cache.find(unionField);
  if (found == cache.end())
    found = cache.try_emplace(unionField, gather(*unionField, ast)).first;
  const Evidence &evidence = found->second;
  if (!evidence.known || !evidence.consistent || evidence.tag == nullptr)
    return std::nullopt;
  if (tag != nullptr)
    *tag = evidence.tag->getNameAsString();
  const clang::SourceManager &sources = ast.getSourceManager();
  const auto after = [&](clang::SourceLocation at) {
    return clang::Lexer::getLocForEndOfToken(sources.getFileLoc(at), 0, sources,
                                             ast.getLangOpts());
  };
  Want wanted;
  wanted.kind = Want::Kind::Write;
  wanted.declaration = unionField;
  wanted.level = "likely";
  wanted.aspect = "tag";
  wanted.fact = "tagged_by(" + evidence.tag->getNameAsString() + ")";
  wanted.macros = true;
  // The union's members, each with the values its reads were under; one
  // that holds a pointer and is read under none leaves the choice to the
  // program.
  const clang::RecordDecl *self = completeRecord(unionField->getType());
  for (const clang::FieldDecl *each : self->fields()) {
    const auto values = evidence.values.find(each);
    if (values == evidence.values.end() || values->second.empty()) {
      if (notPlain(each->getType()))
        return std::nullopt;
      continue;
    }
    std::string list;
    for (const auto &[text, value] : values->second)
      list += (list.empty() ? "" : ", ") + text;
    wanted.insertions.emplace_back(after(each->getSourceRange().getEnd()),
                                   " WEAVEC_WHEN(" + list + ")");
  }
  const clang::SourceLocation unionEnd =
      unionField->isAnonymousStructOrUnion()
          ? after(self->getBraceRange().getEnd())
          : after(unionField->getSourceRange().getEnd());
  wanted.insertions.emplace_back(
      unionEnd, " WEAVEC_TAGGED_BY(" + evidence.tag->getNameAsString() + ")");
  wanted.description =
      "declare 'WEAVEC_TAGGED_BY(" + evidence.tag->getNameAsString() +
      ")' on '" + unionField->getNameAsString() +
      "' and 'WEAVEC_WHEN' on its members, by the tests of '" +
      evidence.tag->getNameAsString() + "' its reads are under";
  return wanted;
}

//===-- The rules (§3.2)
//---------------------------------------------------===//

namespace {

class UnionRules final : public clang::RecursiveASTVisitor<UnionRules> {
public:
  UnionRules(clang::ASTContext &ast, const TypeRuleReport &report,
             FixPlanner *fixes)
      : ast(ast), report(report), fixes(fixes) {}

  // NOLINTBEGIN(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
  bool TraverseMemberExpr(clang::MemberExpr *member) {
    // The base of a member access is a use of the union through its
    // struct; anything else that names the union is a use of it apart.
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl())) {
      const auto tagged = taggedUnion(*field);
      if (tagged && tagged->problem.empty() && !bases.contains(member)) {
        report(member->getMemberLoc(), "union-pointer",
               "'" + field->getNameAsString() + "' is used without '" +
                   tagged->tagName + "'",
               "use a member of it, or copy the struct that holds both");
      }
    }
    if (const auto *base = llvm::dyn_cast<clang::MemberExpr>(
            member->getBase()->IgnoreParenImpCasts()))
      bases.insert(base);
    return RecursiveASTVisitor::TraverseMemberExpr(member);
  }

  bool VisitMemberExpr(clang::MemberExpr *member) {
    // A tag is changed by an assignment, which clears its union; nothing
    // else may hold its address.
    if (const auto *tag =
            llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
        tag != nullptr && tagOf(*tag) != nullptr &&
        escapes(*member, /*tag=*/true))
      report(member->getMemberLoc(), "union-pointer",
             "'" + tag->getNameAsString() + "' is the tag of '" +
                 tagOf(*tag)->getNameAsString() +
                 "': it is changed only by an assignment, which clears the "
                 "union",
             "assign it ('x->" + tag->getNameAsString() +
                 " = value'), and read it by value");
    const clang::MemberExpr *field = unionAccess(*member);
    if (field == nullptr)
      return true;
    if (escapes(*member, /*tag=*/false)) {
      const auto *chosen =
          llvm::cast<clang::FieldDecl>(member->getMemberDecl());
      const auto *unionField =
          llvm::cast<clang::FieldDecl>(field->getMemberDecl());
      report(member->getMemberLoc(), "union-pointer",
             "the address of '" + chosen->getNameAsString() +
                 "' would outlive the check of '" +
                 taggedUnion(*unionField)->tagName + "'",
             "copy '" + chosen->getNameAsString() + "' out, or pass '" +
                 unionField->getParent()->getNameAsString() + "'");
    }
    const auto tagged =
        taggedUnion(*llvm::cast<clang::FieldDecl>(field->getMemberDecl()));
    const auto *chosen =
        llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if (chosen != nullptr && !tagged->when.contains(chosen))
      report(member->getMemberLoc(), "union-pointer",
             "'" + chosen->getNameAsString() +
                 "' has no 'WEAVEC_WHEN': no "
                 "value of '" +
                 tagged->tagName + "' is for it",
             "say the values of '" + tagged->tagName +
                 "' it is for: 'WEAVEC_WHEN(…)' after its name");
    return true;
  }

  /// A member pointer to a tag or to a tagged union's member.
  bool VisitDeclRefExpr(clang::DeclRefExpr *reference) {
    const auto *field = llvm::dyn_cast<clang::FieldDecl>(reference->getDecl());
    if (field == nullptr)
      return true;
    const bool member =
        field->getParent()->isUnion() &&
        llvm::any_of(field->getParent()->getParent()->decls(),
                     [&](const clang::Decl *each) {
                       const auto *other =
                           llvm::dyn_cast<clang::FieldDecl>(each);
                       return other != nullptr &&
                              other->getType()->getAsRecordDecl() ==
                                  field->getParent() &&
                              taggedUnion(*other).has_value();
                     });
    if (tagOf(*field) != nullptr || member || taggedUnion(*field))
      report(reference->getLocation(), "union-pointer",
             "a member pointer to '" + field->getNameAsString() +
                 "' would reach a tagged union apart from its check",
             "use the member through its struct");
    return true;
  }

  bool VisitInitListExpr(clang::InitListExpr *list) {
    agrees(list->isSemanticForm() ? *list : *list->getSemanticForm());
    return true;
  }

  bool VisitCompoundStmt(clang::CompoundStmt *block) {
    memberBeforeTag(*block);
    return true;
  }
  // NOLINTEND(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)

private:
  clang::ASTContext &ast;
  const TypeRuleReport &report;
  FixPlanner *fixes;
  llvm::DenseSet<const clang::MemberExpr *> bases;
  llvm::DenseSet<const clang::InitListExpr *> lists;

  /// Whether the glvalue `at` (a tagged union's member, or a tag) is used
  /// other than as the rules allow: read, assigned or stepped, a member or
  /// an element of it used so, copied as a whole, or (a member without
  /// pointers, not a tag) taken as a pointer to `const`.
  bool escapes(const clang::Expr &at, bool tag) {
    const clang::Stmt *child = &at;
    const bool plain = !notPlain(at.getType());
    for (unsigned depth = 0; depth < 32; ++depth) {
      const auto parents = ast.getParents(*child);
      if (parents.empty())
        return false;
      if (const auto *variable = parents[0].get<clang::VarDecl>())
        return variable->getType()->isReferenceType();
      const auto *parent = parents[0].get<clang::Stmt>();
      if (parent == nullptr)
        return false;
      if (llvm::isa<clang::ParenExpr>(parent)) {
        child = parent;
        continue;
      }
      if (const auto *cast = llvm::dyn_cast<clang::ImplicitCastExpr>(parent)) {
        switch (cast->getCastKind()) {
        case clang::CK_LValueToRValue:
          return false;
        case clang::CK_NoOp:
          child = parent;
          continue;
        case clang::CK_ArrayToPointerDecay: {
          // An element read or written in place.
          const auto above = ast.getParents(*parent);
          const auto *subscript =
              above.empty() ? nullptr
                            : above[0].get<clang::ArraySubscriptExpr>();
          if (subscript != nullptr && subscript->getBase() == parent && !tag) {
            child = subscript;
            continue;
          }
          return true;
        }
        default:
          return cast->isGLValue();
        }
      }
      if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(parent)) {
        if (member->getBase() == child && !member->isArrow() && !tag &&
            !llvm::isa<clang::CXXMethodDecl>(member->getMemberDecl())) {
          child = parent;
          continue;
        }
        return true;
      }
      if (const auto *op = llvm::dyn_cast<clang::BinaryOperator>(parent)) {
        // In C++ an assignment, and a comma, give their left or right side
        // as a glvalue: what is done with that is what is done with it.
        if (op->isGLValue() &&
            ((op->isAssignmentOp() && op->getLHS() == child) ||
             (op->getOpcode() == clang::BO_Comma && op->getRHS() == child))) {
          child = parent;
          continue;
        }
        return false;
      }
      if (const auto *op = llvm::dyn_cast<clang::UnaryOperator>(parent)) {
        if (op->isIncrementDecrementOp())
          return false;
        if (op->getOpcode() == clang::UO_AddrOf)
          return tag || !plain ||
                 !op->getType()->getPointeeType().isConstQualified();
        return false;
      }
      if (llvm::isa<clang::ConditionalOperator,
                    clang::BinaryConditionalOperator>(parent))
        return llvm::cast<clang::Expr>(parent)->isGLValue();
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(parent)) {
        // An argument bound to a reference, or the object of a member call.
        if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call);
            member != nullptr && member->getImplicitObjectArgument() == child)
          return true;
        const clang::FunctionDecl *callee = call->getDirectCallee();
        for (unsigned i = 0; i < call->getNumArgs(); ++i)
          if (call->getArg(i) == child) {
            const unsigned skip =
                llvm::isa<clang::CXXOperatorCallExpr>(call) &&
                        callee != nullptr &&
                        llvm::isa<clang::CXXMethodDecl>(callee)
                    ? 1
                    : 0;
            if (callee == nullptr || i < skip ||
                i - skip >= callee->getNumParams())
              return true;
            return callee->getParamDecl(i - skip)->getType()->isReferenceType();
          }
        return true;
      }
      if (const auto *construct =
              llvm::dyn_cast<clang::CXXConstructExpr>(parent))
        return !(construct->getConstructor()->isCopyOrMoveConstructor() &&
                 construct->getConstructor()->isTrivial());
      // (A function returning a reference to it: Safe C++'s `escape`
      // rejects a borrow of a parameter returned without a mark; with one,
      // the caller's use is checked as its own.)
      if (llvm::isa<clang::ReturnStmt>(parent))
        return false;
      if (llvm::isa<clang::MaterializeTemporaryExpr,
                    clang::CXXBindTemporaryExpr, clang::ExprWithCleanups>(
              parent))
        return true;
      return false;
    }
    return false;
  }

  /// §3.2: an initializer of a struct with a tagged union sets the tag to a
  /// constant that the member it initializes is for.
  void agrees(const clang::InitListExpr &list) {
    if (!lists.insert(&list).second)
      return;
    const clang::RecordDecl *record = list.getType()->getAsRecordDecl();
    if (record == nullptr || record->isUnion() ||
        record->getDefinition() == nullptr)
      return;
    std::vector<const clang::FieldDecl *> fields;
    for (const clang::FieldDecl *each : record->getDefinition()->fields())
      fields.push_back(each);
    const auto initOf =
        [&](const clang::FieldDecl *field) -> const clang::Expr * {
      for (std::size_t i = 0; i < fields.size(); ++i)
        if (fields[i] == field && i < list.getNumInits())
          return list.getInit(static_cast<unsigned>(i));
      return nullptr;
    };
    for (const clang::FieldDecl *field : fields) {
      const auto tagged = taggedUnion(*field);
      if (!tagged || !tagged->problem.empty())
        continue;
      const clang::Expr *init = initOf(field);
      const auto *payload = llvm::dyn_cast_or_null<clang::InitListExpr>(
          init != nullptr ? init->IgnoreParenImpCasts() : nullptr);
      // A union given whole (`.u = v`) has a member no tag was checked
      // for: `v`'s bytes may be another member's.
      if (init != nullptr && payload == nullptr &&
          !llvm::isa<clang::ImplicitValueInitExpr>(init)) {
        report(init->getExprLoc(), "union-pointer",
               "'" + field->getNameAsString() +
                   "' is initialized as a whole union, whose member '" +
                   tagged->tagName + "' does not say",
               "initialize one member, '." + field->getNameAsString() +
                   ".<member> = …', with a value of '" + tagged->tagName +
                   "' it is for");
        continue;
      }
      const clang::FieldDecl *chosen =
          payload != nullptr ? payload->getInitializedFieldInUnion() : nullptr;
      if (chosen == nullptr || payload->getNumInits() == 0 ||
          llvm::isa<clang::ImplicitValueInitExpr>(payload->getInit(0)))
        continue;
      const clang::Expr *tagInit = initOf(tagged->tag);
      std::uint64_t value = 0;
      if (tagInit != nullptr &&
          !llvm::isa<clang::ImplicitValueInitExpr>(tagInit)) {
        clang::Expr::EvalResult result;
        if (!tagInit->EvaluateAsInt(result, ast)) {
          report(tagInit->getExprLoc(), "union-pointer",
                 "'" + tagged->tagName + "' is not a constant where '" +
                     chosen->getNameAsString() + "' is initialized",
                 "initialize the tag with the constant the member is for, or "
                 "assign both after");
          continue;
        }
        value = result.Val.getInt().getZExtValue() & tagged->mask;
      }
      const auto values = tagged->when.find(chosen);
      if (values == tagged->when.end() ||
          !llvm::is_contained(values->second, value))
        report(payload->getInit(0)->getExprLoc(), "union-pointer",
               "'" + chosen->getNameAsString() + "' is initialized while '" +
                   tagged->tagName + "' is " + std::to_string(value),
               "initialize '" + tagged->tagName + "' with a value '" +
                   chosen->getNameAsString() + "' is for");
    }
  }

  /// The tagged union's member access an lvalue goes through, if any.
  static const clang::MemberExpr *throughMember(const clang::Expr &lvalue) {
    const clang::Expr *at = lvalue.IgnoreParenImpCasts();
    while (true) {
      const auto *member = llvm::dyn_cast<clang::MemberExpr>(at);
      if (const auto *subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(at);
          subscript != nullptr && subscript->getBase()
                                      ->IgnoreParenImpCasts()
                                      ->getType()
                                      ->isArrayType()) {
        at = subscript->getBase()->IgnoreParenImpCasts();
        continue;
      }
      if (member == nullptr)
        return nullptr;
      if (unionAccess(*member) != nullptr)
        return member;
      if (member->isArrow())
        return nullptr;
      at = member->getBase()->IgnoreParenImpCasts();
    }
  }

  /// The variable, and the tagged union field, of a store to a member of
  /// `x->u` or `x.u` for a local or parameter `x`.
  struct Store {
    const clang::ValueDecl *base = nullptr;
    const clang::FieldDecl *unionField = nullptr;
    const clang::FieldDecl *member = nullptr;
    const clang::FieldDecl *tag = nullptr;
    clang::SourceLocation at;
    const clang::Stmt *statement = nullptr;
    /// A store of null or zero: what a clear of the member writes.
    bool clears = false;
  };
  static std::optional<Store> storeOf(const clang::Stmt *statement) {
    const auto *assign = llvm::dyn_cast<clang::BinaryOperator>(statement);
    if (assign == nullptr || assign->getOpcode() != clang::BO_Assign)
      return std::nullopt;
    const auto *target =
        llvm::dyn_cast<clang::MemberExpr>(assign->getLHS()->IgnoreParens());
    if (target == nullptr)
      return std::nullopt;
    const auto baseOf =
        [](const clang::Expr *base) -> const clang::ValueDecl * {
      if (const auto *named =
              llvm::dyn_cast<clang::DeclRefExpr>(base->IgnoreParenImpCasts()))
        return named->getDecl();
      return nullptr;
    };
    Store out;
    out.at = target->getMemberLoc();
    out.statement = statement;
    out.clears = assign->getRHS()->isNullPointerConstant(
                     target->getMemberDecl()->getASTContext(),
                     clang::Expr::NPC_ValueDependentIsNotNull) !=
                 clang::Expr::NPCK_NotNull;
    if (const clang::MemberExpr *member = throughMember(*target)) {
      const auto *field = llvm::cast<clang::MemberExpr>(
          member->getBase()->IgnoreParenImpCasts());
      out.unionField = llvm::cast<clang::FieldDecl>(field->getMemberDecl());
      out.member = llvm::cast<clang::FieldDecl>(member->getMemberDecl());
      out.base = baseOf(field->getBase());
      return out.base != nullptr ? std::optional(out) : std::nullopt;
    }
    if (const auto *field =
            llvm::dyn_cast<clang::FieldDecl>(target->getMemberDecl())) {
      out.tag = field;
      out.unionField = tagOf(*field);
      out.base = baseOf(target->getBase());
      return out.base != nullptr && out.unionField != nullptr
                 ? std::optional(out)
                 : std::nullopt;
    }
    return std::nullopt;
  }

  /// The certain fix: the tag's store, moved before the member's.
  void tagFirst(const Store &member, const Store &tag) {
    if (fixes == nullptr)
      return;
    const clang::SourceManager &sources = ast.getSourceManager();
    const clang::SourceLocation begin =
        sources.getFileLoc(tag.statement->getBeginLoc());
    const clang::SourceLocation last =
        sources.getFileLoc(tag.statement->getEndLoc());
    if (begin.isInvalid() || last.isInvalid())
      return;
    // Through the statement's `;`.
    const std::optional<clang::Token> semicolon =
        clang::Lexer::findNextToken(last, sources, ast.getLangOpts());
    if (!semicolon || !semicolon->is(clang::tok::semi))
      return;
    const clang::SourceLocation end = semicolon->getEndLoc();
    const std::string text =
        clang::Lexer::getSourceText(
            clang::CharSourceRange::getCharRange(begin, end), sources,
            ast.getLangOpts())
            .str();
    // The tag's line goes, with its indentation and its end; it is written
    // again on a line of its own before the member's, indented as that is.
    bool invalid = false;
    const llvm::StringRef buffer =
        sources.getBufferData(sources.getFileID(begin), &invalid);
    if (invalid)
      return;
    unsigned from = sources.getFileOffset(begin);
    unsigned to = sources.getFileOffset(end);
    while (from > 0 && (buffer[from - 1] == ' ' || buffer[from - 1] == '\t'))
      --from;
    const bool wholeLine = (from == 0 || buffer[from - 1] == '\n') &&
                           to < buffer.size() && buffer[to] == '\n';
    if (wholeLine)
      ++to;
    else
      from = sources.getFileOffset(begin);
    const clang::SourceLocation memberAt =
        sources.getFileLoc(member.statement->getBeginLoc());
    unsigned indentFrom = sources.getFileOffset(memberAt);
    while (indentFrom > 0 &&
           (buffer[indentFrom - 1] == ' ' || buffer[indentFrom - 1] == '\t'))
      --indentFrom;
    const std::string indent =
        buffer.substr(indentFrom, sources.getFileOffset(memberAt) - indentFrom)
            .str();
    Want wanted;
    wanted.kind = Want::Kind::Write;
    wanted.level = "certain";
    wanted.aspect = "order";
    wanted.fact = "tag-first";
    wanted.removal = clang::CharSourceRange::getCharRange(
        sources.getComposedLoc(sources.getFileID(begin), from),
        sources.getComposedLoc(sources.getFileID(begin), to));
    wanted.insertions.emplace_back(memberAt, text + "\n" + indent);
    wanted.description = "set '" + tag.tag->getNameAsString() + "' first";
    fixes->want(member.at, std::move(wanted));
  }

  void memberBeforeTag(const clang::CompoundStmt &block) {
    std::vector<Store> stores;
    for (const clang::Stmt *statement : block.body())
      if (const auto store = storeOf(statement))
        stores.push_back(*store);
    for (std::size_t i = 0; i < stores.size(); ++i) {
      // (A member stored after its tag in the block is the member the tag
      // was set for.)
      if (stores[i].member == nullptr || stores[i].clears ||
          llvm::any_of(llvm::ArrayRef(stores).take_front(i),
                       [&](const Store &earlier) {
                         return earlier.tag != nullptr &&
                                earlier.base == stores[i].base &&
                                earlier.unionField == stores[i].unionField;
                       }))
        continue;
      for (std::size_t j = i + 1; j < stores.size(); ++j)
        if (stores[j].tag != nullptr && stores[j].base == stores[i].base &&
            stores[j].unionField == stores[i].unionField) {
          tagFirst(stores[i], stores[j]);
          report(stores[i].at, "union-pointer",
                 "'" + stores[i].member->getNameAsString() +
                     "' is stored before '" + stores[j].tag->getNameAsString() +
                     "' is set, and the store of '" +
                     stores[j].tag->getNameAsString() + "' clears it",
                 "fix: set '" + stores[j].tag->getNameAsString() + "' first");
          break;
        }
    }
  }
};

} // namespace

void checkTaggedUnions(clang::ASTContext &ast,
                       const clang::FunctionDecl &function,
                       const TypeRuleReport &report, FixPlanner *fixes) {
  if (function.getBody() == nullptr)
    return;
  UnionRules rules(ast, report, fixes);
  rules.TraverseStmt(function.getBody());
}

void checkTaggedExpression(clang::ASTContext &ast, const clang::Stmt &statement,
                           const TypeRuleReport &report) {
  UnionRules rules(ast, report, nullptr);
  // (The visitor takes what it reads by pointers that are not `const`; it
  // changes nothing.)
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  rules.TraverseStmt(const_cast<clang::Stmt *>(&statement));
}

void checkTaggedRecord(const clang::RecordDecl &record,
                       const TypeRuleReport &report) {
  for (const clang::FieldDecl *field : record.fields())
    if (const auto tagged = taggedUnion(*field);
        tagged && !tagged->problem.empty())
      report(field->getLocation(), "bad-annotation", tagged->problem,
             "the tag is an integer field of the same struct, and each "
             "member that holds a pointer says the values it is for");
}

bool taggedMember(const clang::MemberExpr &member) {
  return unionAccess(member) != nullptr;
}

} // namespace weavec::frontend
