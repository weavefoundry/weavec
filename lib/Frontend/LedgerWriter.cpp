//===- LedgerWriter.cpp - Ledger JSON, SARIF and fingerprints -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/LedgerWriter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace weavec::frontend {

using core::Facet;
using core::FacetRecord;
using core::FunctionLedger;
using core::Ledger;
using core::LedgerDiagnostic;
using core::LedgerSummary;
using core::SiteOutcome;
using core::UnitLedger;

std::string_view toString(LedgerFormat format) noexcept {
  switch (format) {
  case LedgerFormat::Json:
    return "json";
  case LedgerFormat::Sarif:
    return "sarif";
  }
  return "<invalid>";
}

std::optional<LedgerFormat> parseLedgerFormat(std::string_view text) {
  if (text == "json")
    return LedgerFormat::Json;
  if (text == "sarif")
    return LedgerFormat::Sarif;
  return std::nullopt;
}

/// JSON needs valid UTF-8; source text and paths need not be.
static std::string utf8(llvm::StringRef text) {
  return llvm::json::isUTF8(text) ? text.str() : llvm::json::fixUTF8(text);
}

/// An enumeration's spelling as a JSON string.
static llvm::json::Value spelled(std::string_view spelling) {
  return std::string(spelling);
}

static llvm::json::Value stringOrNull(llvm::StringRef text) {
  if (text.empty())
    return nullptr;
  return utf8(text);
}

static bool isAsciiSpace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

//===----------------------------------------------------------------------===//
// Fingerprints (§12.3)
//===----------------------------------------------------------------------===//

std::string normalizeFingerprintMessage(llvm::StringRef message) {
  std::string normal;
  normal.reserve(message.size());
  bool inDigits = false;
  bool inSpace = false;
  for (const char c : message) {
    if (llvm::isDigit(c)) {
      if (!inDigits)
        normal += '0';
      inDigits = true;
      inSpace = false;
      continue;
    }
    inDigits = false;
    if (isAsciiSpace(c)) {
      if (!inSpace)
        normal += ' ';
      inSpace = true;
      continue;
    }
    inSpace = false;
    normal += c;
  }
  const std::size_t first = normal.find_first_not_of(' ');
  if (first == std::string::npos)
    return {};
  const std::size_t last = normal.find_last_not_of(' ');
  return normal.substr(first, last - first + 1);
}

std::string computeFingerprint(llvm::StringRef key, llvm::StringRef path,
                               llvm::StringRef function,
                               llvm::StringRef message, std::size_t ordinal) {
  static constexpr llvm::StringLiteral Separator = "\x1f";
  llvm::SHA256 hash;
  hash.update("weavec-fp/1");
  for (const llvm::StringRef part : {key, path, function, message}) {
    hash.update(Separator);
    hash.update(part);
  }
  hash.update(Separator);
  hash.update(std::to_string(ordinal));
  return llvm::toHex(hash.final(), /*LowerCase=*/true).substr(0, 32);
}

//===----------------------------------------------------------------------===//
// Paths
//===----------------------------------------------------------------------===//

static std::string currentDirectory() {
  llvm::SmallString<256> cwd;
  if (llvm::sys::fs::current_path(cwd))
    return {};
  return cwd.str().str();
}

/// `path` made absolute against `workingDirectory` (the process's when
/// empty), without `.` and `..` components, with `/` separators.
static std::string absolutePath(llvm::StringRef path,
                                llvm::StringRef workingDirectory) {
  llvm::SmallString<256> result;
  if (llvm::sys::path::is_absolute(path)) {
    result = path;
  } else {
    result = workingDirectory.empty() ? llvm::StringRef(currentDirectory())
                                      : workingDirectory;
    llvm::sys::path::append(result, path);
  }
  llvm::sys::path::remove_dots(result, /*remove_dot_dot=*/true);
  return llvm::sys::path::convert_to_slash(result);
}

std::string detectFingerprintRoot(llvm::StringRef sourceFile,
                                  llvm::StringRef workingDirectory) {
  const std::string cwd =
      workingDirectory.empty() ? currentDirectory() : workingDirectory.str();
  std::string fallback = absolutePath(cwd, cwd);
  if (sourceFile.empty())
    return fallback;
  const std::string source = absolutePath(sourceFile, cwd);
  llvm::StringRef directory =
      llvm::sys::path::parent_path(source, llvm::sys::path::Style::posix);
  while (!directory.empty()) {
    llvm::SmallString<256> marker(directory);
    llvm::sys::path::append(marker, ".git");
    if (llvm::sys::fs::exists(marker))
      return directory.str();
    const llvm::StringRef parent =
        llvm::sys::path::parent_path(directory, llvm::sys::path::Style::posix);
    if (parent == directory)
      break;
    directory = parent;
  }
  return fallback;
}

