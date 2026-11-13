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
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr)
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

  /// §2: a library class the table marks `erased`, or a library template
  /// with one among its arguments (a vector of callbacks).
  [[nodiscard]] bool erased(clang::QualType type) {
    const clang::CXXRecordDecl *record =
        type.getNonReferenceType()->getAsCXXRecordDecl();
    if (record == nullptr || !isLibrary(*record))
      return false;
    record = record->getCanonicalDecl();
    if (const auto found = erasures.find(record); found != erasures.end())
      return found->second;
    erasures[record] = false;
    bool erases =
        core::LibrarySpec::shipped().erasedClass(qualifiedName(*record));
    if (const auto *specialization =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
        specialization != nullptr && !erases)
      for (const clang::TemplateArgument &argument :
           specialization->getTemplateArgs().asArray())
        erases |= argument.getKind() == clang::TemplateArgument::Type &&
                  erased(argument.getAsType());
    erasures[record] = erases;
    return erases;
  }

  /// Whether an object of type `given` can be what a reference or pointer
  /// to `type` names: the same type, or one derived from it or a base.
  [[nodiscard]] static bool related(clang::QualType given,
                                    clang::QualType type) {
    type = type.getNonReferenceType();
    if (type->isPointerType())
      type = type->getPointeeType();
    given = given.getNonReferenceType();
    if (clang::ASTContext::hasSameUnqualifiedType(given, type))
      return true;
    const clang::CXXRecordDecl *derived = given->getAsCXXRecordDecl();
    const clang::CXXRecordDecl *base = type->getAsCXXRecordDecl();
    return derived != nullptr && base != nullptr && derived->hasDefinition() &&
           base->hasDefinition() &&
           (derived->isDerivedFrom(base) || base->isDerivedFrom(derived));
  }

  /// §7: whether an object of class `record` can keep a borrow of an
  /// object of type `given` it is constructed from: it has a reference
  /// member (or a borrowed pointer) to such a type, or a member that is a
  /// library view or erases, which could point anywhere. A class of the
  /// program whose members own what they hold keeps none.
  [[nodiscard]] bool mayBorrow(const clang::CXXRecordDecl *record,
                               clang::QualType given, unsigned depth = 0) {
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr || depth > 8)
      return true;
    if (isLibrary(*record)) {
      // A library template keeps what its arguments could (a vector of a
      // class with a reference member); a library view of its own points
      // anywhere.
      const auto *specialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
      bool viaArguments = false;
      if (specialization != nullptr)
        for (const clang::TemplateArgument &argument :
             specialization->getTemplateArgs().asArray()) {
          if (argument.getKind() != clang::TemplateArgument::Type)
            continue;
          const clang::QualType type = argument.getAsType();
          const clang::CXXRecordDecl *of = type->getAsCXXRecordDecl();
          if (type->isReferenceType()
                  ? related(given, type)
                  : (viewClass(of) && mayBorrow(of, given, depth + 1)))
            return true;
          viaArguments |= type->isReferenceType() || viewClass(of);
        }
      return !viaArguments;
    }
    for (const clang::CXXBaseSpecifier &base : record->bases())
      if (viewClass(base.getType()->getAsCXXRecordDecl()) &&
          mayBorrow(base.getType()->getAsCXXRecordDecl(), given, depth + 1))
        return true;
    for (const clang::FieldDecl *field : record->fields()) {
      clang::QualType type = field->getType();
      while (const clang::ArrayType *array = ast.getAsArrayType(type))
        type = array->getElementType();
      if (type->isReferenceType() ||
          (type->isPointerType() && !type->isFunctionPointerType() &&
           (record->isLambda() ||
            hasAnnotation(annotationsOf(field->getType(), ast),
                          BorrowedAnnotation)))) {
        if (related(given, type) || type->getPointeeType()->isVoidType())
          return true;
        continue;
      }
      const clang::CXXRecordDecl *of = type->getAsCXXRecordDecl();
      if (of == nullptr)
        continue;
      if (isLibrary(*of) ? (viewClass(of) || erased(type))
                         : (viewClass(of) && mayBorrow(of, given, depth + 1)))
        return true;
    }
    return false;
  }

  /// §5: a member function the table says resets its object.
  [[nodiscard]] static bool resets(const clang::CXXMethodDecl &method) {
    const clang::CXXRecordDecl *record = method.getParent();
    return method.getIdentifier() != nullptr && isLibrary(*record) &&
           core::LibrarySpec::shipped().resetsClass(qualifiedName(*record),
                                                    method.getName());
  }

  /// A local of this type has a set of borrows the rules follow.
  [[nodiscard]] bool tracked(clang::QualType type) {
    return view(type) || erased(type);
  }

  /// A view that points elsewhere (`std::string_view`, an iterator, a
  /// closure): what its member functions return is where it points, not
  /// inside it.
  [[nodiscard]] static bool pointerClass(clang::QualType type) {
    const clang::CXXRecordDecl *record =
        type.getNonReferenceType()->getAsCXXRecordDecl();
    return record != nullptr &&
           (record->hasAttr<clang::PointerAttr>() || record->isLambda());
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
    // An instantiation of a member template is compared as it was written
    // (`find(const K &)` with its const twin).
    const clang::FunctionTemplateDecl *primary = method.getPrimaryTemplate();
    const clang::FunctionDecl *templated =
        primary != nullptr ? primary->getTemplatedDecl() : nullptr;
    const clang::FunctionDecl &mine =
        templated != nullptr ? *templated : method;
    bool twin = false;
    for (const clang::NamedDecl *other :
         method.getParent()->lookup(method.getDeclName())) {
      const auto *candidate =
          llvm::dyn_cast_or_null<clang::CXXMethodDecl>(other->getAsFunction());
      if (candidate == nullptr || !candidate->isConst() ||
          candidate->getNumParams() != mine.getNumParams())
        continue;
      bool same = true;
      for (unsigned i = 0; i < mine.getNumParams() && same; ++i)
        same = clang::ASTContext::hasSameType(
            candidate->getParamDecl(i)->getType(),
            mine.getParamDecl(i)->getType());
      if (same) {
        twin = true;
        break;
      }
    }
    accessors[method.getCanonicalDecl()] = twin;
    return twin;
  }

  /// §6: a parameter of a library function that is not a member, taken by
  /// reference to an object that is not const, where an overload takes the
  /// same object as const (`std::get`, `std::begin`): the function only
  /// gives access through it.
  [[nodiscard]] static bool
  accessorParameter(const clang::ParmVarDecl &parameter) {
    const auto *function =
        llvm::dyn_cast<clang::FunctionDecl>(parameter.getDeclContext());
    if (function == nullptr || !isLibrary(*function) ||
        llvm::isa<clang::CXXMethodDecl>(function))
      return false;
    const clang::FunctionDecl *pattern =
        function->getTemplateInstantiationPattern();
    const clang::FunctionDecl &mine = pattern != nullptr ? *pattern : *function;
    const unsigned index = parameter.getFunctionScopeIndex();
    if (index >= mine.getNumParams())
      return false;
    const clang::QualType type = mine.getParamDecl(index)->getType();
    if (!type->isLValueReferenceType() ||
        type->getPointeeType().isConstQualified())
      return false;
    return llvm::any_of(
        mine.getDeclContext()->getRedeclContext()->lookup(mine.getDeclName()),
        [&](const clang::NamedDecl *other) {
          const clang::FunctionDecl *candidate = other->getAsFunction();
          if (candidate == nullptr ||
              candidate->getNumParams() != mine.getNumParams())
            return false;
          const clang::QualType twin =
              candidate->getParamDecl(index)->getType();
          return twin->isLValueReferenceType() &&
                 twin->getPointeeType().isConstQualified() &&
                 clang::ASTContext::hasSameUnqualifiedType(
                     twin->getPointeeType(), type->getPointeeType());
        });
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
                ->lookup(
                    record->getASTContext().DeclarationNames.getCXXOperatorName(
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
          name = (space->getName() + "::" + name).str();
      } else if (const auto *outer =
                     llvm::dyn_cast<clang::CXXRecordDecl>(context)) {
        name = (outer->getName() + "::" + name).str();
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
      return type->isReferenceType() || viewClass(type->getAsCXXRecordDecl());
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
