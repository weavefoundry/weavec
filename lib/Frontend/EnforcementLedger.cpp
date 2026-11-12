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
#include "llvm/ADT/StringMap.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
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
      // Every copy removed, or none left: the optimizer removed a guard a
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
      {.K = "display", .V = llvm::demangle(row.function)},
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
  llvm::json::Array functions;
  for (const LedgerFunction &function : ledger.functions)
    functions.push_back(
        llvm::json::Object{{.K = "function", .V = function.function},
                           {.K = "tier", .V = function.tier},
                           {.K = "file", .V = function.file},
                           {.K = "line", .V = function.line},
                           {.K = "owns", .V = function.owns}});
  llvm::json::Array fields;
  for (const LedgerField &field : ledger.fields)
    fields.push_back(llvm::json::Object{{.K = "record", .V = field.record},
                                        {.K = "field", .V = field.field},
                                        {.K = "kind", .V = field.kind},
                                        {.K = "file", .V = field.file},
                                        {.K = "line", .V = field.line}});
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
               {.K = "functions", .V = std::move(functions)},
               {.K = "fields", .V = std::move(fields)},
               {.K = "rows", .V = std::move(rows)},
           }}},
  };
  std::string text;
  llvm::raw_string_ostream os(text);
  os << llvm::formatv("{0:2}", llvm::json::Value(std::move(document))) << '\n';
  // `-fweavec-ledger=out/` names a directory, made if it is missing.
  if (!path.empty() && llvm::sys::path::is_separator(path.back()))
    std::ignore = llvm::sys::fs::create_directories(path);
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
  const auto safe = llvm::count_if(ledger.functions, [](const auto &function) {
    return function.tier == "safe";
  });
  if (safe == 0)
    return text;
  os << "; " << safe << " of " << ledger.functions.size()
     << " functions safe";
  // RFC 0043 §11.
  const auto kind = [&](llvm::StringRef name) {
    return llvm::count_if(ledger.fields,
                          [&](const auto &field) { return field.kind == name; });
  };
  os << "; " << kind("owner") << " owner and " << kind("reference")
     << " reference fields";
  return text;
}

//===----------------------------------------------------------------------===//
// The audit (RFC 0042 §10, RFC 0043 §11)
//===----------------------------------------------------------------------===//

namespace {
/// What the audit lists of one program.
struct Audit {
  /// Functions that are not safe: what safe functions trust (T-A1, O-A1).
  llvm::json::Array trusted;
  /// Accesses of safe functions that kept a guard, and why.
  llvm::json::Array guarded;
  /// Library calls of safe functions that kept a checked wrapper.
  llvm::json::Array wrapped;
  /// Reference fields, tested where safe code loads them.
  llvm::json::Array references;
  /// Global owners safe code reads in place.
  llvm::json::Array globals;
  unsigned safe = 0;
  unsigned functions = 0;
};
} // namespace

/// The ledgers `operand` names: a file, or a directory's `*.ledger.json`.
static void ledgersOf(llvm::StringRef operand,
                      std::vector<std::string> &paths) {
  if (!llvm::sys::fs::is_directory(operand)) {
    paths.push_back(operand.str());
    return;
  }
  std::error_code error;
  for (llvm::sys::fs::recursive_directory_iterator it(operand, error), end;
       it != end && !error; it.increment(error))
    if (llvm::StringRef(it->path()).ends_with(".ledger.json"))
      paths.push_back(it->path());
  std::ranges::sort(paths);
}

