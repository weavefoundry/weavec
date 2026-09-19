//===- DeferredCodeGenConsumerTest.cpp - CodeGen after the analysis -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/DeferredCodeGenConsumer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#ifndef WEAVEC_CLANG_INCLUDE_DIRS
#define WEAVEC_CLANG_INCLUDE_DIRS ""
#endif

namespace weavec::frontend {

using Disposition = DeferredCodeGenConsumer::Disposition;
using Log = std::vector<std::string>;

static std::string nameOf(const clang::Decl *decl) {
  const auto *named = llvm::dyn_cast_or_null<clang::NamedDecl>(decl);
  return named ? named->getNameAsString() : std::string("?");
}

static std::string namesOf(clang::DeclGroupRef group) {
  std::string names;
  for (const clang::Decl *decl : group)
    names += " " + nameOf(decl);
  return names;
}

namespace {

/// Stands in for the code generator: logs what reaches it.
class Recorder final : public clang::SemaConsumer {
public:
  explicit Recorder(Log &log) : log(log) {}
  void Initialize(clang::ASTContext & /*context*/) override {
    log.emplace_back("Initialize");
  }
  void InitializeSema(clang::Sema & /*sema*/) override {
    log.emplace_back("InitializeSema");
  }
  void ForgetSema() override { log.emplace_back("ForgetSema"); }
  bool HandleTopLevelDecl(clang::DeclGroupRef group) override {
    log.push_back("TopLevelDecl" + namesOf(group));
    return true;
  }
  void HandleInterestingDecl(clang::DeclGroupRef group) override {
    log.push_back("InterestingDecl" + namesOf(group));
  }
  void HandleTagDeclDefinition(clang::TagDecl *decl) override {
    log.push_back("TagDeclDefinition " + nameOf(decl));
  }
  void HandleTagDeclRequiredDefinition(const clang::TagDecl *decl) override {
    log.push_back("TagDeclRequiredDefinition " + nameOf(decl));
  }
  void CompleteTentativeDefinition(clang::VarDecl *decl) override {
    log.push_back("TentativeDefinition " + nameOf(decl));
  }
  void CompleteExternalDeclaration(clang::DeclaratorDecl *decl) override {
    log.push_back("ExternalDeclaration " + nameOf(decl));
  }
  void HandleTranslationUnit(clang::ASTContext & /*context*/) override {
    log.emplace_back("TranslationUnit");
  }

private:
  Log &log;
};

/// Parses with the recorder behind a DeferredCodeGenConsumer, or alone.
class RecordingAction final : public clang::ASTFrontendAction {
public:
  RecordingAction(Log &log, bool defer, Log *atHook)
      : log(log), defer(defer), atHook(atHook) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*compiler*/,
                    llvm::StringRef /*inFile*/) override {
    auto recorder = std::make_unique<Recorder>(log);
    if (!defer)
      return recorder;
    return std::make_unique<DeferredCodeGenConsumer>(
        std::move(recorder), [this](clang::ASTContext & /*context*/,
                                    clang::Sema & /*sema*/) { *atHook = log; });
  }

private:
  Log &log;
  bool defer;
  Log *atHook;
};

} // namespace

// Every callback C reaches: definitions, a type required complete, a
// tentative definition, #pragma weak and a static function defined late.
static constexpr llvm::StringLiteral Unit = R"C(
struct pair { int a, b; };
int tentative;
int sum(struct pair p) { return p.a + p.b + (int)sizeof(struct pair); }
void hook(void);
#pragma weak hook
static int late(int);
int early(int x) { return late(x) + tentative; }
static int late(int x) { return x; }
)C";

static Log parse(bool defer, Log *atHook = nullptr) {
  Log log;
  EXPECT_TRUE(clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<RecordingAction>(log, defer, atHook), Unit, {"-std=c99"},
      "unit.c"));
  return log;
}

TEST(DeferredCodeGenConsumerTest, ReplaysEveryCallInOrderAfterTheHook) {
  const Log direct = parse(/*defer=*/false);
  Log atHook;
  const Log deferred = parse(/*defer=*/true, &atHook);
  // The inner consumer sees exactly what it sees without deferral...
  EXPECT_EQ(deferred, direct);
  ASSERT_GE(direct.size(), 8U);
  EXPECT_EQ(direct.front(), "Initialize");
  EXPECT_EQ(direct.back(), "ForgetSema");
  // ...but when the hook runs, nothing but the set-up has reached it.
  EXPECT_EQ(atHook, (Log{"Initialize", "InitializeSema"}));
}

