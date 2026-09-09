//===- CompactReport.h - Streaming shared report tables (RFC 0020) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_FRONTEND_COMPACTREPORT_H
#define WEAVEC_FRONTEND_COMPACTREPORT_H

#include "weavec/Core/Safety.h"

#include "llvm/Support/raw_ostream.h"

#include <array>
#include <map>
#include <unordered_map>
#include <vector>

namespace weavec::frontend {

/// Intern directly from immutable contracts, without an expanded JSON DOM.
class CompactReport {
public:
  explicit CompactReport(bool includeFunctions = false)
      : includeFunctions(includeFunctions) {}
  ~CompactReport() = default;
  CompactReport(const CompactReport &) = delete;
  CompactReport &operator=(const CompactReport &) = delete;
  std::uint64_t obligation(const core::SafetyObligation &entry);
  void writeTables(llvm::raw_ostream &out) const;

private:
  template <typename T>
  struct Table {
    std::map<T, std::uint64_t> ids;
    // Map nodes own values and remain stable as the table grows.
    std::vector<const T *> values;
    std::uint64_t intern(T value) {
      const auto [it, inserted] =
          ids.try_emplace(std::move(value), values.size());
      if (inserted)
        values.push_back(&it->first);
      return it->second;
    }
  };
  std::uint64_t location(const core::SourceLocation &value);
  Table<std::string> strings;
  Table<std::array<std::uint64_t, 3>> locations;
  Table<std::vector<std::uint64_t>> paths;
  Table<std::array<std::uint64_t, 7>> obligations;
  bool includeFunctions;
  // Report units own all rows throughout serialization.
  std::unordered_map<const core::SafetyObligation *, std::uint64_t> rowIds;
};

} // namespace weavec::frontend
#endif // WEAVEC_FRONTEND_COMPACTREPORT_H
