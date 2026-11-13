//===- SafeCxxKinds.h - Kinds of C++ types for Safe C++ ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0044 §2. What Safe C++'s rules need to know of a type: whether its
// objects hold owners (a destructor to run), whether they hold a borrow (a
// view), whether the library table says the type erases what it holds, and
// whether a member function only gives access (§6: a non-const member
// function with a const overload of the same parameters). Each is
// structural, or read from an attribute Clang attaches (`gsl::Pointer`,
// `gsl::Owner`); no type or function is known by its name here.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_FRONTEND_SAFECXXKINDS_H
#define WEAVEC_LIB_FRONTEND_SAFECXXKINDS_H

#include "SafeCheckImpl.h"

#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

#include <string>

namespace weavec::frontend {

/// Whether `declaration` is a system header's (the standard library, the
/// SDK, anything behind -isystem): code Safe C++ trusts and does not check.
[[nodiscard]] inline bool isLibrary(const clang::Decl &declaration) {
  const clang::SourceManager &sources =
      declaration.getASTContext().getSourceManager();
  return sources.isInSystemHeader(
      sources.getExpansionLoc(declaration.getLocation()));
}

[[nodiscard]] inline bool hasAnnotate(const clang::Decl &declaration,
                                      llvm::StringRef name) {
  return llvm::any_of(declaration.specific_attrs<clang::AnnotateAttr>(),
                      [&](const clang::AnnotateAttr *attribute) {
                        return attribute->getAnnotation() == name;
                      });
}

class CxxKinds {
public:
  explicit CxxKinds(const clang::ASTContext &ast) : ast(ast) {}

  /// An object of `type` has a destructor to run: changing it can end a
  /// borrow, and moving it leaves it moved.
  [[nodiscard]] bool holdsOwners(clang::QualType type) const {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr)
      return false;
    record = record->getDefinition();
    return record == nullptr || !record->hasTrivialDestructor();
  }

  /// A class whose objects hold a borrow.
  [[nodiscard]] bool viewClass(const clang::CXXRecordDecl *record) {
    if (record == nullptr || (record = record->getDefinition()) == nullptr)
      return false;
    const auto [found, inserted] = views.try_emplace(record, false);
    if (!inserted)
      return found->second;
    const bool view = computeView(*record);
    views[record] = view;
    return view;
  }

  /// A pointer or reference (not to a function), as the type of a local, a
  /// parameter or a result.
  [[nodiscard]] static bool pointerLike(clang::QualType type) {
    if (type->isReferenceType())
      return !type->getPointeeType()->isFunctionType();
    return type->isPointerType() && !type->isFunctionPointerType();
  }

  /// §2: a view, as the type of a local, a parameter or a result.
  [[nodiscard]] bool view(clang::QualType type) {
    return pointerLike(type) || viewClass(type->getAsCXXRecordDecl());
  }

  /// §2: a library class the table marks `erased`.
  [[nodiscard]] bool erased(clang::QualType type) {
    const clang::CXXRecordDecl *record =
        type.getNonReferenceType()->getAsCXXRecordDecl();
    if (record == nullptr || !isLibrary(*record))
      return false;
    const auto [found, inserted] =
        erasures.try_emplace(record->getCanonicalDecl(), false);
    if (inserted)
      found->second =
          core::LibrarySpec::shipped().erasedClass(qualifiedName(*record));
    return found->second;
  }

  /// A local of this type has a set of borrows the rules follow.
  [[nodiscard]] bool tracked(clang::QualType type) {
    return view(type) || erased(type);
  }

  /// A view that owns nothing itself (`std::string_view`, an iterator): a
  /// borrow taken through it points where it points.
  [[nodiscard]] bool pureView(clang::QualType type) {
    type = type.getNonReferenceType();
    return viewClass(type->getAsCXXRecordDecl()) && !holdsOwners(type);
  }

  /// §6: a non-const member function with a const overload of the same
  /// parameters only gives access.
  [[nodiscard]] bool accessor(const clang::CXXMethodDecl &method) {
    if (method.isConst() || method.isStatic())
      return false;
    const auto [found, inserted] =
        accessors.try_emplace(method.getCanonicalDecl(), false);
    if (!inserted)
      return found->second;
    bool twin = false;
    for (const clang::NamedDecl *other :
         method.getParent()->lookup(method.getDeclName())) {
      const auto *candidate = llvm::dyn_cast<clang::CXXMethodDecl>(other);
      if (candidate == nullptr || !candidate->isConst() ||
          candidate->getNumParams() != method.getNumParams())
        continue;
      bool same = true;
      for (unsigned i = 0; i < method.getNumParams() && same; ++i)
        same = ast.hasSameType(candidate->getParamDecl(i)->getType(),
                               method.getParamDecl(i)->getType());
      if (same) {
        twin = true;
        break;
      }
    }
    accessors[method.getCanonicalDecl()] = twin;
    return twin;
  }

