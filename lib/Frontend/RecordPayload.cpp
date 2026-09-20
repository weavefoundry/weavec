//===- RecordPayload.cpp - The unit record's payload (RFC 0030) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/RecordPayload.h"

#include "weavec/Core/SummaryIO.h"
#include "weavec/Frontend/UnitRecord.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <utility>

namespace weavec::frontend::record {

//===----------------------------------------------------------------------===//
// Kinds, declarations and site rows
//===----------------------------------------------------------------------===//

std::string spellKind(const core::PointerKind &kind) {
  return std::string(core::toString(kind.source)) + " " + kind.toString();
}

std::optional<core::PointerKind> parseKind(std::string_view text) {
  const std::size_t space = text.find(' ');
  if (space == std::string_view::npos)
    return std::nullopt;
  const std::optional<core::KindSource> source =
      core::parseKindSource(text.substr(0, space));
  if (!source)
    return std::nullopt;
  return core::PointerKind::parse(text.substr(space + 1), *source);
}

bool DeclaredInterface::empty() const noexcept {
  return !result && !ownership &&
         std::ranges::all_of(params, [](const DeclaredParam &param) {
           return !param.kind && !param.ownership;
         });
}

std::vector<FunctionRows> siteRows(const core::UnitLedger &unit) {
  std::vector<FunctionRows> functions;
  functions.reserve(unit.functions.size());
  for (const core::FunctionLedger &function : unit.functions) {
    FunctionRows rows{.function = function.name,
                      .file = function.file,
                      .line = function.line,
                      .linkage = function.linkage,
                      .rows = {}};
    rows.rows.reserve(function.sites.size());
    for (const core::Site &site : function.sites) {
      SiteRow row{.ordinal = site.ordinal,
                  .kind = site.kind,
                  .line = site.location.line,
                  .column = site.location.column,
                  .facets = {}};
      for (const core::Facet facet : core::AllFacets) {
        if (const core::FacetRecord *record = site.facet(facet)) {
          core::FacetDecision decision = record->decision;
          decision.detail.clear();
          row.facets.at(static_cast<std::size_t>(facet)) = std::move(decision);
        }
      }
      rows.rows.push_back(std::move(row));
    }
    functions.push_back(std::move(rows));
  }
  return functions;
}

core::UnitLedger unitLedgerOf(std::span<const FunctionRows> rows) {
  core::UnitLedger unit;
  unit.functions.reserve(rows.size());
  for (const FunctionRows &function : rows) {
    core::FunctionLedger ledger{.name = function.function,
                                .file = function.file,
                                .line = function.line,
                                .linkage = function.linkage,
                                .overBudget = false,
                                .requireSafe = false,
                                .callsSetjmp = false,
                                .sites = {}};
    ledger.sites.reserve(function.rows.size());
    for (const SiteRow &row : function.rows) {
      core::Site site;
      site.ordinal = row.ordinal;
      site.kind = row.kind;
      site.location = core::SourceLocation{.file = function.file,
                                           .line = row.line,
                                           .column = row.column,
                                           .opaque = 0};
      if (row.kind == core::SiteKind::Call)
        site.boundary = core::Boundary::Call;
      for (const core::Facet facet : core::AllFacets) {
        const auto &decision = row.facets.at(static_cast<std::size_t>(facet));
        if (!decision)
          continue;
        core::FacetRecord &record = site.addFacet(facet);
        record.decided = true;
        record.decision = *decision;
      }
      ledger.sites.push_back(std::move(site));
    }
    unit.functions.push_back(std::move(ledger));
  }
  return unit;
}

//===----------------------------------------------------------------------===//
// Writing
//===----------------------------------------------------------------------===//

/// JSON strings must be valid UTF-8; names and paths need not be.
static std::string utf8(llvm::StringRef text) {
  return llvm::json::isUTF8(text) ? text.str() : llvm::json::fixUTF8(text);
}

static llvm::json::Value orNull(const std::optional<std::string> &text) {
  if (!text)
    return nullptr;
  return utf8(*text);
}

static llvm::json::Object locationJson(const core::SourceLocation &location) {
  llvm::json::Object json;
  json["file"] = utf8(location.file);
  json["line"] = static_cast<std::int64_t>(location.line);
  json["column"] = static_cast<std::int64_t>(location.column);
  return json;
}

static llvm::json::Value
locationOrNull(const std::optional<core::SourceLocation> &location) {
  if (!location)
    return nullptr;
  return locationJson(*location);
}

template <typename Range>
static llvm::json::Array strings(const Range &range) {
  llvm::json::Array array;
  for (const std::string &text : range)
    array.push_back(utf8(text));
  return array;
}

static std::string hexOf(std::string_view bytes) {
  return llvm::toHex(llvm::StringRef(bytes.data(), bytes.size()),
                     /*LowerCase=*/true);
}

static llvm::json::Array interfacesJson(const core::InterfaceTypes &types) {
  llvm::json::Array array;
  for (const auto &[name, type] : types) {
    llvm::json::Object entry;
    entry["name"] = utf8(name);
    entry["type"] = type ? llvm::json::Value(hexOf(type->encode()))
                         : llvm::json::Value(nullptr);
    array.push_back(std::move(entry));
  }
  return array;
}

static llvm::json::Object functionJson(
    const std::string &name, const analysis::ExportedFunction &function,
    const FunctionInterface *interface, const core::GlobalNamer &names) {
  static const FunctionInterface None;
  const FunctionInterface &facts = interface != nullptr ? *interface : None;
  llvm::json::Object json;
  json["name"] = utf8(name);
  json["linkage"] = function.external ? "external" : "internal";
  json["addressTaken"] = function.addressTaken;
  json["typeKey"] = utf8(function.typeKey);
  json["summary"] = core::printSummary(function.summary.get(), names);
  llvm::json::Array params;
  for (const std::optional<std::string> &kind : facts.params)
    params.push_back(orNull(kind));
  llvm::json::Object kinds;
  kinds["params"] = std::move(params);
  kinds["result"] = orNull(facts.result);
  json["kinds"] = std::move(kinds);
  llvm::json::Array relies;
  for (const std::uint32_t param : facts.reliesOnSingle)
    relies.push_back(static_cast<std::int64_t>(param));
  json["reliesOnSingle"] = std::move(relies);
  llvm::json::Array requirements;
  for (const ExportedRequirement &requirement : facts.requirements) {
    llvm::json::Object entry;
    entry["param"] = static_cast<std::int64_t>(requirement.param);
    entry["kind"] = utf8(requirement.kind);
    entry["guard"] = orNull(requirement.guard);
    entry["element"] = static_cast<std::int64_t>(requirement.element);
    requirements.push_back(std::move(entry));
  }
  json["requirements"] = std::move(requirements);
  json["location"] = locationOrNull(facts.location);
  llvm::json::Array callbacks;
  for (const auto &[bindings, summary] : function.specializations) {
    llvm::json::Object entry;
    entry["bindings"] = core::printCallbackBindings(bindings, names);
    entry["summary"] = core::printSummary(summary.get(), names);
    callbacks.push_back(std::move(entry));
  }
  llvm::json::Array memory;
  for (const auto &[context, summary] : function.memorySpecializations) {
    llvm::json::Object entry;
    entry["context"] = core::printCallContext(context, names);
    entry["summary"] = core::printSummary(summary.get(), names);
    memory.push_back(std::move(entry));
  }
  llvm::json::Object contexts;
  contexts["acceptsCallbacks"] = function.acceptsCallbacks;
  contexts["acceptsMemory"] = function.acceptsMemoryContexts;
  contexts["callbacks"] = std::move(callbacks);
  contexts["memory"] = std::move(memory);
  json["contexts"] = std::move(contexts);
  return json;
}

static llvm::json::Object importJson(const std::string &name,
                                     const ImportInterface &facts) {
  llvm::json::Array params;
  for (const DeclaredParam &param : facts.declared.params) {
    llvm::json::Object entry;
    entry["name"] = utf8(param.name);
    entry["kind"] = orNull(param.kind);
    entry["ownership"] = orNull(param.ownership);
    params.push_back(std::move(entry));
  }
  llvm::json::Object declared;
  declared["params"] = std::move(params);
  declared["result"] = orNull(facts.declared.result);
  declared["ownership"] = orNull(facts.declared.ownership);
  llvm::json::Array calls;
  for (const ImportCall &call : facts.calls) {
    llvm::json::Array args;
    for (const std::optional<bool> &valid : call.args)
      args.push_back(valid ? llvm::json::Value(*valid)
                           : llvm::json::Value(nullptr));
    llvm::json::Array evidence;
    for (const ArgumentEvidence &argument : call.evidence) {
      llvm::json::Object known;
      known["bytes"] =
          argument.bytes
              ? llvm::json::Value(static_cast<std::int64_t>(*argument.bytes))
              : llvm::json::Value(nullptr);
      known["exact"] = argument.exact;
      known["null"] = argument.null;
      known["value"] = argument.value ? llvm::json::Value(*argument.value)
                                      : llvm::json::Value(nullptr);
      evidence.push_back(std::move(known));
    }
    llvm::json::Object entry;
    entry["function"] = utf8(call.function);
    entry["site"] =
        call.site ? llvm::json::Value(static_cast<std::int64_t>(*call.site))
                  : llvm::json::Value(nullptr);
    entry["args"] = std::move(args);
    entry["evidence"] = std::move(evidence);
    calls.push_back(std::move(entry));
  }
  llvm::json::Object json;
  json["name"] = utf8(name);
  json["declared"] = std::move(declared);
  json["location"] = locationOrNull(facts.location);
  json["calls"] = std::move(calls);
  return json;
}

static llvm::json::Array siteRowsJson(std::span<const FunctionRows> sites) {
  llvm::json::Array array;
  for (const FunctionRows &function : sites) {
    llvm::json::Array rows;
    for (const SiteRow &row : function.rows) {
      llvm::json::Array cells{static_cast<std::int64_t>(row.ordinal),
                              std::string(core::toString(row.kind)),
                              static_cast<std::int64_t>(row.line),
                              static_cast<std::int64_t>(row.column)};
      for (const auto &facet : row.facets)
        cells.push_back(facet ? llvm::json::Value(facet->compact())
                              : llvm::json::Value(nullptr));
      rows.push_back(std::move(cells));
    }
    llvm::json::Object entry;
    entry["function"] = utf8(function.function);
    entry["file"] = utf8(function.file);
    entry["line"] = static_cast<std::int64_t>(function.line);
    entry["linkage"] = std::string(core::toString(function.linkage));
    entry["rows"] = std::move(rows);
    array.push_back(std::move(entry));
  }
  return array;
}

static llvm::json::Object slotRulesJson(const SlotFacts &slots) {
  llvm::json::Object json;
  json["unit"] = utf8(slots.unit);
  json["defined"] = strings(slots.defined);
  json["exported"] = strings(slots.exported);
  json["confinedRecords"] = strings(slots.confinedRecords);
  json["escapedStatics"] = strings(slots.escapedStatics);
  return json;
}

static llvm::json::Array slotRowsJson(std::span<const core::SlotRow> rows) {
  llvm::json::Array array;
  for (const core::SlotRow &row : rows) {
    llvm::json::Array sources;
    for (const core::SlotKey &source : row.sources)
      sources.push_back(utf8(source.toString()));
    llvm::json::Object entry;
    entry["slot"] = utf8(row.slot.toString());
    entry["targets"] = strings(row.targets);
    entry["sources"] = std::move(sources);
    entry["open"] = orNull(row.open);
    array.push_back(std::move(entry));
  }
  return array;
}

llvm::json::Object toJson(const Payload &payload) {
  const analysis::UnitExports &exports = payload.exports;
  const InterfaceFacts &facts = payload.facts;
  const core::GlobalNamer names = [&exports](std::uint32_t id) {
    return exports.globals.nameOf(id).str();
  };
  llvm::json::Object json;

  llvm::json::Array functions;
  for (const auto &[name, function] : exports.functions) {
    const auto interface = facts.functions.find(name);
    functions.push_back(functionJson(
        name, function,
        interface == facts.functions.end() ? nullptr : &interface->second,
        names));
  }
  json["functions"] = std::move(functions);

  llvm::json::Array globals;
  for (std::uint32_t id = 0; id < exports.globals.size(); ++id) {
    const std::string name = exports.globals.nameOf(id).str();
    const auto known = facts.globals.find(name);
    llvm::json::Object entry;
    entry["name"] = utf8(name);
    entry["typeKey"] =
        known == facts.globals.end() ? "" : utf8(known->second.typeKey);
    entry["kind"] =
        known == facts.globals.end() ? "" : utf8(known->second.kind);
    globals.push_back(std::move(entry));
  }
  json["globals"] = std::move(globals);

  std::set<std::string> importNames = exports.imports;
  for (const auto &[name, import] : facts.imports)
    importNames.insert(name);
  llvm::json::Array imports;
  static const ImportInterface NoImport;
  for (const std::string &name : importNames) {
    const auto known = facts.imports.find(name);
    imports.push_back(importJson(
        name, known == facts.imports.end() ? NoImport : known->second));
  }
  json["imports"] = std::move(imports);
  json["indirect"] = strings(exports.indirectTypes);
  json["unknown"] = strings(exports.unknownCallees);
  json["unknownIndirect"] = strings(exports.unknownIndirectTypes);
  json["slots"] = slotRowsJson(facts.slots.rows);
  json["slotRules"] = slotRulesJson(facts.slots);

  llvm::json::Array slotKinds;
  for (const SlotKindRow &row : facts.slotKinds) {
    llvm::json::Array demotedBy;
    for (const core::SourceLocation &store : row.demotedBy)
      demotedBy.push_back(locationJson(store));
    llvm::json::Object entry;
    entry["slot"] = utf8(row.slot);
    entry["kind"] = utf8(row.kind);
    entry["demotedBy"] = std::move(demotedBy);
    slotKinds.push_back(std::move(entry));
  }
  json["slotKinds"] = std::move(slotKinds);

  llvm::json::Array invariants;
  for (const InvariantRow &row : facts.invariants) {
    llvm::json::Object entry;
    entry["struct"] = utf8(row.record);
    entry["field"] = utf8(row.field);
    entry["template"] = utf8(row.templ);
    entry["verdict"] = std::string(core::toString(row.verdict));
    entry["relied"] = row.relied;
    entry["store"] = locationOrNull(row.store);
    invariants.push_back(std::move(entry));
  }
  json["invariants"] = std::move(invariants);

  llvm::json::Array memoryRequests;
  for (const auto &[symbol, requests] : exports.memoryRequests) {
    for (const core::CallContext &context : requests) {
      llvm::json::Object entry;
      entry["function"] = utf8(symbol);
      entry["context"] = core::printCallContext(context, names);
      memoryRequests.push_back(std::move(entry));
    }
  }
  llvm::json::Array callbackRequests;
  for (const auto &[symbol, requests] : exports.callbackRequests) {
    for (const core::CallbackBindings &bindings : requests) {
      llvm::json::Object entry;
      entry["function"] = utf8(symbol);
      entry["bindings"] = core::printCallbackBindings(bindings, names);
      callbackRequests.push_back(std::move(entry));
    }
  }
  llvm::json::Object contexts;
  contexts["memoryRequests"] = std::move(memoryRequests);
  contexts["callbackRequests"] = std::move(callbackRequests);
  json["contexts"] = std::move(contexts);

  json["countFields"] = strings(exports.countFields);
  llvm::json::Array witnesses;
  for (const analysis::SizedFieldWitness &witness :
       exports.sizedFields.witnesses) {
    llvm::json::Object entry;
    entry["field"] = utf8(witness.field);
    entry["count"] = utf8(witness.count);
    entry["scale"] = witness.scale;
    entry["productType"] =
        witness.productType ? llvm::json::Value(witness.productType->toString())
                            : llvm::json::Value(nullptr);
    witnesses.push_back(std::move(entry));
  }
  llvm::json::Array pairs;
  for (const analysis::UnsizedPair &pair : exports.sizedFields.unsizedPairs) {
    llvm::json::Object entry;
    entry["field"] = utf8(pair.field);
    entry["count"] = utf8(pair.count);
    pairs.push_back(std::move(entry));
  }
  llvm::json::Object sized;
  sized["witnesses"] = std::move(witnesses);
  sized["unsizedFields"] = strings(exports.sizedFields.unsizedFields);
  sized["unsizedPairs"] = std::move(pairs);
  json["sizedFields"] = std::move(sized);
  json["sizedFieldLoads"] = strings(exports.sizedFieldLoads);

  llvm::json::Object interfaces;
  interfaces["globals"] = interfacesJson(exports.globalInterfaces);
  interfaces["objects"] = interfacesJson(exports.objectInterfaces);
  json["interfaces"] = std::move(interfaces);

  llvm::json::Array boundaries;
  for (const analysis::BoundaryRow &row : facts.boundaries) {
    llvm::json::Object entry;
    entry["function"] = utf8(row.function);
    entry["site"] = static_cast<std::int64_t>(row.site);
    entry["reason"] = std::string(core::toString(row.reason));
    entry["placeClass"] = utf8(row.placeClass);
    boundaries.push_back(std::move(entry));
  }
  json["boundaries"] = std::move(boundaries);
  json["sites"] = siteRowsJson(payload.sites);

  llvm::json::Array reported;
  for (const ReportedDiagnostic &diagnostic : payload.reported) {
    llvm::json::Object entry;
    entry["id"] = utf8(diagnostic.id);
    entry["file"] = utf8(diagnostic.file);
    entry["line"] = static_cast<std::int64_t>(diagnostic.line);
    entry["column"] = static_cast<std::int64_t>(diagnostic.column);
    reported.push_back(std::move(entry));
  }
  json["reported"] = std::move(reported);
  json["definesAllocator"] = facts.allocator.has_value();
  llvm::json::Object a5;
  a5["loweredAllocations"] = facts.loweredAllocations;
  a5["nonLoweredAllocations"] = payload.a5.nonLoweredAllocations;
  a5["bypassedDeclarations"] = payload.a5.bypassedDeclarations;
  a5["allocator"] = orNull(facts.allocator);
  json["a5"] = std::move(a5);
  return json;
}

//===----------------------------------------------------------------------===//
// Reading
//===----------------------------------------------------------------------===//

namespace {

/// Reads a payload the field table accepted. Every `read*` returns false
/// after recording the first problem, with its path, in `error`.
class PayloadReader {
public:
  PayloadReader(std::string_view source, std::string &error) : error(error) {
    payload.exports.source = std::string(source);
    resolve = [this](std::string_view name) {
      return std::optional(payload.exports.globals.idFor(name));
    };
  }

