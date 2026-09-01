//===- Annotations.cpp - WeaveC source annotations ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Annotations.h"

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
  case Annotation::Unsafe:
    set.unsafe = true;
    break;
  case Annotation::Invalid:
    set.invalid = true;
    break;
  }
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

} // namespace weavec::analysis
