//===- ClangDiagnosticSink.h - Forward core diagnostics to Clang -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_CLANGDIAGNOSTICSINK_H
#define WEAVEC_FRONTEND_CLANGDIAGNOSTICSINK_H

#include "weavec/Core/Diagnostic.h"

#include "clang/Basic/Diagnostic.h"

namespace weavec::frontend {

/// Emits core diagnostics through Clang's `DiagnosticsEngine`, so they get
/// the same rendering (colours, caret, `-fdiagnostics-format`, ...) as the
/// compiler's own diagnostics.
class ClangDiagnosticSink final : public core::DiagnosticSink {
public:
  explicit ClangDiagnosticSink(clang::DiagnosticsEngine &diags)
      : engine(diags) {}

  void report(const core::Diagnostic &diagnostic) override;

private:
  clang::DiagnosticsEngine &engine;

  void emit(const core::Diagnostic &diagnostic, bool isNote);
};

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_CLANGDIAGNOSTICSINK_H
