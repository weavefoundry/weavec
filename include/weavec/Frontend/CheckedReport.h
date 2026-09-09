//===- CheckedReport.h - Checked scope and evidence (RFC 0018) --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_FRONTEND_CHECKEDREPORT_H
#define WEAVEC_FRONTEND_CHECKEDREPORT_H

#include "weavec/Analysis/ProgramDatabase.h"

#include <map>
#include <set>
#include <string>
#include <string_view>

namespace weavec::frontend {

class CheckedReport {
public:
  bool compact = false;
  /// Prevent provisional contracts from being published as settled proofs.
  void invalidate(std::string_view reason);
  void record(const analysis::UnitExports &unit);
  [[nodiscard]] static bool failed(const analysis::UnitExports &unit,
                                   bool allowDeferred = false);
  [[nodiscard]] std::string json(bool invocationOK = true) const;
  /// Validate requested scope and atomically publish a report, including
  /// failure.
  bool finish(std::string_view path, const std::set<std::string> &requested,
              bool invocationOK = true) const;

private:
  void write(llvm::raw_ostream &out, bool invocationOK) const;
  std::map<std::string, analysis::UnitExports> units;
};

} // namespace weavec::frontend
#endif // WEAVEC_FRONTEND_CHECKEDREPORT_H
