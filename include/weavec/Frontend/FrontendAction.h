//===- FrontendAction.h - Clang frontend integration -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Glue between Clang's frontend/tooling machinery and the WeaveC analyses.
// `WeaveCAction` can be run by `clang::tooling::ClangTool`, embedded in a
// custom driver, or (eventually) loaded as a plugin.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_FRONTENDACTION_H
#define WEAVEC_FRONTEND_FRONTENDACTION_H

#include "weavec/Analysis/FunctionAnalysis.h"

#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <memory>

namespace weavec::frontend {

/// User-configurable behaviour of the frontend action.
struct FrontendOptions {
  analysis::AnalysisOptions analysis;
  /// Only analyse declarations in the main file (not in included headers).
  bool mainFileOnly = true;
};

/// Runs WeaveC's analyses over each translation unit.
class WeaveCAction final : public clang::ASTFrontendAction {
public:
  explicit WeaveCAction(FrontendOptions opts) : options(opts) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override;

private:
  FrontendOptions options;
};

/// Creates a factory usable with `clang::tooling::ClangTool::run`.
std::unique_ptr<clang::tooling::FrontendActionFactory>
createWeaveCActionFactory(FrontendOptions options);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_FRONTENDACTION_H