TEST(DeferredCodeGenConsumerTest, ForwardsLateCallsAtOnceAndFailsWithoutSema) {
  const std::unique_ptr<clang::ASTUnit> ast =
      clang::tooling::buildASTFromCodeWithArgs(
          "int a; int b(void) { return a; }", {"-std=c99"}, "unit.c");
  ASSERT_TRUE(ast);
  clang::ASTContext &context = ast->getASTContext();
  std::vector<clang::Decl *> decls;
  for (clang::Decl *decl : context.getTranslationUnitDecl()->decls())
    if (llvm::isa<clang::VarDecl, clang::FunctionDecl>(decl))
      decls.push_back(decl);
  ASSERT_EQ(decls.size(), 2U);

  Log log;
  bool hookRan = false;
  DeferredCodeGenConsumer consumer(
      std::make_unique<Recorder>(log),
      [&hookRan](clang::ASTContext & /*context*/, clang::Sema & /*sema*/) {
        hookRan = true;
      });
  consumer.Initialize(context);
  for (clang::Decl *decl : decls)
    EXPECT_TRUE(consumer.HandleTopLevelDecl(clang::DeclGroupRef(decl)));
  EXPECT_EQ(consumer.pendingCalls(), 2U);
  EXPECT_EQ(log, (Log{"Initialize"}));

  // Driven without Sema, the analysis cannot run: an internal error, and
  // the calls still reach the inner consumer, in order.
  consumer.HandleTranslationUnit(context);
  EXPECT_FALSE(hookRan);
  EXPECT_TRUE(context.getDiagnostics().hasErrorOccurred());
  EXPECT_EQ(log, (Log{"Initialize", "TopLevelDecl a", "TopLevelDecl b",
                      "TranslationUnit"}));
  EXPECT_EQ(consumer.pendingCalls(), 0U);

  // A call after the replay (a deserialisation the code generator
  // triggers) goes straight through.
  consumer.HandleInterestingDecl(clang::DeclGroupRef(decls[1]));
  EXPECT_EQ(log.back(), "InterestingDecl b");
  EXPECT_EQ(consumer.pendingCalls(), 0U);
}

TEST(DeferredCodeGenConsumerTest, ForwardsOnlySemaSetUpAndQueries) {
  // RFC 0030, section 10.5: what is forwarded at once; the rest waits.
  const std::set<std::string> forwarded = {"Initialize",
                                           "InitializeSema",
                                           "ForgetSema",
                                           "GetASTMutationListener",
                                           "GetASTDeserializationListener",
                                           "PrintStats",
                                           "shouldSkipFunctionBody"};
  for (const DeferredCodeGenConsumer::Callback &callback :
       DeferredCodeGenConsumer::callbacks()) {
    const std::string name = callback.name.str();
    if (name == "HandleTranslationUnit")
      EXPECT_EQ(callback.disposition, Disposition::Finishes);
    else if (forwarded.contains(name))
      EXPECT_EQ(callback.disposition, Disposition::Forwarded) << name;
    else
      EXPECT_EQ(callback.disposition, Disposition::Recorded) << name;
  }
}

/// The names of the virtual member functions a header declares, but for
/// destructors and SemaConsumer's vtable anchor.
static std::set<std::string> virtualsDeclaredIn(llvm::StringRef header) {
  std::set<std::string> names;
  llvm::SmallVector<llvm::StringRef, 4> dirs;
  llvm::StringRef(WEAVEC_CLANG_INCLUDE_DIRS).split(dirs, ';', -1, false);
  for (const llvm::StringRef dir : dirs) {
    llvm::SmallString<256> path(dir);
    llvm::sys::path::append(path, header);
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer)
      continue;
    std::string text;
    llvm::SmallVector<llvm::StringRef, 256> lines;
    (*buffer)->getBuffer().split(lines, '\n');
    for (const llvm::StringRef line : lines)
      text += line.split("//").first.str() + "\n";
    const std::regex declaration(R"(\bvirtual\s+[^;{}()]*?(~?\w+)\s*\()");
    for (auto match =
             std::sregex_iterator(text.begin(), text.end(), declaration);
         match != std::sregex_iterator(); ++match) {
      const std::string name = (*match)[1].str();
      if (name.front() != '~' && name != "anchor")
        names.insert(name);
    }
    break;
  }
  return names;
}

// The LLVM-upgrade checklist (RFC 0030, section 10.5): a virtual added
// upstream and not overridden would reach the code generator out of order.
TEST(DeferredCodeGenConsumerTest, HandlesEveryVirtualOfTheClangConsumers) {
  std::set<std::string> declared =
      virtualsDeclaredIn("clang/AST/ASTConsumer.h");
  const std::set<std::string> sema =
      virtualsDeclaredIn("clang/Sema/SemaConsumer.h");
  ASSERT_GE(declared.size(), 20U)
      << "ASTConsumer.h not found in " << WEAVEC_CLANG_INCLUDE_DIRS;
  ASSERT_EQ(sema.size(), 2U);
  declared.insert(sema.begin(), sema.end());

  std::set<std::string> handled;
  for (const DeferredCodeGenConsumer::Callback &callback :
       DeferredCodeGenConsumer::callbacks())
    handled.insert(callback.name.str());
  EXPECT_EQ(declared, handled)
      << "clang::ASTConsumer or clang::SemaConsumer changed: override every "
         "virtual in DeferredCodeGenConsumer, forwarding or recording it, and "
         "list it in callbacks()";
}

} // namespace weavec::frontend