  bool read(const llvm::json::Object &json);
  Payload take() { return std::move(payload); }

private:
  Payload payload;
  std::string &error;
  core::GlobalResolver resolve;

  bool fail(const std::string &where, const std::string &what) {
    error = where + ": " + what;
    return false;
  }

  static std::string text(const llvm::json::Object &json, llvm::StringRef key) {
    return json.getString(key)->str();
  }
  static std::optional<std::string> optionalText(const llvm::json::Object &json,
                                                 llvm::StringRef key) {
    if (const auto value = json.getString(key))
      return value->str();
    return std::nullopt;
  }
  static const llvm::json::Array &array(const llvm::json::Object &json,
                                        llvm::StringRef key) {
    return *json.getArray(key);
  }
  static const llvm::json::Object &object(const llvm::json::Value &value) {
    return *value.getAsObject();
  }
  static std::optional<std::uint64_t> unsignedOf(const llvm::json::Value &v) {
    if (const auto value = v.getAsUINT64())
      return *value;
    if (const auto value = v.getAsInteger(); value && *value >= 0)
      return static_cast<std::uint64_t>(*value);
    return std::nullopt;
  }
  bool u32(const llvm::json::Value &value, const std::string &where,
           std::uint32_t &out) {
    const auto number = unsignedOf(value);
    if (!number || *number > UINT32_MAX)
      return fail(where, "expected an integer between 0 and 4294967295");
    out = static_cast<std::uint32_t>(*number);
    return true;
  }
  bool u64(const llvm::json::Value &value, const std::string &where,
           std::uint64_t &out) {
    const auto number = unsignedOf(value);
    if (!number)
      return fail(where, "expected a non-negative integer");
    out = *number;
    return true;
  }
  bool location(const llvm::json::Value &value, const std::string &where,
                std::optional<core::SourceLocation> &out) {
    if (value.kind() == llvm::json::Value::Null) {
      out.reset();
      return true;
    }
    const llvm::json::Object &json = object(value);
    core::SourceLocation location{
        .file = text(json, "file"), .line = 0, .column = 0, .opaque = 0};
    if (!u32(*json.get("line"), where + ".line", location.line) ||
        !u32(*json.get("column"), where + ".column", location.column))
      return false;
    out = std::move(location);
    return true;
  }
  bool summary(const llvm::json::Object &json, const std::string &where,
               analysis::ExportedSummary &out) {
    std::string problem;
    const auto parsed =
        core::parseSummary(text(json, "summary"), resolve, &problem);
    if (!parsed)
      return fail(where + ".summary", problem.empty() ? "malformed" : problem);
    out.assign(*parsed);
    return true;
  }
  bool kind(const std::optional<std::string> &spelling,
            const std::string &where) {
    if (spelling && !parseKind(*spelling))
      return fail(where, "unknown kind '" + *spelling + "'");
    return true;
  }
  static bool plainKind(const std::optional<std::string> &spelling) {
    return !spelling || core::PointerKind::parse(*spelling).has_value();
  }

