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
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

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

llvm::json::Value fixJson(const RuleFix &fix) {
  llvm::json::Array edits;
  for (const FixEdit &edit : fix.edits) {
    llvm::json::Object object{{.K = "file", .V = edit.file},
                              {.K = "offset", .V = edit.offset},
                              {.K = "text", .V = edit.text},
                              {.K = "needsInclude", .V = edit.needsInclude}};
    // RFC 0051 §4.1: what a removal takes out.
    if (!edit.removed.empty())
      object["removed"] = edit.removed;
    edits.push_back(std::move(object));
  }
  return llvm::json::Object{{.K = "level", .V = fix.level},
                            {.K = "declaration", .V = fix.declaration},
                            {.K = "name", .V = fix.name},
                            {.K = "aspect", .V = fix.aspect},
                            {.K = "fact", .V = fix.fact},
                            {.K = "text", .V = fix.text},
                            {.K = "evidence", .V = fix.evidence},
                            {.K = "edits", .V = std::move(edits)}};
}

llvm::json::Value errorJson(const RuleError &error) {
  llvm::json::Array subjects;
  for (const std::string &subject : error.subjects)
    subjects.push_back(subject);
  llvm::json::Array fixes;
  for (const RuleFix &fix : error.fixes)
    fixes.push_back(fixJson(fix));
  return llvm::json::Object{{.K = "rule", .V = error.rule},
                            {.K = "file", .V = error.file},
                            {.K = "line", .V = error.line},
                            {.K = "column", .V = error.column},
                            {.K = "message", .V = error.message},
                            {.K = "owner", .V = error.owner},
                            {.K = "ownerFile", .V = error.ownerFile},
                            {.K = "ownerLine", .V = error.ownerLine},
                            {.K = "global", .V = error.global},
                            {.K = "surveyed", .V = error.surveyed},
                            {.K = "subjects", .V = std::move(subjects)},
                            {.K = "fixes", .V = std::move(fixes)}};
}

static std::string text(const llvm::json::Object &object, llvm::StringRef key) {
  return object.getString(key).value_or("").str();
}
static unsigned number(const llvm::json::Object &object, llvm::StringRef key) {
  return static_cast<unsigned>(object.getInteger(key).value_or(0));
}

bool readFix(const llvm::json::Value &value, RuleFix &fix) {
  const llvm::json::Object *object = value.getAsObject();
  if (object == nullptr)
    return false;
  fix.level = text(*object, "level");
  fix.declaration = text(*object, "declaration");
  fix.name = text(*object, "name");
  fix.aspect = text(*object, "aspect");
  fix.fact = text(*object, "fact");
  fix.text = text(*object, "text");
  fix.evidence = text(*object, "evidence");
  if (const llvm::json::Array *edits = object->getArray("edits"))
    for (const llvm::json::Value &each : *edits)
      if (const llvm::json::Object *edit = each.getAsObject())
        fix.edits.push_back(
            {.file = text(*edit, "file"),
             .offset = number(*edit, "offset"),
             .text = text(*edit, "text"),
             .removed = text(*edit, "removed"),
             .needsInclude = edit->getBoolean("needsInclude").value_or(false)});
  return true;
}

bool readError(const llvm::json::Value &value, RuleError &error) {
  const llvm::json::Object *object = value.getAsObject();
  if (object == nullptr)
    return false;
  error.rule = text(*object, "rule");
  error.file = text(*object, "file");
  error.line = number(*object, "line");
  error.column = number(*object, "column");
  error.message = text(*object, "message");
  error.owner = text(*object, "owner");
  error.ownerFile = text(*object, "ownerFile");
  error.ownerLine = number(*object, "ownerLine");
  error.global = object->getBoolean("global").value_or(false);
  error.surveyed = object->getBoolean("surveyed").value_or(false);
  if (const llvm::json::Array *subjects = object->getArray("subjects"))
    for (const llvm::json::Value &each : *subjects)
      if (const auto subject = each.getAsString())
        error.subjects.push_back(subject->str());
  if (const llvm::json::Array *fixes = object->getArray("fixes"))
    for (const llvm::json::Value &each : *fixes) {
      RuleFix fix;
      if (readFix(each, fix))
        error.fixes.push_back(std::move(fix));
    }
  return true;
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

/// Whether the ledger at `path` is the one of `object`.
static bool sameObject(llvm::StringRef path, llvm::StringRef object) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return false;
  llvm::Expected<llvm::json::Value> parsed =
      llvm::json::parse((*buffer)->getBuffer());
  if (!parsed) {
    llvm::consumeError(parsed.takeError());
    return false;
  }
  const llvm::json::Object *document = parsed->getAsObject();
  const llvm::json::Array *units =
      document == nullptr ? nullptr : document->getArray("units");
  const llvm::json::Object *unit = units == nullptr || units->empty()
                                       ? nullptr
                                       : units->front().getAsObject();
  return unit != nullptr && unit->getString("object") == object;
}