std::string pathRelativeToRoot(llvm::StringRef path, llvm::StringRef root,
                               llvm::StringRef workingDirectory) {
  if (path.empty())
    return {};
  std::string absolute = absolutePath(path, workingDirectory);
  const std::string base = absolutePath(root, workingDirectory);
  llvm::StringRef prefix = base;
  if (prefix.size() > 1 && prefix.ends_with("/"))
    prefix = prefix.drop_back();
  const llvm::StringRef full = absolute;
  if (full == prefix)
    return ".";
  if (prefix == "/")
    return full.drop_front().str();
  if (full.starts_with(prefix) && full.size() > prefix.size() &&
      full[prefix.size()] == '/')
    return full.drop_front(prefix.size() + 1).str();
  return absolute;
}

namespace {
/// Relativises every path of one ledger.
class PathMapper {
public:
  PathMapper(const Ledger &ledger, const LedgerWriteOptions &options)
      : cwd(options.workingDirectory.empty() ? currentDirectory()
                                             : options.workingDirectory),
        root(ledger.root.empty() ? absolutePath(cwd, cwd) : ledger.root) {}

  [[nodiscard]] std::string operator()(llvm::StringRef path) const {
    return utf8(pathRelativeToRoot(path, root, cwd));
  }
  [[nodiscard]] const std::string &rootPath() const { return root; }

private:
  std::string cwd;
  std::string root;
};

/// Every fingerprint of one ledger. They are computed together, because a
/// row's ordinal counts the rows of its function with the same key and
/// message.
struct Fingerprints {
  /// Indexed by unit, function, site and facet.
  std::vector<
      std::vector<std::vector<std::array<std::string, core::FacetCount>>>>
      facets;
  std::vector<std::string> diagnostics;
};

/// One row waiting for its ordinal.
struct PendingRow {
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  /// Document order, to break ties.
  std::size_t sequence = 0;
  std::string *out = nullptr;
  /// A fingerprint the ledger already carries, kept as it is.
  bool preset = false;
};
} // namespace

/// §12.1: the file that holds a function's definition; the unit's source
/// unless the producer recorded another.
static const std::string &functionFile(const UnitLedger &unit,
                                       const FunctionLedger &function) {
  return function.file.empty() ? unit.source : function.file;
}

/// The file of a site: its own location's, else its function's (§12.1:
/// sites take their function's file).
static const std::string &siteFile(const UnitLedger &unit,
                                   const FunctionLedger &function,
                                   const core::Site &site) {
  return site.location.file.empty() ? functionFile(unit, function)
                                    : site.location.file;
}

