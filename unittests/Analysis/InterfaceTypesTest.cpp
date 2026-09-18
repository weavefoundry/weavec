//===- InterfaceTypesTest.cpp - Private C interfaces (RFC 0028) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../../lib/Analysis/InterfaceTypes.h"

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include <gtest/gtest.h>

namespace weavec::analysis {

static const clang::VarDecl *interfaceVariable(const clang::ASTContext &context,
                                               llvm::StringRef name) {
  for (const auto *decl : context.getTranslationUnitDecl()->decls())
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
        var && var->getName() == name)
      return var;
  return nullptr;
}

static std::unique_ptr<clang::ASTUnit>
interfaceAST(const std::string &code, const std::string &name = "interface.c") {
  return clang::tooling::buildASTFromCodeWithArgs(
      code, {"-std=c17", "-x", "c", "-w"}, name);
}

TEST(InterfaceTypes, PrivateNestedStorageRetainsTargetLayoutAndQualifiers) {
  auto owner = interfaceAST(R"c(
    struct node { unsigned value; struct node *next; };
    static struct {
      const struct node *head;
      struct { void *(*allocate)(unsigned long); void (*release)(void *); } hooks;
      unsigned counters[4];
    } state;
  )c");
  auto foreign = interfaceAST("struct node; int client(void);", "client.c");
  ASSERT_TRUE(owner);
  ASSERT_TRUE(foreign);
  const auto &source = owner->getASTContext();
  auto &target = foreign->getASTContext();
  const auto *state = interfaceVariable(source, "state");
  ASSERT_NE(state, nullptr);
  const auto description = describeInterfaceType(state->getType(), source);
  ASSERT_TRUE(description);
  const auto count =
      std::distance(target.getTranslationUnitDecl()->decls_begin(),
                    target.getTranslationUnitDecl()->decls_end());
  const auto materialized = materializeInterfaceType(*description, target);
  ASSERT_FALSE(materialized.isNull());
  EXPECT_EQ(target.getTypeSizeInChars(materialized),
            source.getTypeSizeInChars(state->getType()));
  EXPECT_EQ(describeInterfaceType(materialized, target), description);
  EXPECT_EQ(count, std::distance(target.getTranslationUnitDecl()->decls_begin(),
                                 target.getTranslationUnitDecl()->decls_end()));
  for (const auto *decl : target.getTranslationUnitDecl()->decls())
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl))
      EXPECT_FALSE(record->isCompleteDefinition());
}

TEST(InterfaceTypes, AnonymousTypedefsRetainViewsWithoutEnteringClientLookup) {
  auto owner = interfaceAST(R"c(
    typedef struct { const unsigned char *data; unsigned long offset; } cursor;
    typedef struct { cursor saved; const cursor *current; } session;
    static const session state;
  )c");
  auto foreign = interfaceAST("typedef int cursor; int client;", "client.c");
  ASSERT_TRUE(owner);
  ASSERT_TRUE(foreign);
  const auto &source = owner->getASTContext();
  auto &target = foreign->getASTContext();
  const auto description = describeInterfaceType(
      interfaceVariable(source, "state")->getType(), source);
  ASSERT_TRUE(description);
  ASSERT_EQ(description->nodes.front().typedefName, "session");
  const auto declarations =
      std::distance(target.getTranslationUnitDecl()->decls_begin(),
                    target.getTranslationUnitDecl()->decls_end());
  const auto materialized = materializeInterfaceType(*description, target);
  ASSERT_FALSE(materialized.isNull());
  EXPECT_TRUE(materialized.isConstQualified());
  EXPECT_EQ(describeInterfaceType(materialized, target), description);
  EXPECT_EQ(declarations,
            std::distance(target.getTranslationUnitDecl()->decls_begin(),
                          target.getTranslationUnitDecl()->decls_end()));
  auto forged = *description;
  forged.nodes.front().typedefName = "other";
  EXPECT_TRUE(materializeInterfaceType(forged, target).isNull());
  forged.nodes.front().typedefName.clear();
  EXPECT_TRUE(materializeInterfaceType(forged, target).isNull());
}

TEST(InterfaceTypes, TargetMismatchAndForgedViewsCannotBeMaterialized) {
  auto owner =
      interfaceAST("struct item { int value; }; static struct item state;");
  auto foreign = interfaceAST("int client;", "client.c");
  ASSERT_TRUE(owner);
  ASSERT_TRUE(foreign);
  const auto &source = owner->getASTContext();
  auto &target = foreign->getASTContext();
  const auto description = describeInterfaceType(
      interfaceVariable(source, "state")->getType(), source);
  ASSERT_TRUE(description);
  auto invalid = *description;
  invalid.nodes[0].bytes *= 2;
  EXPECT_TRUE(materializeInterfaceType(invalid, target).isNull());
  invalid = *description;
  invalid.nodes[0].view = "forged";
  EXPECT_TRUE(materializeInterfaceType(invalid, target).isNull());
  invalid = *description;
  invalid.nodes[1].kind = core::InterfaceKind::Floating;
  EXPECT_TRUE(materializeInterfaceType(invalid, target).isNull());
}

