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
// function with a const overload of the same parameters). RFC 0046 §2:
// whether a value of it may be sent to another thread, whether one object
// of it may be shared by two, whether a `const` path to it can write
// nothing, and which members of a class its lock guards. Each is
// structural, or read from an attribute Clang attaches (`gsl::Pointer`,
// `gsl::Owner`) or from the library table; no type or function is known by
// its name here.
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
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
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

/// RFC 0046 §6: a class with exactly one lock member, and the members that
/// lock guards.
struct LockedClass {
  const clang::FieldDecl *lock = nullptr;
  llvm::SmallPtrSet<const clang::FieldDecl *, 4> locked;
  /// §7: members that are, or hold, a thread object.
  llvm::SmallPtrSet<const clang::FieldDecl *, 2> threads;
  /// More than one member is a lock: the class is not a locked class.
  bool severalLocks = false;
};

/// RFC 0046 §6: a guard the flow knows is held: on the lock member `lock`
/// of `this` (a null `object`) or of the local or parameter `object`, by
/// the guard local `guard`; `shared` to read.
struct HeldGuard {
  const clang::VarDecl *guard = nullptr;
  const clang::VarDecl *object = nullptr;
  const clang::FieldDecl *lock = nullptr;
  bool shared = false;
  bool operator==(const HeldGuard &) const = default;
};

class CxxKinds {
public:
  explicit CxxKinds(const clang::ASTContext &ast) : ast(ast) {}

  /// RFC 0046 §6: the guards held where a lambda is written as an argument
  /// for a parameter that does not keep it, which its body holds too; the
  /// flow of the function that writes it records them.
  llvm::DenseMap<const clang::LambdaExpr *, llvm::SmallVector<HeldGuard, 2>>
      lambdaGuards;
  /// `-mllvm -weavec-thread-rules=0`: RFC 0046's rules are off.
  bool threadRules = true;

  /// The library table's row for the class of `type` has `trait`.
  [[nodiscard]] static bool tableIs(clang::QualType type,
                                    llvm::StringRef trait) {
    const clang::CXXRecordDecl *record =
        type.isNull() ? nullptr
                      : type.getNonReferenceType()->getAsCXXRecordDecl();
    return record != nullptr && isLibrary(*record) &&
           core::LibrarySpec::shipped().classIs(qualifiedName(*record),
                                                trait.str());
  }
  [[nodiscard]] static bool threadClass(clang::QualType type) {
    return tableIs(type, "thread");
  }
  [[nodiscard]] static bool lockClass(clang::QualType type) {
    return tableIs(type, "lock");
  }
  [[nodiscard]] static bool guardClass(clang::QualType type) {
    return tableIs(type, "guard");
  }

