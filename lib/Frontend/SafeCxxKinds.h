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
  /// RFC 0049 §3: the unit's fix planner, which a rule tells what it wants
  /// before it reports.
  FixPlanner *fixes = nullptr;
  void want(clang::SourceLocation at, Want wanted) const {
    if (fixes != nullptr)
      fixes->want(at, std::move(wanted));
  }

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

  /// §7, RFC 0052 §6 (D7): whether an object of class `record` can keep a
  /// borrow of an object of type `given` it is made from: one of its
  /// borrowing members (a reference; a pointer of a class that is not an
  /// owner, or one marked WEAVEC_BORROWED; in a member, a base, or a block
  /// an owner keeps) names a type `given` reaches; or it erases what it
  /// holds. The library's classes are read as the program's are.
  [[nodiscard]] bool mayBorrow(const clang::CXXRecordDecl *record,
                               clang::QualType given) {
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr)
      return true;
    given = given.getNonReferenceType().getCanonicalType().getUnqualifiedType();
    const auto key = std::make_pair(record, given.getTypePtr());
    if (const auto found = borrowsOf.find(key); found != borrowsOf.end())
      return found->second;
    Seen seen;
    const bool answer = keepsBorrow(record, given, seen, 0, false);
    borrowsOf[key] = answer;
    return answer;
  }

  /// RFC 0052 §6: whether an object of class `record` can keep a borrow
  /// of what a view of type `given` points to: of a type one of the view's
  /// borrowing members names. An erased view points anywhere.
  [[nodiscard]] bool mayBorrowThrough(const clang::CXXRecordDecl *record,
                                      clang::QualType given) {
    llvm::SmallVector<clang::QualType, 4> named;
    Seen seen;
    if (!pointees(given, named, seen, 0, false))
      return true;
    // RFC 0053 §5: or keep copies of the view's elements, which point
    // where they do (a vector made from a list of pointers or of
    // reference wrappers).
    return llvm::any_of(named, [&](clang::QualType pointee) {
      const clang::CXXRecordDecl *of = pointee->getAsCXXRecordDecl();
      return mayBorrow(record, pointee) ||
             (pointee->isPointerType() && !pointee->isFunctionPointerType() &&
              mayBorrow(record, pointee->getPointeeType())) ||
             (of != nullptr && borrowing(of) &&
              reaches(ast.getCanonicalTagType(record), pointee));
    });
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

  /// RFC 0047 §5.1: objects of the class are changed through pointers
  /// other objects keep: it, or a base, is WEAVEC_ALIASED.
  [[nodiscard]] static bool aliased(const clang::CXXRecordDecl *record,
                                    unsigned depth = 0) {
    if (record == nullptr || depth > 12)
      return false;
    record = record->getDefinition();
    return record != nullptr &&
           (hasAnnotate(*record, AliasedAnnotation) ||
            llvm::any_of(record->bases(),
                         [&](const clang::CXXBaseSpecifier &base) {
                           return aliased(base.getType()->getAsCXXRecordDecl(),
                                          depth + 1);
                         }));
  }

  /// RFC 0047 §5.2 (K): what an object of `type` owns holds storage that
  /// changes type in place, but for a string's characters: a union of two
  /// members that are more than placeholders, a callable or `std::any`
  /// that erases what it holds, or an owned object of a polymorphic class
  /// that is not aliased, whose derived class could hold one. `strings`:
  /// a string's characters count too (RFC 0047 §5.2, W).
  /// RFC 0052 §7: followed to any depth, each class once.
  [[nodiscard]] bool switchesAny(clang::QualType type, bool strings = false) {
    llvm::SmallPtrSet<const clang::CXXRecordDecl *, 16> seen;
    return switchesAny(type, strings, seen, 0);
  }
  [[nodiscard]] bool
  switchesAny(clang::QualType type, bool strings,
              llvm::SmallPtrSetImpl<const clang::CXXRecordDecl *> &seen,
              unsigned depth) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > Deepest ||
        !seen.insert(record->getCanonicalDecl()).second)
      return false;
    if (isLibrary(*record)) {
      const std::string name = qualifiedName(*record);
      const core::LibrarySpec &table = core::LibrarySpec::shipped();
      if (table.classMember(name, "strings", "c_str"))
        return strings;
      if (shared(record))
        return false;
      if (table.classIs(name, "erased") && !table.classIs(name, "thread") &&
          !table.classMember(name, "joins", "get"))
        return true;
      // A template not yet instantiated is known by its arguments.
      if (const auto *specialization =
              llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record))
        for (const clang::QualType argument : typeArguments(*specialization)) {
          const clang::CXXRecordDecl *of = argument->getAsCXXRecordDecl();
          if ((of != nullptr && of->hasDefinition() &&
               of->getDefinition()->isPolymorphic() && !isLibrary(*of) &&
               !aliased(of)) ||
              switchesAny(argument, strings, seen, depth + 1))
            return true;
        }
    }
    record = record->getDefinition();
    if (record == nullptr)
      return false;
    if (record->isUnion()) {
      unsigned members = 0;
      for (const clang::FieldDecl *field : record->fields()) {
        const clang::CXXRecordDecl *of = field->getType()->getAsCXXRecordDecl();
        const bool placeholder =
            of != nullptr && of->hasDefinition() && of->isEmpty();
        if (!field->getType()->isCharType() && !placeholder)
          ++members;
      }
      if (members >= 2)
        return true;
    }
    return llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return switchesAny(base.getType(), strings, seen,
                                             depth + 1);
                        }) ||
           llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
             return switchesAny(field->getType(), strings, seen, depth + 1);
           });
  }

  /// RFC 0047 §5.2 (S): an object of `type` is or holds a library string.
  [[nodiscard]] static bool holdsString(clang::QualType type,
                                        unsigned depth = 0) {
    type = type.getNonReferenceType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || depth > 8 || !record->hasDefinition())
      return false;
    record = record->getDefinition();
    if (isLibrary(*record) && core::LibrarySpec::shipped().classMember(
                                  qualifiedName(*record), "strings", "c_str"))
      return true;
    if (const auto *specialization =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
        specialization != nullptr && isLibrary(*record))
      return llvm::any_of(typeArguments(*specialization),
                          [&](clang::QualType argument) {
                            return holdsString(argument, depth + 1);
                          });
    return llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
      return holdsString(field->getType(), depth + 1);
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

  /// RFC 0052 §2 (D2): whether an object of type `given` can be, contain,
  /// or own an object of type `target` (a pointer or reference target
  /// stands for what it names): through its bases, members and elements,
  /// and the pointees of the pointers an owner keeps of its own blocks (a
  /// library class with a destructor, a class of the program that declares
  /// one). Where a view points, a reference member or a pointer member of
  /// a class that is not an owner, is not part of the object: the flow
  /// follows it by the view's own loans. `void` reaches every type (a
  /// `void *` erases what it points to); bytes reach only bytes, since safe
  /// code makes no object in them; an incomplete class, and a chain deeper
  /// than `Deepest` classes, reach everything. A search of the types'
  /// graph, each class visited once; the answer is kept per pair.
  [[nodiscard]] bool reaches(clang::QualType given, clang::QualType target) {
    target = target.getNonReferenceType();
    if (target->isPointerType())
      target = target->getPointeeType();
    target = target.getCanonicalType().getUnqualifiedType();
    given = given.getNonReferenceType().getCanonicalType().getUnqualifiedType();
    if (target->isVoidType() || target->isIncompleteType())
      return true;
    const auto key = std::make_pair(given.getTypePtr(), target.getTypePtr());
    if (const auto found = reaching.find(key); found != reaching.end())
      return found->second;
    Seen seen;
    const bool answer = search(given, target, seen, 0);
    reaching[key] = answer;
    return answer;
  }

  /// RFC 0052 §4 (D4): a view of `type` can change what it points to: it
  /// is, or holds as a member or base, a reference or a pointer to what is
  /// not `const` (`std::span<T>`, `std::reference_wrapper<T>`, an
  /// iterator; not `std::string_view`, `std::span<const T>`, a
  /// `const_iterator`). A type that erases what it holds is not read here:
  /// what a callable may change is read from its body, and one the
  /// function cannot see is believed (RFC 0052 §5.2).
  [[nodiscard]] bool writesThrough(clang::QualType type, unsigned depth = 0) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    if (type->isPointerType())
      return !type->isFunctionPointerType() &&
             !type->getPointeeType().isConstQualified();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr || erased(type))
      return false;
    // RFC 0053: a type deeper than the search follows may write through
    // what it holds.
    if (depth > Deepest)
      return true;
    record = record->getDefinition();
    if (record == nullptr)
      return false;
    // RFC 0053 §6 (D5): a class derived from a polymorphic one may write
    // through what it holds.
    if (!isLibrary(*record) && record->isPolymorphic() &&
        !record->hasAttr<clang::FinalAttr>())
      return true;
    if (const auto found = writers.find(record); found != writers.end())
      return found->second;
    writers[record] = false;
    bool writes = false;
    for (const clang::CXXBaseSpecifier &base : record->bases())
      writes = writes || writesThrough(base.getType(), depth + 1);
    for (const clang::FieldDecl *field : record->fields()) {
      const clang::QualType member = field->getType();
      writes = writes || (member->isReferenceType()
                              ? !member->getPointeeType().isConstQualified() &&
                                    !member->getPointeeType()->isFunctionType()
                              : writesThrough(member, depth + 1));
    }
    writers[record] = writes;
    return writes;
  }

  /// RFC 0052 §6: a view of class `record` made from an object of type
  /// `given` points at it, not into it: a reference or pointer it holds
  /// (in a member, a base, or a member that is itself such a view) names
  /// an object of `given`'s type, or of an element's for an array
  /// (`std::reference_wrapper<T>` of a `T`, `std::span<T>` of a `T[N]`),
  /// and nothing it holds could name what the object owns. So every
  /// borrowing member names the object's type or one the object does not
  /// reach (a `Parser` of `Lexer &` and `Stats &`; not one that also keeps
  /// a `std::string_view`, which can point into the lexer's text), and the
  /// object owns none of its own type (a reference to a tree's node can
  /// name a child). A view that erases what it holds points anywhere.
  [[nodiscard]] bool pointsAt(const clang::CXXRecordDecl *record,
                              clang::QualType given) {
    given = given.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(given))
      given = array->getElementType();
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr)
      return false;
    llvm::SmallVector<clang::QualType, 4> named;
    Seen seen;
    if (!pointees(ast.getCanonicalTagType(record), named, seen, 0, false))
      return false;
    bool names = false;
    for (const clang::QualType pointee : named) {
      if (related(given, pointee))
        names = true;
      else if (reaches(given, pointee))
        return false;
    }
    return names && !ownsItsOwn(given);
  }

  /// Whether an object of type `given` keeps, in a block it owns or in a
  /// member, another object a reference to its type could name (a node's
  /// children, a list's next): a change of the object's value can then end
  /// what such a reference names. Its own bases are the object itself.
  [[nodiscard]] bool ownsItsOwn(clang::QualType given) {
    given = given.getNonReferenceType().getCanonicalType().getUnqualifiedType();
    const clang::CXXRecordDecl *record = given->getAsCXXRecordDecl();
    if (record == nullptr)
      return false;
    record = record->getDefinition();
    if (record == nullptr)
      return true;
    if (const auto found = selfOwners.find(record); found != selfOwners.end())
      return found->second;
    Seen seen;
    const bool answer = ownsOfType(record, given, seen, 0, false);
    selfOwners[record] = answer;
    return answer;
  }

  /// RFC 0052 §6 (D7): a class whose objects may hold a borrow: a view
  /// class, or one with a pointer member (in an array, a base, or a member
  /// of such a class) that is not a function's, unless the class is an
  /// owner of what it points to. A class of the program is an owner when
  /// it declares a destructor, which only code that is not safe can give
  /// it; one of the library's when its destructor does anything (a pointer
  /// an owner keeps of its own block, `std::string`'s or `std::vector`'s,
  /// borrows nothing). A library class with a trivial destructor and a
  /// pointer or reference member is a view (`std::initializer_list`,
  /// `std::ranges::ref_view`), which the flow follows as one.
  [[nodiscard]] bool borrowing(const clang::CXXRecordDecl *record,
                               unsigned depth = 0) {
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr)
      return false;
    // RFC 0053: a class deeper than the search follows may borrow.
    if (depth > Deepest || viewClass(record))
      return true;
    if (record->isLambda() ||
        (isLibrary(*record)
             ? !record->hasTrivialDestructor() && !libraryOfViews(*record)
             : ownsBlocks(*record)))
      return false;
    if (isLibrary(*record) && libraryOfViews(*record))
      return true;
    if (const auto found = borrowers.find(record); found != borrowers.end())
      return found->second;
    borrowers[record] = false;
    const auto holds = [&](clang::QualType member) {
      while (const clang::ArrayType *array = ast.getAsArrayType(member))
        member = array->getElementType();
      if (member->isPointerType())
        return !member->isFunctionPointerType();
      if (member->isReferenceType())
        return !member->getPointeeType()->isFunctionType();
      return borrowing(member->getAsCXXRecordDecl(), depth + 1);
    };
    const bool any =
        llvm::any_of(record->bases(),
                     [&](const clang::CXXBaseSpecifier &base) {
                       return holds(base.getType());
                     }) ||
        llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
          return holds(field->getType());
        });
    borrowers[record] = any;
    return any;
  }

  /// RFC 0052 §5.3 (D6): the first capture a callable that is kept may not
  /// have: one by reference, `this`, or a copy of a pointer or of a view
  /// (a `std::shared_ptr` or `std::weak_ptr` owns). Empty when there is
  /// none.
  [[nodiscard]] std::string keptCapture(const clang::LambdaExpr &lambda) {
    for (const clang::LambdaCapture &capture : lambda.captures()) {
      if (capture.capturesThis()) {
        if (capture.getCaptureKind() == clang::LCK_This)
          return "'this'";
        continue;
      }
      if (!capture.capturesVariable())
        continue;
      const clang::ValueDecl *variable = capture.getCapturedVar();
      const std::string name = "'" + variable->getNameAsString() + "'";
      if (capture.getCaptureKind() == clang::LCK_ByRef)
        return name + " by reference";
      const clang::QualType type = variable->getType();
      if (pointerLike(type))
        return name + ", a pointer";
      const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
      if (record != nullptr && !shared(record) &&
          (view(type) || borrowing(record)))
        return name + ", which holds a borrow";
    }
    return {};
  }

  /// RFC 0053 §7 (D7): a class of the program is an owner only if its
  /// destructor does something it wrote (not `= default`).
  [[nodiscard]] static bool ownsBlocks(const clang::CXXRecordDecl &record) {
    const clang::CXXDestructorDecl *destructor = record.getDestructor();
    return destructor != nullptr && destructor->isUserProvided();
  }

  /// RFC 0053 §1: an object of `type` (a binder, a functor) holds views of
  /// `const` objects only, none of which may be changed through `const`
  /// (a view, a callable, a `mutable` owner): a call of it only reads.
  [[nodiscard]] bool readsOnly(clang::QualType type) {
    llvm::SmallVector<clang::QualType, 4> named;
    Seen seen;
    if (!pointees(type, named, seen, 0, false) || named.empty())
      return false;
    return llvm::all_of(named, [&](clang::QualType pointee) {
      const clang::CXXRecordDecl *record = pointee->getAsCXXRecordDecl();
      return pointee.isConstQualified() && !erased(pointee) &&
             !(tracked(pointee) && writesThrough(pointee)) &&
             (record == nullptr || !record->isLambda()) &&
             !mutableOwners(pointee);
    });
  }

  /// RFC 0053 §7 (D7): a class of the program with a `mutable` member that
  /// holds owners, in it or in a member: a `const` call may change it.
  /// A base counts, and so does an element or a type argument of the
  /// library's (a `std::optional`, a `std::vector`, a `std::pair` of one).
  [[nodiscard]] bool mutableOwners(clang::QualType type) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr)
      return false;
    return held(mutables, record,
                [&] { return computeMutableOwners(*record); });
  }

  [[nodiscard]] bool computeMutableOwners(const clang::CXXRecordDecl &record) {
    if (isLibrary(record)) {
      const auto *specialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&record);
      return specialization != nullptr &&
             llvm::any_of(typeArguments(*specialization),
                          [&](clang::QualType argument) {
                            return mutableOwners(argument);
                          });
    }
    return llvm::any_of(record.bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return mutableOwners(base.getType());
                        }) ||
           llvm::any_of(record.fields(), [&](const clang::FieldDecl *field) {
             return (field->isMutable() && holdsOwners(field->getType())) ||
                    mutableOwners(field->getType());
           });
  }

  /// RFC 0053 §7 (D6): making, copying or destroying an object of `type`
  /// runs code of the program (a constructor or destructor it wrote, in
  /// the class, a member, a base, or an element of the library's), which
  /// may change any global.
  [[nodiscard]] bool runsProgram(clang::QualType type) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    if (type->isPointerType())
      return false;
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr || record->isLambda())
      return false;
    return held(programs, record, [&] { return computeRunsProgram(*record); });
  }

  [[nodiscard]] bool computeRunsProgram(const clang::CXXRecordDecl &record) {
    if (isLibrary(record)) {
      const auto *specialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&record);
      return specialization != nullptr &&
             llvm::any_of(typeArguments(*specialization),
                          [&](clang::QualType argument) {
                            return runsProgram(argument);
                          });
    }
    const auto wrote = [](const clang::FunctionDecl *special) {
      return special != nullptr && special->isUserProvided();
    };
    return wrote(record.getDestructor()) ||
           llvm::any_of(record.ctors(), wrote) ||
           llvm::any_of(record.methods(),
                        [&](const clang::CXXMethodDecl *method) {
                          return (method->isCopyAssignmentOperator() ||
                                  method->isMoveAssignmentOperator()) &&
                                 wrote(method);
                        }) ||
           llvm::any_of(record.bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return runsProgram(base.getType());
                        }) ||
           llvm::any_of(record.fields(), [&](const clang::FieldDecl *field) {
             return runsProgram(field->getType());
           });
  }

  /// The note of a kept callable's `escape` (RFC 0052 §9).
  [[nodiscard]] static llvm::StringRef keptFix(bool sharedClass) {
    return sharedClass
               ? "capture '[weak = weak_from_this()]' and lock it in the body, "
                 "so that the callable does not outlive the object it uses"
               : "a kept callable owns what it uses: capture by value what "
                 "the body needs, a 'std::shared_ptr' or 'std::weak_ptr' to "
                 "the object, or take the object as the callable's argument";
  }

  /// The lambda an initializer makes, through the conversions to the type
  /// that keeps it.
  [[nodiscard]] static const clang::LambdaExpr *
  lambdaIn(const clang::Expr *expression) {
    while (expression != nullptr) {
      expression = expression->IgnoreImplicit()->IgnoreParens();
      if (const auto *lambda = llvm::dyn_cast<clang::LambdaExpr>(expression))
        return lambda;
      if (const auto *construct =
              llvm::dyn_cast<clang::CXXConstructExpr>(expression);
          construct != nullptr && construct->getNumArgs() >= 1)
        expression = construct->getArg(0);
      else if (const auto *cast = llvm::dyn_cast<clang::CastExpr>(expression))
        expression = cast->getSubExpr();
      else if (const auto *temporary =
                   llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
        expression = temporary->getSubExpr();
      else
        return nullptr;
    }
    return nullptr;
  }

  /// The class derives from a library base that keeps a library pointer
  /// to it (`std::enable_shared_from_this`, whose member is a
  /// `std::weak_ptr` of the class): it can give a `weak_from_this()`.
  [[nodiscard]] static bool sharesFromThis(const clang::CXXRecordDecl *record) {
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr)
      return false;
    const clang::CXXRecordDecl *self = record->getCanonicalDecl();
    for (const clang::CXXBaseSpecifier &base : record->bases()) {
      const clang::CXXRecordDecl *of = base.getType()->getAsCXXRecordDecl();
      if (of == nullptr || !of->hasDefinition() || !isLibrary(*of))
        continue;
      for (const clang::FieldDecl *field : of->getDefinition()->fields()) {
        const auto *keeps =
            llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
                field->getType()->getAsCXXRecordDecl());
        if (keeps != nullptr && isLibrary(*keeps) &&
            llvm::any_of(typeArguments(*keeps), [&](clang::QualType argument) {
              const clang::CXXRecordDecl *named =
                  argument->getAsCXXRecordDecl();
              return named != nullptr && named->getCanonicalDecl() == self;
            }))
          return true;
      }
    }
    return false;
  }

  /// RFC 0052 §6: the aliasing constructor of a shared pointer
  /// (`std::shared_ptr<T>(owner, pointer)`), which owns what `owner` owns
  /// and points where `pointer` does.
  [[nodiscard]] static bool aliasing(const clang::CXXConstructorDecl &made) {
    return shared(made.getParent()) && made.getNumParams() == 2 &&
           made.getParamDecl(0)->getType()->isReferenceType() &&
           shared(made.getParamDecl(0)
                      ->getType()
                      ->getPointeeType()
                      ->getAsCXXRecordDecl()) &&
           made.getParamDecl(1)->getType()->isPointerType();
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
    return view(type) || erased(type) ||
           borrowing(type.getNonReferenceType()->getAsCXXRecordDecl()) ||
           polymorphicHolder(type);
  }

  /// RFC 0053 §6 (D5): a polymorphic class of the program that is not
  /// final, or a library template of one (a smart pointer to a base):
  /// followed by the flow, with the loans of what it was made from and
  /// none by default.
  [[nodiscard]] bool polymorphicHolder(clang::QualType type,
                                       unsigned depth = 0) {
    type = type.getNonReferenceType();
    if (type->isPointerType())
      type = type->getPointeeType();
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr || depth > 4)
      return false;
    if (!isLibrary(*record))
      return record->isPolymorphic() && !record->hasAttr<clang::FinalAttr>();
    const auto *specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
    return specialization != nullptr &&
           llvm::any_of(typeArguments(*specialization),
                        [&](clang::QualType argument) {
                          return polymorphicHolder(argument, depth + 1);
                        });
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
    // An assignment or a swap changes its object, whatever twins it has
    // (C++23's `const` assignment of a `std::pair` or `std::tuple` of
    // references).
    if (method.isConst() || method.isStatic() || changesOnly(method))
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
    // (Read on the instantiation: libstdc++'s `swap` returns an
    // `enable_if<…>::type` as written.)
    if (index >= mine.getNumParams() || changesOnly(*function))
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

  /// An assignment, or a function that gives no access (returns nothing:
  /// `swap`), whose `const` twin is not one that only reads.
  [[nodiscard]] static bool changesOnly(const clang::FunctionDecl &function) {
    return function.getReturnType()->isVoidType() ||
           (function.isOverloadedOperator() &&
            (function.getOverloadedOperator() == clang::OO_Equal ||
             clang::isCompoundAssignmentOperator(
                 function.getOverloadedOperator())));
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

  /// A question that holds of a class when it holds of something the class
  /// has (RFC 0053's D6 and D7): its settled answers, the classes being
  /// asked, and whether an answer leaned on one of them. A class met again
  /// while it is asked adds nothing its other parts do not, so there it is
  /// taken not to hold; an answer that leaned on that is kept only if it
  /// holds, which no such assumption can undo, or if no class is still
  /// being asked. (Kept while its class was still asked, a "no" would
  /// answer for every class that has this one, whichever was asked first.)
  struct Held {
    llvm::DenseMap<const clang::CXXRecordDecl *, bool> answers;
    llvm::SmallPtrSet<const clang::CXXRecordDecl *, 8> asking;
    bool leaned = false;
  };

  template <typename Compute>
  [[nodiscard]] static bool
  held(Held &question, const clang::CXXRecordDecl *record, Compute compute) {
    if (const auto found = question.answers.find(record);
        found != question.answers.end())
      return found->second;
    if (question.asking.contains(record)) {
      question.leaned = true;
      return false;
    }
    question.asking.insert(record);
    const bool outer = question.leaned;
    question.leaned = false;
    const bool answer = compute();
    question.asking.erase(record);
    if (answer || !question.leaned || question.asking.empty())
      question.answers[record] = answer;
    question.leaned = !question.asking.empty() && (question.leaned || outer);
    return answer;
  }

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
  llvm::DenseMap<const clang::CXXRecordDecl *, bool> writers;
  llvm::DenseMap<const clang::CXXRecordDecl *, bool> borrowers;
  llvm::DenseMap<const clang::CXXRecordDecl *, bool> selfOwners;
  Held mutables;
  Held programs;
  llvm::DenseMap<const clang::CXXRecordDecl *, bool> erasures;
  llvm::DenseMap<const clang::CXXMethodDecl *, bool> accessors;
  llvm::DenseMap<std::pair<const clang::Type *, const clang::Type *>, bool>
      reaching;
  /// RFC 0052 §2: how many classes deep a question about types is followed
  /// before it is taken to reach everything. Each class is visited once per
  /// question, so this bounds only the recursion; the library's own classes
  /// nest deeper than ten (libc++'s `std::variant` has eleven bases above
  /// its union).
  static constexpr unsigned Deepest = 64;

  /// RFC 0053 §5 (D4): a library template one of whose type arguments is a
  /// pointer or a class that borrows (`std::vector<T *>`,
  /// `std::optional<std::reference_wrapper<T>>`, `std::unique_ptr<Flip>`):
  /// a view of what it is made from, however it is made.
  [[nodiscard]] bool libraryOfViews(const clang::CXXRecordDecl &record) {
    const auto *specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&record);
    if (specialization == nullptr || erased(ast.getCanonicalTagType(&record)))
      return false;
    return llvm::any_of(
        typeArguments(*specialization), [&](clang::QualType argument) {
          if (pointerLike(argument))
            return true;
          // (A C library's struct, `FILE`, points into what the C library
          // keeps.)
          const clang::CXXRecordDecl *of = argument->getAsCXXRecordDecl();
          return of != nullptr && of != &record &&
                 (!isLibrary(*of) || !of->isCLike() ||
                  llvm::isa<clang::ClassTemplateSpecializationDecl>(of)) &&
                 borrowing(of, 1);
        });
  }

  /// The classes a search has visited, owned or not.
  using Seen =
      llvm::SmallDenseSet<std::pair<const clang::CXXRecordDecl *, unsigned>,
                          16>;
  llvm::DenseMap<std::pair<const clang::CXXRecordDecl *, const clang::Type *>,
                 bool>
      borrowsOf;

  /// The types the borrowing members of an object of `type` name (as
  /// `keepsBorrow` finds them); false when it erases what it holds, or is
  /// incomplete, and so may point to anything.
  [[nodiscard]] bool pointees(clang::QualType type,
                              llvm::SmallVectorImpl<clang::QualType> &named,
                              Seen &seen, unsigned depth, bool owned) {
    type = type.getNonReferenceType();
    while (const clang::ArrayType *array = ast.getAsArrayType(type))
      type = array->getElementType();
    if (type->isPointerType()) {
      named.push_back(type->getPointeeType());
      return true;
    }
    const clang::CXXRecordDecl *record = type->getAsCXXRecordDecl();
    if (record == nullptr)
      return true;
    record = record->getDefinition();
    if (record == nullptr || depth > Deepest || erased(type))
      return false;
    if (!seen.insert({record, owned ? 1U : 0U}).second)
      return true;
    const bool owner =
        !record->isLambda() &&
        (owned || (isLibrary(*record) ? !record->hasTrivialDestructor()
                                      : ownsBlocks(*record)));
    const auto member = [&](clang::QualType of, bool borrowed) {
      while (const clang::ArrayType *array = ast.getAsArrayType(of))
        of = array->getElementType();
      if (of->isReferenceType()) {
        named.push_back(of->getPointeeType());
        return true;
      }
      if (of->isPointerType()) {
        if (of->isFunctionPointerType())
          return true;
        if (owner && !borrowed)
          return pointees(of->getPointeeType(), named, seen, depth + 1, false);
        named.push_back(of->getPointeeType());
        return true;
      }
      return pointees(of, named, seen, depth + 1, owner);
    };
    return llvm::all_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return member(base.getType(), false);
                        }) &&
           llvm::all_of(record->fields(), [&](const clang::FieldDecl *field) {
             return member(field->getType(),
                           hasAnnotation(annotationsOf(field->getType(), ast),
                                         BorrowedAnnotation));
           });
  }

  [[nodiscard]] bool keepsBorrow(const clang::CXXRecordDecl *record,
                                 clang::QualType given, Seen &seen,
                                 unsigned depth, bool owned) {
    if (record != nullptr)
      record = record->getDefinition();
    if (record == nullptr || depth > Deepest)
      return true;
    if (!seen.insert({record, owned ? 1U : 0U}).second)
      return false;
    if (erased(ast.getCanonicalTagType(record)))
      return true;
    const bool owner =
        !record->isLambda() &&
        (owned || (isLibrary(*record) ? !record->hasTrivialDestructor()
                                      : ownsBlocks(*record)));
    const auto member = [&](clang::QualType type, bool borrowed) {
      while (const clang::ArrayType *array = ast.getAsArrayType(type))
        type = array->getElementType();
      if (type->isReferenceType())
        return !type->getPointeeType()->isFunctionType() &&
               reaches(given, type->getPointeeType());
      if (type->isPointerType()) {
        if (type->isFunctionPointerType())
          return false;
        if (owner && !borrowed) {
          // An owner's own block: what is in it may borrow. RFC 0053 §5:
          // a block of pointers (`std::vector<T *>`'s) borrows what they
          // point to.
          const clang::QualType block = type->getPointeeType();
          if (block->isPointerType())
            return !block->isFunctionPointerType() &&
                   reaches(given, block->getPointeeType());
          const clang::CXXRecordDecl *of = block->getAsCXXRecordDecl();
          return of != nullptr &&
                 keepsBorrow(of, given, seen, depth + 1, false);
        }
        return reaches(given, type->getPointeeType());
      }
      const clang::CXXRecordDecl *of = type->getAsCXXRecordDecl();
      return of != nullptr && keepsBorrow(of, given, seen, depth + 1, owner);
    };
    return llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return member(base.getType(), false);
                        }) ||
           llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
             return member(field->getType(),
                           hasAnnotation(annotationsOf(field->getType(), ast),
                                         BorrowedAnnotation));
           });
  }

  [[nodiscard]] bool search(clang::QualType given, clang::QualType target,
                            Seen &seen, unsigned depth, bool owned = false) {
    given = given.getNonReferenceType().getCanonicalType();
    while (const clang::ArrayType *array = ast.getAsArrayType(given))
      given = array->getElementType().getCanonicalType();
    if (depth > Deepest)
      return true;
    if (related(given, target))
      return true;
    if (given->isVoidType())
      return true;
    if (given->isCharType() || given->isStdByteType())
      return target->isCharType() || target->isStdByteType();
    const clang::CXXRecordDecl *record = given->getAsCXXRecordDecl();
    if (record == nullptr)
      return false;
    record = record->getDefinition();
    if (record == nullptr)
      return true;
    if (!seen.insert({record, owned ? 1U : 0U}).second)
      return false;
    // An owner's pointers are its own blocks, in its members too (the
    // union `std::any` keeps its pointer in); a lambda's are its captures.
    const bool owner =
        !record->isLambda() &&
        (owned || (isLibrary(*record) ? !record->hasTrivialDestructor()
                                      : ownsBlocks(*record)));
    const auto member = [&](clang::QualType type) {
      while (const clang::ArrayType *array = ast.getAsArrayType(type))
        type = array->getElementType();
      if (type->isReferenceType())
        return false;
      if (type->isPointerType())
        return owner && !type->isFunctionPointerType() &&
               search(type->getPointeeType(), target, seen, depth + 1);
      return search(type, target, seen, depth + 1, owner);
    };
    return llvm::any_of(record->bases(),
                        [&](const clang::CXXBaseSpecifier &base) {
                          return member(base.getType());
                        }) ||
           llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
             return member(field->getType());
           });
  }

  /// `ownsItsOwn`: the members of `record` and of its bases, searched for
  /// an object related to `target`. A base is not asked whether it is
  /// related: it is part of the object the question is about.
  [[nodiscard]] bool ownsOfType(const clang::CXXRecordDecl *record,
                                clang::QualType target, Seen &seen,
                                unsigned depth, bool owned) {
    if (depth > Deepest)
      return true;
    const bool owner =
        !record->isLambda() &&
        (owned || (isLibrary(*record) ? !record->hasTrivialDestructor()
                                      : record->hasUserDeclaredDestructor()));
    for (const clang::CXXBaseSpecifier &base : record->bases()) {
      const clang::CXXRecordDecl *of = base.getType()->getAsCXXRecordDecl();
      if (of != nullptr)
        of = of->getDefinition();
      if (of == nullptr || ownsOfType(of, target, seen, depth + 1, owner))
        return true;
    }
    return llvm::any_of(record->fields(), [&](const clang::FieldDecl *field) {
      clang::QualType type = field->getType();
      while (const clang::ArrayType *array = ast.getAsArrayType(type))
        type = array->getElementType();
      if (type->isReferenceType())
        return false;
      if (type->isPointerType())
        return owner && !type->isFunctionPointerType() &&
               search(type->getPointeeType(), target, seen, depth + 1);
      return search(type, target, seen, depth + 1, owner);
    });
  }

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
