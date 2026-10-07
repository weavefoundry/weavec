//===- Diagnostic.cpp - Frontend-neutral diagnostics ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Diagnostic.h"

#include <algorithm>
#include <utility>

namespace weavec::core {

Diagnostic &Diagnostic::addNote(std::string noteMessage,
                                SourceLocation noteLocation) {
  notes.push_back(Diagnostic{
      .severity = Severity::Note,
      .id = id,
      .message = std::move(noteMessage),
      .location = std::move(noteLocation),
      .notes = {},
      .fixits = {},
  });
  return *this;
}

void DiagnosticCollector::report(const Diagnostic &diagnostic) {
  items.push_back(diagnostic);
}

} // namespace weavec::core
