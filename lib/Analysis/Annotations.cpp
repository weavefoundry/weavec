//===- Annotations.cpp - WeaveC source annotations ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Annotations.h"

#include "clang/AST/TypeLoc.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"

namespace weavec::analysis {

std::optional<Annotation> parseAnnotation(llvm::StringRef text) {
  if (!text.starts_with(spelling::Prefix))
    return std::nullopt;
  if (text == spelling::Owned)
    return Annotation::Owned;
  if (text == spelling::Borrowed)
    return Annotation::Borrowed;
  if (text == spelling::MutBorrowed)
    return Annotation::MutBorrowed;
  if (text == spelling::Raw)
    return Annotation::Raw;
  if (text == spelling::Unsafe)
    return Annotation::Unsafe;
  return Annotation::Invalid;
}

static void apply(AnnotationSet &set, Annotation annotation) {
  switch (annotation) {
  case Annotation::Owned:
    set.owned = true;
    break;
  case Annotation::Borrowed:
    set.borrowed = true;
    break;
  case Annotation::MutBorrowed:
    set.mutBorrowed = true;
    break;
  case Annotation::Raw:
    set.raw = true;
    break;
  case Annotation::Unsafe:
    set.unsafe = true;
    break;
  case Annotation::Invalid:
    set.invalid = true;
    break;
  }
}

void AnnotationSet::merge(const AnnotationSet &other) noexcept {
  owned = owned || other.owned;
  borrowed = borrowed || other.borrowed;
  mutBorrowed = mutBorrowed || other.mutBorrowed;
  raw = raw || other.raw;
  unsafe = unsafe || other.unsafe;
  invalid = invalid || other.invalid;
}

AnnotationSet getAnnotations(const clang::Decl &decl) {
  AnnotationSet set;
  for (const auto *attr : decl.specific_attrs<clang::AnnotateAttr>()) {
    if (const auto parsed = parseAnnotation(attr->getAnnotation()))
      apply(set, *parsed);
  }
  return set;
}

bool isUnsafeBlock(const clang::Stmt &stmt) {
  const auto *attributed = llvm::dyn_cast<clang::AttributedStmt>(&stmt);
  if (attributed == nullptr)
    return false;
  return llvm::any_of(attributed->getAttrs(), [](const clang::Attr *attr) {
    const auto *annotate = llvm::dyn_cast<clang::AnnotateAttr>(attr);
    return annotate != nullptr &&
           parseAnnotation(annotate->getAnnotation()) == Annotation::Unsafe;
  });
}

const char *macroSpelling(const AnnotationSet &set) noexcept {
  if (set.owned)
    return "WEAVEC_OWNED";
  if (set.borrowed)
    return "WEAVEC_BORROWED";
  if (set.mutBorrowed)
    return "WEAVEC_MUT";
  if (set.raw)
    return "WEAVEC_RAW";
  return nullptr;
}

// -- Function-pointer types ---------------------------------------------------

bool FunctionTypeAnnotations::anyOwnership() const noexcept {
  return result.ownership() || llvm::any_of(params, [](const AnnotationSet &s) {
           return s.ownership();
         });
}

/// Walks from a declarator's written type down to the prototype it names,
/// through pointers, parens, attributes and typedefs. Annotations on a
/// typedef passed through describe the function's result, since a typedef
/// of a function-pointer type has no other pointer to describe.
static void walkToPrototype(clang::TypeLoc loc, FunctionTypeAnnotations &out,
                            unsigned depth) {
  // Typedef chains are finite, but be defensive against odd ASTs.
  if (depth > 16)
    return;
  while (!loc.isNull()) {
    if (const auto proto = loc.getAs<clang::FunctionProtoTypeLoc>()) {
      out.prototype = proto.getTypePtr();
      out.params.assign(proto.getNumParams(), AnnotationSet{});
      for (unsigned i = 0; i < proto.getNumParams(); ++i) {
        if (const clang::ParmVarDecl *param = proto.getParam(i))
          out.params[i].merge(getAnnotations(*param));
      }
      return;
    }
    if (const auto typedefLoc = loc.getAs<clang::TypedefTypeLoc>()) {
      const clang::TypedefNameDecl *decl = typedefLoc.getTypePtr()->getDecl();
      if (decl == nullptr)
        return;
      out.result.merge(getAnnotations(*decl));
      const clang::TypeSourceInfo *info = decl->getTypeSourceInfo();
      if (info == nullptr)
        return;
      walkToPrototype(info->getTypeLoc(), out, depth + 1);
      return;
    }
    // Every other wrapper (pointer, parens, attributes, qualifiers, arrays of
    // callbacks, elaboration) has exactly one inner type location; a leaf
    // yields a null one and ends the walk.
    loc = loc.getNextTypeLoc();
  }
}

FunctionTypeAnnotations
collectFunctionTypeAnnotations(const clang::Decl &decl) {
  FunctionTypeAnnotations out;
  const clang::TypeSourceInfo *info = nullptr;
  if (const auto *declarator = llvm::dyn_cast<clang::DeclaratorDecl>(&decl)) {
    info = declarator->getTypeSourceInfo();
  } else if (const auto *typedefDecl =
                 llvm::dyn_cast<clang::TypedefNameDecl>(&decl)) {
    info = typedefDecl->getTypeSourceInfo();
  }
  if (info == nullptr)
    return out;
  // Ownership annotations on the declarator itself describe the result.
  out.result.merge(getAnnotations(decl));
  walkToPrototype(info->getTypeLoc(), out, 0);
  if (out.prototype == nullptr) {
    out.result = AnnotationSet{};
    out.params.clear();
  }
  return out;
}

} // namespace weavec::analysis
