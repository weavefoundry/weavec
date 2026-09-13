//===- CompactReport.cpp - Shared checked report records (RFC 0020) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "CompactReport.h"

#include "llvm/Support/raw_ostream.h"

namespace weavec::frontend {

std::uint64_t CompactReport::location(const core::SourceLocation &value) {
  return locations.intern(
      {strings.intern(value.file), value.line, value.column});
}

std::uint64_t CompactReport::callPath(const core::SafetyCallPath &value) {
  const auto *identity = value.entries().data();
  if (const auto found = pathIds.find(identity); found != pathIds.end())
    return found->second.id;
  std::vector<std::uint64_t> calls;
  calls.reserve(value.size());
  for (const auto &call : value)
    calls.push_back(location(call));
  const auto id = paths.intern(std::move(calls));
  // RFC 0020: this bounded memo changes neither table contents nor ordering.
  // Clearing it merely repeats interning; retained paths prevent address reuse.
  if (pathIds.size() >= 1024)
    pathIds.clear();
  pathIds.emplace(identity, PathReference{.path = value, .id = id});
  return id;
}

std::uint64_t CompactReport::obligation(const core::SafetyObligation &entry) {
  if (const auto found = rowIds.find(&entry); found != rowIds.end())
    return found->second;
  // Intern paths before row fields, preserving the existing first-use order.
  const auto calls = callPath(entry.calls);
  const auto id = obligations.intern(
      {strings.intern(std::string(core::toString(entry.property))),
       strings.intern(std::string(core::toString(entry.outcome))),
       location(entry.location), strings.intern(entry.subject),
       strings.intern(entry.reason), calls,
       includeFunctions ? strings.intern(entry.function) : 0});
  rowIds.emplace(&entry, id);
  return id;
}

void CompactReport::writeTables(llvm::raw_ostream &out) const {
  out << ",\"strings\":[";
  bool first = true;
  for (const auto *value : strings.values) {
    if (!first)
      out << ',';
    first = false;
    out << core::safetyJsonString(*value);
  }
  out << ']';
  const auto table = [&](std::string_view name, const auto &values) {
    out << ',' << core::safetyJsonString(name) << ":[";
    bool firstRow = true;
    for (const auto *row : values) {
      if (!firstRow)
        out << ',';
      firstRow = false;
      out << '[';
      bool firstValue = true;
      unsigned ordinal = 0;
      for (const auto value : *row) {
        if (name == "obligation_records" && !includeFunctions && ordinal == 6)
          break;
        ++ordinal;
        if (!firstValue)
          out << ',';
        firstValue = false;
        out << value;
      }
      out << ']';
    }
    out << ']';
  };
  table("locations", locations.values);
  table("call_paths", paths.values);
  table("obligation_records", obligations.values);
  out << R"(,"obligation_fields":["property","outcome","location","subject","reason","calls")";
  if (includeFunctions)
    out << R"(,"function")";
  out << ']';
}

} // namespace weavec::frontend
