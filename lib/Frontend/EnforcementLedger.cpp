//===- EnforcementLedger.cpp - What the guard pass decided ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/EnforcementLedger.h"

#include "weavec/Config/Version.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <system_error>
#include <tuple>

namespace weavec::frontend {

unsigned EnforcementLedger::add(LedgerRow row) {
  rows.push_back(std::move(row));
  copies.emplace_back();
  return static_cast<unsigned>(rows.size() - 1);
}

void EnforcementLedger::copy(unsigned index, bool stays,
                             llvm::StringRef reason) {
  if (index >= copies.size())
    return;
  Copies &seen = copies[index];
  if (stays) {
    ++seen.stay;
    return;
  }
  ++seen.removed;
  seen.reason = reason.str();
}

void EnforcementLedger::finish() {
  guarded = proven = unguarded = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    LedgerRow &row = rows[i];
    const Copies &seen = copies[i];
    if (row.outcome == LedgerRow::Outcome::Guarded && seen.stay == 0) {
      // Every copy removed, or none left: the optimiser removed a guard a
      // dominating one made redundant, or the access with its code.
      row.outcome = LedgerRow::Outcome::Proven;
      row.reason = seen.removed != 0 ? seen.reason : "optimized";
    }
    switch (row.outcome) {
    case LedgerRow::Outcome::Guarded:
      ++guarded;
      break;
    case LedgerRow::Outcome::Proven:
      ++proven;
      break;
    case LedgerRow::Outcome::Unguarded:
      ++unguarded;
      break;
    }
  }
}

const char *outcomeName(LedgerRow::Outcome outcome) {
  switch (outcome) {
  case LedgerRow::Outcome::Guarded:
    return "guarded";
  case LedgerRow::Outcome::Proven:
    return "proven";
  case LedgerRow::Outcome::Unguarded:
    return "unguarded";
  }
  return "guarded";
}

static llvm::json::Value rowJson(const LedgerRow &row) {
  return llvm::json::Object{
      {.K = "function", .V = row.function},
      {.K = "file", .V = row.file},
      {.K = "line", .V = row.line},
      {.K = "column", .V = row.column},
      {.K = "operation", .V = row.operation},
      {.K = "bytes", .V = static_cast<std::int64_t>(row.bytes)},
      {.K = "outcome", .V = outcomeName(row.outcome)},
      {.K = "reason", .V = row.reason},
  };
}

bool isLedgerDirectory(llvm::StringRef value) {
  if (value.empty())
    return false;
  return llvm::sys::path::is_separator(value.back()) ||
         llvm::sys::fs::is_directory(value);
}

/// The file a `-fweavec-ledger` value writes for `artifact`:
/// `<dir>/<name>.ledger.json`, with `<name>` the artifact's file name, for
/// the directory form, and the value itself otherwise.
static std::string ledgerPathFor(llvm::StringRef value,
                                 llvm::StringRef artifact) {
  if (!isLedgerDirectory(value))
    return value.str();
  llvm::SmallString<256> path(value);
  llvm::sys::path::append(path,
                          llvm::sys::path::filename(artifact) + ".ledger.json");
  return path.str().str();
}

/// Writes `contents` to a uniquely named temporary file next to `path` and
/// renames it into place, so parallel writers never interleave.
static bool writeFileAtomically(llvm::StringRef path, llvm::StringRef contents,
                                std::string &error) {
  const auto fail = [&](const std::string &message) {
    error = message;
    return false;
  };
  int fd = -1;
  llvm::SmallString<256> temporary;
  if (const std::error_code ec = llvm::sys::fs::createUniqueFile(
          path + ".tmp-%%%%%%%%", fd, temporary))
    return fail("cannot create a temporary file for '" + path.str() +
                "': " + ec.message());
  {
    llvm::raw_fd_ostream out(fd, /*shouldClose=*/true);
    out << contents;
    out.close();
    if (out.has_error()) {
      const std::string message = out.error().message();
      out.clear_error();
      std::ignore = llvm::sys::fs::remove(temporary);
      return fail("cannot write '" + temporary.str().str() + "': " + message);
    }
  }
  if (const std::error_code ec = llvm::sys::fs::rename(temporary, path)) {
    std::ignore = llvm::sys::fs::remove(temporary);
    return fail("cannot rename '" + temporary.str().str() + "' to '" +
                path.str() + "': " + ec.message());
  }
  return true;
}

bool writeEnforcementLedger(const EnforcementLedger &ledger,
                            const EnforcementUnit &unit, llvm::StringRef path,
                            std::string &error) {
  llvm::json::Array rows;
  for (const LedgerRow &row : ledger.rows)
    rows.push_back(rowJson(row));
  llvm::json::Object document{
      {.K = "schema", .V = "weavec-ledger"},
      {.K = "version", .V = EnforcementLedgerVersion},
      {.K = "producer",
       .V = llvm::json::Object{{.K = "name", .V = "weavec-cc"},
                               {.K = "version", .V = WEAVEC_VERSION_STRING}}},
      {.K = "units",
       .V =
           llvm::json::Array{llvm::json::Object{
               {.K = "source", .V = unit.source},
               {.K = "object", .V = unit.object},
               {.K = "target", .V = unit.target},
               {.K = "config",
                .V = llvm::json::Object{{.K = "checks", .V = unit.checks},
                                        {.K = "zeroInit", .V = unit.zeroInit}}},
               {.K = "summary",
                .V =
                    llvm::json::Object{
                        {.K = "accesses",
                         .V = static_cast<std::int64_t>(ledger.accesses())},
                        {.K = "proven",
                         .V = static_cast<std::int64_t>(ledger.proven)},
                        {.K = "guarded",
                         .V = static_cast<std::int64_t>(ledger.guarded)},
                        {.K = "unguarded",
                         .V = static_cast<std::int64_t>(ledger.unguarded)}}},
               {.K = "rows", .V = std::move(rows)},
           }}},
  };
  std::string text;
  llvm::raw_string_ostream os(text);
  os << llvm::formatv("{0:2}", llvm::json::Value(std::move(document))) << '\n';
  return writeFileAtomically(
      ledgerPathFor(path, unit.object.empty() ? unit.source : unit.object),
      text, error);
}

std::string enforcementSummary(const EnforcementLedger &ledger,
                               llvm::StringRef source) {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << "weavec: " << source << ": " << ledger.accesses()
     << " accesses: " << ledger.proven << " proven, " << ledger.guarded
     << " guarded, " << ledger.unguarded << " unguarded";
  return text;
}

} // namespace weavec::frontend