  bool readGlobals(const llvm::json::Array &globals);
  bool readFunction(const llvm::json::Object &json, const std::string &where);
  bool readImport(const llvm::json::Object &json, const std::string &where);
  bool readSlots(const llvm::json::Object &json);
  bool readKindsAndInvariants(const llvm::json::Object &json);
  bool readContexts(const llvm::json::Object &json);
  bool readFieldFacts(const llvm::json::Object &json);
  bool readInterfaces(const llvm::json::Object &json);
  bool readRows(const llvm::json::Object &json);
  bool readRest(const llvm::json::Object &json);
};

} // namespace

bool PayloadReader::readGlobals(const llvm::json::Array &globals) {
  if (globals.size() > MaxGlobalNames)
    return fail("payload.globals", "more than " +
                                       std::to_string(MaxGlobalNames) +
                                       " global names");
  for (std::size_t i = 0; i < globals.size(); ++i) {
    const std::string where = "payload.globals[" + std::to_string(i) + "]";
    const llvm::json::Object &json = object(globals[i]);
    const std::string name = text(json, "name");
    if (name.empty() || payload.exports.globals.find(name))
      return fail(where, "empty or duplicate global name '" + name + "'");
    (void)payload.exports.globals.idFor(name);
    GlobalInterface facts{.typeKey = text(json, "typeKey"),
                          .kind = text(json, "kind")};
    if (!facts.kind.empty() && !parseKind(facts.kind))
      return fail(where + ".kind", "unknown kind '" + facts.kind + "'");
    if (facts != GlobalInterface{})
      payload.facts.globals.emplace(name, std::move(facts));
  }
  return true;
}

bool PayloadReader::readFunction(const llvm::json::Object &json,
                                 const std::string &where) {
  const std::string name = text(json, "name");
  if (name.empty() || payload.exports.functions.contains(name))
    return fail(where, "empty or duplicate function '" + name + "'");
  const std::string linkage = text(json, "linkage");
  if (linkage != "external" && linkage != "internal")
    return fail(where + ".linkage", "unknown linkage '" + linkage + "'");
  analysis::ExportedFunction function;
  function.external = linkage == "external";
  function.addressTaken = *json.getBoolean("addressTaken");
  function.typeKey = text(json, "typeKey");
  if (!summary(json, where, function.summary))
    return false;

  FunctionInterface facts;
  const llvm::json::Object &kinds = *json.getObject("kinds");
  const llvm::json::Array &params = array(kinds, "params");
  for (std::size_t i = 0; i < params.size(); ++i) {
    std::optional<std::string> spelling;
    if (const auto value = params[i].getAsString())
      spelling = value->str();
    if (!kind(spelling, where + ".kinds.params[" + std::to_string(i) + "]"))
      return false;
    facts.params.push_back(std::move(spelling));
  }
  facts.result = optionalText(kinds, "result");
  if (!kind(facts.result, where + ".kinds.result"))
    return false;
  const llvm::json::Array &relies = array(json, "reliesOnSingle");
  for (std::size_t i = 0; i < relies.size(); ++i) {
    std::uint32_t param = 0;
    if (!u32(relies[i], where + ".reliesOnSingle[" + std::to_string(i) + "]",
             param))
      return false;
    facts.reliesOnSingle.push_back(param);
  }
  const llvm::json::Array &requirements = array(json, "requirements");
  for (std::size_t i = 0; i < requirements.size(); ++i) {
    const std::string at = where + ".requirements[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(requirements[i]);
    ExportedRequirement requirement{.param = 0,
                                    .kind = text(entry, "kind"),
                                    .guard = optionalText(entry, "guard")};
    if (!u32(*entry.get("param"), at + ".param", requirement.param))
      return false;
    if (!u64(*entry.get("element"), at + ".element", requirement.element))
      return false;
    if (!core::PointerKind::parse(requirement.kind))
      return fail(at + ".kind", "unknown kind '" + requirement.kind + "'");
    facts.requirements.push_back(std::move(requirement));
  }
  if (!location(*json.get("location"), where + ".location", facts.location))
    return false;

  const llvm::json::Object &contexts = *json.getObject("contexts");
  function.acceptsCallbacks = *contexts.getBoolean("acceptsCallbacks");
  function.acceptsMemoryContexts = *contexts.getBoolean("acceptsMemory");
  const llvm::json::Array &callbacks = array(contexts, "callbacks");
  if (callbacks.size() > core::MaxCallbackContexts)
    return fail(where + ".contexts.callbacks", "too many specializations");
  for (std::size_t i = 0; i < callbacks.size(); ++i) {
    const std::string at =
        where + ".contexts.callbacks[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(callbacks[i]);
    const auto bindings =
        core::parseCallbackBindings(text(entry, "bindings"), resolve);
    if (!bindings)
      return fail(at + ".bindings", "malformed callback bindings");
    analysis::ExportedSummary specialized;
    if (!summary(entry, at, specialized))
      return false;
    if (!function.specializations.emplace(*bindings, std::move(specialized))
             .second)
      return fail(at, "duplicate callback specialization");
  }
  const llvm::json::Array &memory = array(contexts, "memory");
  if (memory.size() > core::MaxMemoryContexts)
    return fail(where + ".contexts.memory", "too many specializations");
  for (std::size_t i = 0; i < memory.size(); ++i) {
    const std::string at =
        where + ".contexts.memory[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(memory[i]);
    const auto context =
        core::parseCallContext(text(entry, "context"), resolve);
    if (!context)
      return fail(at + ".context", "malformed call context");
    analysis::ExportedSummary specialized;
    if (!summary(entry, at, specialized))
      return false;
    if (!function.memorySpecializations
             .emplace(*context, std::move(specialized))
             .second)
      return fail(at, "duplicate memory specialization");
  }
  payload.exports.functions.emplace(name, std::move(function));
  if (facts != FunctionInterface{})
    payload.facts.functions.emplace(name, std::move(facts));
  return true;
}

/// The ownership annotations a declaration can state, as `weavec.h` spells
/// them.
static bool isOwnershipSpelling(std::string_view text) {
  return text == "WEAVEC_OWNED" || text == "WEAVEC_BORROWED" ||
         text == "WEAVEC_MUT" || text == "WEAVEC_RAW" ||
         text == "WEAVEC_RETAINS" || text == "WEAVEC_RELEASES";
}

bool PayloadReader::readImport(const llvm::json::Object &json,
                               const std::string &where) {
  const std::string name = text(json, "name");
  if (name.empty() || payload.exports.imports.contains(name))
    return fail(where, "empty or duplicate import '" + name + "'");
  ImportInterface facts;
  const llvm::json::Object &declared = *json.getObject("declared");
  const llvm::json::Array &params = array(declared, "params");
  for (std::size_t i = 0; i < params.size(); ++i) {
    const std::string at =
        where + ".declared.params[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(params[i]);
    DeclaredParam param{.name = text(entry, "name"),
                        .kind = optionalText(entry, "kind"),
                        .ownership = optionalText(entry, "ownership")};
    if (!plainKind(param.kind))
      return fail(at + ".kind", "unknown kind '" + *param.kind + "'");
    if (param.ownership && !isOwnershipSpelling(*param.ownership))
      return fail(at + ".ownership",
                  "unknown annotation '" + *param.ownership + "'");
    facts.declared.params.push_back(std::move(param));
  }
  facts.declared.result = optionalText(declared, "result");
  if (!plainKind(facts.declared.result))
    return fail(where + ".declared.result",
                "unknown kind '" + *facts.declared.result + "'");
  facts.declared.ownership = optionalText(declared, "ownership");
  if (facts.declared.ownership &&
      !isOwnershipSpelling(*facts.declared.ownership))
    return fail(where + ".declared.ownership",
                "unknown annotation '" + *facts.declared.ownership + "'");
  if (!location(*json.get("location"), where + ".location", facts.location))
    return false;
  const llvm::json::Array &calls = array(json, "calls");
  for (std::size_t i = 0; i < calls.size(); ++i) {
    const std::string at = where + ".calls[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(calls[i]);
    ImportCall call{
        .function = text(entry, "function"), .site = std::nullopt, .args = {}};
    if (const llvm::json::Value *site = entry.get("site");
        site->kind() != llvm::json::Value::Null) {
      std::uint32_t ordinal = 0;
      if (!u32(*site, at + ".site", ordinal))
        return false;
      call.site = ordinal;
    }
    for (const llvm::json::Value &arg : array(entry, "args"))
      call.args.push_back(arg.getAsBoolean());
    const llvm::json::Array &evidence = array(entry, "evidence");
    for (std::size_t a = 0; a < evidence.size(); ++a) {
      const std::string at2 = at + ".evidence[" + std::to_string(a) + "]";
      const llvm::json::Object &known = object(evidence[a]);
      ArgumentEvidence argument;
      if (const llvm::json::Value *bytes = known.get("bytes");
          bytes->kind() != llvm::json::Value::Null) {
        std::uint64_t width = 0;
        if (!u64(*bytes, at2 + ".bytes", width))
          return false;
        argument.bytes = width;
      }
      argument.exact = *known.getBoolean("exact");
      argument.null = *known.getBoolean("null");
      if (const llvm::json::Value *value = known.get("value");
          value->kind() != llvm::json::Value::Null) {
        const std::optional<std::int64_t> number = value->getAsInteger();
        if (!number)
          return fail(at2 + ".value", "expected an integer");
        argument.value = *number;
      }
      call.evidence.push_back(argument);
    }
    facts.calls.push_back(std::move(call));
  }
  payload.exports.imports.insert(name);
  if (facts != ImportInterface{})
    payload.facts.imports.emplace(name, std::move(facts));
  return true;
}

bool PayloadReader::readSlots(const llvm::json::Object &json) {
  const llvm::json::Array &rows = array(json, "slots");
  std::set<core::SlotKey> seen;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const std::string where = "payload.slots[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(rows[i]);
    const auto slot = core::SlotKey::parse(text(entry, "slot"));
    if (!slot || slot->isLocal() || !seen.insert(*slot).second)
      return fail(where + ".slot", "malformed, local or duplicate slot '" +
                                       text(entry, "slot") + "'");
    core::SlotRow row{.slot = *slot,
                      .targets = {},
                      .sources = {},
                      .open = optionalText(entry, "open")};
    for (const llvm::json::Value &target : array(entry, "targets"))
      row.targets.push_back(target.getAsString()->str());
    const llvm::json::Array &sources = array(entry, "sources");
    for (std::size_t s = 0; s < sources.size(); ++s) {
      const auto source = core::SlotKey::parse(*sources[s].getAsString());
      if (!source || source->isLocal())
        return fail(where + ".sources[" + std::to_string(s) + "]",
                    "malformed or local slot");
      row.sources.push_back(*source);
    }
    payload.facts.slots.rows.push_back(std::move(row));
  }
  const llvm::json::Object &rules = *json.getObject("slotRules");
  SlotFacts &slots = payload.facts.slots;
  slots.unit = text(rules, "unit");
  const auto names = [&](llvm::StringRef key, std::set<std::string> &into) {
    for (const llvm::json::Value &name : array(rules, key))
      into.insert(name.getAsString()->str());
  };
  names("defined", slots.defined);
  names("exported", slots.exported);
  names("confinedRecords", slots.confinedRecords);
  names("escapedStatics", slots.escapedStatics);
  return true;
}

