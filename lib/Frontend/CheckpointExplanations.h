//===- CheckpointExplanations.h - Shared checkpoint ledgers (RFC 0020)
//-----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_FRONTEND_CHECKPOINTEXPLANATIONS_H
#define WEAVEC_FRONTEND_CHECKPOINTEXPLANATIONS_H

#include "CompactReport.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include "llvm/Support/JSON.h"

namespace weavec::frontend {

/// Private checkpoint format 2. The caller owns all source rows while writing.
class CheckpointExplanations {
public:
  llvm::json::Array extract(analysis::UnitExports &exports);
  void writeTables(llvm::raw_ostream &out) const;

  static std::optional<std::vector<core::SafetyLedger>>
  decode(const llvm::json::Object &object);
  static bool restore(analysis::UnitExports &exports,
                      const llvm::json::Array &references,
                      const std::vector<core::SafetyLedger> &ledgers);

private:
  CompactReport rows{true};
  // First column is the exhaustion flag; following columns are row ids.
  std::map<std::vector<std::uint64_t>, std::uint64_t> ids;
  std::vector<const std::vector<std::uint64_t> *> ledgers;
};

} // namespace weavec::frontend
#endif // WEAVEC_FRONTEND_CHECKPOINTEXPLANATIONS_H