bool writeEnforcementLedger(const EnforcementLedger &ledger,
                            const EnforcementUnit &unit, llvm::StringRef path,
                            std::string &error) {
  llvm::json::Array rows;
  for (const LedgerRow &row : ledger.rows)
    rows.push_back(rowJson(row));
  llvm::json::Array functions;
  for (const LedgerFunction &function : ledger.functions) {
    llvm::json::Object entry{{.K = "function", .V = function.function},
                             {.K = "tier", .V = function.tier},
                             {.K = "file", .V = function.file},
                             {.K = "line", .V = function.line},
                             {.K = "owns", .V = function.owns},
                             {.K = "starts", .V = function.starts},
                             {.K = "locks", .V = function.locks},
                             {.K = "sends", .V = function.sends},
                             {.K = "handlers", .V = function.handlers}};
    // RFC 0047 §7.
    if (function.surveyed)
      entry["surveyed"] = true;
    // RFC 0054 §4.
    if (function.adapter)
      entry["adapter"] = true;
    functions.push_back(std::move(entry));
  }
  llvm::json::Array fields;
  for (const LedgerField &field : ledger.fields) {
    llvm::json::Object entry{{.K = "record", .V = field.record},
                             {.K = "field", .V = field.field},
                             {.K = "kind", .V = field.kind},
                             {.K = "file", .V = field.file},
                             {.K = "line", .V = field.line}};
    if (field.kind == "class") {
      entry["sendable"] = field.sendable;
      entry["shareable"] = field.shareable;
    }
    fields.push_back(std::move(entry));
  }
  // RFC 0049 §7.
  llvm::json::Array errors;
  for (const RuleError &each : ledger.errors)
    errors.push_back(errorJson(each));
  llvm::json::Array evidence;
  for (const RuleFix &fix : ledger.evidence)
    evidence.push_back(fixJson(fix));
  llvm::json::Array files;
  for (const ReadFile &file : ledger.files)
    files.push_back(llvm::json::Object{{.K = "path", .V = file.path},
                                       {.K = "size", .V = file.size},
                                       {.K = "hash", .V = file.hash}});
  llvm::json::Array command;
  for (const std::string &argument : unit.command)
    command.push_back(argument);
  llvm::json::Array library;
  for (const LedgerLibrary &entry : ledger.library)
    library.push_back(llvm::json::Object{{.K = "kind", .V = entry.kind},
                                         {.K = "function", .V = entry.function},
                                         {.K = "file", .V = entry.file},
                                         {.K = "line", .V = entry.line},
                                         {.K = "reason", .V = entry.reason}});
  llvm::json::Object config{{.K = "checks", .V = unit.checks},
                            {.K = "zeroInit", .V = unit.zeroInit}};
  if (!unit.safe.empty())
    config["safe"] = unit.safe;
  llvm::json::Object document{
      {.K = "schema", .V = "weavec-ledger"},
      {.K = "version", .V = EnforcementLedgerVersion},
      {.K = "producer",
       .V = llvm::json::Object{{.K = "name", .V = "weavec-cc"},
                               {.K = "version", .V = WEAVEC_VERSION_STRING}}},
      {.K = "units",
       .V = llvm::json::Array{llvm::json::Object{
           {.K = "source", .V = unit.source},
           {.K = "object", .V = unit.object},
           {.K = "target", .V = unit.target},
           {.K = "config", .V = std::move(config)},
           {.K = "summary",
            .V = llvm::json::
                Object{{.K = "accesses",
                        .V = static_cast<std::int64_t>(ledger.accesses())},
                       {.K = "proven",
                        .V = static_cast<std::int64_t>(ledger.proven)},
                       {.K = "guarded",
                        .V = static_cast<std::int64_t>(ledger.guarded)},
                       {.K = "unguarded",
                        .V =
                            static_cast<std::int64_t>(ledger.unguarded)},
                       {.K = "typedSites",
                        .V =
                            static_cast<std::int64_t>(ledger.typedSites)},
                       {.K = "placedLocals",
                        .V =
                            static_cast<std::int64_t>(ledger.placedLocals)},
                       {.K = "checkedCasts",
                        .V =
                            static_cast<std::int64_t>(ledger.checkedCasts)},
                       {.K = "checkedMembers",
                        .V =
                            static_cast<std::int64_t>(ledger.checkedMembers)}}},
           // RFC 0051 §8: what the unit's options defined, and
           // what GuardDefine placed in its safe functions.
           {.K = "defined",
            .V =
                llvm::json::Object{
                    {.K = "unit", .V = unit.defined},
                    {.K = "unreachable",
                     .V = static_cast<std::int64_t>(
                         ledger.defined.unreachable)},
                    {.K = "missingReturn",
                     .V = static_cast<std::int64_t>(ledger.defined
                                                        .missingReturn)},
                    {.K = "division",
                     .V = static_cast<std::int64_t>(ledger.defined.division)},
                    {.K = "shifts",
                     .V = static_cast<std::int64_t>(ledger.defined.shifts)},
                    {.K = "conversions",
                     .V = static_cast<std::int64_t>(ledger.defined
                                                        .conversions)},
                    {.K = "probed",
                     .V = static_cast<std::int64_t>(ledger.defined.probed)}}},
           {.K = "functions", .V = std::move(functions)},
           {.K = "fields", .V = std::move(fields)},
           {.K = "partial", .V = unit.partial},
           {.K = "command",
            .V = llvm::json::Object{{.K = "directory", .V = unit.directory},
                                    {.K = "arguments", .V = std::move(command)},
                                    {.K = "cxx", .V = unit.cxx}}},
           {.K = "files", .V = std::move(files)},
           {.K = "errors", .V = std::move(errors)},
           {.K = "evidence", .V = std::move(evidence)},
           {.K = "library", .V = std::move(library)},
           {.K = "rows", .V = std::move(rows)},
       }}},
  };
  std::string text;
  llvm::raw_string_ostream os(text);
  os << llvm::formatv("{0:2}", llvm::json::Value(std::move(document))) << '\n';
  // `-fweavec-ledger=out/` names a directory, made if it is missing.
  if (!path.empty() && llvm::sys::path::is_separator(path.back()))
    std::ignore = llvm::sys::fs::create_directories(path);
  // RFC 0049 §7: a unit with no object (-fsyntax-only) is named by its
  // source and a hash of its absolute path, so that two sources of one
  // name (in one directory or in two) do not share a ledger.
  std::string artifact = unit.object.empty() ? unit.source : unit.object;
  const auto hashed = [&] {
    llvm::SmallString<256> full(artifact);
    std::ignore = llvm::sys::fs::make_absolute(full);
    return artifact + "." +
           llvm::utohexstr(llvm::xxh3_64bits(full.str())).substr(0, 8);
  };
  if (unit.object.empty() && unit.partial)
    artifact = hashed();
  std::string chosen = ledgerPathFor(path, artifact);
  // RFC 0049 §7: two objects of one name in a directory (`pair0/pair.c.o`,
  // `pair1/pair.c.o`) do not share a ledger: the first claims the name, and
  // another object's gets a hash of its path.
  if (isLedgerDirectory(path) && !unit.object.empty()) {
    int fd = -1;
    if (!llvm::sys::fs::openFileForWrite(chosen, fd,
                                         llvm::sys::fs::CD_CreateNew)) {
      std::ignore = llvm::sys::Process::SafelyCloseFileDescriptor(fd);
    } else if (!sameObject(chosen, unit.object)) {
      chosen = ledgerPathFor(path, hashed());
    }
  }
  return writeFileAtomically(chosen, text, error);
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
  os << "; " << safe << " of " << ledger.functions.size() << " functions safe";
  // RFC 0043 §11.
  const auto kind = [&](llvm::StringRef name) {
    return llvm::count_if(
        ledger.fields, [&](const auto &field) { return field.kind == name; });
  };
  os << "; " << kind("owner") << " owner and " << kind("reference")
     << " reference fields";
  // RFC 0046 §12.
  const auto starts = std::accumulate(
      ledger.functions.begin(), ledger.functions.end(), 0U,
      [](unsigned sum, const auto &function) { return sum + function.starts; });
  if (kind("lock") != 0 || starts != 0)
    os << "; " << kind("lock") << " locked classes, " << starts
       << " thread starts";
  // RFC 0045 §10.
  if (ledger.typedSites != 0 || !ledger.library.empty())
    os << "; " << ledger.typedSites << " typed allocation sites, "
       << llvm::count_if(
              ledger.library,
              [](const auto &entry) { return entry.kind == "trusted"; })
       << " trusted library functions";
  // RFC 0048 §9.
  if (ledger.placedLocals != 0)
    os << "; " << ledger.placedLocals << " placed locals";
  // RFC 0054 §5.
  if (ledger.checkedCasts != 0 || ledger.checkedMembers != 0)
    os << "; " << ledger.checkedCasts << " checked conversions, "
       << ledger.checkedMembers << " checked union members";
  return text;
}