bool PayloadReader::readKindsAndInvariants(const llvm::json::Object &json) {
  const llvm::json::Array &kinds = array(json, "slotKinds");
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    const std::string where = "payload.slotKinds[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(kinds[i]);
    SlotKindRow row{.slot = text(entry, "slot"),
                    .kind = text(entry, "kind"),
                    .demotedBy = {}};
    if (!core::PointerKind::parse(row.kind))
      return fail(where + ".kind", "unknown kind '" + row.kind + "'");
    const llvm::json::Array &stores = array(entry, "demotedBy");
    for (std::size_t s = 0; s < stores.size(); ++s) {
      std::optional<core::SourceLocation> store;
      if (!location(stores[s], where + ".demotedBy[" + std::to_string(s) + "]",
                    store))
        return false;
      row.demotedBy.push_back(std::move(*store));
    }
    payload.facts.slotKinds.push_back(std::move(row));
  }
  const llvm::json::Array &invariants = array(json, "invariants");
  for (std::size_t i = 0; i < invariants.size(); ++i) {
    const std::string where = "payload.invariants[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(invariants[i]);
    const auto verdict = core::parseVerdict(text(entry, "verdict"));
    if (!verdict)
      return fail(where + ".verdict",
                  "unknown verdict '" + text(entry, "verdict") + "'");
    InvariantRow row{.record = text(entry, "struct"),
                     .field = text(entry, "field"),
                     .templ = text(entry, "template"),
                     .verdict = *verdict,
                     .relied = *entry.getBoolean("relied"),
                     .store = std::nullopt};
    if (!location(*entry.get("store"), where + ".store", row.store))
      return false;
    payload.facts.invariants.push_back(std::move(row));
  }
  const llvm::json::Array &boundaries = array(json, "boundaries");
  for (std::size_t i = 0; i < boundaries.size(); ++i) {
    const std::string where = "payload.boundaries[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(boundaries[i]);
    const auto reason = core::parseUnresolvedReason(text(entry, "reason"));
    if (!reason || (*reason != core::UnresolvedReason::DanglingEscape &&
                    *reason != core::UnresolvedReason::SecondOwner))
      return fail(where + ".reason",
                  "not a boundary reason: '" + text(entry, "reason") + "'");
    analysis::BoundaryRow row{.unit = {},
                              .function = text(entry, "function"),
                              .site = 0,
                              .reason = *reason,
                              .placeClass = text(entry, "placeClass")};
    if (!u32(*entry.get("site"), where + ".site", row.site))
      return false;
    payload.facts.boundaries.push_back(std::move(row));
  }
  return true;
}

