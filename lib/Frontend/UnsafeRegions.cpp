//===- UnsafeRegions.cpp - WEAVEC_UNSAFE source ranges --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/UnsafeRegions.h"

#include "weavec/Analysis/Annotations.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <utility>

namespace weavec::frontend {

void UnsafeRegions::add(llvm::StringRef file, Range range) {
  ranges[file].push_back(range);
}

static bool before(unsigned lineA, unsigned columnA, unsigned lineB,
                   unsigned columnB) {
  return lineA < lineB || (lineA == lineB && columnA < columnB);
}

bool UnsafeRegions::contains(llvm::StringRef file, unsigned line,
                             unsigned column) const {
  const auto found = ranges.find(file);
  if (found == ranges.end() || line == 0)
    return false;
  return std::ranges::any_of(found->second, [&](const Range &range) {
    if (column == 0)
      return range.beginLine <= line && line <= range.endLine;
    return !before(line, column, range.beginLine, range.beginColumn) &&
           !before(range.endLine, range.endColumn, line, column);
  });
}

std::string UnsafeRegions::normalise(llvm::StringRef file,
                                     llvm::StringRef directory) {
  llvm::SmallString<256> path;
  if (llvm::sys::path::is_absolute(file) || directory.empty()) {
    path = file;
  } else {
    path = directory;
    llvm::sys::path::append(path, file);
  }
  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return std::string(path);
}

namespace {

/// Notes whether the unit expanded `WEAVEC_UNSAFE`.
class UnsafeMacroWatch final : public clang::PPCallbacks {
public:
  explicit UnsafeMacroWatch(std::shared_ptr<bool> seen)
      : seen(std::move(seen)) {}

  void MacroExpands(const clang::Token &name,
                    const clang::MacroDefinition & /*definition*/,
                    clang::SourceRange /*range*/,
                    const clang::MacroArgs * /*arguments*/) override {
    if (const clang::IdentifierInfo *identifier = name.getIdentifierInfo())
      if (identifier->getName() == "WEAVEC_UNSAFE")
        *seen = true;
  }

private:
  std::shared_ptr<bool> seen;
};

class UnsafeVisitor final : public clang::RecursiveASTVisitor<UnsafeVisitor> {
public:
  UnsafeVisitor(const clang::SourceManager &sources, std::string directory,
                UnsafeRegions &out)
      : sources(sources), directory(std::move(directory)), out(out) {}

  // NOLINTNEXTLINE(readability-identifier-naming): RecursiveASTVisitor's name
  bool VisitFunctionDecl(clang::FunctionDecl *function) {
    if (function->doesThisDeclarationHaveABody() &&
        isUnsafe(function->specific_attrs<clang::AnnotateAttr>()))
      add(function->getBody()->getSourceRange());
    return true;
  }

  // NOLINTNEXTLINE(readability-identifier-naming): RecursiveASTVisitor's name
  bool VisitAttributedStmt(clang::AttributedStmt *statement) {
    for (const clang::Attr *attribute : statement->getAttrs())
      if (const auto *annotate = llvm::dyn_cast<clang::AnnotateAttr>(attribute))
        if (annotate->getAnnotation() == analysis::spelling::Unsafe)
          add(statement->getSubStmt()->getSourceRange());
    return true;
  }

private:
  const clang::SourceManager &sources;
  std::string directory;
  UnsafeRegions &out;

  template <typename Range>
  static bool isUnsafe(Range attributes) {
    return std::ranges::any_of(attributes, [](const clang::AnnotateAttr *a) {
      return a->getAnnotation() == analysis::spelling::Unsafe;
    });
  }

public:
  void add(clang::SourceRange range) {
    const clang::PresumedLoc begin =
        sources.getPresumedLoc(sources.getExpansionLoc(range.getBegin()));
    const clang::PresumedLoc end =
        sources.getPresumedLoc(sources.getExpansionLoc(range.getEnd()));
    if (begin.isInvalid() || end.isInvalid() ||
        llvm::StringRef(begin.getFilename()) != end.getFilename())
      return;
    out.add(UnsafeRegions::normalise(begin.getFilename(), directory),
            UnsafeRegions::Range{.beginLine = begin.getLine(),
                                 .beginColumn = begin.getColumn(),
                                 .endLine = end.getLine(),
                                 .endColumn = end.getColumn() + 1});
  }
};

class UnsafeRegionCollector final : public clang::ASTConsumer {
public:
  UnsafeRegionCollector(std::shared_ptr<bool> seen, std::string directory,
                        std::shared_ptr<UnsafeRegions> out)
      : seen(std::move(seen)), directory(std::move(directory)),
        out(std::move(out)) {}

  /// A function its author keeps from AddressSanitizer (`no_sanitize
  /// ("address")`, `no_sanitize_address`, `disable_sanitizer_
  /// instrumentation`) reads memory no guard should check (a conservative
  /// garbage collector's stack scan): its body is an unsafe region, as is a
  /// function annotated unsafe by hand.
  bool HandleTopLevelDecl(clang::DeclGroupRef group) override {
    for (clang::Decl *declaration : group) {
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
      if (function == nullptr || !function->doesThisDeclarationHaveABody() ||
          !(keptFromSanitizer(*function) || annotatedUnsafe(*function)))
        continue;
      UnsafeVisitor visitor(function->getASTContext().getSourceManager(),
                            directory, *out);
      visitor.add(function->getBody()->getSourceRange());
    }
    return true;
  }

  void HandleTranslationUnit(clang::ASTContext &context) override {
    if (!*seen)
      return;
    UnsafeVisitor visitor(context.getSourceManager(), directory, *out);
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  std::shared_ptr<bool> seen;
  std::string directory;
  std::shared_ptr<UnsafeRegions> out;

  /// `WEAVEC_UNSAFE` spelled by hand (`annotate("weavec.unsafe")`), which
  /// the macro watch does not see.
  static bool annotatedUnsafe(const clang::FunctionDecl &function) {
    return std::ranges::any_of(function.specific_attrs<clang::AnnotateAttr>(),
                               [](const clang::AnnotateAttr *attribute) {
                                 return attribute->getAnnotation() ==
                                        analysis::spelling::Unsafe;
                               });
  }

  static bool keptFromSanitizer(const clang::FunctionDecl &function) {
    if (function.hasAttr<clang::DisableSanitizerInstrumentationAttr>())
      return true;
    return std::ranges::any_of(function.specific_attrs<clang::NoSanitizeAttr>(),
                               [](const clang::NoSanitizeAttr *attribute) {
                                 return static_cast<bool>(
                                     attribute->getMask() &
                                     clang::SanitizerKind::Address);
                               });
  }
};

} // namespace

std::unique_ptr<clang::ASTConsumer>
createUnsafeRegionCollector(clang::CompilerInstance &compiler,
                            std::shared_ptr<UnsafeRegions> out) {
  auto seen = std::make_shared<bool>(false);
  compiler.getPreprocessor().addPPCallbacks(
      std::make_unique<UnsafeMacroWatch>(seen));
  std::string directory = compiler.getCodeGenOpts().DebugCompilationDir;
  if (directory.empty()) {
    llvm::SmallString<256> current;
    if (!llvm::sys::fs::current_path(current))
      directory = std::string(current);
  }
  return std::make_unique<UnsafeRegionCollector>(
      std::move(seen), std::move(directory), std::move(out));
}

} // namespace weavec::frontend