//===----------------------------------------------------------------------===//
// The audit (RFC 0042 §10, RFC 0043 §11, RFC 0046 §12)
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
  /// RFC 0044 §12: members that share an object safe C++ may change, and
  /// members that erase the type of what they hold.
  llvm::json::Array shared;
  llvm::json::Array erased;
  /// Global owners safe code reads in place.
  llvm::json::Array globals;
  /// RFC 0045 §10: the library functions fast units run without guards
  /// (once each), and the allocations they could not type.
  llvm::json::Array library;
  llvm::json::Array untyped;
  /// RFC 0046 §12: what (Z1) to (Z6) rest on: functions that are not safe
  /// and start threads or use locks, their WEAVEC_SENDS parameters, and the
  /// globals of code that is not safe that safe code names.
  llvm::json::Array threads;
  llvm::json::Array sends;
  /// RFC 0054 §4: functions that are not safe and only convert and call.
  llvm::json::Array adapters;
  llvm::json::Array threadGlobals;
  /// RFC 0051 §8: what (N1) to (N6) rest on: the units with safe functions
  /// not compiled with -fweavec-safe (their library code keeps the command
  /// line's arithmetic), and the functions that are not safe and register
  /// a signal handler.
  llvm::json::Array undefinedUnits;
  llvm::json::Array handlers;
  std::set<std::string> libraryNames;
  std::set<std::string> fields;
  unsigned safe = 0;
  unsigned functions = 0;
  /// RFC 0047 §7: each function once (a header's are listed by each unit
  /// that includes it), by file, line and name: safe, or surveyed; and the
  /// aliased classes.
  struct Surveyed {
    bool safe = false;
    bool surveyed = false;
  };
  std::map<std::tuple<std::string, unsigned, std::string>, Surveyed> converted;
  /// RFC 0049 §8: the surveyed errors, each once (a header's are reported
  /// by each unit that includes it).
  std::map<std::string, RuleError> errors;
  llvm::json::Array aliased;
  /// RFC 0049 §8: the counted members code outside their class can write,
  /// and the classes with a member function that is not safe.
  std::set<std::pair<std::string, std::string>> writable;
  std::map<std::pair<std::string, std::string>, llvm::json::Object> counted;
  /// RFC 0048 §9: the locals placed in typed memory, which the source does
  /// not show as allocations.
  std::int64_t placed = 0;
};
} // namespace

