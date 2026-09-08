//===- Sidecar.cpp - The per-object summary file --------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Sidecar.h"

#include "weavec/Core/CheckedIO.h"
#include "weavec/Core/SummaryIO.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace weavec::frontend {

std::string sidecarPathFor(llvm::StringRef output) {
  return output.str() + ".weavec";
}

/// A field key with its spaces as `~`, and back.
static std::string withoutSpaces(llvm::StringRef key) {
  std::string result = key.str();
  std::ranges::replace(result, ' ', '~');
  return result;
}

static std::string withSpaces(llvm::StringRef key) {
  std::string result = key.str();
  std::ranges::replace(result, '~', ' ');
  return result;
}

std::string printUnitRecord(const UnitRecord &record) {
  std::string text;
  llvm::raw_string_ostream os(text);
  const analysis::UnitExports &exports = record.exports;
  const core::GlobalNamer names = [&exports](std::uint32_t id) {
    return exports.globals.nameOf(id).str();
  };
  os << "weavec-summaries " << SidecarFormatVersion << '\n';
  if (!record.objectDigest.empty())
    os << "checked-object " << record.objectDigest << '\n';
  if (!record.commandDigest.empty())
    os << "checked-command " << record.commandDigest << '\n';
  for (const auto &[path, digest] : exports.checkedInputs)
    os << "checked-input " << llvm::toHex(path, true) << ' ' << digest << '\n';
  if (!exports.checkedTarget.empty())
    os << "checked-target " << exports.checkedTarget << '\n';
  for (const auto &[name, contract] : exports.checkedDefinitions)
    os << "checked-definition " << core::CallTargets::function(name).toString()
       << ' ' << core::printCheckedContract(contract, names) << '\n';
  if (!exports.source.empty())
    os << "source " << exports.source << '\n';
  if (!record.workingDirectory.empty())
    os << "cwd " << record.workingDirectory << '\n';
  for (const std::string &arg : record.command)
    os << "arg " << arg << '\n';
  for (const auto &[name, targets] : exports.callbackGlobals)
    os << "callback-global " << core::CallTargets::function(name).toString()
       << ' ' << targets.toString() << '\n';
  for (const auto &[symbol, requests] : exports.callbackRequests) {
    for (const auto &bindings : requests)
      os << "callback-request "
         << core::CallTargets::function(symbol).toString() << ' '
         << core::printCallbackBindings(bindings) << '\n';
  }
  for (const auto &[symbol, requests] : exports.memoryRequests)
    for (const auto &input : requests)
      os << "memory-request " << core::CallTargets::function(symbol).toString()
         << ' ' << core::printCallContext(input, names) << '\n';
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
  // RFC 0012, *Sized fields*: several keys on one line, so the spaces in a
  // key (`struct buf.data`) are spelled `~`, as RFC 0011 spells field keys
  // in offsets.
  for (const analysis::SizedFieldWitness &w : exports.sizedFields.witnesses) {
    os << "sized-field " << withoutSpaces(w.field) << ' '
       << withoutSpaces(w.count) << ' ' << w.scale;
    if (w.productType)
      os << ' ' << w.productType->toString();
    os << '\n';
  }
  for (const std::string &field : exports.sizedFields.unsizedFields)
    os << "unsized-field " << withoutSpaces(field) << '\n';
  for (const analysis::UnsizedPair &pair : exports.sizedFields.unsizedPairs) {
    os << "unsized-field " << withoutSpaces(pair.field) << ' '
       << withoutSpaces(pair.count) << '\n';
  }
  for (const std::string &field : exports.sizedFieldLoads)
    os << "loads-field " << withoutSpaces(field) << '\n';
  for (const ReportedDiagnostic &d : record.reported) {
    os << "reported " << d.id << ' ' << d.line << ' ' << d.column << ' '
       << d.file << '\n';
  }
  for (const auto &[name, function] : exports.functions) {
    os << "function " << name << ' '
       << (function.external ? "external" : "internal") << ' '
       << (function.addressTaken ? "address-taken" : "plain");
    if (!function.typeKey.empty())
      os << ' ' << function.typeKey;
    os << '\n';
    if (function.acceptsCallbacks)
      os << "accepts-callbacks\n";
    if (function.acceptsMemoryContexts)
      os << "accepts-memory-contexts\n";
    os << core::printSummary(function.summary, names);
    for (const auto &[input, summary] : function.memorySpecializations) {
      os << "memory-specialization " << core::printCallContext(input, names)
         << '\n';
      os << core::printSummary(summary, names);
    }
    for (const auto &[bindings, summary] : function.specializations) {
      os << "specialization " << core::printCallbackBindings(bindings) << '\n';
      os << core::printSummary(summary, names);
    }
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
  std::optional<core::CallbackBindings> specialized;
  std::optional<core::CallContext> memorySpecialized;
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
      if (memorySpecialized) {
        if (!current->memorySpecializations
                 .emplace(*memorySpecialized, *summary)
                 .second)
          return fail("duplicate memory specialization");
        memorySpecialized.reset();
      } else if (specialized) {
        if (!current->specializations.emplace(*specialized, *summary).second)
          return fail("duplicate callback specialization");
        specialized.reset();
      } else {
        current->summary = *summary;
      }
      continue;
    }

    const auto [kind, rawValue] = line.split(' ');
    const llvm::StringRef value = rawValue.trim();
    if ((memorySpecialized || specialized) && kind != "summary")
      return fail("specialization without summary");
    const auto digestValid = [](llvm::StringRef digest) {
      return digest.size() == 64 && std::ranges::all_of(digest, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             });
    };
    if (kind == "checked-object" || kind == "checked-command") {
      auto &digest =
          kind == "checked-object" ? record.objectDigest : record.commandDigest;
      if (!digest.empty() || !digestValid(value))
        return fail("invalid checked digest");
      digest = value.str();
    } else if (kind == "checked-input") {
      const auto [path, digest] = value.split(' ');
      if (path.empty() || path.size() > 131072 || path.size() % 2 != 0 ||
          !std::ranges::all_of(path,
                               [](char c) { return llvm::isHexDigit(c); }) ||
          !digestValid(digest) || exports.checkedInputs.size() >= 65536 ||
          !exports.checkedInputs.emplace(llvm::fromHex(path), digest.str())
               .second)
        return fail("invalid checked input");
    } else if (kind == "checked-target") {
      if (!exports.checkedTarget.empty() || value.empty())
        return fail("invalid checked target");
      exports.checkedTarget = value.str();
    } else if (kind == "checked-definition") {
      const auto [nameText, contractText] = value.split(' ');
      const auto name = core::CallTargets::parse(nameText.str());
      const auto contract =
          core::parseCheckedContract(contractText.str(), resolve);
      if (!name || !name->resolved() || name->functions.size() != 1 ||
          !contract || exports.checkedDefinitions.size() >= 65536 ||
          !exports.checkedDefinitions
               .emplace(*name->functions.begin(), *contract)
               .second)
        return fail("invalid checked definition");
    } else if (kind == "accepts-memory-contexts") {
      if (!current || !value.empty())
        return fail("invalid memory interface");
      current->acceptsMemoryContexts = true;
    } else if (kind == "memory-request") {
      const auto [symbolText, inputText] = value.split(' ');
      const auto symbol = core::CallTargets::parse(symbolText.str());
      const auto input = core::parseCallContext(inputText.str(), resolve);
      if (!symbol || !symbol->resolved() || symbol->functions.size() != 1 ||
          !input)
        return fail("invalid memory request");
      auto &requests = exports.memoryRequests[*symbol->functions.begin()];
      if (!requests.insert(*input).second)
        return fail("duplicate memory request");
      if (requests.size() > core::MaxMemoryContexts)
        return fail("too many memory requests");
    } else if (kind == "memory-specialization") {
      if (!current || specialized || memorySpecialized ||
          current->memorySpecializations.size() >= core::MaxMemoryContexts)
        return fail("invalid memory specialization record");
      memorySpecialized = core::parseCallContext(value.str(), resolve);
      if (!memorySpecialized)
        return fail("invalid memory context");
    } else if (kind == "accepts-callbacks") {
      if (!current || !value.empty())
        return fail("invalid callback interface");
      current->acceptsCallbacks = true;
    } else if (kind == "callback-global") {
      const auto [nameText, targetsText] = value.split(' ');
      const auto name = core::CallTargets::parse(nameText.str());
      const auto targets = core::CallTargets::parse(targetsText.str());
      if (!name || !name->resolved() || name->functions.size() != 1 ||
          !targets ||
          !exports.callbackGlobals.emplace(*name->functions.begin(), *targets)
               .second)
        return fail("invalid callback global");
    } else if (kind == "callback-request") {
      const auto [symbolText, bindingText] = value.split(' ');
      const auto symbol = core::CallTargets::parse(symbolText.str());
      const auto bindings = core::parseCallbackBindings(bindingText.str());
      if (!symbol || !symbol->resolved() || symbol->functions.size() != 1 ||
          !bindings)
        return fail("invalid callback request");
      auto &requests = exports.callbackRequests[*symbol->functions.begin()];
      requests.insert(*bindings);
      if (requests.size() > core::MaxCallbackContexts)
        return fail("too many callback requests");
    } else if (kind == "specialization") {
      if (!current || specialized ||
          current->specializations.size() >= core::MaxCallbackContexts)
        return fail("invalid specialization record");
      specialized = core::parseCallbackBindings(value.str());
      if (!specialized)
        return fail("invalid callback bindings");
    } else if (kind == "source") {
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
    } else if (kind == "loads-field") {
      exports.sizedFieldLoads.insert(withSpaces(value));
    } else if (kind == "sized-field") {
      // `<field> <count> <scale>`, keys without spaces.
      llvm::SmallVector<llvm::StringRef, 3> fields;
      value.split(fields, ' ');
      std::int64_t scale = 0;
      const auto productType = fields.size() == 4
                                   ? core::IntegerType::parse(fields[3].str())
                                   : std::nullopt;
      if ((fields.size() != 3 && fields.size() != 4) || fields[0].empty() ||
          fields[1].empty() || fields[2].getAsInteger(10, scale) ||
          scale <= 0 ||
          (fields.size() == 4 &&
           (!productType || productType->isSigned || productType->isBoolean ||
            static_cast<std::uint64_t>(scale) > productType->mask())))
        return fail("line " + std::to_string(lineNumber) +
                    ": malformed 'sized-field' line");
      exports.sizedFields.witnesses.insert(
          analysis::SizedFieldWitness{.field = withSpaces(fields[0]),
                                      .count = withSpaces(fields[1]),
                                      .scale = scale,
                                      .productType = productType});
    } else if (kind == "unsized-field") {
      // `<field>` or `<field> <count>`.
      llvm::SmallVector<llvm::StringRef, 2> fields;
      value.split(fields, ' ');
      if (fields.empty() || fields.size() > 2 || fields[0].empty())
        return fail("line " + std::to_string(lineNumber) +
                    ": malformed 'unsized-field' line");
      if (fields.size() == 1) {
        exports.sizedFields.unsizedFields.insert(withSpaces(fields[0]));
      } else {
        exports.sizedFields.unsizedPairs.insert(analysis::UnsizedPair{
            .field = withSpaces(fields[0]), .count = withSpaces(fields[1])});
      }
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
      if (specialized)
        return fail("specialization without summary");
      current = &exports.functions[fields[0].str()];
      current->external = fields[1] == "external";
      current->addressTaken = fields[2] == "address-taken";
      current->typeKey = fields.size() == 4 ? fields[3].str() : std::string();
    }
    // Unknown line kinds are skipped for forward compatibility.
  }
  if (specialized || memorySpecialized)
    return fail("specialization without summary");
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