  /// A library class Clang marks as an owner (a container, a smart
  /// pointer).
  [[nodiscard]] static bool libraryOwner(const clang::CXXRecordDecl *record) {
    return record != nullptr && isLibrary(*record) &&
           record->hasAttr<clang::OwnerAttr>();
  }

  /// A library class that gives `->`, can be copied, and is neither one of
  /// Clang's owners (which own alone) nor a view: a shared pointer.
  [[nodiscard]] static bool shared(const clang::CXXRecordDecl *record) {
    if (record == nullptr || !isLibrary(*record) || !record->hasDefinition() ||
        record->hasAttr<clang::OwnerAttr>() ||
        record->hasAttr<clang::PointerAttr>() ||
        llvm::none_of(record->ctors(),
                      [](const clang::CXXConstructorDecl *constructor) {
                        return constructor->isCopyConstructor() &&
                               !constructor->isDeleted();
                      }))
      return false;
    return !record
                ->lookup(record->getASTContext().DeclarationNames.getCXXOperatorName(
                    clang::OO_Arrow))
                .empty();
  }

  /// The name the table's class rows use: qualified, without inline
  /// namespaces.
  [[nodiscard]] static std::string
  qualifiedName(const clang::CXXRecordDecl &record) {
    std::string name = record.getName().str();
    for (const clang::DeclContext *context = record.getDeclContext();
         context != nullptr; context = context->getParent()) {
      if (const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
        if (!space->isInline() && !space->isAnonymousNamespace())
          name = space->getName().str() + "::" + name;
      } else if (const auto *outer =
                     llvm::dyn_cast<clang::CXXRecordDecl>(context)) {
        name = outer->getName().str() + "::" + name;
      }
    }
    return name;
  }

private:
  const clang::ASTContext &ast;
  llvm::DenseMap<const clang::CXXRecordDecl *, bool> views;
  llvm::DenseMap<const clang::CXXRecordDecl *, bool> erasures;
  llvm::DenseMap<const clang::CXXMethodDecl *, bool> accessors;

  [[nodiscard]] bool viewArgument(const clang::TemplateArgument &argument) {
    if (argument.getKind() == clang::TemplateArgument::Type) {
      const clang::QualType type = argument.getAsType();
      return type->isReferenceType() ||
             viewClass(type->getAsCXXRecordDecl());
    }
    if (argument.getKind() == clang::TemplateArgument::Pack)
      return llvm::any_of(argument.pack_elements(),
                          [this](const clang::TemplateArgument &inner) {
                            return viewArgument(inner);
                          });
    return false;
  }

  [[nodiscard]] bool computeView(const clang::CXXRecordDecl &record) {
    if (record.hasAttr<clang::PointerAttr>())
      return true;
    if (isLibrary(record)) {
      // A library template is a view when one of its arguments is: what it
      // holds is not read from its private members.
      const auto *specialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&record);
      return specialization != nullptr &&
             llvm::any_of(specialization->getTemplateArgs().asArray(),
                          [this](const clang::TemplateArgument &argument) {
                            return viewArgument(argument);
                          });
    }
    for (const clang::CXXBaseSpecifier &base : record.bases())
      if (viewClass(base.getType()->getAsCXXRecordDecl()))
        return true;
    for (const clang::FieldDecl *field : record.fields()) {
      clang::QualType type = field->getType();
      if (type->isReferenceType())
        return true;
      while (const clang::ArrayType *array = ast.getAsArrayType(type))
        type = array->getElementType();
      if (type->isPointerType()) {
        // A pointer member is a reference, unless it is a lambda's capture
        // or says WEAVEC_BORROWED.
        if (type->isFunctionPointerType())
          continue;
        if (record.isLambda() ||
            hasAnnotation(annotationsOf(field->getType(), ast),
                          BorrowedAnnotation))
          return true;
        continue;
      }
      if (viewClass(type->getAsCXXRecordDecl()))
        return true;
    }
    return false;
  }
};

/// RFC 0044 §5 to §9: the flow rules for one safe function's body (a
/// lambda's too). Whether the dataflow settled.
bool checkCxxFlow(clang::ASTContext &ast, CxxKinds &kinds,
                  const clang::FunctionDecl &function, const clang::Stmt &body,
                  const RuleReport &report);

} // namespace weavec::frontend

#endif // WEAVEC_LIB_FRONTEND_SAFECXXKINDS_H