TEST(InterfaceTypes, UnsupportedLayoutsFailConservatively) {
  auto owner = interfaceAST(R"c(
    static union { int number; void *pointer; } choice;
    static struct { unsigned value : 3; } bits;
    static struct { int n; char data[]; } flexible;
    static _Atomic(int) atomic;
  )c");
  ASSERT_TRUE(owner);
  const auto &context = owner->getASTContext();
  for (const auto *name : {"choice", "bits", "flexible", "atomic"}) {
    const auto *var = interfaceVariable(context, name);
    ASSERT_NE(var, nullptr);
    EXPECT_FALSE(describeInterfaceType(var->getType(), context)) << name;
  }
}

TEST(InterfaceTypes, PrivateIdentitiesAreIndependentOfTypeButLocalToTheirUnit) {
  auto first = interfaceAST("static int state;", "one.c");
  auto changed = interfaceAST("static long state;", "one.c");
  auto second = interfaceAST("static int state;", "two.c");
  ASSERT_TRUE(first);
  ASSERT_TRUE(changed);
  ASSERT_TRUE(second);
  const auto *a = interfaceVariable(first->getASTContext(), "state");
  const auto *b = interfaceVariable(changed->getASTContext(), "state");
  const auto *c = interfaceVariable(second->getASTContext(), "state");
  // The declaration offset changes with this spelling; stable identity uses
  // its source position, never the encoded layout.
  EXPECT_NE(privateStorageName(*a), privateStorageName(*c));
  auto copy = interfaceAST("static int state;", "one.c");
  EXPECT_EQ(privateStorageName(*a), privateStorageName(*interfaceVariable(
                                        copy->getASTContext(), "state")));
  EXPECT_NE(describeInterfaceType(a->getType(), first->getASTContext()),
            describeInterfaceType(b->getType(), changed->getASTContext()));
}

TEST(InterfaceTypes, ConflictingImportedStorageCannotReuseAnOldAdapter) {
  auto owner =
      interfaceAST("static struct hooks { void (*release)(void *); } state;");
  auto foreign = interfaceAST("int client;", "client.c");
  ASSERT_TRUE(owner);
  ASSERT_TRUE(foreign);
  GlobalTable local;
  GlobalTable remote;
  const auto *state = interfaceVariable(owner->getASTContext(), "state");
  const auto name = local.portableName(local.idFor(*state));
  ASSERT_TRUE(name);
  const auto imported =
      remote.importName(*name, foreign->getASTContext(), local.interfaces);
  ASSERT_TRUE(imported);
  EXPECT_FALSE(remote.importName(*name, foreign->getASTContext()));
  auto conflicting = local.interfaces;
  conflicting[*name].reset();
  EXPECT_FALSE(remote.importName(*name, foreign->getASTContext(), conflicting));
  EXPECT_FALSE(remote.importName("@weavec-state:missing",
                                 foreign->getASTContext(), local.interfaces));
  EXPECT_EQ(
      remote.importName(*name, foreign->getASTContext(), local.interfaces),
      imported);
}

TEST(InterfaceTypes, ArraysOfRecordsAndQualifiedPointersRoundTrip) {
  auto owner = interfaceAST(R"c(
    struct item { const char *name; unsigned count; };
    static struct { struct item entries[2]; int *restrict selected; } state;
  )c");
  auto foreign = interfaceAST("int client;", "client.c");
  ASSERT_TRUE(owner);
  ASSERT_TRUE(foreign);
  const auto &source = owner->getASTContext();
  auto &target = foreign->getASTContext();
  const auto description = describeInterfaceType(
      interfaceVariable(source, "state")->getType(), source);
  ASSERT_TRUE(description);
  const auto type = materializeInterfaceType(*description, target);
  ASSERT_FALSE(type.isNull());
  EXPECT_EQ(describeInterfaceType(type, target), description);
}