bool PayloadReader::readContexts(const llvm::json::Object &json) {
  analysis::UnitExports &exports = payload.exports;
  const llvm::json::Object &contexts = *json.getObject("contexts");
  const llvm::json::Array &memory = array(contexts, "memoryRequests");
  for (std::size_t i = 0; i < memory.size(); ++i) {
    const std::string where =
        "payload.contexts.memoryRequests[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(memory[i]);
    const std::string symbol = text(entry, "function");
    const auto context =
        core::parseCallContext(text(entry, "context"), resolve);
    if (symbol.empty() || !context)
      return fail(where, "malformed memory request");
    auto &requests = exports.memoryRequests[symbol];
    if (!requests.insert(*context).second)
      return fail(where, "duplicate memory request");
    if (requests.size() > MaxContextRequests)
      return fail(where, "too many memory requests");
  }
  const llvm::json::Array &callbacks = array(contexts, "callbackRequests");
  for (std::size_t i = 0; i < callbacks.size(); ++i) {
    const std::string where =
        "payload.contexts.callbackRequests[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(callbacks[i]);
    const std::string symbol = text(entry, "function");
    const auto bindings =
        core::parseCallbackBindings(text(entry, "bindings"), resolve);
    if (symbol.empty() || !bindings)
      return fail(where, "malformed callback request");
    auto &requests = exports.callbackRequests[symbol];
    if (!requests.insert(*bindings).second)
      return fail(where, "duplicate callback request");
    if (requests.size() > MaxContextRequests)
      return fail(where, "too many callback requests");
  }
  return true;
}

