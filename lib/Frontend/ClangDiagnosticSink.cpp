//===- ClangDiagnosticSink.cpp - Forward core diagnostics to Clang --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/ClangDiagnosticSink.h"

#include "weavec/Analysis/ClangLocation.h"

#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"

namespace weavec::frontend {

static clang::DiagnosticsEngine::Level toClangLevel(core::Severity severity) {
  switch (severity) {
  case core::Severity::Note:
    return clang::DiagnosticsEngine::Note;
  case core::Severity::Warning:
    return clang::DiagnosticsEngine::Warning;
  case core::Severity::Error:
    return clang::DiagnosticsEngine::Error;
  }
  return clang::DiagnosticsEngine::Error;
}

void ClangDiagnosticSink::report(const core::Diagnostic &diagnostic) {
  emit(diagnostic, /*isNote=*/false);
  for (const core::Diagnostic &note : diagnostic.notes)
    emit(note, /*isNote=*/true);
}

void ClangDiagnosticSink::emit(const core::Diagnostic &diagnostic,
                               bool isNote) {
  // getCustomDiagID caches per (level, format string), so this is cheap.
  // Messages are passed as arguments so '%' in user identifiers is safe.
  const clang::DiagnosticsEngine::Level level =
      isNote ? clang::DiagnosticsEngine::Note
             : toClangLevel(diagnostic.severity);
  const unsigned id = isNote ? engine.getCustomDiagID(level, "%0")
                             : engine.getCustomDiagID(level, "%0 [weavec::%1]");

  auto location = analysis::toClangLocation(diagnostic.location);
  if (location.isInvalid() && engine.hasSourceManager() &&
      diagnostic.location.line != 0 && !diagnostic.location.file.empty()) {
    const auto &sources = engine.getSourceManager();
    if (const auto file = sources.getFileManager().getOptionalFileRef(
            diagnostic.location.file))
      location = sources.translateFileLineCol(&file->getFileEntry(),
                                              diagnostic.location.line,
                                              diagnostic.location.column);
  }
  auto builder = engine.Report(location, id);
  builder << diagnostic.message;
  if (!isNote)
    builder << diagnostic.id;
  for (const core::FixItHint &fixit : diagnostic.fixits) {
    const clang::SourceLocation at = analysis::toClangLocation(fixit.location);
    if (at.isValid())
      builder << clang::FixItHint::CreateInsertion(at, fixit.insertion);
  }
}

} // namespace weavec::frontend