static Fingerprints computeFingerprints(const Ledger &ledger,
                                        const PathMapper &paths) {
  // Rows group by (path, function, key, normalised message).
  using GroupKey =
      std::tuple<std::string, std::string, std::string, std::string>;
  std::map<GroupKey, std::vector<PendingRow>> groups;
  std::size_t sequence = 0;
  Fingerprints result;
  result.facets.resize(ledger.units.size());
  for (std::size_t u = 0; u < ledger.units.size(); ++u) {
    const UnitLedger &unit = ledger.units[u];
    auto &unitRows = result.facets[u];
    unitRows.resize(unit.functions.size());
    for (std::size_t f = 0; f < unit.functions.size(); ++f) {
      const FunctionLedger &function = unit.functions[f];
      unitRows[f].resize(function.sites.size());
      for (std::size_t s = 0; s < function.sites.size(); ++s) {
        const core::Site &site = function.sites[s];
        const std::string path = paths(siteFile(unit, function, site));
        const std::string message =
            normalizeFingerprintMessage(core::siteText(site.text));
        for (const Facet facet : core::AllFacets) {
          const FacetRecord *record = site.facet(facet);
          if (record == nullptr)
            continue;
          std::string &out = unitRows[f][s].at(static_cast<std::size_t>(facet));
          out = record->fingerprint;
          groups[{path, function.name,
                  core::facetRowKey(site.kind, facet, record->decision),
                  message}]
              .push_back(PendingRow{.line = site.location.line,
                                    .column = site.location.column,
                                    .sequence = sequence++,
                                    .out = &out,
                                    .preset = !out.empty()});
        }
      }
    }
  }
  result.diagnostics.resize(ledger.diagnostics.size());
  for (std::size_t d = 0; d < ledger.diagnostics.size(); ++d) {
    const LedgerDiagnostic &diagnostic = ledger.diagnostics[d];
    std::string &out = result.diagnostics[d];
    out = diagnostic.fingerprint;
    groups[{paths(diagnostic.location.file),
            diagnostic.function.empty() ? FileScopeFunction.str()
                                        : diagnostic.function,
            diagnostic.id, normalizeFingerprintMessage(diagnostic.message)}]
        .push_back(PendingRow{.line = diagnostic.location.line,
                              .column = diagnostic.location.column,
                              .sequence = sequence++,
                              .out = &out,
                              .preset = !out.empty()});
  }
  for (auto &[key, rows] : groups) {
    std::ranges::sort(rows, [](const PendingRow &a, const PendingRow &b) {
      return std::tie(a.line, a.column, a.sequence) <
             std::tie(b.line, b.column, b.sequence);
    });
    const auto &[path, function, rowKey, message] = key;
    for (std::size_t ordinal = 0; ordinal < rows.size(); ++ordinal) {
      if (!rows[ordinal].preset)
        *rows[ordinal].out =
            computeFingerprint(rowKey, path, function, message, ordinal);
    }
  }
  return result;
}

void assignFingerprints(Ledger &ledger, const LedgerWriteOptions &options) {
  const Fingerprints fingerprints =
      computeFingerprints(ledger, PathMapper(ledger, options));
  for (std::size_t u = 0; u < ledger.units.size(); ++u) {
    UnitLedger &unit = ledger.units[u];
    for (std::size_t f = 0; f < unit.functions.size(); ++f) {
      FunctionLedger &function = unit.functions[f];
      for (std::size_t s = 0; s < function.sites.size(); ++s) {
        for (const Facet facet : core::AllFacets) {
          if (FacetRecord *record = function.sites[s].facet(facet))
            record->fingerprint = fingerprints.facets[u][f][s].at(
                static_cast<std::size_t>(facet));
        }
      }
    }
  }
  for (std::size_t d = 0; d < ledger.diagnostics.size(); ++d)
    ledger.diagnostics[d].fingerprint = fingerprints.diagnostics[d];
}

//===----------------------------------------------------------------------===//
// JSON (§12.1)
//===----------------------------------------------------------------------===//

/// `0.0710000` -> `0.071`: six decimals, trailing zeros dropped, so the
/// output is deterministic and readable.
static std::string formatShare(double share) {
  std::string text = llvm::formatv("{0:F6}", share).str();
  while (!text.empty() && text.back() == '0')
    text.pop_back();
  if (!text.empty() && text.back() == '.')
    text.pop_back();
  return text.empty() ? "0" : text;
}

static void writeOutcomeCounts(llvm::json::OStream &json,
                               const core::OutcomeCounts &counts) {
  json.attribute("proven", counts.proven);
  json.attribute("checked", counts.checked);
  json.attribute("violation", counts.violation);
  json.attribute("unresolved", counts.unresolved);
  json.attribute("trusted", counts.trusted);
}

static void writeSummary(llvm::json::OStream &json,
                         const LedgerSummary &summary, bool verify,
                         const core::A5Counts *a5) {
  json.object([&] {
    json.attribute("sites", summary.sites);
    writeOutcomeCounts(json, summary.outcomes);
    json.attributeObject("facets", [&] {
      for (const Facet facet : core::AllFacets)
        json.attributeObject(core::toString(facet), [&] {
          writeOutcomeCounts(
              json, summary.facets.at(static_cast<std::size_t>(facet)));
        });
    });
    json.attributeObject("unresolvedReasons", [&] {
      for (const core::UnresolvedReason reason : core::allUnresolvedReasons())
        json.attribute(
            core::toString(reason),
            summary.unresolvedReasons.at(static_cast<std::size_t>(reason)));
    });
    json.attributeObject("trustedReasons", [&] {
      for (const core::TrustReason reason : core::allTrustReasons())
        json.attribute(
            core::toString(reason),
            summary.trustedReasons.at(static_cast<std::size_t>(reason)));
    });
    json.attributeObject("unresolvedShare", [&] {
      json.attributeBegin("spatialNull");
      json.rawValue(formatShare(summary.spatialNullShare()));
      json.attributeEnd();
    });
    json.attribute("errors", summary.errors);
    json.attribute("warnings", summary.warnings);
    json.attribute("functions", summary.functions);
    json.attributeArray("overBudget", [&] {
      for (const std::string &name : summary.overBudget)
        json.value(utf8(name));
    });
    if (verify)
      json.attribute("verifyChecks", summary.verifyChecks);
    if (a5 != nullptr)
      json.attributeObject("a5", [&] {
        json.attribute("nonLoweredAllocations", a5->nonLoweredAllocations);
        json.attribute("bypassedDeclarations", a5->bypassedDeclarations);
      });
  });
}

