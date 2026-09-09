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

std::uint64_t CompactReport::obligation(const core::SafetyObligation &entry) {
  if (const auto found = rowIds.find(&entry); found != rowIds.end())
    return found->second;
  std::vector<std::uint64_t> calls;
  calls.reserve(entry.calls.size());
  for (const auto &call : entry.calls)
    calls.push_back(location(call));
  const auto id = obligations.intern(
      {strings.intern(std::string(core::toString(entry.property))),
       strings.intern(std::string(core::toString(entry.outcome))),
       location(entry.location), strings.intern(entry.subject),
       strings.intern(entry.reason), paths.intern(std::move(calls)),
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
