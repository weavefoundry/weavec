//===- InputIdentity.cpp - Preprocessed cache inputs (RFC 0020) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/AnalysisCache.h"
#include "weavec/Frontend/CheckedArtifacts.h"

#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/PreprocessorOutputOptions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"

#include "llvm/Support/FileSystem.h"

namespace weavec::frontend {

bool supportsInputIdentity(const clang::CompilerInvocation &invocation) {
  const auto &inputs = invocation.getFrontendOpts().Inputs;
  if (inputs.size() != 1 || !inputs.front().isFile() ||
      inputs.front().getFile() == "-")
    return false;
  const auto &pp = invocation.getPreprocessorOpts();
  return pp.ImplicitPCHInclude.empty() && pp.RemappedFiles.empty() &&
         pp.RemappedFileBuffers.empty() && !invocation.getLangOpts().Modules &&
         invocation.getHeaderSearchOpts().VFSOverlayFiles.empty() &&
         invocation.getFrontendOpts().Plugins.empty();
}

namespace {

class VolatileMacros final : public clang::PPCallbacks {
public:
  explicit VolatileMacros(bool &unstable) : unstable(unstable) {}
  void MacroExpands(const clang::Token &token, const clang::MacroDefinition &,
                    clang::SourceRange, const clang::MacroArgs *) override {
    if (const auto *identifier = token.getIdentifierInfo()) {
      const auto name = identifier->getName();
      if (name == "__TIME__" || name == "__DATE__" || name == "__TIMESTAMP__")
        unstable = true;
    }
  }

private:
  bool &unstable;
};

class InputIdentityAction final : public clang::PreprocessorFrontendAction {
public:
  explicit InputIdentityAction(std::string &identity) : identity(identity) {}

protected:
  bool BeginInvocation(clang::CompilerInstance &compiler) override {
    return supportsInputIdentity(compiler.getInvocation());
  }
  void ExecuteAction() override {
    auto &compiler = getCompilerInstance();
    auto &pp = compiler.getPreprocessor();
    const auto &invocation = compiler.getInvocation();
    // External ASTs, plugins and overlays need additional input manifests.
    // Conservatively bypass them until the cache can validate those artifacts.
    if (!invocation.getPreprocessorOpts().ImplicitPCHInclude.empty() ||
        invocation.getLangOpts().Modules ||
        !invocation.getHeaderSearchOpts().VFSOverlayFiles.empty() ||
        !invocation.getFrontendOpts().Plugins.empty())
      return;
    bool unstable = false;
    pp.addPPCallbacks(std::make_unique<VolatileMacros>(unstable));
    std::string expanded;
    llvm::raw_string_ostream stream(expanded);
    clang::PreprocessorOutputOptions output;
    output.ShowCPP = true;
    output.ShowComments = true;
    output.ShowMacros = true;
    output.ShowIncludeDirectives = true;
    clang::DoPrintPreprocessedInput(pp, &stream, output);
    if (unstable || compiler.getDiagnostics().hasErrorOccurred())
      return;
    clang::CompilerInvocation normalized(invocation);
    normalized.getFrontendOpts().OutputFile.clear();
    llvm::SmallString<256> effectiveDirectory(
        invocation.getFileSystemOpts().WorkingDir);
    if (llvm::sys::fs::make_absolute(effectiveDirectory))
      return;
    llvm::SmallString<256> canonicalDirectory;
    if (llvm::sys::fs::real_path(effectiveDirectory, canonicalDirectory))
      return;
    effectiveDirectory = canonicalDirectory;
    normalized.getFileSystemOpts().WorkingDir = effectiveDirectory.str().str();
    std::vector<std::string> parts = normalized.getCC1CommandLine();
    parts.push_back(effectiveDirectory.str().str());
    parts.push_back(checkedDigest(expanded));
    std::map<std::string, std::string> files;
    const auto &sm = compiler.getSourceManager();
    for (auto it = sm.fileinfo_begin(); it != sm.fileinfo_end(); ++it) {
      const auto buffer = it->second->getBufferIfLoaded();
      if (!buffer)
        continue;
      files[it->first.getName().str()] = checkedDigest(buffer->getBuffer());
    }
    for (const auto &[file, digest] : files) {
      parts.push_back(file);
      parts.push_back(digest);
    }
    identity = checkedCommandDigest(parts);
  }

private:
  std::string &identity;
};

class InputIdentityFactory final
    : public clang::tooling::FrontendActionFactory {
public:
  explicit InputIdentityFactory(std::string &identity) : identity(identity) {}
  std::unique_ptr<clang::FrontendAction> create() override {
    // ClangTool can issue multiple commands for one source. Such a unit is
    // handled by its normal frontend path and is not eligible for retention.
    if (created++) {
      identity.clear();
      return std::make_unique<clang::SyntaxOnlyAction>();
    }
    return std::make_unique<InputIdentityAction>(identity);
  }

private:
  std::string &identity;
  unsigned created = 0;
};

} // namespace

std::unique_ptr<clang::tooling::FrontendActionFactory>
createInputIdentityFactory(std::string &identity) {
  identity.clear();
  return std::make_unique<InputIdentityFactory>(identity);
}

std::string analysisOptionsIdentity(const FrontendOptions &options) {
  static int binaryAnchor = 0;
  static const auto Binary = checkedFileDigest(
      llvm::sys::fs::getMainExecutable(nullptr, &binaryAnchor));
  if (!Binary || options.analysis.dumpStream ||
      options.analysis.reportUnannotated)
    return {};
  const auto &analysis = options.analysis;
  std::vector<std::string> parts{"weavec-analysis-cache-1",
                                 *Binary,
                                 analysis.checkContracts ? "1" : "0",
                                 analysis.checked ? "1" : "0",
                                 analysis.deferCheckedCalls ? "1" : "0",
                                 analysis.strictExterns ? "1" : "0",
                                 analysis.deferBoundary ? "1" : "0",
                                 analysis.exclusiveBorrows ? "1" : "0",
                                 options.mainFileOnly ? "1" : "0",
                                 options.bindCheckedInputs ? "1" : "0"};
  for (const auto &name : analysis.checkedFunctions)
    parts.push_back(name);
  for (const auto id : core::diag::All) {
    parts.emplace_back(id);
    parts.push_back(
        std::to_string(static_cast<unsigned>(options.control.levelFor(id))));
  }
  return checkedCommandDigest(parts);
}

} // namespace weavec::frontend