static void writeCheck(llvm::json::OStream &json,
                       const core::FacetCheck &check) {
  json.object([&] {
    json.attribute("template", spelled(core::toString(check.kind)));
    if (check.proven)
      json.attribute("proven", true);
  });
}

static void writeFixit(llvm::json::OStream &json, const core::FixItHint &fixit,
                       const PathMapper &paths) {
  json.object([&] {
    json.attribute("file", paths(fixit.location.file));
    json.attribute("line", fixit.location.line);
    json.attribute("column", fixit.location.column);
    json.attribute("insertion", utf8(fixit.insertion));
  });
}

static void writeRequirement(llvm::json::OStream &json,
                             const core::Requirement &requirement) {
  json.object([&] {
    if (requirement.argument)
      json.attribute("arg", *requirement.argument);
    else
      json.attribute("arg", nullptr);
    json.attribute("need", requirement.need ? stringOrNull(*requirement.need)
                                            : llvm::json::Value(nullptr));
    json.attribute("have", requirement.have ? stringOrNull(*requirement.have)
                                            : llvm::json::Value(nullptr));
    json.attribute("outcome",
                   spelled(core::toString(requirement.decision.outcome)));
    if (const std::string_view reason = requirement.decision.reasonText();
        !reason.empty())
      json.attribute("reason", spelled(reason));
    if (requirement.check) {
      json.attributeBegin("check");
      writeCheck(json, *requirement.check);
      json.attributeEnd();
    }
  });
}

/// Violations, unresolved and trusted facets carry every key; proven and
/// checked ones only those with content (§12.1's example).
static bool carriesEveryKey(SiteOutcome outcome) noexcept {
  return outcome == SiteOutcome::Violation ||
         outcome == SiteOutcome::Unresolved || outcome == SiteOutcome::Trusted;
}

static void writeFacet(llvm::json::OStream &json, const FacetRecord &record,
                       const std::string &fingerprint,
                       const PathMapper &paths) {
  json.object([&] {
    json.attribute("outcome", spelled(core::toString(record.outcome())));
    const bool every = carriesEveryKey(record.outcome());
    if (every)
      json.attribute("reason", stringOrNull(record.decision.reasonText()));
    if (every || !record.decision.detail.empty())
      json.attribute("detail", stringOrNull(record.decision.detail));
    if (record.fixit) {
      json.attributeBegin("fixit");
      writeFixit(json, *record.fixit, paths);
      json.attributeEnd();
    } else if (every) {
      json.attribute("fixit", nullptr);
    }
    if (every || !record.requirements.empty())
      json.attributeArray("requirements", [&] {
        for (const core::Requirement &requirement : record.requirements)
          writeRequirement(json, requirement);
      });
    if (record.check) {
      json.attributeBegin("check");
      writeCheck(json, *record.check);
      json.attributeEnd();
    }
    if (record.diagnostic)
      json.attribute("diagnostic", *record.diagnostic);
    else if (every)
      json.attribute("diagnostic", nullptr);
    json.attribute("fingerprint", fingerprint);
  });
}

/// The facets of a site object, in the order of §12.1's example.
static constexpr std::array<Facet, core::FacetCount> SiteFacetOrder{
    Facet::Null, Facet::Spatial, Facet::Temporal, Facet::Assertion};

