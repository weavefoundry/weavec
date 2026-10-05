//===- ObjectRegistration.h - Stack and global objects (RFC 0032) -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0032 §4 and §5. A guard asks the runtime which object a pointer points
// into. Heap objects are the allocator's; the stack and global objects of a
// unit are *registered* by code `CheckEmitter` adds:
//
//   char buf[8];             ->  char buf[8], *__weavec_frame_1
//                                    __attribute__((cleanup(__weavec_stack_leave)))
//                                    = __weavec_stack_enter(buf, sizeof buf);
//   setjmp(env)              ->  __weavec_stack_rewind(setjmp(env))
//   int table[64];  (global) ->  static const void *const __weavec_global_1[2]
//                                    __attribute__((used, section(...)))
//                                    = { &table, (void *)sizeof table };
//
// The plan is pure: it reads the AST and decides what is registered.
//
// Stack objects are the *escaping locals* of each emitted function: the
// variables of automatic storage, parameters included, whose address is
// taken, or which are (or hold) an array that decays to a pointer anywhere
// but as the base of a subscript. A local that is only read, written and
// subscripted directly is never looked up and is not registered, and
// neither is one whose address is only an argument of library calls that
// take no callback and guard none of their arguments (RFC 0034 §4). A function
// that calls a returns-twice function registers none (a `longjmp` skips the
// cleanups) and has each such call wrapped instead, so that the entries of
// the frames a `longjmp` abandoned are dropped. A function with automatic
// objects the plan does not register (compound literals, `alloca`) enters
// its objects *loose*: the address one past such an object may be the start
// of an unknown one.
//
// Global objects are the variables of static storage duration the unit
// defines (file-scope and static locals, tentative definitions included),
// except thread-locals, variables in a named section or a non-default
// address space, weak definitions, and types that are incomplete or have a
// flexible array member.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_OBJECTREGISTRATION_H
#define WEAVEC_FRONTEND_OBJECTREGISTRATION_H

#include <vector>

namespace clang {
class ASTContext;
class CallExpr;
class DeclStmt;
class FunctionDecl;
class VarDecl;
} // namespace clang

namespace weavec::analysis {
class SiteIndex;
} // namespace weavec::analysis

namespace weavec::core {
class LibrarySpec;
struct CheckPlan;
} // namespace weavec::core

namespace weavec::frontend {

struct ObjectOptions {
  /// `-f[no-]weavec-stack-objects`.
  bool stack = true;
  /// `-f[no-]weavec-global-objects`.
  bool globals = true;
};

/// One escaping local.
struct StackObject {
  const clang::VarDecl *variable = nullptr;
  /// The declaration statement that declares it; null for a parameter,
  /// which is entered at the start of the body.
  const clang::DeclStmt *statement = nullptr;
  const clang::FunctionDecl *function = nullptr;
  /// The function has automatic objects that are not registered (an
  /// `alloca`, a compound literal): one may start at this one's end.
  bool loose = false;
  /// Declared in a nested scope of the function: it may leave scope, and
  /// its storage be reused, while the function runs (RFC 0032 §13).
  bool scoped = false;
};

/// A call to a returns-twice function.
struct ReturnsTwiceCall {
  const clang::CallExpr *call = nullptr;
  const clang::FunctionDecl *function = nullptr;
};

struct ObjectPlan {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<StackObject> stack = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<ReturnsTwiceCall> rewinds = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<const clang::VarDecl *> globals = {};

  [[nodiscard]] bool empty() const noexcept {
    return stack.empty() && rewinds.empty() && globals.empty();
  }
};

/// Plans the registrations of the unit: the escaping locals and the
/// returns-twice calls of the functions `sites` holds (the emitted ones),
/// and the globals the unit defines. `checks` (may be null: every library
/// argument then counts as guarded) says which calls guard their arguments.
[[nodiscard]] ObjectPlan planObjects(clang::ASTContext &context,
                                     const analysis::SiteIndex &sites,
                                     const core::LibrarySpec &library,
                                     const core::CheckPlan *checks,
                                     const ObjectOptions &options = {});

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_OBJECTREGISTRATION_H
