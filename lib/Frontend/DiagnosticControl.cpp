//===- DiagnosticControl.cpp - -W flags for WeaveC diagnostics ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/DiagnosticControl.h"

#include <utility>

namespace weavec::frontend {

namespace {

/// The shape of a `-W` flag once its prefix is recognised.
struct Request {
  /// `weavec` (the group) or `weavec-<id>`.
  llvm::StringRef target;
  DiagnosticControl::Level level = DiagnosticControl::Level::Default;
  /// `-Wno-weavec-<id>`: only legal for warnings.
  bool disables = false;
};

} // namespace

static std::optional<Request> classify(llvm::StringRef flag) {
  Request request;
  llvm::StringRef rest;
  if (flag.consume_front("-Werror=")) {
    request.level = DiagnosticControl::Level::Error;
    rest = flag;
  } else if (flag.consume_front("-Wno-error=")) {
    request.level = DiagnosticControl::Level::Warning;
    rest = flag;
  } else if (flag.consume_front("-Wno-")) {
    request.level = DiagnosticControl::Level::Off;
    request.disables = true;
    rest = flag;
  } else if (flag.consume_front("-W")) {
    request.level = DiagnosticControl::Level::Default;
    rest = flag;
  } else {
    return std::nullopt;
  }
  if (rest != "weavec" && !rest.starts_with("weavec-"))
    return std::nullopt;
  request.target = rest;
  return request;
}

bool DiagnosticControl::isWeaveCFlag(llvm::StringRef flag) {
  return classify(flag).has_value();
}

bool DiagnosticControl::parse(llvm::StringRef flag, std::string &error) {
  const std::optional<Request> request = classify(flag);
  if (!request)
    return false;

  if (request->target == "weavec") {
    // Later flags win, as in Clang, except that `-Werror=weavec` does not
    // resurrect a warning an earlier `-Wno-weavec-<id>` disabled.
    all = request->level;
    const bool keepDisabled =
        request->level == Level::Error || request->level == Level::Warning;
    for (auto it = perId.begin(); it != perId.end();) {
      if (keepDisabled && it->second == Level::Off)
        ++it;
      else
        it = perId.erase(it);
    }
    return true;
  }

  const llvm::StringRef id = request->target.drop_front(sizeof("weavec-") - 1);
  if (!core::diag::isKnown(id)) {
    error =
        "unknown WeaveC diagnostic '" + id.str() + "' in '" + flag.str() + "'";
    return true;
  }
  if (request->disables && !core::diag::isWarningByDefault(id)) {
    error = "'" + flag.str() + "': '" + id.str() +
            "' is an error and cannot be disabled; use -Wno-error=weavec-" +
            id.str() + " to make it a warning";
    return true;
  }
  perId[id.str()] = request->level;
  return true;
}

DiagnosticControl::Level
DiagnosticControl::levelFor(std::string_view id) const {
  if (const auto it = perId.find(id); it != perId.end())
    return it->second;
  return all;
}

std::optional<core::Diagnostic>
DiagnosticControl::apply(const core::Diagnostic &diagnostic) const {
  const Level level = levelFor(diagnostic.id);
  const bool warning = core::diag::isWarningByDefault(diagnostic.id);
  switch (level) {
  case Level::Default:
    return diagnostic;
  case Level::Off:
    // `-Wno-weavec` is a group request: it disables the warnings and leaves
    // the errors alone.
    if (warning)
      return std::nullopt;
    return diagnostic;
  case Level::Warning: {
    core::Diagnostic adjusted = diagnostic;
    if (adjusted.severity == core::Severity::Error)
      adjusted.severity = core::Severity::Warning;
    return adjusted;
  }
  case Level::Error: {
    core::Diagnostic adjusted = diagnostic;
    if (adjusted.severity == core::Severity::Warning)
      adjusted.severity = core::Severity::Error;
    return adjusted;
  }
  }
  return diagnostic;
}

void FilteringSink::report(const core::Diagnostic &diagnostic) {
  const std::optional<core::Diagnostic> adjusted = table.apply(diagnostic);
  if (!adjusted)
    return;
  const ReportedDiagnostic key = ReportedDiagnostic::of(*adjusted);
  if (skip != nullptr && skip->contains(key))
    return;
  if (once != nullptr && adjusted->id == core::diag::AnnotationRequired &&
      !once->insert(adjusted->message).second)
    return;
  forwarded.insert(key);
  if (adjusted->severity == core::Severity::Error)
    ++errorCount;
  else if (adjusted->severity == core::Severity::Warning)
    ++warningCount;
  downstream.report(*adjusted);
}

} // namespace weavec::frontend