static void writeSite(llvm::json::OStream &json, const core::Site &site,
                      const std::array<std::string, core::FacetCount> &prints,
                      const PathMapper &paths) {
  json.object([&] {
    json.attribute("ordinal", site.ordinal);
    json.attribute("kind", spelled(core::toString(site.kind)));
    json.attribute("line", site.location.line);
    json.attribute("column", site.location.column);
    // §12.1: whitespace removed, at most 80 bytes, whatever the producer
    // stored.
    json.attribute("text", utf8(core::siteText(site.text)));
    if (site.boundary)
      json.attribute("boundary", spelled(core::toString(*site.boundary)));
    else
      json.attribute("boundary", nullptr);
    json.attribute("callee", stringOrNull(site.callee));
    json.attributeObject("facets", [&] {
      for (const Facet facet : SiteFacetOrder) {
        const FacetRecord *record = site.facet(facet);
        if (record == nullptr)
          continue;
        json.attributeBegin(core::toString(facet));
        writeFacet(json, *record, prints.at(static_cast<std::size_t>(facet)),
                   paths);
        json.attributeEnd();
      }
    });
  });
}

static void writeDiagnostic(llvm::json::OStream &json,
                            const LedgerDiagnostic &diagnostic,
                            const std::string &fingerprint,
                            const PathMapper &paths) {
  json.object([&] {
    json.attribute("id", utf8(diagnostic.id));
    json.attribute("severity", spelled(core::toString(diagnostic.severity)));
    json.attribute("certainty", spelled(core::toString(diagnostic.certainty)));
    json.attribute("message", utf8(diagnostic.message));
    json.attribute("file", paths(diagnostic.location.file));
    json.attribute("line", diagnostic.location.line);
    json.attribute("column", diagnostic.location.column);
    json.attribute("function", stringOrNull(diagnostic.function));
    if (diagnostic.site)
      json.attribute("site", *diagnostic.site);
    else
      json.attribute("site", nullptr);
    if (diagnostic.facet)
      json.attribute("facet", spelled(core::toString(*diagnostic.facet)));
    else
      json.attribute("facet", nullptr);
    json.attributeArray("notes", [&] {
      for (const core::LedgerNote &note : diagnostic.notes)
        json.object([&] {
          json.attribute("message", utf8(note.message));
          json.attribute("file", paths(note.location.file));
          json.attribute("line", note.location.line);
          json.attribute("column", note.location.column);
        });
    });
    json.attribute("fingerprint", fingerprint);
  });
}

static void writeAssumptions(llvm::json::OStream &json,
                             const core::Assumptions &assumptions) {
  json.object([&] {
    json.attributeObject("A1", [&] {
      json.attribute("exportedRequirements",
                     assumptions.a1.exportedRequirements);
      json.attribute("verified", assumptions.a1.verified);
      json.attribute("reliesOnSingle", assumptions.a1.reliesOnSingle);
      json.attribute("unverifiedCallers", assumptions.a1.unverifiedCallers);
    });
    json.attributeObject("A3", [&] {
      json.attribute("headerInvariants", assumptions.a3.headerInvariants);
      json.attribute("unverified", assumptions.a3.unverified);
      json.attributeArray("inputsWithoutRecords", [&] {
        for (const std::string &input : assumptions.a3.inputsWithoutRecords)
          json.value(utf8(input));
      });
    });
    json.attributeObject("A4", [&] {
      json.attribute("concurrencySites", assumptions.a4.concurrencySites);
    });
    json.attributeObject("A5", [&] {
      json.attribute("nonLoweredAllocations",
                     assumptions.a5.nonLoweredAllocations);
      json.attribute("bypassedDeclarations",
                     assumptions.a5.bypassedDeclarations);
      if (assumptions.a5.allocatorDefinedBy)
        json.attribute("allocatorDefinedBy",
                       utf8(*assumptions.a5.allocatorDefinedBy));
      else
        json.attribute("allocatorDefinedBy", nullptr);
    });
  });
}

