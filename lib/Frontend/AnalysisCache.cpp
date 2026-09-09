//===- AnalysisCache.cpp - Settled checkpoints (RFC 0020) ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/AnalysisCache.h"

#include "CheckpointExplanations.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/CheckedArtifacts.h"
#include "weavec/Frontend/Sidecar.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <limits>

namespace weavec::frontend {

static constexpr std::size_t MaxCheckpointBytes =
    std::size_t{256} * 1024 * 1024;
static constexpr std::uint64_t MaxDecodedCheckpointBytes =
    std::uint64_t{4} * 1024 * 1024 * 1024;
static constexpr std::size_t CompressAboveBytes = std::size_t{1024} * 1024;
static constexpr std::size_t MaxCheckpointUnits = 4096;
static constexpr std::size_t MaxDiagnostics = 1000000;

static llvm::json::Value locationJSON(const core::SourceLocation &location) {
  return llvm::json::Array{location.file, location.line, location.column};
}

static llvm::json::Value diagnosticJSON(const core::Diagnostic &diagnostic) {
  llvm::json::Array notes;
  for (const auto &note : diagnostic.notes)
    notes.push_back(diagnosticJSON(note));
  llvm::json::Array fixits;
  for (const auto &fixit : diagnostic.fixits)
    fixits.push_back(
        llvm::json::Array{locationJSON(fixit.location), fixit.insertion});
  return llvm::json::Object{
      {.K = "severity", .V = static_cast<unsigned>(diagnostic.severity)},
      {.K = "id", .V = std::string(diagnostic.id)},
      {.K = "message", .V = diagnostic.message},
      {.K = "location", .V = locationJSON(diagnostic.location)},
      {.K = "notes", .V = std::move(notes)},
      {.K = "fixits", .V = std::move(fixits)}};
}

static bool parseLocation(const llvm::json::Value &value,
                          core::SourceLocation &location) {
  const auto *array = value.getAsArray();
  if (!array || array->size() != 3)
    return false;
  const auto file = (*array)[0].getAsString();
  const auto line = (*array)[1].getAsUINT64();
  const auto column = (*array)[2].getAsUINT64();
  if (!file || !line || !column || *line > UINT32_MAX || *column > UINT32_MAX)
    return false;
  location = {.file = file->str(),
              .line = static_cast<std::uint32_t>(*line),
              .column = static_cast<std::uint32_t>(*column)};
  return true;
}

static bool parseDiagnostic(const llvm::json::Value &value,
                            core::Diagnostic &diagnostic, std::size_t &budget,
                            unsigned depth = 0) {
  if (!budget || depth > 16)
    return false;
  --budget;
  const auto *object = value.getAsObject();
  if (!object || object->size() != 6)
    return false;
  const auto severity = object->getInteger("severity");
  const auto id = object->getString("id");
  const auto message = object->getString("message");
  const auto *location = object->get("location");
  const auto *notes = object->getArray("notes");
  const auto *fixits = object->getArray("fixits");
  if (!severity || *severity < 0 ||
      *severity > static_cast<int>(core::Severity::Error) || !id || !message ||
      !location || !notes || !fixits || fixits->size() > 4096 ||
      !parseLocation(*location, diagnostic.location))
    return false;
  // Diagnostic ids are string_views: never retain a view into the JSON buffer.
  if (id->empty()) {
    diagnostic.id = {};
  } else {
    const auto *known = std::ranges::find(core::diag::All, id->str());
    if (known == core::diag::All.end())
      return false;
    diagnostic.id = *known;
  }
  diagnostic.severity = static_cast<core::Severity>(*severity);
  diagnostic.message = message->str();
  for (const auto &note : *notes) {
    diagnostic.notes.emplace_back();
    if (!parseDiagnostic(note, diagnostic.notes.back(), budget, depth + 1))
      return false;
  }
  for (const auto &entry : *fixits) {
    const auto *pair = entry.getAsArray();
    if (!pair || pair->size() != 2 || !(*pair)[1].getAsString())
      return false;
    core::FixItHint fixit;
    if (!parseLocation((*pair)[0], fixit.location))
      return false;
    fixit.insertion = (*pair)[1].getAsString()->str();
    diagnostic.fixits.push_back(std::move(fixit));
  }
  return true;
}

static std::string checkpointPath(std::string_view directory,
                                  std::string_view key) {
  llvm::SmallString<256> path(directory);
  llvm::sys::path::append(path, std::string(key) + ".wcache");
  return path.str().str();
}

static bool validKey(std::string_view key) {
  return key.size() == 64 && std::ranges::all_of(key, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::optional<AnalysisCheckpoint>
readAnalysisCheckpoint(std::string_view directory, std::string_view key,
                       core::AnalysisStats *stats) {
  core::AnalysisTimer timer(stats, "cache_read");
  if (!validKey(key))
    return std::nullopt;
  const auto path = checkpointPath(directory, key);
  std::uint64_t size = 0;
  if (llvm::sys::fs::file_size(path, size) || size > MaxCheckpointBytes)
    return std::nullopt;
  const auto buffer = llvm::MemoryBuffer::getFile(path, false, false);
  if (!buffer || (*buffer)->getBufferSize() > MaxCheckpointBytes)
    return std::nullopt;
  const auto text = (*buffer)->getBuffer();
  const auto [digest, encoded] = text.split('\n');
  if (!validKey(digest))
    return std::nullopt;
  llvm::StringRef payload = encoded;
  llvm::SmallVector<std::uint8_t, 0> decoded;
  if (payload.starts_with("zstd-v1 ")) {
    const auto [header, bytes] = payload.split('\n');
    std::uint64_t decodedSize = 0;
    if (header.drop_front(8).getAsInteger(10, decodedSize) || !decodedSize ||
        decodedSize > MaxDecodedCheckpointBytes ||
        decodedSize > std::numeric_limits<std::size_t>::max() ||
        !llvm::compression::zstd::isAvailable())
      return std::nullopt;
    if (auto error = llvm::compression::zstd::decompress(
            llvm::arrayRefFromStringRef(bytes), decoded,
            static_cast<std::size_t>(decodedSize))) {
      llvm::consumeError(std::move(error));
      return std::nullopt;
    }
    if (decoded.size() != decodedSize)
      return std::nullopt;
    payload = llvm::toStringRef(decoded);
  }
  if (digest != checkedDigest(payload)) {
    if (stats)
      stats->add("cache_invalid_records");
    return std::nullopt;
  }
  auto parsed = llvm::json::parse(payload);
  if (!parsed) {
    llvm::consumeError(parsed.takeError());
    return std::nullopt;
  }
  const auto *object = parsed->getAsObject();
  if (!object || object->size() != 10 || object->getInteger("version") != 2 ||
      object->getString("key") != llvm::StringRef(key))
    return std::nullopt;
  const auto *units = object->getArray("units");
  const auto imported = object->getString("imports");
  if (!units || units->empty() || units->size() > MaxCheckpointUnits ||
      !imported || !validKey(*imported))
    return std::nullopt;
  const auto ledgers = CheckpointExplanations::decode(*object);
  if (!ledgers)
    return std::nullopt;
  AnalysisCheckpoint result;
  result.importedIdentity = imported->str();
  std::size_t budget = MaxDiagnostics;
  for (const auto &value : *units) {
    const auto *unit = value.getAsObject();
    if (!unit || unit->size() != 5)
      return std::nullopt;
    const auto *references = unit->getArray("ledgers");
    const auto record = unit->getString("record");
    const auto seen = unit->getString("sized_pairs_seen");
    const auto *diagnostics = unit->getArray("diagnostics");
    const auto *dependencies = unit->getArray("dependencies");
    if (!record || !seen || !diagnostics || !dependencies || !references ||
        dependencies->size() > 100000)
      return std::nullopt;
    auto exports = parseUnitRecord(*record);
    // The sidecar parser tolerates some extensions. A cache uses only this
    // build's canonical records; lossy or noncanonical decoding is a miss.
    if (!exports || printUnitRecord(*exports) != *record)
      return std::nullopt;
    auto seenRecord = parseUnitRecord(*seen);
    if (!seenRecord || printUnitRecord(*seenRecord) != *seen)
      return std::nullopt;
    result.sizedPairsSeen.push_back(
        std::move(seenRecord->exports.sizedFields.witnesses));
    if (!CheckpointExplanations::restore(exports->exports, *references,
                                         *ledgers))
      return std::nullopt;
    UnitResult output;
    output.exports = std::move(exports->exports);
    for (const auto &dependency : *dependencies) {
      const auto name = dependency.getAsString();
      if (!name)
        return std::nullopt;
      output.dependencies.insert(name->str());
    }
    for (const auto &diagnostic : *diagnostics) {
      output.diagnostics.emplace_back();
      if (!parseDiagnostic(diagnostic, output.diagnostics.back(), budget))
        return std::nullopt;
    }
    result.units.push_back(std::move(output));
  }
  return result;
}

static bool preservesProducer(const UnitRecord &record, std::string_view text) {
  const auto decoded = parseUnitRecord(text);
  if (!decoded || printUnitRecord(*decoded) != text)
    return false;
  // The sidecar reader assigns global ids in first-use order. Compare in the
  // producer's namespace, retaining unused names without manufacturing facts.
  analysis::UnitExports names;
  names.globals = record.exports.globals;
  analysis::ProgramDatabase numbering;
  numbering.add(names);
  const auto restored = numbering.renumbered(decoded->exports);
  return restored.globals == record.exports.globals &&
         restored.functions == record.exports.functions &&
         restored.checkedDefinitions == record.exports.checkedDefinitions;
}

std::string checkpointExportsIdentity(const analysis::UnitExports &exports) {
  UnitRecord record;
  record.exports = exports;
  CheckpointExplanations explanations;
  auto references = explanations.extract(record.exports);
  const auto encoded = printUnitRecord(record);
  if (!preservesProducer(record, encoded))
    return {};
  std::string text;
  llvm::raw_string_ostream out(text);
  out << "{\"record\":" << core::safetyJsonString(encoded)
      << ",\"ledgers\":" << llvm::json::Value(std::move(references));
  explanations.writeTables(out);
  out << '}';
  return checkedDigest(text);
}

bool writeAnalysisCheckpoint(std::string_view directory, std::string_view key,
                             const AnalysisCheckpoint &checkpoint,
                             core::AnalysisStats *stats) {
  core::AnalysisTimer timer(stats, "cache_write");
  if (!validKey(key) || !validKey(checkpoint.importedIdentity) ||
      checkpoint.units.empty() || checkpoint.units.size() > MaxCheckpointUnits)
    return false;
  CheckpointExplanations explanations;
  llvm::json::Array units;
  unsigned index = 0;
  for (const auto &unit : checkpoint.units) {
    llvm::json::Array diagnostics;
    for (const auto &diagnostic : unit.diagnostics)
      diagnostics.push_back(diagnosticJSON(diagnostic));
    llvm::json::Array dependencies;
    for (const auto &dependency : unit.dependencies)
      dependencies.push_back(dependency);
    UnitRecord record;
    record.exports = unit.exports;
    auto references = explanations.extract(record.exports);
    auto text = printUnitRecord(record);
    // Expanded checked transport can fall back to a limited contract. The
    // stripped record must preserve its producer, not merely parse itself.
    if (!preservesProducer(record, text)) {
      if (stats)
        stats->add("cache_write_failures");
      return false;
    }
    UnitRecord seen;
    if (index < checkpoint.sizedPairsSeen.size())
      seen.exports.sizedFields.witnesses = checkpoint.sizedPairsSeen[index];
    ++index;
    units.push_back(llvm::json::Object{
        {.K = "record", .V = std::move(text)},
        {.K = "ledgers", .V = std::move(references)},
        {.K = "sized_pairs_seen", .V = printUnitRecord(seen)},
        {.K = "diagnostics", .V = std::move(diagnostics)},
        {.K = "dependencies", .V = std::move(dependencies)}});
  }
  std::string payload;
  llvm::raw_string_ostream out(payload);
  out << R"({"version":2,"key":)" << core::safetyJsonString(key)
      << ",\"imports\":" << core::safetyJsonString(checkpoint.importedIdentity)
      << ",\"units\":" << llvm::json::Value(std::move(units));
  explanations.writeTables(out);
  out << '}';
  if (stats)
    stats->add("cache_uncompressed_bytes", payload.size());
  std::string encoded;
  if (payload.size() <= MaxDecodedCheckpointBytes &&
      payload.size() >= CompressAboveBytes &&
      llvm::compression::zstd::isAvailable()) {
    llvm::SmallVector<std::uint8_t, 0> compressed;
    llvm::compression::zstd::compress(
        llvm::arrayRefFromStringRef(payload), compressed,
        llvm::compression::zstd::BestSpeedCompression);
    encoded = "zstd-v1 " + std::to_string(payload.size()) + '\n';
    const auto bytes = llvm::toStringRef(compressed);
    encoded.append(bytes.data(), bytes.size());
  } else if (payload.size() + 65 <= MaxCheckpointBytes) {
    encoded = payload;
  }
  if (encoded.empty() || encoded.size() + 65 > MaxCheckpointBytes ||
      llvm::sys::fs::create_directories(directory) ||
      !writeAtomicText(checkpointPath(directory, key),
                       checkedDigest(payload) + '\n' + encoded)) {
    if (stats)
      stats->add("cache_write_failures");
    return false;
  }
  if (stats) {
    stats->add("cache_writes");
    stats->add("cache_bytes_written", encoded.size() + 65);
  }
  return true;
}

} // namespace weavec::frontend
