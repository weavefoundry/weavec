//===- Sidecar.cpp - The per-object summary file --------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Sidecar.h"

#include "weavec/Core/SummaryIO.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace weavec::frontend {

std::string sidecarPathFor(llvm::StringRef output) {
  return output.str() + ".weavec";
}

std::string printUnitRecord(const UnitRecord &record) {
  std::string text;
  llvm::raw_string_ostream os(text);
  const analysis::UnitExports &exports = record.exports;
  os << "weavec-summaries " << SidecarFormatVersion << '\n';
  if (!exports.source.empty())
    os << "source " << exports.source << '\n';
  if (!record.workingDirectory.empty())
    os << "cwd " << record.workingDirectory << '\n';
  for (const std::string &arg : record.command)
    os << "arg " << arg << '\n';
  for (const std::string &name : exports.imports)
    os << "import " << name << '\n';
  for (const std::string &key : exports.indirectTypes)
    os << "indirect " << key << '\n';
  for (const std::string &name : exports.unknownCallees)
    os << "unknown " << name << '\n';
  for (const std::string &key : exports.unknownIndirectTypes)
    os << "unknown-indirect " << key << '\n';
  for (const std::string &key : exports.countFields)
    os << "count-field " << key << '\n';
  for (const ReportedDiagnostic &d : record.reported) {
    os << "reported " << d.id << ' ' << d.line << ' ' << d.column << ' '
       << d.file << '\n';
  }
  const core::GlobalNamer names = [&exports](std::uint32_t id) {
    return exports.globals.nameOf(id).str();
  };
  for (const auto &[name, function] : exports.functions) {
    os << "function " << name << ' '
       << (function.external ? "external" : "internal") << ' '
       << (function.addressTaken ? "address-taken" : "plain");
    if (!function.typeKey.empty())
      os << ' ' << function.typeKey;
    os << '\n';
    os << core::printSummary(function.summary, names);
  }
  return text;
}

std::optional<UnitRecord> parseUnitRecord(llvm::StringRef text,
                                          std::string *error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr)
      *error = std::move(message);
    return std::nullopt;
  };

  UnitRecord record;
  analysis::UnitExports &exports = record.exports;
  const core::GlobalResolver resolve = [&exports](std::string_view name) {
    return std::optional(exports.globals.idFor(name));
  };

  llvm::StringRef rest = text;
  unsigned lineNumber = 0;
  bool haveHeader = false;
  analysis::ExportedFunction *current = nullptr;
  while (!rest.empty()) {
    llvm::StringRef line;
    std::tie(line, rest) = rest.split('\n');
    ++lineNumber;
    line = line.rtrim("\r");
    if (line.trim().empty())
      continue;

    if (!haveHeader) {
      const auto [magic, version] = line.split(' ');
      unsigned parsed = 0;
      if (magic != "weavec-summaries" ||
          version.trim().getAsInteger(10, parsed))
        return fail("not a weavec summary file");
      if (parsed != SidecarFormatVersion)
        return fail("unsupported format " + std::to_string(parsed));
      haveHeader = true;
      continue;
    }

    if (line == "summary") {
      if (current == nullptr)
        return fail("line " + std::to_string(lineNumber) +
                    ": summary record without a function");
      // The record runs to the `end` line.
      std::string block = "summary\n";
      bool closed = false;
      while (!rest.empty()) {
        llvm::StringRef inner;
        std::tie(inner, rest) = rest.split('\n');
        ++lineNumber;
        block += inner.str();
        block += '\n';
        if (inner.trim() == "end") {
          closed = true;
          break;
        }
      }
      if (!closed)
        return fail("line " + std::to_string(lineNumber) +
                    ": summary record without 'end'");
      std::string summaryError;
      const auto summary = core::parseSummary(block, resolve, &summaryError);
      if (!summary)
        return fail("line " + std::to_string(lineNumber) + ": " + summaryError);
      current->summary = *summary;
      continue;
    }

    const auto [kind, rawValue] = line.split(' ');
    const llvm::StringRef value = rawValue.trim();
    if (kind == "source") {
      exports.source = value.str();
    } else if (kind == "cwd") {
      record.workingDirectory = value.str();
    } else if (kind == "arg") {
      record.command.push_back(value.str());
    } else if (kind == "import") {
      exports.imports.insert(value.str());
    } else if (kind == "indirect") {
      exports.indirectTypes.insert(value.str());
    } else if (kind == "unknown") {
      exports.unknownCallees.insert(value.str());
    } else if (kind == "unknown-indirect") {
      exports.unknownIndirectTypes.insert(value.str());
    } else if (kind == "count-field") {
      exports.countFields.insert(value.str());
    } else if (kind == "reported") {
      // `<id> <line> <column> <file>`; the file may contain spaces.
      llvm::SmallVector<llvm::StringRef, 4> fields;
      value.split(fields, ' ', /*MaxSplit=*/3);
      ReportedDiagnostic d;
      if (fields.size() < 3 || fields[0].empty() ||
          fields[1].getAsInteger(10, d.line) ||
          fields[2].getAsInteger(10, d.column))
        return fail("line " + std::to_string(lineNumber) +
                    ": malformed 'reported' line");
      d.id = fields[0].str();
      if (fields.size() == 4)
        d.file = fields[3].str();
      record.reported.insert(std::move(d));
    } else if (kind == "function") {
      // `<name> <linkage> <address-taken|plain> [<type key>]`; the type key
      // contains spaces.
      llvm::SmallVector<llvm::StringRef, 4> fields;
      value.split(fields, ' ', /*MaxSplit=*/3);
      if (fields.size() < 3 || fields[0].empty() ||
          (fields[1] != "external" && fields[1] != "internal") ||
          (fields[2] != "address-taken" && fields[2] != "plain"))
        return fail("line " + std::to_string(lineNumber) +
                    ": malformed 'function' line");
      current = &exports.functions[fields[0].str()];
      current->external = fields[1] == "external";
      current->addressTaken = fields[2] == "address-taken";
      current->typeKey = fields.size() == 4 ? fields[3].str() : std::string();
    }
    // Unknown line kinds are skipped for forward compatibility.
  }
  if (!haveHeader)
    return fail("empty file");
  return record;
}

bool writeSidecar(llvm::StringRef path, const UnitRecord &record,
                  std::string *error) {
  const std::string text = printUnitRecord(record);
  llvm::SmallString<256> temp(path);
  temp += ".tmp";
  {
    std::error_code ec;
    llvm::raw_fd_ostream out(temp, ec, llvm::sys::fs::OF_None);
    if (ec) {
      if (error != nullptr)
        *error = "cannot write '" + temp.str().str() + "': " + ec.message();
      return false;
    }
    out << text;
  }
  if (const std::error_code ec = llvm::sys::fs::rename(temp, path)) {
    if (error != nullptr)
      *error = "cannot rename '" + temp.str().str() + "': " + ec.message();
    std::ignore = llvm::sys::fs::remove(temp);
    return false;
  }
  return true;
}

std::optional<UnitRecord> readSidecar(llvm::StringRef path,
                                      std::string *error) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
      llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    if (error != nullptr)
      *error = buffer.getError().message();
    return std::nullopt;
  }
  return parseUnitRecord((*buffer)->getBuffer(), error);
}

} // namespace weavec::frontend
