//===- DeferredCodeGenConsumer.cpp - CodeGen after the analysis -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/DeferredCodeGenConsumer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Sema/Sema.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

#include <array>
#include <utility>

namespace weavec::frontend {

using Disposition = DeferredCodeGenConsumer::Disposition;

// In the order of clang/AST/ASTConsumer.h, then clang/Sema/SemaConsumer.h.
static constexpr std::array<DeferredCodeGenConsumer::Callback, 22> Callbacks{{
    {.name = "Initialize", .disposition = Disposition::Forwarded},
    {.name = "HandleTopLevelDecl", .disposition = Disposition::Recorded},
    {.name = "HandleInlineFunctionDefinition",
     .disposition = Disposition::Recorded},
    {.name = "HandleInterestingDecl", .disposition = Disposition::Recorded},
    {.name = "HandleTranslationUnit", .disposition = Disposition::Finishes},
    {.name = "HandleTagDeclDefinition", .disposition = Disposition::Recorded},
    {.name = "HandleTagDeclRequiredDefinition",
     .disposition = Disposition::Recorded},
    {.name = "HandleCXXImplicitFunctionInstantiation",
     .disposition = Disposition::Recorded},
    {.name = "HandleTopLevelDeclInObjCContainer",
     .disposition = Disposition::Recorded},
    {.name = "HandleImplicitImportDecl", .disposition = Disposition::Recorded},
    {.name = "CompleteTentativeDefinition",
     .disposition = Disposition::Recorded},
    {.name = "CompleteExternalDeclaration",
     .disposition = Disposition::Recorded},
    {.name = "AssignInheritanceModel", .disposition = Disposition::Recorded},
    {.name = "HandleCXXStaticMemberVarInstantiation",
     .disposition = Disposition::Recorded},
    {.name = "HandleOpenACCRoutineReference",
     .disposition = Disposition::Recorded},
    {.name = "HandleVTable", .disposition = Disposition::Recorded},
    {.name = "GetASTMutationListener", .disposition = Disposition::Forwarded},
    {.name = "GetASTDeserializationListener",
     .disposition = Disposition::Forwarded},
    {.name = "PrintStats", .disposition = Disposition::Forwarded},
    {.name = "shouldSkipFunctionBody", .disposition = Disposition::Forwarded},
    {.name = "InitializeSema", .disposition = Disposition::Forwarded},
    {.name = "ForgetSema", .disposition = Disposition::Forwarded},
}};

DeferredCodeGenConsumer::DeferredCodeGenConsumer(
    std::unique_ptr<clang::ASTConsumer> inner, Hook hook)
    : inner(std::move(inner)), hook(std::move(hook)) {}

DeferredCodeGenConsumer::~DeferredCodeGenConsumer() = default;

llvm::ArrayRef<DeferredCodeGenConsumer::Callback>
DeferredCodeGenConsumer::callbacks() {
  return Callbacks;
}

//===----------------------------------------------------------------------===//
// Forwarded at once
//===----------------------------------------------------------------------===//

void DeferredCodeGenConsumer::Initialize(clang::ASTContext &context) {
  inner->Initialize(context);
}

void DeferredCodeGenConsumer::InitializeSema(clang::Sema &s) {
  sema = &s;
  if (auto *consumer = llvm::dyn_cast<clang::SemaConsumer>(inner.get()))
    consumer->InitializeSema(s);
}

void DeferredCodeGenConsumer::ForgetSema() {
  sema = nullptr;
  if (auto *consumer = llvm::dyn_cast<clang::SemaConsumer>(inner.get()))
    consumer->ForgetSema();
}

clang::ASTMutationListener *DeferredCodeGenConsumer::GetASTMutationListener() {
  return inner->GetASTMutationListener();
}

clang::ASTDeserializationListener *
DeferredCodeGenConsumer::GetASTDeserializationListener() {
  return inner->GetASTDeserializationListener();
}

void DeferredCodeGenConsumer::PrintStats() {
  inner->PrintStats();
}

bool DeferredCodeGenConsumer::shouldSkipFunctionBody(clang::Decl *decl) {
  return inner->shouldSkipFunctionBody(decl);
}

//===----------------------------------------------------------------------===//
// Recorded
//===----------------------------------------------------------------------===//

void DeferredCodeGenConsumer::record(Call call) {
  if (passThrough)
    replay(call);
  else
    recorded.push_back(call);
}

bool DeferredCodeGenConsumer::HandleTopLevelDecl(clang::DeclGroupRef group) {
  record(Call{.kind = Kind::TopLevelDecl, .group = group});
  // The code generator never stops the parse.
  return true;
}

void DeferredCodeGenConsumer::HandleInlineFunctionDefinition(
    clang::FunctionDecl *decl) {
  record(Call{.kind = Kind::InlineFunctionDefinition, .decl = decl});
}

void DeferredCodeGenConsumer::HandleInterestingDecl(clang::DeclGroupRef group) {
  record(Call{.kind = Kind::InterestingDecl, .group = group});
}

void DeferredCodeGenConsumer::HandleTagDeclDefinition(clang::TagDecl *decl) {
  record(Call{.kind = Kind::TagDeclDefinition, .decl = decl});
}

void DeferredCodeGenConsumer::HandleTagDeclRequiredDefinition(
    const clang::TagDecl *decl) {
  record(Call{.kind = Kind::TagDeclRequiredDefinition, .constDecl = decl});
}

void DeferredCodeGenConsumer::CompleteTentativeDefinition(
    clang::VarDecl *decl) {
  record(Call{.kind = Kind::TentativeDefinition, .decl = decl});
}

void DeferredCodeGenConsumer::CompleteExternalDeclaration(
    clang::DeclaratorDecl *decl) {
  record(Call{.kind = Kind::ExternalDeclaration, .decl = decl});
}

void DeferredCodeGenConsumer::HandleImplicitImportDecl(
    clang::ImportDecl *decl) {
  record(Call{.kind = Kind::ImplicitImportDecl, .decl = decl});
}

void DeferredCodeGenConsumer::HandleOpenACCRoutineReference(
    const clang::FunctionDecl *function,
    const clang::OpenACCRoutineDecl *routine) {
  record(Call{.kind = Kind::OpenACCRoutineReference,
              .constDecl = function,
              .routine = routine});
}

void DeferredCodeGenConsumer::HandleTopLevelDeclInObjCContainer(
    clang::DeclGroupRef group) {
  record(Call{.kind = Kind::TopLevelDeclInObjCContainer, .group = group});
}

void DeferredCodeGenConsumer::HandleCXXImplicitFunctionInstantiation(
    clang::FunctionDecl *decl) {
  record(Call{.kind = Kind::CXXImplicitFunctionInstantiation, .decl = decl});
}

void DeferredCodeGenConsumer::HandleCXXStaticMemberVarInstantiation(
    clang::VarDecl *decl) {
  record(Call{.kind = Kind::CXXStaticMemberVarInstantiation, .decl = decl});
}

void DeferredCodeGenConsumer::AssignInheritanceModel(
    clang::CXXRecordDecl *decl) {
  record(Call{.kind = Kind::InheritanceModel, .decl = decl});
}

void DeferredCodeGenConsumer::HandleVTable(clang::CXXRecordDecl *decl) {
  record(Call{.kind = Kind::VTable, .decl = decl});
}

void DeferredCodeGenConsumer::replay(const Call &call) {
  switch (call.kind) {
  case Kind::TopLevelDecl:
    (void)inner->HandleTopLevelDecl(call.group);
    return;
  case Kind::InlineFunctionDefinition:
    inner->HandleInlineFunctionDefinition(
        llvm::cast<clang::FunctionDecl>(call.decl));
    return;
  case Kind::InterestingDecl:
    inner->HandleInterestingDecl(call.group);
    return;
  case Kind::TagDeclDefinition:
    inner->HandleTagDeclDefinition(llvm::cast<clang::TagDecl>(call.decl));
    return;
  case Kind::TagDeclRequiredDefinition:
    inner->HandleTagDeclRequiredDefinition(
        llvm::cast<clang::TagDecl>(call.constDecl));
    return;
  case Kind::TentativeDefinition:
    inner->CompleteTentativeDefinition(llvm::cast<clang::VarDecl>(call.decl));
    return;
  case Kind::ExternalDeclaration:
    inner->CompleteExternalDeclaration(
        llvm::cast<clang::DeclaratorDecl>(call.decl));
    return;
  case Kind::ImplicitImportDecl:
    inner->HandleImplicitImportDecl(llvm::cast<clang::ImportDecl>(call.decl));
    return;
  case Kind::OpenACCRoutineReference:
    inner->HandleOpenACCRoutineReference(
        llvm::cast<clang::FunctionDecl>(call.constDecl), call.routine);
    return;
  case Kind::TopLevelDeclInObjCContainer:
    inner->HandleTopLevelDeclInObjCContainer(call.group);
    return;
  case Kind::CXXImplicitFunctionInstantiation:
    inner->HandleCXXImplicitFunctionInstantiation(
        llvm::cast<clang::FunctionDecl>(call.decl));
    return;
  case Kind::CXXStaticMemberVarInstantiation:
    inner->HandleCXXStaticMemberVarInstantiation(
        llvm::cast<clang::VarDecl>(call.decl));
    return;
  case Kind::InheritanceModel:
    inner->AssignInheritanceModel(llvm::cast<clang::CXXRecordDecl>(call.decl));
    return;
  case Kind::VTable:
    inner->HandleVTable(llvm::cast<clang::CXXRecordDecl>(call.decl));
    return;
  }
  llvm_unreachable("unknown recorded consumer call");
}

//===----------------------------------------------------------------------===//
// The end of the unit
//===----------------------------------------------------------------------===//

void DeferredCodeGenConsumer::HandleTranslationUnit(
    clang::ASTContext &context) {
  if (hook) {
    if (sema != nullptr) {
      hook(context, *sema);
    } else {
      // ParseAST always creates Sema; a consumer driven some other way
      // cannot run the analysis, and must not compile unchecked code.
      clang::DiagnosticsEngine &diagnostics = context.getDiagnostics();
      diagnostics.Report(diagnostics.getCustomDiagID(
          clang::DiagnosticsEngine::Error,
          "WeaveC internal error: the unit was parsed without semantic "
          "analysis; build with -fno-weavec to bypass"));
    }
  }
  // Calls that arrive from here on (the code generator can trigger
  // deserialisation) go straight to the inner consumer, as without deferral.
  passThrough = true;
  for (const Call &call : std::exchange(recorded, {}))
    replay(call);
  inner->HandleTranslationUnit(context);
}

//===----------------------------------------------------------------------===//
// weavec-cc
//===----------------------------------------------------------------------===//

bool isCodeGenAction(clang::frontend::ActionKind action) {
  switch (action) {
  case clang::frontend::EmitAssembly:
  case clang::frontend::EmitBC:
  case clang::frontend::EmitLLVM:
  case clang::frontend::EmitLLVMOnly:
  case clang::frontend::EmitCodeGenOnly:
  case clang::frontend::EmitObj:
    return true;
  default:
    return false;
  }
}

std::unique_ptr<clang::ASTConsumer>
createDeferredCodeGenConsumer(std::unique_ptr<clang::ASTConsumer> analysis,
                              std::unique_ptr<clang::ASTConsumer> codeGen) {
  // Only the analysis consumer's HandleTranslationUnit runs; the WeaveC
  // consumer overrides nothing else.
  return std::make_unique<DeferredCodeGenConsumer>(
      std::move(codeGen),
      [analysis = std::move(analysis)](clang::ASTContext &context,
                                       clang::Sema & /*sema*/) {
        if (analysis)
          analysis->HandleTranslationUnit(context);
      });
}

} // namespace weavec::frontend
