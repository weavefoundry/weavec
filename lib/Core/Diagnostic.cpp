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

std::string_view toString(Severity severity) noexcept {
  switch (severity) {
  case Severity::Note:
    return "note";
  case Severity::Warning:
    return "warning";
  case Severity::Error:
    return "error";
  }
  return "<invalid>";
}

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

Diagnostic &Diagnostic::addFixIt(SourceLocation at, std::string insertion) {
  fixits.push_back(
      FixItHint{.location = std::move(at), .insertion = std::move(insertion)});
  return *this;
}

void DiagnosticCollector::report(const Diagnostic &diagnostic) {
  items.push_back(diagnostic);
}

std::size_t DiagnosticCollector::count(Severity severity) const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      items, [severity](const auto &d) { return d.severity == severity; }));
}

} // namespace weavec::core