bool PayloadReader::readFieldFacts(const llvm::json::Object &json) {
  analysis::UnitExports &exports = payload.exports;
  for (const llvm::json::Value &key : array(json, "countFields"))
    exports.countFields.insert(key.getAsString()->str());
  const llvm::json::Object &sized = *json.getObject("sizedFields");
  const llvm::json::Array &witnesses = array(sized, "witnesses");
  for (std::size_t i = 0; i < witnesses.size(); ++i) {
    const std::string where =
        "payload.sizedFields.witnesses[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(witnesses[i]);
    analysis::SizedFieldWitness witness{.field = text(entry, "field"),
                                        .count = text(entry, "count"),
                                        .scale = 0,
                                        .productType = std::nullopt};
    const auto scale = entry.getInteger("scale");
    if (!scale || *scale <= 0 || witness.field.empty() || witness.count.empty())
      return fail(where, "malformed sized-field witness");
    witness.scale = *scale;
    if (const auto type = optionalText(entry, "productType")) {
      witness.productType = core::IntegerType::parse(*type);
      if (!witness.productType || witness.productType->isSigned ||
          witness.productType->isBoolean ||
          static_cast<std::uint64_t>(*scale) > witness.productType->mask())
        return fail(where + ".productType",
                    "malformed product type '" + *type + "'");
    }
    exports.sizedFields.witnesses.insert(std::move(witness));
  }
  for (const llvm::json::Value &field : array(sized, "unsizedFields"))
    exports.sizedFields.unsizedFields.insert(field.getAsString()->str());
  for (const llvm::json::Value &pair : array(sized, "unsizedPairs"))
    exports.sizedFields.unsizedPairs.insert(
        analysis::UnsizedPair{.field = text(object(pair), "field"),
                              .count = text(object(pair), "count")});
  for (const llvm::json::Value &key : array(json, "sizedFieldLoads"))
    exports.sizedFieldLoads.insert(key.getAsString()->str());
  return true;
}

