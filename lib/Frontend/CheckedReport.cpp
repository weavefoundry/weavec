//===- CheckedReport.cpp - Deterministic safety JSON (RFC 0018) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/CheckedReport.h"

#include "weavec/Config/Version.h"
#include "weavec/Core/SummaryIO.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>

namespace weavec::frontend {

void CheckedReport::record(const analysis::UnitExports &unit) {
  if (!unit.checkedDefinitions.empty())
    units.insert_or_assign(unit.source, unit);
}

void CheckedReport::invalidate(std::string_view reason) {
  for (auto &[source, unit] : units)
    for (auto &[name, contract] : unit.checkedDefinitions)
      contract.obligations.add(
          {.property = core::SafetyProperty::Semantics,
           .outcome = core::SafetyOutcome::Unresolved,
           .location = {.file = source, .line = 0, .column = 0, .opaque = 0},
           .function = name,
           .subject = "whole-program analysis",
           .reason = std::string(reason),
           .calls = {}});
}

bool CheckedReport::failed(const analysis::UnitExports &unit,
                           bool allowDeferred) {
  return std::ranges::any_of(
      unit.checkedDefinitions, [allowDeferred](const auto &entry) {
        const auto &contract = entry.second;
        return contract.selected && (!contract.computed || contract.limited ||
                                     !contract.obligations.complete() ||
                                     (!allowDeferred && contract.deferred));
      });
}

static std::string checkedLocation(const core::SourceLocation &location) {
  return "{\"file\":" + core::safetyJsonString(location.file) +
         ",\"line\":" + std::to_string(location.line) +
         ",\"column\":" + std::to_string(location.column) + "}";
}

std::string CheckedReport::json(bool invocationOK) const {
  std::ostringstream out;
  const auto quote = core::safetyJsonString;
  out << R"({"version":2,"invocation_ok":)" << (invocationOK ? "true" : "false")
      << ",\"guarantee\":\"conditional safety of selected source "
         "functions\",\"units\":[";
  std::size_t total = 0;
  std::size_t selected = 0;
  std::size_t complete = 0;
  std::size_t trusted = 0;
  std::size_t deferred = 0;
  bool firstUnit = true;
  for (const auto &[source, unit] : units) {
    if (!firstUnit)
      out << ',';
    firstUnit = false;
    out << "{\"source\":" << quote(source)
        << ",\"target\":" << quote(unit.checkedTarget) << ",\"functions\":[";
    const core::GlobalNamer names = [&](std::uint32_t id) {
      return unit.globals.nameOf(id).str();
    };
    bool firstFunction = true;
    for (const auto &[name, contract] : unit.checkedDefinitions) {
      if (!firstFunction)
        out << ',';
      firstFunction = false;
      ++total;
      selected += contract.selected ? 1 : 0;
      complete += contract.complete() ? 1 : 0;
      trusted += contract.complete() && contract.obligations.trusted() ? 1 : 0;
      deferred += contract.deferred ? 1 : 0;
      std::string_view status = "incomplete";
      if (contract.complete()) {
        status = "proven";
        if (!contract.requirements.empty())
          status = "conditional";
        if (contract.obligations.trusted())
          status = "trusted";
      }
      out << "{\"name\":" << quote(name)
          << ",\"signature\":" << quote(contract.signature)
          << ",\"selected\":" << (contract.selected ? "true" : "false")
          << ",\"status\":" << quote(status)
          << ",\"complete\":" << (contract.complete() ? "true" : "false")
          << ",\"deferred\":" << (contract.deferred ? "true" : "false")
          << ",\"limited\":"
          << (contract.limited || contract.obligations.limited() ? "true"
                                                                 : "false");
      const auto requirements = [&](std::string_view key, const auto &entries) {
        out << "," << quote(key) << ":[";
        bool first = true;
        for (const auto &entry : entries) {
          if (!first)
            out << ',';
          first = false;
          out << "{\"kind\":" << quote(core::toString(entry.kind))
              << ",\"path\":"
              << quote(core::printSummaryPath(entry.path, names))
              << ",\"other\":"
              << quote(core::printSummaryPath(entry.other, names))
              << ",\"begin\":" << quote(core::printAffine(entry.begin, names))
              << ",\"end\":" << quote(core::printAffine(entry.end, names))
              << ",\"family\":" << quote(entry.family)
              << ",\"when\":" << quote(core::printGuard(entry.when, names))
              << ",\"on\":"
              << (entry.on ? quote(core::toString(*entry.on)) : "null") << '}';
        }
        out << ']';
      };
      requirements("requirements", contract.requirements);
      requirements("establishes", contract.establishes);
      out << ",\"obligations\":[";
      bool first = true;
      for (const auto &[key, entry] : contract.obligations.entries()) {
        (void)key;
        if (!first)
          out << ',';
        first = false;
        out << "{\"property\":" << quote(core::toString(entry.property))
            << ",\"outcome\":" << quote(core::toString(entry.outcome))
            << ",\"location\":" << checkedLocation(entry.location)
            << ",\"subject\":" << quote(entry.subject)
            << ",\"reason\":" << quote(entry.reason) << ",\"calls\":[";
        bool firstCall = true;
        for (const auto &call : entry.calls) {
          if (!firstCall)
            out << ',';
          firstCall = false;
          out << checkedLocation(call);
        }
        out << "]}";
      }
      out << "]}";
    }
    out << "]}";
  }
  out << "],\"tool_version\":" << quote(WEAVEC_VERSION_STRING)
      << ",\"model_version\":" << core::SummaryFormatVersion
      << R"(,"totals":{"functions":)" << total << ",\"selected\":" << selected
      << ",\"complete\":" << complete << ",\"trusted_complete\":" << trusted
      << ",\"incomplete\":" << total - complete << ",\"deferred\":" << deferred
      << "}}\n";
  return out.str();
}

bool CheckedReport::finish(std::string_view path,
                           const std::set<std::string> &requested,
                           bool invocationOK) const {
  bool complete = true;
  for (const auto &name : requested) {
    bool found = false;
    for (const auto &[source, unit] : units) {
      (void)source;
      found |= unit.checkedDefinitions.contains(name);
    }
    if (!found) {
      llvm::errs() << "weavec: error: checked function '" << name
                   << "' was not found\n";
      complete = false;
    }
  }
  if (path.empty())
    return complete;
  llvm::SmallString<256> temporary;
  int descriptor = -1;
  auto error = llvm::sys::fs::createUniqueFile(
      std::string(path) + ".tmp-%%%%%%", descriptor, temporary);
  if (!error) {
    llvm::raw_fd_ostream stream(descriptor, true);
    stream << json(invocationOK && complete);
    stream.close();
    if (stream.has_error())
      error = stream.error();
    if (!error)
      error = llvm::sys::fs::rename(temporary, path);
  }
  if (error) {
    if (!temporary.empty())
      std::ignore = llvm::sys::fs::remove(temporary);
    llvm::errs() << "weavec: error: cannot write checked report '" << path
                 << "': " << error.message() << '\n';
    return false;
  }
  return complete;
}

} // namespace weavec::frontend
