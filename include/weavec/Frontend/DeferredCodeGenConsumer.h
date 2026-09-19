//===- DeferredCodeGenConsumer.h - CodeGen after the analysis --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030, section 10.5. Clang's code generator emits external functions as
// soon as the parser hands them over, so an analysis that runs at the end of
// the translation unit, and the check rewrites that follow it, would come too
// late for them. `DeferredCodeGenConsumer` sits in front of the code
// generator: it forwards Sema set-up at once, records every other callback in
// call order, and at the end of the unit runs a hook (the WeaveC analysis and
// the rewrites), replays the recorded calls in order and only then lets the
// code generator finish the unit. Without rewrites it changes nothing in the
// object (gate G7, `scripts/codegen-identity.py`) for the corpus.
//
// Known differences: the replay sees the finished AST, so a declaration that
// comes after a callback can change what the code generator does with it.
// A C99 or GNU inline definition that a later declaration makes external,
// or a tentative array completed after a use, is emitted in a different
// place; `weak_import`, `availability` or `visibility` on a later
// redeclaration applies to earlier references too. The code and its
// meaning are otherwise the same.
//
// The consumer overrides every virtual of `clang::ASTConsumer` and
// `clang::SemaConsumer` in LLVM 23 (`callbacks()` lists them); a virtual
// added upstream and not overridden would reach CodeGen out of order, so the
// list is part of the LLVM-upgrade checklist and a unit test compares it with
// the Clang headers.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DEFERREDCODEGENCONSUMER_H
#define WEAVEC_FRONTEND_DEFERREDCODEGENCONSUMER_H

#include "clang/AST/DeclGroup.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Sema/SemaConsumer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace weavec::frontend {

class DeferredCodeGenConsumer final : public clang::SemaConsumer {
public:
  /// Runs at the end of the unit, before the code generator has seen any
  /// declaration: the analysis, planning and (later) the check rewrites.
  using Hook = llvm::unique_function<void(clang::ASTContext &, clang::Sema &)>;

  /// What the consumer does with one virtual of its bases.
  enum class Disposition : std::uint8_t {
    /// Passed to the inner consumer at once.
    Forwarded,
    /// Recorded, and replayed in order at the end of the unit.
    Recorded,
    /// `HandleTranslationUnit`: hook, replay, then the inner consumer's.
    Finishes,
  };

  struct Callback {
    llvm::StringLiteral name;
    Disposition disposition = Disposition::Recorded;
  };

  DeferredCodeGenConsumer(std::unique_ptr<clang::ASTConsumer> inner, Hook hook);
  ~DeferredCodeGenConsumer() override;
  DeferredCodeGenConsumer(const DeferredCodeGenConsumer &) = delete;
  DeferredCodeGenConsumer &operator=(const DeferredCodeGenConsumer &) = delete;
  DeferredCodeGenConsumer(DeferredCodeGenConsumer &&) = delete;
  DeferredCodeGenConsumer &operator=(DeferredCodeGenConsumer &&) = delete;

  /// Every virtual of `ASTConsumer` and `SemaConsumer` the consumer
  /// overrides, with what it does with each (the LLVM-upgrade checklist).
  static llvm::ArrayRef<Callback> callbacks();

  /// Calls recorded and not yet replayed.
  [[nodiscard]] std::size_t pendingCalls() const { return recorded.size(); }

  // Forwarded at once.
  void Initialize(clang::ASTContext &context) override;
  void InitializeSema(clang::Sema &sema) override;
  void ForgetSema() override;
  clang::ASTMutationListener *GetASTMutationListener() override;
  clang::ASTDeserializationListener *GetASTDeserializationListener() override;
  void PrintStats() override;
  bool shouldSkipFunctionBody(clang::Decl *decl) override;

  // Recorded, and replayed in call order.
  bool HandleTopLevelDecl(clang::DeclGroupRef group) override;
  void HandleInlineFunctionDefinition(clang::FunctionDecl *decl) override;
  void HandleInterestingDecl(clang::DeclGroupRef group) override;
  void HandleTagDeclDefinition(clang::TagDecl *decl) override;
  void HandleTagDeclRequiredDefinition(const clang::TagDecl *decl) override;
  void CompleteTentativeDefinition(clang::VarDecl *decl) override;
  void CompleteExternalDeclaration(clang::DeclaratorDecl *decl) override;
  void HandleImplicitImportDecl(clang::ImportDecl *decl) override;
  void HandleOpenACCRoutineReference(
      const clang::FunctionDecl *function,
      const clang::OpenACCRoutineDecl *routine) override;
  void HandleTopLevelDeclInObjCContainer(clang::DeclGroupRef group) override;
  void
  HandleCXXImplicitFunctionInstantiation(clang::FunctionDecl *decl) override;
  void HandleCXXStaticMemberVarInstantiation(clang::VarDecl *decl) override;
  void AssignInheritanceModel(clang::CXXRecordDecl *decl) override;
  void HandleVTable(clang::CXXRecordDecl *decl) override;

  /// Runs the hook, replays the recorded calls, then finishes the inner
  /// consumer's unit.
  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  enum class Kind : std::uint8_t {
    TopLevelDecl,
    InlineFunctionDefinition,
    InterestingDecl,
    TagDeclDefinition,
    TagDeclRequiredDefinition,
    TentativeDefinition,
    ExternalDeclaration,
    ImplicitImportDecl,
    OpenACCRoutineReference,
    TopLevelDeclInObjCContainer,
    CXXImplicitFunctionInstantiation,
    CXXStaticMemberVarInstantiation,
    InheritanceModel,
    VTable,
  };

  /// One recorded call. `group` is set for the three group callbacks,
  /// `decl` for those taking a mutable declaration, `constDecl` for those
  /// taking a const one, and `routine` for the OpenACC reference.
  struct Call {
    Kind kind = Kind::TopLevelDecl;
    clang::DeclGroupRef group;
    clang::Decl *decl = nullptr;
    const clang::Decl *constDecl = nullptr;
    const clang::OpenACCRoutineDecl *routine = nullptr;
  };

  void record(Call call);
  void replay(const Call &call);

  std::unique_ptr<clang::ASTConsumer> inner;
  Hook hook;
  clang::Sema *sema = nullptr;
  std::vector<Call> recorded;
  /// Set once the replay starts: later calls (a deserialisation the code
  /// generator triggers) go to the inner consumer at once, as they would
  /// without deferral.
  bool passThrough = false;
};

/// True for the frontend actions whose AST consumer is Clang's code
/// generator (`-emit-obj`, `-S`, `-emit-llvm`, `-emit-llvm-bc`, ...).
bool isCodeGenAction(clang::frontend::ActionKind action);

/// The consumer `weavec-cc` installs for a C code-generating action:
/// `analysis` sees the whole unit at its end, before `codeGen` sees any of
/// it (RFC 0030, section 10.5).
std::unique_ptr<clang::ASTConsumer>
createDeferredCodeGenConsumer(std::unique_ptr<clang::ASTConsumer> analysis,
                              std::unique_ptr<clang::ASTConsumer> codeGen);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DEFERREDCODEGENCONSUMER_H
