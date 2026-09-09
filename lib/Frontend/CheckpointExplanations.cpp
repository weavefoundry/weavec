//===- CheckpointExplanations.cpp - Shared checkpoint ledgers (RFC 0020)
//---===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "CheckpointExplanations.h"

namespace weavec::frontend {

// This order is part of private checkpoint format 2. Include uncomputed
// generic summaries too, so omission cannot shift another contract's ledger.
template <typename F>
static bool visitContracts(analysis::UnitExports &exports, F visit) {
  for (auto &[name, contract] : exports.checkedDefinitions) {
    (void)name;
    if (!visit(contract))
      return false;
  }
  for (auto &[name, function] : exports.functions) {
    (void)name;
    if (!visit(function.summary.checked))
      return false;
    for (auto &[context, summary] : function.specializations) {
      (void)context;
      if (!visit(summary.checked))
        return false;
    }
    for (auto &[context, summary] : function.memorySpecializations) {
      (void)context;
      if (!visit(summary.checked))
        return false;
    }
  }
  return true;
}

llvm::json::Array
CheckpointExplanations::extract(analysis::UnitExports &exports) {
  llvm::json::Array references;
  visitContracts(exports, [&](core::CheckedContract &contract) {
    std::vector<std::uint64_t> entries{contract.obligations.limited() ? 1U
                                                                      : 0U};
    entries.reserve(contract.obligations.entries().size() + 1);
    for (const auto &[identity, obligation] : contract.obligations.entries()) {
      (void)identity;
      entries.push_back(rows.obligation(obligation));
    }
    const auto [it, inserted] = ids.try_emplace(std::move(entries), ids.size());
    if (inserted)
      ledgers.push_back(&it->first);
    references.push_back(it->second);
    contract.obligations = {};
    return true;
  });
  return references;
}

void CheckpointExplanations::writeTables(llvm::raw_ostream &out) const {
  rows.writeTables(out);
  out << ",\"ledgers\":[";
  bool first = true;
  for (const auto *ledger : ledgers) {
    if (!first)
      out << ',';
    first = false;
    out << '[';
    bool firstEntry = true;
    for (const auto entry : *ledger) {
      if (!firstEntry)
        out << ',';
      firstEntry = false;
      out << entry;
    }
    out << ']';
  }
  out << ']';
}

std::optional<std::vector<core::SafetyLedger>>
CheckpointExplanations::decode(const llvm::json::Object &object) {
  const auto *strings = object.getArray("strings");
  const auto *locations = object.getArray("locations");
  const auto *paths = object.getArray("call_paths");
  const auto *rows = object.getArray("obligation_records");
  const auto *fields = object.getArray("obligation_fields");
  const auto *ledgers = object.getArray("ledgers");
  if (!strings || !locations || !paths || !rows || !fields || !ledgers ||
      *fields != llvm::json::Array{"property", "outcome", "location", "subject",
                                   "reason", "calls", "function"})
    return std::nullopt;
  for (const auto &value : *strings)
    if (!value.getAsString())
      return std::nullopt;
  const auto index = [](const llvm::json::Value &value,
                        std::size_t bound) -> std::optional<std::size_t> {
    const auto number = value.getAsUINT64();
    if (!number || *number >= bound)
      return std::nullopt;
    return number;
  };
  const auto string =
      [&](const llvm::json::Value &value) -> std::optional<llvm::StringRef> {
    const auto at = index(value, strings->size());
    return at ? (*strings)[*at].getAsString() : std::nullopt;
  };
  std::vector<core::SourceLocation> decodedLocations;
  for (const auto &value : *locations) {
    const auto *row = value.getAsArray();
    if (!row || row->size() != 3)
      return std::nullopt;
    const auto file = string((*row)[0]);
    const auto line = (*row)[1].getAsUINT64();
    const auto column = (*row)[2].getAsUINT64();
    if (!file || !line || !column || *line > UINT32_MAX || *column > UINT32_MAX)
      return std::nullopt;
    decodedLocations.push_back({.file = file->str(),
                                .line = static_cast<std::uint32_t>(*line),
                                .column = static_cast<std::uint32_t>(*column),
                                .opaque = 0});
  }
  std::vector<core::SafetyCallPath> decodedPaths;
  for (const auto &value : *paths) {
    const auto *row = value.getAsArray();
    if (!row || row->size() > core::MaxSafetyCallDepth)
      return std::nullopt;
    core::SafetyCallPath path;
    for (const auto &entry : *row) {
      const auto at = index(entry, decodedLocations.size());
      if (!at)
        return std::nullopt;
      path.pushBack(decodedLocations[*at]);
    }
    auto normalized = path;
    normalized.normalize();
    if (normalized != path)
      return std::nullopt;
    decodedPaths.push_back(std::move(normalized));
  }
  std::vector<core::SafetyLedger> decodedRows;
  for (const auto &value : *rows) {
    const auto *row = value.getAsArray();
    if (!row || row->size() != 7)
      return std::nullopt;
    const auto property = string((*row)[0]);
    const auto outcome = string((*row)[1]);
    const auto location = index((*row)[2], decodedLocations.size());
    const auto subject = string((*row)[3]);
    const auto reason = string((*row)[4]);
    const auto calls = index((*row)[5], decodedPaths.size());
    const auto function = string((*row)[6]);
    if (!property || !outcome || !location || !subject || !reason || !calls ||
        !function)
      return std::nullopt;
    const auto parsedProperty = core::parseSafetyProperty(*property);
    const auto parsedOutcome = core::parseSafetyOutcome(*outcome);
    if (!parsedProperty || !parsedOutcome)
      return std::nullopt;
    core::SafetyObligation obligation{.property = *parsedProperty,
                                      .outcome = *parsedOutcome,
                                      .location = decodedLocations[*location],
                                      .function = function->str(),
                                      .subject = subject->str(),
                                      .reason = reason->str(),
                                      .calls = decodedPaths[*calls]};
    core::SafetyLedger ledger;
    ledger.add(obligation);
    // Insertion may normalize transport-only identities/paths. Only exact
    // canonical rows are eligible for checkpoint reuse.
    if (ledger.entries().size() != 1 ||
        ledger.entries().begin()->second != obligation)
      return std::nullopt;
    decodedRows.push_back(std::move(ledger));
  }
  std::vector<core::SafetyLedger> result;
  for (const auto &value : *ledgers) {
    const auto *row = value.getAsArray();
    if (!row || row->empty() || row->size() > core::MaxSafetyObligations + 1)
      return std::nullopt;
    const auto limited = (*row)[0].getAsUINT64();
    if (!limited || *limited > 1)
      return std::nullopt;
    core::SafetyLedger ledger;
    std::string_view previous;
    for (std::size_t i = 1; i < row->size(); ++i) {
      const auto at = index((*row)[i], decodedRows.size());
      if (!at)
        return std::nullopt;
      const auto &entry = *decodedRows[*at].entries().begin();
      if (i > 1 && entry.first <= previous)
        return std::nullopt;
      previous = entry.first;
      ledger.join(decodedRows[*at]);
    }
    if (*limited)
      ledger.markLimited();
    ledger.shareSnapshot();
    result.push_back(std::move(ledger));
  }
  return result;
}

bool CheckpointExplanations::restore(
    analysis::UnitExports &exports, const llvm::json::Array &references,
    const std::vector<core::SafetyLedger> &ledgers) {
  std::size_t next = 0;
  const bool valid =
      visitContracts(exports, [&](core::CheckedContract &contract) {
        if (next == references.size() ||
            !contract.obligations.entries().empty() ||
            contract.obligations.limited())
          return false;
        const auto at = references[next++].getAsUINT64();
        if (!at || *at >= ledgers.size())
          return false;
        const auto &ledger = ledgers[*at];
        if (!contract.computed &&
            (!ledger.entries().empty() || ledger.limited()))
          return false;
        contract.obligations = ledger;
        return true;
      });
  return valid && next == references.size();
}

} // namespace weavec::frontend