bool PayloadReader::readInterfaces(const llvm::json::Object &json) {
  const llvm::json::Object &interfaces = *json.getObject("interfaces");
  const auto read = [&](llvm::StringRef key, core::InterfaceTypes &into) {
    const llvm::json::Array &entries = array(interfaces, key);
    const std::string where = "payload.interfaces." + key.str();
    if (entries.size() > MaxInterfaces)
      return fail(where,
                  "more than " + std::to_string(MaxInterfaces) + " interfaces");
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const std::string at = where + "[" + std::to_string(i) + "]";
      const llvm::json::Object &entry = object(entries[i]);
      const std::string name = text(entry, "name");
      if (name.empty() || into.contains(name))
        return fail(at, "empty or duplicate interface identity");
      const auto encoded = optionalText(entry, "type");
      if (!encoded) {
        into.emplace(name, std::nullopt);
        continue;
      }
      std::string bytes;
      if (encoded->size() > core::MaxInterfaceBytes * 2 ||
          !llvm::tryGetFromHex(*encoded, bytes) || hexOf(bytes) != *encoded)
        return fail(at + ".type", "invalid interface encoding");
      const auto type = core::InterfaceType::decode(bytes);
      if (!type)
        return fail(at + ".type", "invalid interface storage description");
      into.emplace(name, *type);
    }
    return true;
  };
  return read("globals", payload.exports.globalInterfaces) &&
         read("objects", payload.exports.objectInterfaces);
}

