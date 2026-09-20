//===- SiteTestUtils.h - Helpers for the RFC 0030 seam tests ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_UNITTESTS_ANALYSIS_SITETESTUTILS_H
#define WEAVEC_UNITTESTS_ANALYSIS_SITETESTUTILS_H

#include "weavec/Analysis/AttributeReader.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SiteCollector.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace weavec::test {

/// The declarations the site tests call, spelled as the C library does; the
/// `LibrarySpec` matches them by name and signature. `sys.h` is a system
/// header.
inline constexpr const char *SitePrelude = R"c(
typedef unsigned long size_t;
typedef struct FILE FILE;
void *malloc(size_t);
void free(void *);
void *memcpy(void *, const void *, size_t);
size_t strlen(const char *);
int fclose(FILE *);
void exit(int) __attribute__((noreturn));
typedef long jmp_buf[48];
int setjmp(jmp_buf) __attribute__((returns_twice));
#define RAW __attribute__((annotate("weavec.raw")))
#define UNSAFE __attribute__((annotate("weavec.unsafe")))
#define NONNULL __attribute__((annotate("weavec.nonnull")))
#define NULLABLE __attribute__((annotate("weavec.nullable")))
#define SIZED_BY(n) __attribute__((annotate("weavec.sized_by." #n)))
__attribute__((annotate("weavec.assume"))) static inline void
weavec_assume_(int c) { (void)c; }
#define ASSUME(e) weavec_assume_((e) != 0)
#line 1
)c";

/// A parsed unit with its declared kinds and sites.
struct CollectedUnit {
  std::unique_ptr<clang::ASTUnit> ast;
  analysis::KindTable kinds;
  analysis::SiteIndex sites;

  [[nodiscard]] clang::ASTContext &context() const {
    return ast->getASTContext();
  }
  /// The function definition named `name` (or any declaration of it).
  [[nodiscard]] const clang::FunctionDecl *
  function(llvm::StringRef name) const {
    const clang::FunctionDecl *found = nullptr;
    for (const clang::Decl *decl :
         context().getTranslationUnitDecl()->decls()) {
      const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (fn == nullptr || fn->getName() != name)
        continue;
      found = fn;
      if (fn->doesThisDeclarationHaveABody())
        return fn;
    }
    return found;
  }
  /// The ledger rows of the function named `name`, or null.
  [[nodiscard]] const core::FunctionLedger *row(llvm::StringRef name) const {
    for (const core::FunctionLedger &row : sites.ledgers())
      if (row.name == name)
        return &row;
    return nullptr;
  }
};

/// Parses `code` (after `SitePrelude` unless `prelude` is false) as C with
/// `sys.h` available as a system header, and collects its sites.
inline CollectedUnit collectUnit(const std::string &code,
                                 const std::string &systemHeader = {},
                                 bool prelude = true,
                                 std::vector<std::string> args = {}) {
  CollectedUnit unit;
  std::vector<std::string> arguments{
      "-std=gnu17", "-x", "c", "-w", "-isystem", "/virtual/include"};
  arguments.insert(arguments.end(), args.begin(), args.end());
  clang::tooling::FileContentMappings files;
  if (!systemHeader.empty())
    files.emplace_back("/virtual/include/sys.h",
                       "#pragma clang system_header\n" + systemHeader);
  unit.ast = clang::tooling::buildASTFromCodeWithArgs(
      (prelude ? std::string(SitePrelude) : std::string()) + code, arguments,
      "input.c", "clang-tool",
      std::make_shared<clang::PCHContainerOperations>(),
      clang::tooling::getClangStripDependencyFileAdjuster(), files);
  EXPECT_NE(unit.ast, nullptr);
  if (unit.ast == nullptr)
    return unit;
  EXPECT_FALSE(unit.ast->getDiagnostics().hasErrorOccurred());
  const core::LibrarySpec &library = core::LibrarySpec::shipped();
  unit.kinds = analysis::AttributeReader(unit.context(), library).read();
  unit.sites =
      analysis::SiteCollector(unit.context(), unit.kinds, library).collect();
  return unit;
}

/// `<kind> <text> <facets>` for every site of one function, in ordinal
/// order: `deref *p spatial,null,temporal`. Call sites spell their boundary
/// (`call/exit`).
inline std::vector<std::string> describe(const CollectedUnit &unit,
                                         llvm::StringRef function) {
  std::vector<std::string> out;
  const core::FunctionLedger *row = unit.row(function);
  if (row == nullptr)
    return out;
  for (const core::Site &site : row->sites) {
    std::string text(core::toString(site.kind));
    if (site.boundary)
      text += "/" + std::string(core::toString(*site.boundary));
    text += " " + site.text + " ";
    std::string facets;
    for (const core::Facet facet : core::AllFacets)
      if (site.hasFacet(facet))
        facets +=
            (facets.empty() ? "" : ",") + std::string(core::toString(facet));
    out.push_back(text + (facets.empty() ? "-" : facets));
  }
  return out;
}

} // namespace weavec::test

#endif // WEAVEC_UNITTESTS_ANALYSIS_SITETESTUTILS_H