void ledgersOf(llvm::StringRef operand, std::vector<std::string> &paths) {
  if (!llvm::sys::fs::is_directory(operand)) {
    paths.push_back(operand.str());
    return;
  }
  std::error_code error;
  for (llvm::sys::fs::recursive_directory_iterator it(operand, error), end;
       it != end && !error; it.increment(error))
    if (llvm::sys::path::filename(it->path()) == "weavec-fix")
      it.no_push(); // RFC 0049 §6: the fixer's runs are not the project's.
    else if (llvm::StringRef(it->path()).ends_with(".ledger.json"))
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
    if (const llvm::json::Object *summary = unit->getObject("summary"))
      audit.placed += summary->getInteger("placedLocals").value_or(0);
    bool unitDefined = false;
    if (const llvm::json::Object *defined = unit->getObject("defined"))
      unitDefined = defined->getBoolean("unit").value_or(false);
    bool hasSafe = false;
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
        {
          Audit::Surveyed &entry = audit.converted[{
              function->getString("file").value_or("").str(),
              static_cast<unsigned>(function->getInteger("line").value_or(0)),
              name}];
          entry.safe = entry.safe || tier == "safe";
          entry.surveyed = entry.surveyed ||
                           function->getBoolean("surveyed").value_or(false);
        }
        if (tier == "safe") {
          ++audit.safe;
          hasSafe = true;
          continue;
        }
        audit.trusted.push_back(llvm::json::Object(*function));
        if (function->getInteger("handlers").value_or(0) != 0)
          audit.handlers.push_back(llvm::json::Object(*function));
        if (function->getInteger("starts").value_or(0) != 0 ||
            function->getBoolean("locks").value_or(false))
          audit.threads.push_back(llvm::json::Object(*function));
        if (function->getBoolean("sends").value_or(false))
          audit.sends.push_back(llvm::json::Object(*function));
        if (function->getBoolean("adapter").value_or(false))
          audit.adapters.push_back(llvm::json::Object(*function));
      }
    if (hasSafe && !unitDefined && !unit->getBoolean("partial").value_or(false))
      audit.undefinedUnits.push_back(llvm::json::Object{
          {.K = "file", .V = unit->getString("source").value_or("").str()},
          {.K = "line", .V = 0},
          {.K = "source", .V = unit->getString("source").value_or("").str()}});
    if (const llvm::json::Array *fields = unit->getArray("fields"))
      for (const llvm::json::Value &value : *fields)
        if (const llvm::json::Object *field = value.getAsObject();
            field != nullptr &&
            // A header's struct is listed by each unit that includes it.
            audit.fields.insert(llvm::formatv("{0}", value).str()).second) {
          const auto kind = field->getString("kind");
          const std::pair<std::string, std::string> member{
              field->getString("record").value_or("").str(),
              field->getString("field").value_or("").str()};
          if (kind == "counted")
            audit.counted.emplace(member, *field);
          if (kind == "writable")
            audit.writable.insert(member);
          // RFC 0047 §8: a member with a bound is a reference too.
          if (kind == "reference" || kind == "counted")
            audit.references.push_back(llvm::json::Object(*field));
          else if (kind == "shared")
            audit.shared.push_back(llvm::json::Object(*field));
          else if (kind == "erased")
            audit.erased.push_back(llvm::json::Object(*field));
          else if (kind == "global")
            audit.globals.push_back(llvm::json::Object(*field));
          else if (kind == "thread-global")
            audit.threadGlobals.push_back(llvm::json::Object(*field));
          else if (kind == "aliased")
            audit.aliased.push_back(llvm::json::Object(*field));
        }
    if (const llvm::json::Array *errors = unit->getArray("errors"))
      for (const llvm::json::Value &value : *errors) {
        RuleError error;
        if (readError(value, error) && error.surveyed)
          audit.errors.emplace(error.file + ":" + std::to_string(error.line) +
                                   ":" + std::to_string(error.column) + " " +
                                   error.rule + " " + error.message,
                               std::move(error));
      }
    if (const llvm::json::Array *library = unit->getArray("library"))
      for (const llvm::json::Value &value : *library) {
        const llvm::json::Object *entry = value.getAsObject();
        if (entry == nullptr)
          continue;
        if (entry->getString("kind") == "untyped")
          audit.untyped.push_back(llvm::json::Object(*entry));
        else if (audit.libraryNames
                     .insert(entry->getString("function").value_or("").str())
                     .second)
          audit.library.push_back(llvm::json::Object(*entry));
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

/// RFC 0047 §7, RFC 0049 §8: a conversion's progress, from the survey: per
/// file, its functions, how many are safe, and its errors by rule.
namespace {
struct FilePlan {
  std::string file;
  unsigned functions = 0;
  unsigned safe = 0;
  unsigned clean = 0;
  unsigned errors = 0;
  /// The errors with no fix, and their rules.
  unsigned unfixed = 0;
  std::map<std::string, unsigned> open;
  bool surveyed = true;
  std::map<std::string, unsigned> rules;
  llvm::json::Array places;
};
} // namespace

static std::vector<FilePlan> conversionPlan(const Audit &audit) {
  std::map<std::string, FilePlan> files;
  std::map<std::tuple<std::string, unsigned, std::string>, unsigned> owned;
  for (const auto &[key, error] : audit.errors) {
    FilePlan &plan = files[error.ownerFile];
    plan.file = error.ownerFile;
    ++plan.rules[error.rule];
    ++plan.errors;
    if (error.fixes.empty()) {
      ++plan.unfixed;
      ++plan.open[error.rule];
    }
    plan.places.push_back(
        llvm::json::Object{{.K = "file", .V = error.file},
                           {.K = "line", .V = error.line},
                           {.K = "column", .V = error.column},
                           {.K = "rule", .V = error.rule},
                           {.K = "fixes", .V = error.fixes.size()}});
    if (!error.global)
      ++owned[{error.ownerFile, error.ownerLine, error.owner}];
  }
  for (const auto &[key, entry] : audit.converted) {
    FilePlan &plan = files[std::get<0>(key)];
    plan.file = std::get<0>(key);
    ++plan.functions;
    if (entry.safe) {
      ++plan.safe;
      continue;
    }
    plan.surveyed = plan.surveyed && entry.surveyed;
    if (entry.surveyed && !owned.contains(key))
      ++plan.clean;
  }
  std::vector<FilePlan> plans;
  plans.reserve(files.size());
  for (auto &[file, plan] : files)
    plans.push_back(std::move(plan));
  // Fewest errors first; then by name.
  std::ranges::stable_sort(plans, [](const FilePlan &a, const FilePlan &b) {
    return a.errors < b.errors;
  });
  return plans;
}

/// RFC 0049 §8: the fixes the errors carry, and the choices by the errors
/// each would decide, most first.
static void fixesOf(const Audit &audit, std::map<std::string, unsigned> &levels,
                    std::vector<std::pair<std::string, unsigned>> &choices) {
  std::set<std::string> seen;
  std::map<std::pair<std::string, std::string>, unsigned> chosen;
  for (const auto &[key, error] : audit.errors)
    for (const RuleFix &fix : error.fixes) {
      if (fix.level == "choice") {
        ++chosen[{fix.declaration, fix.text}];
        continue;
      }
      if (seen.insert(fix.declaration + " " + fix.aspect + " " + fix.fact)
              .second)
        ++levels[fix.level];
    }
  for (const auto &[declaration, count] : chosen)
    choices.emplace_back(declaration.second, count);
  std::ranges::stable_sort(choices, [](const auto &a, const auto &b) {
    return a.second > b.second;
  });
}

static std::string ruleCounts(const std::map<std::string, unsigned> &rules) {
  std::vector<std::pair<std::string, unsigned>> sorted(rules.begin(),
                                                       rules.end());
  std::ranges::stable_sort(
      sorted, [](const auto &a, const auto &b) { return a.second > b.second; });
  std::string text;
  for (const auto &[rule, count] : sorted)
    text += (text.empty() ? "" : ", ") + rule + " " + std::to_string(count);
  return text;
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
  std::vector<FilePlan> plans = conversionPlan(audit);
  const bool surveyed = llvm::any_of(
      audit.converted, [](const auto &entry) { return entry.second.surveyed; });
  std::map<std::string, unsigned> levels;
  std::vector<std::pair<std::string, unsigned>> choices;
  fixesOf(audit, levels, choices);
  // RFC 0049 §8: the bounds on members that code which is not safe can
  // write, which nothing compares where it does (T-A1, V-A1).
  std::set<std::string> openRecords;
  for (const auto &[key, entry] : audit.converted)
    if (const std::string &name = std::get<2>(key);
        !entry.safe && name.rfind("::") != std::string::npos)
      openRecords.insert(name.substr(0, name.rfind("::")));
  llvm::json::Array unchecked;
  for (const auto &[member, field] : audit.counted)
    if (audit.writable.contains(member) || openRecords.contains(member.first))
      unchecked.push_back(llvm::json::Object(field));
  const auto ready = [](const FilePlan &plan) {
    return plan.surveyed && plan.errors == 0 && plan.safe < plan.functions;
  };
  if (json) {
    llvm::json::Array files;
    for (FilePlan &plan : plans)
      files.push_back(
          llvm::json::Object{{.K = "file", .V = plan.file},
                             {.K = "functions", .V = plan.functions},
                             {.K = "safe", .V = plan.safe},
                             {.K = "clean", .V = plan.clean},
                             {.K = "errors", .V = plan.errors},
                             {.K = "unfixed", .V = plan.unfixed},
                             {.K = "ready", .V = ready(plan)},
                             {.K = "places", .V = std::move(plan.places)}});
    llvm::json::Object fixes;
    for (const auto &[level, count] : levels)
      fixes[level] = count;
    llvm::json::Array chosen;
    for (const auto &[text, count] : choices)
      chosen.push_back(llvm::json::Object{{.K = "choice", .V = text},
                                          {.K = "errors", .V = count}});
    llvm::outs()
        << llvm::formatv(
               "{0:2}",
               llvm::json::Value(llvm::json::Object{
                   {.K = "schema", .V = "weavec-audit"},
                   {.K = "version", .V = 10},
                   {.K = "undefined",
                    .V = llvm::json::Object{{.K = "units",
                                             .V = llvm::json::Value(
                                                 llvm::json::Array(
                                                     audit.undefinedUnits))},
                                            {.K = "handlers",
                                             .V = llvm::json::Value(
                                                 llvm::json::Array(
                                                     audit.handlers))}}},
                   {.K = "functions", .V = audit.functions},
                   {.K = "placedLocals", .V = audit.placed},
                   {.K = "safe", .V = audit.safe},
                   {.K = "trusted", .V = std::move(audit.trusted)},
                   {.K = "guarded", .V = std::move(audit.guarded)},
                   {.K = "wrapped", .V = std::move(audit.wrapped)},
                   {.K = "references", .V = std::move(audit.references)},
                   {.K = "shared", .V = std::move(audit.shared)},
                   {.K = "erased", .V = std::move(audit.erased)},
                   {.K = "globals", .V = std::move(audit.globals)},
                   {.K = "library", .V = std::move(audit.library)},
                   {.K = "untyped", .V = std::move(audit.untyped)},
                   {.K = "threads", .V = std::move(audit.threads)},
                   {.K = "sends", .V = std::move(audit.sends)},
                   {.K = "adapters", .V = std::move(audit.adapters)},
                   {.K = "threadGlobals", .V = std::move(audit.threadGlobals)},
                   {.K = "aliased", .V = std::move(audit.aliased)},
                   {.K = "unchecked", .V = std::move(unchecked)},
                   {.K = "conversion",
                    .V = surveyed
                             ? llvm::json::Value(llvm::json::Object{
                                   {.K = "files", .V = std::move(files)},
                                   {.K = "fixes", .V = std::move(fixes)},
                                   {.K = "choices", .V = std::move(chosen)}})
                             : llvm::json::Value(nullptr)}}))
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
  // RFC 0051 §8: what the guarantee that nothing is undefined rests on.
  if (audit.safe == audit.functions && audit.functions != 0 &&
      audit.undefinedUnits.empty() && audit.handlers.empty()) {
    out << "nothing undefined: every function is safe, and every unit with "
           "safe functions was compiled with -fweavec-safe\n";
  } else if (!audit.undefinedUnits.empty() || !audit.handlers.empty()) {
    out << "\nundefined behavior (what (N1) to (N6) rest on):\n";
    for (const llvm::json::Value &value : audit.undefinedUnits)
      out << "  " << value.getAsObject()->getString("source").value_or("")
          << ": safe functions in a unit not compiled with -fweavec-safe "
             "(the library code it instantiates keeps the command line's "
             "arithmetic)\n";
    for (const llvm::json::Value &value : audit.handlers) {
      const llvm::json::Object &entry = *value.getAsObject();
      out << "  " << where(entry) << ": "
          << entry.getString("function").value_or("")
          << " registers a signal handler (code that is not safe; the "
             "handler runs between any two instructions)\n";
    }
  }
  if (audit.placed != 0)
    out << audit.placed
        << " locals placed in typed memory (their addresses are kept)\n";
  // RFC 0054 §4.
  if (!audit.adapters.empty()) {
    out << "\nfunctions that only convert and call (the conversion is checked "
           "in safe code; each can be safe):\n";
    for (const llvm::json::Value &value : audit.adapters) {
      const llvm::json::Object &entry = *value.getAsObject();
      out << "  " << where(entry) << ": "
          << entry.getString("function").value_or("") << '\n';
    }
  }
  // RFC 0047 §7: the conversion's plan, when the ledgers were surveyed.
  if (surveyed) {
    unsigned functions = 0;
    unsigned safe = 0;
    unsigned clean = 0;
    std::map<std::string, unsigned> total;
    for (const FilePlan &plan : plans) {
      functions += plan.functions;
      safe += plan.safe;
      clean += plan.clean;
      for (const auto &[rule, count] : plan.rules)
        total[rule] += count;
    }
    out << "\nconversion: " << safe << " of " << functions
        << " functions safe ("
        << (functions == 0 ? 0 : ((100 * safe) + (functions / 2)) / functions)
        << "%); " << clean << " more have no error\n";
    // RFC 0049 §8: what the fixer writes, what is left to decide, and what
    // needs a rewrite.
    out << "fixes: " << levels["certain"] << " certain, " << levels["likely"]
        << " likely ('weavec-cc --weavec-fix <ledger>' applies them)\n";
    out << "choices, most errors first:\n";
    for (const auto &[text, count] : choices)
      out << "  " << text << "  " << count
          << (count == 1 ? " error\n" : " errors\n");
    out << "no fix, fewest errors first:\n";
    for (const FilePlan &plan : plans)
      if (plan.unfixed != 0)
        out << "  " << plan.file << "  " << plan.unfixed
            << (plan.unfixed == 1 ? " error: " : " errors: ")
            << ruleCounts(plan.open) << '\n';
    out << "ready (marked or to mark WEAVEC_SAFE_FILE):\n";
    for (const FilePlan &plan : plans)
      if (ready(plan))
        out << "  " << plan.file << "  " << plan.functions - plan.safe
            << (plan.functions - plan.safe == 1 ? " function\n"
                                                : " functions\n");
    out << "by rule: " << ruleCounts(total) << '\n';
  }
  list("functions that are not safe (safe code trusts them; 'owns': a "
       "parameter or the result is WEAVEC_OWNED)",
       audit.trusted, "function", "tier");
  list("accesses of safe functions that keep a guard", audit.guarded,
       "operation", "reason");
  list("library calls of safe functions that keep a checked wrapper",
       audit.wrapped, "operation", "");
  list("reference fields (tested where safe code loads them)", audit.references,
       "field", "");
  list("bounds on members that code which is not safe can write (nothing "
       "compares them where it does: T-A1, V-A1)",
       unchecked, "field", "");
  list("shared members (safe C++ may change what they point to, and does "
       "not follow the other holders)",
       audit.shared, "field", "");
  list("erased members (they may hold borrows their type does not show)",
       audit.erased, "field", "");
  list("global owners read in place (in the function named)", audit.globals,
       "field", "");
  list("library functions fast units run without guards (the library's own "
       "checks stand in for them)",
       audit.library, "function", "");
  list("allocations fast units could not type (they wait for a sweep)",
       audit.untyped, "function", "reason");
  list("functions that are not safe and start threads or use locks (safe "
       "code trusts what they share)",
       audit.threads, "function", "tier");
  list("WEAVEC_SENDS parameters of functions that are not safe (their "
       "callers are trusted to pass what may be sent)",
       audit.sends, "function", "tier");
  list("globals of code that is not safe that safe code names (in the "
       "function named)",
       audit.threadGlobals, "field", "");
  list("aliased classes (changed through pointers other objects keep; a "
       "borrow of what they own is not followed across calls)",
       audit.aliased, "record", "");
  return ok ? 0 : 1;
}

} // namespace weavec::frontend