  /// §7: an object of `type` is, or holds as an element, a thread object.
  [[nodiscard]] bool holdsThread(clang::QualType type, unsigned depth = 0) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    if (threadClass(type))
      return true;
    const auto *specialization =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
            type->getAsCXXRecordDecl());
    return depth < 8 && specialization != nullptr &&
           isLibrary(*specialization) &&
           llvm::any_of(
               typeArguments(*specialization),
               [&](clang::QualType argument) {
                 return holdsThread(argument, depth + 1);
               });
  }

  /// §3: the thread start `expression` is, with what it gives the thread:
  /// a thread object made from a callable, a `spawns` function, or a
  /// `makes` member of a container of thread objects (`emplace_back`).
  [[nodiscard]] std::optional<llvm::SmallVector<const clang::Expr *, 4>>
  startOf(const clang::Expr &expression) {
    llvm::SmallVector<const clang::Expr *, 4> crossing;
    if (const auto *construct =
            llvm::dyn_cast<clang::CXXConstructExpr>(&expression)) {
      if (!threadClass(construct->getType()) || construct->getNumArgs() == 0 ||
          threadClass(construct->getArg(0)->getType()))
        return std::nullopt;
      crossing.append(construct->arg_begin(), construct->arg_end());
      return crossing;
    }
    const auto *call = llvm::dyn_cast<clang::CallExpr>(&expression);
    const clang::FunctionDecl *callee =
        call == nullptr ? nullptr : call->getDirectCallee();
    if (callee == nullptr || !isLibrary(*callee))
      return std::nullopt;
    const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(call);
    const clang::CXXMethodDecl *method =
        member == nullptr ? nullptr : member->getMethodDecl();
    if (method == nullptr && core::LibrarySpec::shipped().functionIs(
                                 qualifiedName(*callee), "spawns")) {
      for (const clang::Expr *argument : call->arguments())
        if (!argument->getType()->isEnumeralType())
          crossing.push_back(argument);
      return crossing;
    }
    if (method == nullptr || method->isConst() || call->getNumArgs() == 0 ||
        !core::LibrarySpec::shipped().classMember(
            qualifiedName(*method->getParent()), "makes",
            method->getNameAsString()) ||
        !holdsThread(member->getObjectType()) ||
        holdsThread(call->getArg(0)->getType()))
      return std::nullopt;
    crossing.append(call->arg_begin(), call->arg_end());
    return crossing;
  }

  /// §2: a value of `type` may be handed to another thread.
  [[nodiscard]] bool sendable(clang::QualType type) {
    return fact(Fact::Sendable, type);
  }
  /// §8: the same, for a locked member, where an erased value comes only
  /// from what may be sent.
  [[nodiscard]] bool sendableMember(clang::QualType type) {
    return fact(Fact::SendableMember, type);
  }
  /// §2: two threads may use one object of `type` at once.
  [[nodiscard]] bool shareable(clang::QualType type) {
    return fact(Fact::Shareable, type);
  }
  /// §2: an object of `type` reached only through `const` access is never
  /// written.
  [[nodiscard]] bool frozen(clang::QualType type) {
    return fact(Fact::Frozen, type);
  }

  /// An object of `type` has a `mutable` member, which a `const` path can
  /// write.
  [[nodiscard]] bool holdsMutable(clang::QualType type, unsigned depth = 0) {
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > 8)
      return false;
    record = record->getDefinition();
    return record == nullptr ||
           llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return holdsMutable(base.getType(), depth + 1);
                        }) ||
           llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
             return field->isMutable() ||
                    holdsMutable(field->getType(), depth + 1);
           });
  }

  /// §6: the lock of `record` and the members it guards.
  [[nodiscard]] const LockedClass &
  lockedClass(const clang::CXXRecordDecl &record) {
    const clang::CXXRecordDecl *definition = record.getDefinition();
    const clang::CXXRecordDecl *key =
        definition != nullptr ? definition : &record;
    if (const auto found = locks.find(key); found != locks.end())
      return found->second;
    LockedClass info;
    if (definition != nullptr && !isLibrary(*definition)) {
      for (const clang::FieldDecl *field : definition->fields()) {
        if (lockClass(field->getType())) {
          info.severalLocks = info.lock != nullptr;
          info.lock = field;
        } else if (holdsThread(field->getType())) {
          info.threads.insert(field);
        }
      }
      if (info.severalLocks)
        info.lock = nullptr;
      if (info.lock != nullptr)
        for (const clang::FieldDecl *field : definition->fields())
          if (field != info.lock && !info.threads.contains(field) &&
              !sharedMember(*field))
            info.locked.insert(field);
    }
    return locks.try_emplace(key, std::move(info)).first->second;
  }

  /// §2: a member that two threads may use with no lock: of shareable
  /// type, a reference to a shareable object, or `const` and frozen.
  [[nodiscard]] bool sharedMember(const clang::FieldDecl &field) {
    const clang::QualType type = field.getType();
    if (type->isReferenceType())
      return shareable(type->getPointeeType());
    return shareable(type) || (type.isConstQualified() && frozen(type));
  }

  /// The member of the class of `type` that keeps it from being sendable
  /// (`Fact::Sendable`) or shareable, for a note; null when it is the type
  /// itself (a library class, a type argument).
  [[nodiscard]] const clang::FieldDecl *blocker(clang::QualType type,
                                                bool toShare) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr || isLibrary(*record))
      return nullptr;
    const LockedClass &info = lockedClass(*record);
    for (const clang::FieldDecl *field : record->fields()) {
      if (toShare ? !(info.threads.contains(field) || sharedMember(*field) ||
                      (info.locked.contains(field) &&
                       sendableMember(field->getType())))
                  : !sendable(field->getType()))
        return field;
    }
    return nullptr;
  }

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
        core::LibrarySpec::shipped().classIs(qualifiedName(*record), "erased");
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

  /// §5: a member function the table says resets its object. The row
  /// names the class of the object; the library may declare the function
  /// in a base (libc++'s `optional`, libstdc++'s `shared_ptr`).
  [[nodiscard]] static bool resets(const clang::CXXMethodDecl &method,
                                   clang::QualType object) {
    if (method.getIdentifier() == nullptr)
      return false;
    if (!object.isNull() && object->isPointerType())
      object = object->getPointeeType();
    const clang::CXXRecordDecl *of =
        object.isNull() ? nullptr
                        : object.getNonReferenceType()->getAsCXXRecordDecl();
    const auto listed = [&](const clang::CXXRecordDecl *record) {
      return record != nullptr && isLibrary(*record) &&
             core::LibrarySpec::shipped().classMember(
                 qualifiedName(*record), "resets", method.getName());
    };
    return listed(of) || listed(method.getParent());
  }

  /// Whether a borrow can point into an object of `type`: it is, or holds,
  /// one of the library's owners, a shared pointer, a view or an erased
  /// object. A `std::mutex` has a destructor and lends nothing.
  [[nodiscard]] bool lends(clang::QualType type, unsigned depth = 0) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr)
      return false;
    record = record->getDefinition();
    if (record == nullptr || depth > 8)
      return true;
    if (isLibrary(*record)) {
      if (record->hasAttr<clang::OwnerAttr>() || shared(record) ||
          viewClass(record) || erased(type))
        return true;
      const auto *specialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
      return specialization != nullptr &&
             llvm::any_of(specialization->getTemplateArgs().asArray(),
                          [&](const clang::TemplateArgument &argument) {
                            return argument.getKind() ==
                                       clang::TemplateArgument::Type &&
                                   lends(argument.getAsType(), depth + 1);
                          });
    }
    return llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return lends(base.getType(), depth + 1);
                        }) ||
           llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
             return lends(field->getType(), depth + 1);
           });
  }

  /// RFC 0045 §7: a `pointee` inside an object of `type` may be in storage
  /// that changes type in place: a union, in the object or in what it
  /// holds (a library template's arguments, a container's elements), with
  /// two members that are more than a one-character placeholder, one of
  /// which holds a `pointee` (a variant's alternative; a short string's
  /// characters beside a long one's pointer, or, in libstdc++, its
  /// capacity, which the library's own code believes). Structural; an
  /// optional, whose union holds one type, does not switch.
  [[nodiscard]] bool switchesFor(clang::QualType type, clang::QualType pointee,
                                 unsigned depth = 0) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > 12)
      return false;
    record = record->getDefinition();
    if (record == nullptr)
      return false;
    if (record->isUnion()) {
      unsigned members = 0;
      bool holds = false;
      for (const clang::FieldDecl *field : record->fields()) {
        const clang::QualType member = field->getType();
        const clang::CXXRecordDecl *of = member->getAsCXXRecordDecl();
        if (member->isCharType() ||
            (of != nullptr && of->hasDefinition() && of->isEmpty()))
          continue;
        ++members;
        holds |= contains(member, pointee);
      }
      if (members >= 2 && holds)
        return true;
    }
    const auto *specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
    if (specialization != nullptr && isLibrary(*record))
      for (const clang::TemplateArgument &argument :
           specialization->getTemplateArgs().asArray())
        if (argument.getKind() == clang::TemplateArgument::Type &&
            switchesFor(argument.getAsType(), pointee, depth + 1))
          return true;
    return llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return switchesFor(base.getType(), pointee,
                                             depth + 1);
                        }) ||
           llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
             return switchesFor(field->getType(), pointee, depth + 1);
           });
  }

  /// An object of `type` is, or holds as a member, an element or a base, a
  /// `pointee`.
  [[nodiscard]] bool contains(clang::QualType type, clang::QualType pointee,
                              unsigned depth = 0) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    if (clang::ASTContext::hasSameUnqualifiedType(type, pointee))
      return true;
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > 12)
      return false;
    record = record->getDefinition();
    return record != nullptr &&
           (llvm::any_of(record->bases(),
                         [&](const clang::CXXBaseSpecifier &base) {
                           return contains(base.getType(), pointee, depth + 1);
                         }) ||
            llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
              return contains(field->getType(), pointee, depth + 1);
            }));
  }

  /// An object of `type` holds a pointer (not to a function).
  [[nodiscard]] bool holdsPointer(clang::QualType type, unsigned depth = 0) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    if (type->isPointerType())
      return !type->isFunctionPointerType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > 12)
      return false;
    record = record->getDefinition();
    return record == nullptr ||
           llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return holdsPointer(base.getType(), depth + 1);
                        }) ||
           llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
             return holdsPointer(field->getType(), depth + 1);
           });
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

  /// §8: a library class the table marks `shared`: a pointer whose object
  /// has other owners.
  [[nodiscard]] static bool shared(const clang::CXXRecordDecl *record) {
    return record != nullptr && isLibrary(*record) &&
           core::LibrarySpec::shipped().classIs(qualifiedName(*record),
                                                "shared");
  }

  /// The name the table's class rows use: qualified, without inline
  /// namespaces.
  [[nodiscard]] static std::string
  qualifiedName(const clang::NamedDecl &record) {
    std::string name = record.getNameAsString();
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

  /// The type arguments of a library template, packs flattened.
  [[nodiscard]] static llvm::SmallVector<clang::QualType, 4>
  typeArguments(const clang::ClassTemplateSpecializationDecl &specialization) {
    llvm::SmallVector<clang::QualType, 4> types;
    const auto add = [&](const clang::TemplateArgument &argument,
                         const auto &self) -> void {
      if (argument.getKind() == clang::TemplateArgument::Type)
        types.push_back(argument.getAsType());
      else if (argument.getKind() == clang::TemplateArgument::Pack)
        for (const clang::TemplateArgument &inner : argument.pack_elements())
          self(inner, self);
    };
    for (const clang::TemplateArgument &argument :
         specialization.getTemplateArgs().asArray())
      add(argument, add);
    return types;
  }

private:
  const clang::ASTContext &ast;
  enum class Fact : std::uint8_t {
    Sendable,
    SendableMember,
    Shareable,
    Frozen
  };
  /// Settled facts; one being computed is assumed to hold (a recursive
  /// type), and a result that leaned on such an assumption is kept only if
  /// it is false, which no assumption can make true.
  llvm::DenseMap<std::pair<const clang::Type *, unsigned>, bool> facts;
  llvm::DenseSet<std::pair<const clang::Type *, unsigned>> visiting;
  bool assumed = false;
  llvm::DenseMap<const clang::CXXRecordDecl *, LockedClass> locks;

  [[nodiscard]] bool fact(Fact which, clang::QualType type) {
    type = type.getCanonicalType().getUnqualifiedType();
    const std::pair<const clang::Type *, unsigned> key{
        type.getTypePtr(), static_cast<unsigned>(which)};
    if (const auto found = facts.find(key); found != facts.end())
      return found->second;
    if (visiting.contains(key)) {
      assumed = true;
      return true;
    }
    visiting.insert(key);
    const bool outer = assumed;
    assumed = false;
    const bool result = compute(which, type);
    visiting.erase(key);
    if (!result || !assumed || visiting.empty())
      facts[key] = result;
    assumed |= outer;
    return result;
  }

  [[nodiscard]] bool compute(Fact which, clang::QualType type) {
    const bool sends = which == Fact::Sendable || which == Fact::SendableMember;
    if (const clang::ArrayType *array = ast.getAsArrayType(type))
      return fact(which, array->getElementType());
    if (type->isFunctionType())
      return false;
    if (pointerLike(type))
      return which == Fact::Shareable ? false
                                      : shareable(type->getPointeeType());
    if (!type->isRecordType())
      return which != Fact::Shareable;
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || record->getDefinition() == nullptr)
      return false;
    record = record->getDefinition();
    if (isLibrary(*record))
      return computeLibrary(which, type, *record);
    if (which != Fact::Shareable && which != Fact::Frozen && sends) {
      return llvm::all_of(record->bases(),
                          [&](const clang::CXXBaseSpecifier &base) {
                            return fact(which, base.getType());
                          }) &&
             llvm::all_of(record->fields(), [&](const clang::FieldDecl *field) {
               return fact(which, field->getType());
             });
    }
    if (which == Fact::Frozen) {
      if (shareable(type))
        return true;
      return llvm::all_of(record->bases(),
                          [&](const clang::CXXBaseSpecifier &base) {
                            return frozen(base.getType());
                          }) &&
             llvm::all_of(record->fields(), [&](const clang::FieldDecl *field) {
               return field->isMutable() ? shareable(field->getType())
                                         : frozen(field->getType());
             });
    }
    const LockedClass &info = lockedClass(*record);
    return llvm::all_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return shareable(base.getType());
                        }) &&
           llvm::all_of(record->fields(), [&](const clang::FieldDecl *field) {
             return info.threads.contains(field) || sharedMember(*field) ||
                    (info.locked.contains(field) &&
                     sendableMember(field->getType()));
           });
  }

  [[nodiscard]] bool computeLibrary(Fact which, clang::QualType type,
                                    const clang::CXXRecordDecl &record) {
    const auto *specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&record);
    const llvm::SmallVector<clang::QualType, 4> arguments =
        specialization != nullptr ? typeArguments(*specialization)
                                  : llvm::SmallVector<clang::QualType, 4>();
    const auto all = [&](Fact of) {
      return llvm::all_of(arguments, [&](clang::QualType argument) {
        return fact(of, argument);
      });
    };
    const bool synchronized =
        tableIs(type, "lock") || tableIs(type, "shareable");
    if (which == Fact::Shareable)
      return synchronized && all(Fact::Sendable);
    if (synchronized)
      return which == Fact::Frozen || all(Fact::Sendable);
    if (guardClass(type))
      return false;
    if (shared(&record)) {
      if (arguments.empty())
        return false;
      const clang::QualType pointee = arguments.front();
      return shareable(pointee) ||
             (pointee.isConstQualified() && frozen(pointee));
    }
    if (threadClass(type))
      return true;
    if (viewClass(&record))
      return false;
    if (erased(type)) {
      // §8: what an erased object holds is not in its type. A future names
      // its result; a callable's signature names nothing it captured.
      if (which == Fact::SendableMember)
        return true;
      if (which == Fact::Frozen || arguments.empty())
        return false;
      return llvm::none_of(arguments,
                           [](clang::QualType argument) {
                             return argument->isFunctionType();
                           }) &&
             all(Fact::Sendable);
    }
    return all(which);
  }

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

/// RFC 0044 §5 to §9, RFC 0046: the flow rules for one safe function's
/// body (a lambda's too, `lambda` the expression that wrote it). Whether
/// the dataflow settled.
bool checkCxxFlow(clang::ASTContext &ast, CxxKinds &kinds,
                  const clang::FunctionDecl &function, const clang::Stmt &body,
                  const clang::LambdaExpr *lambda, const RuleReport &report);

} // namespace weavec::frontend

#endif // WEAVEC_LIB_FRONTEND_SAFECXXKINDS_H