TEST(InterfaceTypes, DescriptorChangesInvalidateConsultingDependencies) {
  auto owner =
      interfaceAST("struct item { int count; }; static struct item state;");
  auto foreign = interfaceAST("struct item; int client;", "client.c");
  ASSERT_TRUE(owner);
  ASSERT_TRUE(foreign);
  const auto &source = owner->getASTContext();
  const auto &target = foreign->getASTContext();
  const auto description = describeInterfaceType(
      interfaceVariable(source, "state")->getType(), source);
  ASSERT_TRUE(description);
  const auto view = description->nodes.front().view;
  UnitExports unit;
  unit.source = "interface.c";
  unit.objectInterfaces.emplace(view, *description);
  ProgramDatabase database;
  database.add(unit);
  SummaryStore store;
  store.setContext(&target);
  store.setDatabase(&database);
  SummaryStore::Dependencies dependencies;
  store.beginDependencies(dependencies);
  ASSERT_FALSE(store.interfaceType(view).isNull());
  const auto snapshot = store.dependencySnapshot();
  store.endDependencies();
  EXPECT_TRUE(dependencies.contains("@interfaces"));
  EXPECT_TRUE(store.dependenciesCurrent(snapshot));
  auto isolated = database;
  unit.objectInterfaces[view].reset();
  database.add(unit);
  EXPECT_FALSE(store.dependenciesCurrent(snapshot));
  EXPECT_TRUE(store.interfaceType(view).isNull());
  store.setDatabase(&isolated);
  EXPECT_FALSE(store.interfaceType(view).isNull());
  EXPECT_FALSE(store.dependenciesCurrent(snapshot));
  EXPECT_NE(database.objectInterfaces, isolated.objectInterfaces);
}

TEST(InterfaceTypes, StorageIdentityDoesNotEncodeItsDescription) {
  auto first = interfaceAST("typedef int  T; static T state;", "one.c");
  auto changed = interfaceAST("typedef char T; static T state;", "one.c");
  ASSERT_TRUE(first);
  ASSERT_TRUE(changed);
  const auto *a = interfaceVariable(first->getASTContext(), "state");
  const auto *b = interfaceVariable(changed->getASTContext(), "state");
  EXPECT_EQ(privateStorageName(*a), privateStorageName(*b));
  EXPECT_NE(describeInterfaceType(a->getType(), first->getASTContext()),
            describeInterfaceType(b->getType(), changed->getASTContext()));
}

TEST(InterfaceTypes, RepeatedDeclarationsInOneMacroExpansionStayDistinct) {
  const std::string code = R"c(
    #define CELL static int state;
    #define BODY { CELL } { CELL }
    void f(void) { BODY }
  )c";
  const auto identities = [&](const std::string &file) {
    auto ast = interfaceAST(code, file);
    std::vector<std::string> names;
    if (!ast)
      return names;
    const auto visit = [&](auto &&self, const clang::Stmt *statement) -> void {
      if (!statement)
        return;
      if (const auto *decls = llvm::dyn_cast<clang::DeclStmt>(statement))
        for (const auto *decl : decls->decls())
          if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
            names.push_back(privateStorageName(*var));
      for (const auto *child : statement->children())
        self(self, child);
    };
    for (const auto *decl :
         ast->getASTContext().getTranslationUnitDecl()->decls())
      if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl))
        visit(visit, function->getBody());
    return names;
  };
  const auto first = identities("macros.c");
  ASSERT_EQ(first.size(), 2U);
  EXPECT_FALSE(first[0].empty());
  EXPECT_NE(first[0], first[1]);
  EXPECT_EQ(first, identities("macros.c"));
  EXPECT_NE(first, identities("another.c"));
}

TEST(InterfaceTypes, ObjectLayoutIdentityDoesNotDependOnFirstUseQualification) {
  auto ast = interfaceAST(
      "struct item { const int value; };"
      "static const struct item fixed; static struct item mutable;");
  ASSERT_TRUE(ast);
  const auto &context = ast->getASTContext();
  const auto *fixed = interfaceVariable(context, "fixed");
  const auto *mutableValue = interfaceVariable(context, "mutable");
  ASSERT_NE(fixed, nullptr);
  ASSERT_NE(mutableValue, nullptr);
  SummaryStore first;
  SummaryStore second;
  first.setContext(&context);
  second.setContext(&context);
  EXPECT_EQ(first.objectView(fixed->getType()),
            second.objectView(mutableValue->getType()));
  EXPECT_EQ(first.objectInterfaces, second.objectInterfaces);
  ASSERT_EQ(first.objectInterfaces.size(), 1U);
  const auto &description = first.objectInterfaces.begin()->second;
  ASSERT_TRUE(description);
  EXPECT_EQ(description->nodes[0].qualifiers, 0U);
  EXPECT_EQ(description->nodes[description->nodes[0].fields[0].type].qualifiers,
            1U);
  const auto storage = describeInterfaceType(fixed->getType(), context);
  ASSERT_TRUE(storage);
  EXPECT_EQ(storage->nodes[0].qualifiers, 1U);
}

} // namespace weavec::analysis