std::string renderLedgerJson(const Ledger &ledger,
                             const LedgerWriteOptions &options) {
  const PathMapper paths(ledger, options);
  const Fingerprints fingerprints = computeFingerprints(ledger, paths);
  const bool verify = ledger.config.checks == core::ChecksMode::Verify;
  std::string text;
  llvm::raw_string_ostream os(text);
  {
    llvm::json::OStream json(os, options.indent);
    json.object([&] {
      json.attribute("schema", "weavec-ledger");
      json.attribute("version", 1);
      json.attributeObject("producer", [&] {
        json.attribute("name", utf8(ledger.producer.name));
        json.attribute("version", utf8(ledger.producer.version));
        json.attribute("revision", utf8(ledger.producer.revision));
      });
      json.attribute("scope", spelled(core::toString(ledger.scope)));
      json.attribute("root", utf8(paths.rootPath()));
      json.attributeObject("config", [&] {
        json.attribute("checks", spelled(core::toString(ledger.config.checks)));
        json.attribute("zeroInit", ledger.config.zeroInit);
        json.attribute("require",
                       spelled(core::toString(ledger.config.require)));
        json.attribute("budget", ledger.config.budget);
      });
      json.attributeBegin("summary");
      writeSummary(json, core::summarize(ledger), verify, nullptr);
      json.attributeEnd();
      if (ledger.scope == core::LedgerScope::Program) {
        json.attributeBegin("assumptions");
        writeAssumptions(json, ledger.assumptions ? *ledger.assumptions
                                                  : core::Assumptions{});
        json.attributeEnd();
      }
      json.attributeArray("units", [&] {
        for (std::size_t u = 0; u < ledger.units.size(); ++u) {
          const UnitLedger &unit = ledger.units[u];
          json.object([&] {
            json.attribute("source", paths(unit.source));
            json.attribute("object", stringOrNull(paths(unit.object)));
            json.attribute("target", utf8(unit.target));
            json.attributeBegin("summary");
            writeSummary(json, core::summarizeUnit(ledger, u), verify,
                         &unit.a5);
            json.attributeEnd();
            json.attributeArray("functions", [&] {
              for (std::size_t f = 0; f < unit.functions.size(); ++f) {
                const FunctionLedger &function = unit.functions[f];
                json.object([&] {
                  json.attribute("name", utf8(function.name));
                  json.attribute("file", paths(functionFile(unit, function)));
                  json.attribute("line", function.line);
                  json.attribute("linkage",
                                 spelled(core::toString(function.linkage)));
                  json.attribute("overBudget", function.overBudget);
                  json.attribute("requireSafe", function.requireSafe);
                  json.attribute("setjmp", function.callsSetjmp);
                  json.attributeArray("sites", [&] {
                    for (std::size_t s = 0; s < function.sites.size(); ++s)
                      writeSite(json, function.sites[s],
                                fingerprints.facets[u][f][s], paths);
                  });
                });
              }
            });
          });
        }
      });
      json.attributeArray("diagnostics", [&] {
        for (std::size_t d = 0; d < ledger.diagnostics.size(); ++d)
          writeDiagnostic(json, ledger.diagnostics[d],
                          fingerprints.diagnostics[d], paths);
      });
    });
  }
  text += '\n';
  return text;
}

//===----------------------------------------------------------------------===//
// SARIF (§12.2)
//===----------------------------------------------------------------------===//

static constexpr llvm::StringLiteral InformationUri =
    "https://github.com/weavefoundry/weavec";

/// Percent-encodes everything but unreserved characters and `/`.
static std::string percentEncode(llvm::StringRef path) {
  std::string encoded;
  encoded.reserve(path.size());
  for (const char c : path) {
    if (llvm::isAlnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
        c == '/') {
      encoded += c;
      continue;
    }
    encoded += '%';
    encoded += llvm::hexdigit(static_cast<unsigned char>(c) >> 4U);
    encoded += llvm::hexdigit(static_cast<unsigned char>(c) & 0xFU);
  }
  return encoded;
}

/// `file:///abs/path`, from an absolute path with `/` separators.
static std::string fileUri(llvm::StringRef absolute) {
  std::string uri = "file://";
  if (!absolute.starts_with("/"))
    uri += '/';
  uri += percentEncode(absolute);
  return uri;
}

/// The `physicalLocation` of a SARIF `location`, for a root-relative (or
/// absolute) path; nothing for an empty path.
static void writeSarifLocation(llvm::json::OStream &json, llvm::StringRef path,
                               std::uint32_t line, std::uint32_t column) {
  if (path.empty())
    return;
  json.attributeObject("physicalLocation", [&] {
    json.attributeObject("artifactLocation", [&] {
      if (llvm::sys::path::is_absolute(path, llvm::sys::path::Style::posix)) {
        json.attribute("uri", fileUri(path));
      } else {
        json.attribute("uri", percentEncode(path));
        json.attribute("uriBaseId", "SRCROOT");
      }
    });
    if (line != 0)
      json.attributeObject("region", [&] {
        json.attribute("startLine", line);
        if (column != 0)
          json.attribute("startColumn", column);
      });
  });
}