static bool auditLedger(llvm::StringRef path, Audit &audit) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    llvm::errs() << "weavec-cc: error: cannot read '" << path
                 << "': " << buffer.getError().message() << '\n';
    return false;
  }
  llvm::Expected<llvm::json::Value> parsed =
      llvm::json::parse((*buffer)->getBuffer());
  const llvm::json::Object *document = parsed ? parsed->getAsObject() : nullptr;
  const llvm::json::Array *units =
      document != nullptr ? document->getArray("units") : nullptr;
  if (units == nullptr) {
    if (!parsed)
      llvm::consumeError(parsed.takeError());
    llvm::errs() << "weavec-cc: error: '" << path
                 << "' is not a WeaveC ledger\n";
    return false;
  }
  for (const llvm::json::Value &each : *units) {
    const llvm::json::Object *unit = each.getAsObject();
    if (unit == nullptr)
      continue;
    llvm::StringMap<std::string> tiers;
    if (const llvm::json::Array *functions = unit->getArray("functions"))
      for (const llvm::json::Value &value : *functions) {
        const llvm::json::Object *function = value.getAsObject();
        if (function == nullptr)
          continue;
        const std::string name =
            function->getString("function").value_or("").str();
        const std::string tier = function->getString("tier").value_or("").str();
        tiers[name] = tier;
        ++audit.functions;
        if (tier == "safe")
          ++audit.safe;
        else
          audit.trusted.push_back(llvm::json::Object(*function));
      }
    if (const llvm::json::Array *fields = unit->getArray("fields"))
      for (const llvm::json::Value &value : *fields)
        if (const llvm::json::Object *field = value.getAsObject()) {
          const auto kind = field->getString("kind");
          if (kind == "reference")
            audit.references.push_back(llvm::json::Object(*field));
          else if (kind == "global")
            audit.globals.push_back(llvm::json::Object(*field));
        }
    const llvm::json::Array *rows = unit->getArray("rows");
    if (rows == nullptr)
      continue;
    for (const llvm::json::Value &value : *rows) {
      const llvm::json::Object *row = value.getAsObject();
      // An entry's checks are the boundary with code that is not safe; a
      // reference's test is listed by its field.
      if (row == nullptr || row->getString("outcome") != "guarded" ||
          row->getString("operation") == "entry" ||
          row->getString("reason") == "reference")
        continue;
      llvm::StringRef name = row->getString("function").value_or("");
      name.consume_back(".weavec.safe");
      if (tiers.lookup(name) != "safe")
        continue;
      if (row->getString("reason") == "checked-call")
        audit.wrapped.push_back(llvm::json::Object(*row));
      else
        audit.guarded.push_back(llvm::json::Object(*row));
    }
  }
  return true;
}

static std::string where(const llvm::json::Object &entry) {
  std::string text = entry.getString("file").value_or("").str();
  if (const auto line = entry.getInteger("line"); line && *line != 0)
    text += ":" + std::to_string(*line);
  if (const auto column = entry.getInteger("column"); column && *column != 0)
    text += ":" + std::to_string(*column);
  return text;
}

int runAudit(llvm::ArrayRef<const char *> operands) {
  bool json = false;
  std::vector<std::string> paths;
  for (const char *operand : operands) {
    const llvm::StringRef text(operand);
    if (text == "--format=json")
      json = true;
    else if (text != "--format=text")
      ledgersOf(text, paths);
  }
  Audit audit;
  bool ok = true;
  for (const std::string &path : paths)
    ok = auditLedger(path, audit) && ok;
  if (json) {
    llvm::outs() << llvm::formatv(
                        "{0:2}",
                        llvm::json::Value(llvm::json::Object{
                            {.K = "schema", .V = "weavec-audit"},
                            {.K = "version", .V = 2},
                            {.K = "functions", .V = audit.functions},
                            {.K = "safe", .V = audit.safe},
                            {.K = "trusted", .V = std::move(audit.trusted)},
                            {.K = "guarded", .V = std::move(audit.guarded)},
                            {.K = "wrapped", .V = std::move(audit.wrapped)},
                            {.K = "references",
                             .V = std::move(audit.references)},
                            {.K = "globals", .V = std::move(audit.globals)}}))
                 << '\n';
    return ok ? 0 : 1;
  }
  llvm::raw_ostream &out = llvm::outs();
  auto list = [&](llvm::StringRef title, const llvm::json::Array &entries,
                  llvm::StringRef what, llvm::StringRef why) {
    out << '\n' << title << ":\n";
    for (const llvm::json::Value &value : entries) {
      const llvm::json::Object &entry = *value.getAsObject();
      out << "  " << where(entry) << ": ";
      if (what == "field")
        out << entry.getString("record").value_or("") << '.';
      out << entry.getString(what).value_or("");
      if (what == "operation")
        out << " in " << entry.getString("function").value_or("");
      if (!why.empty())
        out << " (" << entry.getString(why).value_or("")
            << (entry.getBoolean("owns").value_or(false) ? ", owns" : "")
            << ")";
      out << '\n';
    }
  };
  out << audit.safe << " of " << audit.functions << " functions safe\n";
  list("functions that are not safe (safe code trusts them; 'owns': a "
       "parameter or the result is WEAVEC_OWNED)",
       audit.trusted, "function", "tier");
  list("accesses of safe functions that keep a guard", audit.guarded,
       "operation", "reason");
  list("library calls of safe functions that keep a checked wrapper",
       audit.wrapped, "operation", "");
  list("reference fields (tested where safe code loads them)",
       audit.references, "field", "");
  list("global owners read in place (in the function named)", audit.globals,
       "field", "");
  return ok ? 0 : 1;
}

} // namespace weavec::frontend