bool PayloadReader::readRows(const llvm::json::Object &json) {
  const llvm::json::Array &functions = array(json, "sites");
  for (std::size_t f = 0; f < functions.size(); ++f) {
    const std::string where = "payload.sites[" + std::to_string(f) + "]";
    const llvm::json::Object &entry = object(functions[f]);
    const auto linkage = core::parseLinkage(text(entry, "linkage"));
    if (!linkage)
      return fail(where + ".linkage",
                  "unknown linkage '" + text(entry, "linkage") + "'");
    FunctionRows rows{.function = text(entry, "function"),
                      .file = text(entry, "file"),
                      .line = 0,
                      .linkage = *linkage,
                      .rows = {}};
    if (!u32(*entry.get("line"), where + ".line", rows.line))
      return false;
    const llvm::json::Array &cells = array(entry, "rows");
    for (std::size_t r = 0; r < cells.size(); ++r) {
      const std::string at = where + ".rows[" + std::to_string(r) + "]";
      const llvm::json::Array &row = *cells[r].getAsArray();
      SiteRow site;
      if (!u32(row[0], at + "[0]", site.ordinal) ||
          !u32(row[2], at + "[2]", site.line) ||
          !u32(row[3], at + "[3]", site.column))
        return false;
      if (site.ordinal != r)
        return fail(at + "[0]", "ordinal " + std::to_string(site.ordinal) +
                                    ", expected " + std::to_string(r));
      const auto kind = core::parseSiteKind(*row[1].getAsString());
      if (!kind)
        return fail(at + "[1]",
                    "unknown site kind '" + row[1].getAsString()->str() + "'");
      site.kind = *kind;
      for (std::size_t facet = 0; facet < core::FacetCount; ++facet) {
        const llvm::json::Value &cell = row[4 + facet];
        if (cell.kind() == llvm::json::Value::Null)
          continue;
        const auto decision = core::parseCompactFacet(*cell.getAsString());
        if (!decision)
          return fail(at + "[" + std::to_string(4 + facet) + "]",
                      "malformed facet '" + cell.getAsString()->str() + "'");
        site.facets.at(facet) = *decision;
      }
      rows.rows.push_back(std::move(site));
    }
    payload.sites.push_back(std::move(rows));
  }
  return true;
}

bool PayloadReader::readRest(const llvm::json::Object &json) {
  analysis::UnitExports &exports = payload.exports;
  for (const llvm::json::Value &key : array(json, "indirect"))
    exports.indirectTypes.insert(key.getAsString()->str());
  for (const llvm::json::Value &name : array(json, "unknown"))
    exports.unknownCallees.insert(name.getAsString()->str());
  for (const llvm::json::Value &key : array(json, "unknownIndirect"))
    exports.unknownIndirectTypes.insert(key.getAsString()->str());
  const llvm::json::Array &reported = array(json, "reported");
  for (std::size_t i = 0; i < reported.size(); ++i) {
    const std::string where = "payload.reported[" + std::to_string(i) + "]";
    const llvm::json::Object &entry = object(reported[i]);
    ReportedDiagnostic diagnostic{.id = text(entry, "id"),
                                  .file = text(entry, "file"),
                                  .line = 0,
                                  .column = 0};
    if (!u32(*entry.get("line"), where + ".line", diagnostic.line) ||
        !u32(*entry.get("column"), where + ".column", diagnostic.column))
      return false;
    payload.reported.insert(std::move(diagnostic));
  }
  const llvm::json::Object &a5 = *json.getObject("a5");
  payload.facts.allocator = optionalText(a5, "allocator");
  if (*json.getBoolean("definesAllocator") !=
      payload.facts.allocator.has_value())
    return fail("payload.definesAllocator",
                "disagrees with 'payload.a5.allocator'");
  return u64(*a5.get("loweredAllocations"), "payload.a5.loweredAllocations",
             payload.facts.loweredAllocations) &&
         u64(*a5.get("nonLoweredAllocations"),
             "payload.a5.nonLoweredAllocations",
             payload.a5.nonLoweredAllocations) &&
         u64(*a5.get("bypassedDeclarations"), "payload.a5.bypassedDeclarations",
             payload.a5.bypassedDeclarations);
}

bool PayloadReader::read(const llvm::json::Object &json) {
  // The global names first: the summaries and contexts spell roots by them,
  // and the ids keep the producer's order (RFC 0022).
  if (!readGlobals(array(json, "globals")))
    return false;
  const llvm::json::Array &functions = array(json, "functions");
  for (std::size_t i = 0; i < functions.size(); ++i)
    if (!readFunction(object(functions[i]),
                      "payload.functions[" + std::to_string(i) + "]"))
      return false;
  const llvm::json::Array &imports = array(json, "imports");
  for (std::size_t i = 0; i < imports.size(); ++i)
    if (!readImport(object(imports[i]),
                    "payload.imports[" + std::to_string(i) + "]"))
      return false;
  return readSlots(json) && readKindsAndInvariants(json) &&
         readContexts(json) && readFieldFacts(json) && readInterfaces(json) &&
         readRows(json) && readRest(json);
}

std::optional<Payload> payloadFromJson(const llvm::json::Object &json,
                                       std::string_view source,
                                       std::string &error) {
  // Not braces: they would make a one-element array.
  const llvm::json::Value value = llvm::json::Object(json);
  if (auto problem = validate(value, payloadSchema(), "payload")) {
    error = std::move(*problem);
    return std::nullopt;
  }
  PayloadReader reader(source, error);
  if (!reader.read(json))
    return std::nullopt;
  return reader.take();
}

} // namespace weavec::frontend::record