namespace {
/// `runs[0].tool.driver.rules`: the diagnostic ids the ledger uses, then
/// one rule per unresolved and per trusted reason.
class SarifRules {
public:
  explicit SarifRules(const Ledger &ledger) {
    std::vector<std::string> ids;
    ids.reserve(ledger.diagnostics.size());
    for (const LedgerDiagnostic &diagnostic : ledger.diagnostics)
      ids.push_back(diagnostic.id);
    std::ranges::sort(ids);
    const auto [first, last] = std::ranges::unique(ids);
    ids.erase(first, last);
    for (std::string &id : ids)
      add(std::move(id));
    for (const core::UnresolvedReason reason : core::allUnresolvedReasons())
      add("unresolved/" + std::string(core::toString(reason)));
    for (const core::TrustReason reason : core::allTrustReasons())
      add("trusted/" + std::string(core::toString(reason)));
  }

  [[nodiscard]] const std::vector<std::string> &ids() const { return order; }
  [[nodiscard]] std::size_t indexOf(const std::string &id) const {
    return index.at(id);
  }

private:
  void add(std::string id) {
    index.emplace(id, order.size());
    order.push_back(std::move(id));
  }

  std::vector<std::string> order;
  std::map<std::string, std::size_t> index;
};
} // namespace

/// `<facet> of <kind> '<text>' is <unresolved|trusted>: <detail>`, with the
/// reason when there is no detail.
static std::string facetMessage(const core::Site &site, Facet facet,
                                const FacetRecord &record) {
  std::string message(core::toString(facet));
  message += " of ";
  message += core::toString(site.kind);
  message += " '";
  message += core::siteText(site.text);
  message += "' is ";
  message += core::toString(record.outcome());
  message += ": ";
  message += record.decision.detail.empty()
                 ? std::string(record.decision.reasonText())
                 : record.decision.detail;
  return utf8(message);
}

std::string renderLedgerSarif(const Ledger &ledger,
                              const LedgerWriteOptions &options) {
  const PathMapper paths(ledger, options);
  const Fingerprints fingerprints = computeFingerprints(ledger, paths);
  const SarifRules rules(ledger);
  const bool verify = ledger.config.checks == core::ChecksMode::Verify;
  std::string text;
  llvm::raw_string_ostream os(text);
  {
    llvm::json::OStream json(os, options.indent);
    json.object([&] {
      json.attribute("$schema",
                     "https://json.schemastore.org/sarif-2.1.0.json");
      json.attribute("version", "2.1.0");
      json.attributeArray("runs", [&] {
        json.object([&] {
          json.attributeObject("tool", [&] {
            json.attributeObject("driver", [&] {
              json.attribute("name", utf8(ledger.producer.name));
              if (!ledger.producer.version.empty())
                json.attribute("semanticVersion",
                               utf8(ledger.producer.version));
              json.attribute("informationUri", InformationUri);
              json.attributeArray("rules", [&] {
                for (const std::string &id : rules.ids())
                  json.object([&] { json.attribute("id", utf8(id)); });
              });
            });
          });
          json.attributeArray("invocations", [&] {
            json.object([&] {
              json.attribute("executionSuccessful",
                             options.executionSuccessful);
            });
          });
          json.attributeObject("originalUriBaseIds", [&] {
            json.attributeObject("SRCROOT", [&] {
              std::string root = fileUri(paths.rootPath());
              if (!llvm::StringRef(root).ends_with("/"))
                root += '/';
              json.attribute("uri", root);
            });
          });
          json.attributeArray("results", [&] {
            for (std::size_t d = 0; d < ledger.diagnostics.size(); ++d) {
              const LedgerDiagnostic &diagnostic = ledger.diagnostics[d];
              json.object([&] {
                json.attribute("ruleId", utf8(diagnostic.id));
                json.attribute("ruleIndex", rules.indexOf(diagnostic.id));
                json.attribute("level",
                               diagnostic.severity == core::Severity::Error
                                   ? "error"
                                   : "warning");
                json.attribute("kind", "fail");
                json.attributeObject("message", [&] {
                  json.attribute("text", utf8(diagnostic.message));
                });
                json.attributeArray("locations", [&] {
                  const std::string file = paths(diagnostic.location.file);
                  if (!file.empty())
                    json.object([&] {
                      writeSarifLocation(json, file, diagnostic.location.line,
                                         diagnostic.location.column);
                    });
                });
                if (!diagnostic.notes.empty())
                  json.attributeArray("relatedLocations", [&] {
                    for (std::size_t n = 0; n < diagnostic.notes.size(); ++n) {
                      const core::LedgerNote &note = diagnostic.notes[n];
                      json.object([&] {
                        json.attribute("id", n);
                        json.attributeObject("message", [&] {
                          json.attribute("text", utf8(note.message));
                        });
                        writeSarifLocation(json, paths(note.location.file),
                                           note.location.line,
                                           note.location.column);
                      });
                    }
                  });
                json.attributeObject("partialFingerprints", [&] {
                  json.attribute("weavec/v1", fingerprints.diagnostics[d]);
                });
                json.attributeObject("properties", [&] {
                  json.attribute("certainty",
                                 spelled(core::toString(diagnostic.certainty)));
                });
              });
            }
            for (std::size_t u = 0; u < ledger.units.size(); ++u) {
              const UnitLedger &unit = ledger.units[u];
              for (std::size_t f = 0; f < unit.functions.size(); ++f) {
                const FunctionLedger &function = unit.functions[f];
                for (std::size_t s = 0; s < function.sites.size(); ++s) {
                  const core::Site &site = function.sites[s];
                  for (const Facet facet : core::AllFacets) {
                    const FacetRecord *record = site.facet(facet);
                    if (record == nullptr ||
                        (record->outcome() != SiteOutcome::Unresolved &&
                         record->outcome() != SiteOutcome::Trusted) ||
                        record->decision.reasonText().empty())
                      continue;
                    const bool unresolved =
                        record->outcome() == SiteOutcome::Unresolved;
                    const std::string ruleId =
                        std::string(unresolved ? "unresolved/" : "trusted/") +
                        std::string(record->decision.reasonText());
                    json.object([&] {
                      json.attribute("ruleId", ruleId);
                      json.attribute("ruleIndex", rules.indexOf(ruleId));
                      json.attribute("level", unresolved ? "note" : "none");
                      json.attribute("kind", unresolved ? "open" : "review");
                      json.attributeObject("message", [&] {
                        json.attribute("text",
                                       facetMessage(site, facet, *record));
                      });
                      json.attributeArray("locations", [&] {
                        json.object([&] {
                          writeSarifLocation(
                              json, paths(siteFile(unit, function, site)),
                              site.location.line, site.location.column);
                        });
                      });
                      json.attributeObject("partialFingerprints", [&] {
                        json.attribute("weavec/v1",
                                       fingerprints.facets[u][f][s].at(
                                           static_cast<std::size_t>(facet)));
                      });
                    });
                  }
                }
              }
            }
          });
          json.attributeObject("properties", [&] {
            json.attributeObject("weavec", [&] {
              json.attributeBegin("summary");
              writeSummary(json, core::summarize(ledger), verify, nullptr);
              json.attributeEnd();
            });
          });
        });
      });
    });
  }
  text += '\n';
  return text;
}

std::string renderLedger(const Ledger &ledger, LedgerFormat format,
                         const LedgerWriteOptions &options) {
  return format == LedgerFormat::Sarif ? renderLedgerSarif(ledger, options)
                                       : renderLedgerJson(ledger, options);
}

//===----------------------------------------------------------------------===//
// Files (§16)
//===----------------------------------------------------------------------===//

bool isLedgerDirectory(llvm::StringRef value) {
  if (value.empty())
    return false;
  return llvm::sys::path::is_separator(value.back()) ||
         llvm::sys::fs::is_directory(value);
}

std::string ledgerPathFor(llvm::StringRef value, llvm::StringRef artifact) {
  if (!isLedgerDirectory(value))
    return value.str();
  llvm::SmallString<256> path(value);
  llvm::sys::path::append(path,
                          llvm::sys::path::filename(artifact) + ".ledger.json");
  return path.str().str();
}

bool writeFileAtomically(llvm::StringRef path, llvm::StringRef contents,
                         std::string *error) {
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
      *error = message;
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

bool writeLedger(llvm::StringRef path, const Ledger &ledger,
                 LedgerFormat format, const LedgerWriteOptions &options,
                 std::string *error) {
  return writeFileAtomically(path, renderLedger(ledger, format, options),
                             error);
}

} // namespace weavec::frontend
