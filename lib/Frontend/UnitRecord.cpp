//===- UnitRecord.cpp - The format-28 unit record (RFC 0030) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/UnitRecord.h"

#include "weavec/Config/Version.h"
#include "weavec/Core/SummaryIO.h"
#include "weavec/Frontend/LedgerWriter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <utility>

namespace weavec::frontend::record {

std::string_view toString(FieldType type) noexcept {
  switch (type) {
  case FieldType::String:
    return "string";
  case FieldType::Integer:
    return "integer";
  case FieldType::Boolean:
    return "boolean";
  case FieldType::Object:
    return "object";
  case FieldType::Array:
    return "array";
  case FieldType::Tuple:
    return "tuple";
  }
  return "<invalid>";
}

//===----------------------------------------------------------------------===//
// The field table
//===----------------------------------------------------------------------===//

static constexpr FieldSpec scalar(std::string_view name, FieldType type,
                                  bool nullable = false) {
  return FieldSpec{
      .name = name, .type = type, .nullable = nullable, .members = {}};
}

static constexpr FieldSpec object(std::string_view name,
                                  std::span<const FieldSpec> members,
                                  bool nullable = false) {
  return FieldSpec{.name = name,
                   .type = FieldType::Object,
                   .nullable = nullable,
                   .members = members};
}

/// `element` holds exactly one spec.
static constexpr FieldSpec array(std::string_view name,
                                 std::span<const FieldSpec> element) {
  return FieldSpec{.name = name,
                   .type = FieldType::Array,
                   .nullable = false,
                   .members = element};
}

static constexpr FieldSpec tuple(std::string_view name,
                                 std::span<const FieldSpec> positions) {
  return FieldSpec{.name = name,
                   .type = FieldType::Tuple,
                   .nullable = false,
                   .members = positions};
}

static constexpr FieldType String = FieldType::String;
static constexpr FieldType Integer = FieldType::Integer;
static constexpr FieldType Boolean = FieldType::Boolean;

// Array elements shared by several fields.
static constexpr std::array StringElement{scalar({}, String)};
static constexpr std::array NullableStringElement{scalar({}, String, true)};
static constexpr std::array IntegerElement{scalar({}, Integer)};
static constexpr std::array NullableBooleanElement{scalar({}, Boolean, true)};
static constexpr std::array LocationFields{
    scalar("file", String), scalar("line", Integer), scalar("column", Integer)};
static constexpr std::array LocationElement{object({}, LocationFields)};

// The header.
static constexpr std::array ProducerFields{scalar("name", String),
                                           scalar("version", String),
                                           scalar("revision", String)};
static constexpr std::array ConfigFields{
    scalar("checks", String), scalar("zeroInit", Boolean),
    scalar("require", String), scalar("budget", Integer)};
static constexpr std::array ObjectFields{scalar("path", String),
                                         scalar("digest", String)};
static constexpr std::array HeaderFields{object("producer", ProducerFields),
                                         scalar("source", String),
                                         scalar("cwd", String),
                                         array("command", StringElement),
                                         scalar("target", String),
                                         object("config", ConfigFields),
                                         object("object", ObjectFields)};
static constexpr FieldSpec HeaderSchema = object({}, HeaderFields);

// payload.functions
static constexpr std::array KindsFields{array("params", NullableStringElement),
                                        scalar("result", String, true)};
static constexpr std::array RequirementFields{scalar("param", Integer),
                                              scalar("kind", String),
                                              scalar("guard", String, true)};
static constexpr std::array RequirementElement{object({}, RequirementFields)};
static constexpr std::array CallbackSummaryFields{scalar("bindings", String),
                                                  scalar("summary", String)};
static constexpr std::array CallbackSummaryElement{
    object({}, CallbackSummaryFields)};
static constexpr std::array MemorySummaryFields{scalar("context", String),
                                                scalar("summary", String)};
static constexpr std::array MemorySummaryElement{
    object({}, MemorySummaryFields)};
/// RFC 0014 and 0016: the call contexts the definition accepts and the
/// summaries it was specialised to.
static constexpr std::array FunctionContextFields{
    scalar("acceptsCallbacks", Boolean), scalar("acceptsMemory", Boolean),
    array("callbacks", CallbackSummaryElement),
    array("memory", MemorySummaryElement)};
static constexpr std::array FunctionFields{
    scalar("name", String),
    scalar("linkage", String),
    scalar("addressTaken", Boolean),
    scalar("typeKey", String),
    scalar("summary", String),
    object("kinds", KindsFields),
    array("reliesOnSingle", IntegerElement),
    array("requirements", RequirementElement),
    object("location", LocationFields, true),
    object("contexts", FunctionContextFields)};
static constexpr std::array FunctionElement{object({}, FunctionFields)};

// payload.globals and payload.imports
static constexpr std::array GlobalFields{
    scalar("name", String), scalar("typeKey", String), scalar("kind", String)};
static constexpr std::array GlobalElement{object({}, GlobalFields)};
static constexpr std::array DeclaredParamFields{
    scalar("name", String), scalar("kind", String, true),
    scalar("ownership", String, true)};
static constexpr std::array DeclaredParamElement{
    object({}, DeclaredParamFields)};
static constexpr std::array DeclaredFields{
    array("params", DeclaredParamElement), scalar("result", String, true),
    scalar("ownership", String, true)};
static constexpr std::array CallFields{scalar("function", String),
                                       scalar("site", Integer, true),
                                       array("args", NullableBooleanElement)};
static constexpr std::array CallElement{object({}, CallFields)};
static constexpr std::array ImportFields{
    scalar("name", String), object("declared", DeclaredFields),
    object("location", LocationFields, true), array("calls", CallElement)};
static constexpr std::array ImportElement{object({}, ImportFields)};

// payload.slots, payload.slotRules, payload.slotKinds and payload.invariants
static constexpr std::array SlotFields{
    scalar("slot", String), array("targets", StringElement),
    array("sources", StringElement), scalar("open", String, true)};
static constexpr std::array SlotElement{object({}, SlotFields)};
/// The inputs of §9.3's closed-slot rules the unit contributes.
static constexpr std::array SlotRuleFields{
    scalar("unit", String), array("defined", StringElement),
    array("exported", StringElement), array("confinedRecords", StringElement),
    array("escapedStatics", StringElement)};
static constexpr std::array SlotKindFields{scalar("slot", String),
                                           scalar("kind", String),
                                           array("demotedBy", LocationElement)};
static constexpr std::array SlotKindElement{object({}, SlotKindFields)};
static constexpr std::array InvariantFields{
    scalar("struct", String),   scalar("field", String),
    scalar("template", String), scalar("verdict", String),
    scalar("relied", Boolean),  object("store", LocationFields, true)};
static constexpr std::array InvariantElement{object({}, InvariantFields)};

// The RFC 0010, 0012, 0014, 0016 and 0028 facts, carried unchanged.
static constexpr std::array MemoryRequestFields{scalar("function", String),
                                                scalar("context", String)};
static constexpr std::array MemoryRequestElement{
    object({}, MemoryRequestFields)};
static constexpr std::array CallbackRequestFields{scalar("function", String),
                                                  scalar("bindings", String)};
static constexpr std::array CallbackRequestElement{
    object({}, CallbackRequestFields)};
static constexpr std::array CallbackGlobalFields{scalar("global", String),
                                                 scalar("targets", String)};
static constexpr std::array CallbackGlobalElement{
    object({}, CallbackGlobalFields)};
static constexpr std::array ContextFields{
    array("memoryRequests", MemoryRequestElement),
    array("callbackRequests", CallbackRequestElement),
    array("callbackGlobals", CallbackGlobalElement)};
static constexpr std::array WitnessFields{
    scalar("field", String), scalar("count", String), scalar("scale", Integer),
    scalar("productType", String, true)};
static constexpr std::array WitnessElement{object({}, WitnessFields)};
static constexpr std::array PairFields{scalar("field", String),
                                       scalar("count", String)};
static constexpr std::array PairElement{object({}, PairFields)};
static constexpr std::array SizedFieldFields{
    array("witnesses", WitnessElement), array("unsizedFields", StringElement),
    array("unsizedPairs", PairElement)};
static constexpr std::array InterfaceFields{scalar("name", String),
                                            scalar("type", String, true)};
static constexpr std::array InterfaceElement{object({}, InterfaceFields)};
static constexpr std::array InterfacesFields{
    array("globals", InterfaceElement), array("objects", InterfaceElement)};

// payload.boundaries, payload.sites, payload.reported and payload.a5
static constexpr std::array BoundaryFields{
    scalar("function", String), scalar("site", Integer),
    scalar("reason", String), scalar("placeClass", String)};
static constexpr std::array BoundaryElement{object({}, BoundaryFields)};
/// `[ordinal, kind, line, column, spatial, null, temporal, assertion]`.
static constexpr std::array SiteRowPositions{
    scalar({}, Integer),      scalar({}, String),
    scalar({}, Integer),      scalar({}, Integer),
    scalar({}, String, true), scalar({}, String, true),
    scalar({}, String, true), scalar({}, String, true)};
static constexpr std::array SiteRowElement{tuple({}, SiteRowPositions)};
static constexpr std::array SiteFields{
    scalar("function", String), scalar("file", String), scalar("line", Integer),
    scalar("linkage", String), array("rows", SiteRowElement)};
static constexpr std::array SiteElement{object({}, SiteFields)};
static constexpr std::array ReportedFields{
    scalar("id", String), scalar("file", String), scalar("line", Integer),
    scalar("column", Integer)};
static constexpr std::array ReportedElement{object({}, ReportedFields)};
/// §11: what the unit's zero-initialisation lowered and left, and the
/// allocator function it defines.
static constexpr std::array A5Fields{scalar("loweredAllocations", Integer),
                                     scalar("nonLoweredAllocations", Integer),
                                     scalar("bypassedDeclarations", Integer),
                                     scalar("allocator", String, true)};

static constexpr std::array PayloadFields{
    array("functions", FunctionElement),
    array("globals", GlobalElement),
    array("imports", ImportElement),
    array("indirect", StringElement),
    array("unknown", StringElement),
    array("unknownIndirect", StringElement),
    array("slots", SlotElement),
    object("slotRules", SlotRuleFields),
    array("slotKinds", SlotKindElement),
    array("invariants", InvariantElement),
    object("contexts", ContextFields),
    array("countFields", StringElement),
    object("sizedFields", SizedFieldFields),
    array("sizedFieldLoads", StringElement),
    object("interfaces", InterfacesFields),
    array("boundaries", BoundaryElement),
    array("sites", SiteElement),
    array("reported", ReportedElement),
    scalar("definesAllocator", Boolean),
    object("a5", A5Fields)};
static constexpr FieldSpec PayloadSchema = object({}, PayloadFields);

const FieldSpec &headerSchema() noexcept {
  return HeaderSchema;
}
const FieldSpec &payloadSchema() noexcept {
  return PayloadSchema;
}

static void appendCanonical(std::string &text, const FieldSpec &spec) {
  if (!spec.name.empty()) {
    text += spec.name;
    text += ':';
  }
  const auto appendMembers = [&](char open, char close) {
    text += open;
    for (std::size_t i = 0; i < spec.members.size(); ++i) {
      if (i != 0)
        text += ',';
      appendCanonical(text, spec.members[i]);
    }
    text += close;
  };
  text += toString(spec.type);
  switch (spec.type) {
  case FieldType::String:
  case FieldType::Integer:
  case FieldType::Boolean:
    break;
  case FieldType::Object:
    appendMembers('{', '}');
    break;
  case FieldType::Array:
    appendMembers('<', '>');
    break;
  case FieldType::Tuple:
    appendMembers('(', ')');
    break;
  }
  if (spec.nullable)
    text += '?';
}

std::string schemaText() {
  // The summaries and call contexts are SummaryIO text inside strings: a new
  // summary format is a new schema too, so a record of the old one is stale
  // rather than read with its unknown lines skipped.
  std::string text = "weavec-record-schema\nsummary-format:" +
                     std::to_string(core::SummaryFormatVersion) + "\nheader:";
  appendCanonical(text, headerSchema());
  text += "\npayload:";
  appendCanonical(text, payloadSchema());
  text += '\n';
  return text;
}

std::array<std::uint8_t, DigestSize> schemaFingerprint() {
  return llvm::SHA256::hash(llvm::arrayRefFromStringRef(schemaText()));
}

//===----------------------------------------------------------------------===//
// Validation and paths
//===----------------------------------------------------------------------===//

static bool isInteger(const llvm::json::Value &value) {
  return value.getAsInteger().has_value() || value.getAsUINT64().has_value();
}

std::optional<std::string> validate(const llvm::json::Value &value,
                                    const FieldSpec &spec,
                                    std::string_view path) {
  const std::string where(path);
  const auto expected = [&](std::string_view what) {
    return where + ": expected " + std::string(what);
  };
  if (value.kind() == llvm::json::Value::Null) {
    if (spec.nullable)
      return std::nullopt;
    return expected(toString(spec.type)) + ", not null";
  }
  switch (spec.type) {
  case FieldType::String:
    if (value.kind() != llvm::json::Value::String)
      return expected("a string");
    return std::nullopt;
  case FieldType::Integer:
    if (!isInteger(value))
      return expected("an integer");
    return std::nullopt;
  case FieldType::Boolean:
    if (value.kind() != llvm::json::Value::Boolean)
      return expected("a boolean");
    return std::nullopt;
  case FieldType::Object: {
    const llvm::json::Object *members = value.getAsObject();
    if (members == nullptr)
      return expected("an object");
    for (const FieldSpec &member : spec.members) {
      const llvm::json::Value *child = members->get(member.name);
      if (child == nullptr)
        return where + ": missing key '" + std::string(member.name) + "'";
      if (auto problem =
              validate(*child, member, where + "." + std::string(member.name)))
        return problem;
    }
    // Unknown keys are never skipped silently (§13.1). Report the least one,
    // so the message does not depend on hash order.
    std::optional<std::string> unknown;
    for (const auto &entry : *members) {
      const llvm::StringRef key = entry.first;
      const bool known =
          std::ranges::any_of(spec.members, [&](const FieldSpec &member) {
            return key == llvm::StringRef(member.name);
          });
      if (!known && (!unknown || key < llvm::StringRef(*unknown)))
        unknown = key.str();
    }
    if (unknown)
      return where + ": unknown key '" + *unknown + "'";
    return std::nullopt;
  }
  case FieldType::Array: {
    const llvm::json::Array *elements = value.getAsArray();
    if (elements == nullptr)
      return expected("an array");
    for (std::size_t i = 0; i < elements->size(); ++i) {
      if (auto problem = validate((*elements)[i], spec.members.front(),
                                  where + "[" + std::to_string(i) + "]"))
        return problem;
    }
    return std::nullopt;
  }
  case FieldType::Tuple: {
    const llvm::json::Array *elements = value.getAsArray();
    if (elements == nullptr || elements->size() != spec.members.size())
      return expected("an array of " + std::to_string(spec.members.size()));
    for (std::size_t i = 0; i < elements->size(); ++i) {
      if (auto problem = validate((*elements)[i], spec.members[i],
                                  where + "[" + std::to_string(i) + "]"))
        return problem;
    }
    return std::nullopt;
  }
  }
  return expected("a known type");
}

static void collectSchemaPaths(const FieldSpec &spec, const std::string &path,
                               std::vector<std::string> &paths) {
  paths.push_back(path);
  switch (spec.type) {
  case FieldType::String:
  case FieldType::Integer:
  case FieldType::Boolean:
    return;
  case FieldType::Object:
    for (const FieldSpec &member : spec.members)
      collectSchemaPaths(member, path + "." + std::string(member.name), paths);
    return;
  case FieldType::Array:
  case FieldType::Tuple:
    for (const FieldSpec &member : spec.members)
      collectSchemaPaths(member, path + "[]", paths);
    return;
  }
}

static void sortUnique(std::vector<std::string> &paths) {
  std::ranges::sort(paths);
  const auto [first, last] = std::ranges::unique(paths);
  paths.erase(first, last);
}

std::vector<std::string> schemaPaths() {
  std::vector<std::string> paths;
  collectSchemaPaths(headerSchema(), "header", paths);
  collectSchemaPaths(payloadSchema(), "payload", paths);
  sortUnique(paths);
  return paths;
}

static void collectValuePaths(const llvm::json::Value &value,
                              const std::string &path,
                              std::vector<std::string> &paths) {
  paths.push_back(path);
  if (const llvm::json::Object *members = value.getAsObject()) {
    for (const auto &entry : *members)
      collectValuePaths(entry.second, path + "." + entry.first.str(), paths);
  } else if (const llvm::json::Array *elements = value.getAsArray()) {
    for (const llvm::json::Value &element : *elements)
      collectValuePaths(element, path + "[]", paths);
  }
}

std::vector<std::string> valuePaths(const llvm::json::Value &value,
                                    std::string_view root) {
  std::vector<std::string> paths;
  collectValuePaths(value, std::string(root), paths);
  sortUnique(paths);
  return paths;
}

/// Emits a validated `value` with object keys in table order.
static void emitValue(llvm::json::OStream &json, const llvm::json::Value &value,
                      const FieldSpec &spec) {
  if (value.kind() == llvm::json::Value::Null) {
    json.value(nullptr);
    return;
  }
  switch (spec.type) {
  case FieldType::String:
  case FieldType::Integer:
  case FieldType::Boolean:
    json.value(value);
    return;
  case FieldType::Object:
    json.object([&] {
      const llvm::json::Object &members = *value.getAsObject();
      for (const FieldSpec &member : spec.members) {
        json.attributeBegin(member.name);
        emitValue(json, *members.get(member.name), member);
        json.attributeEnd();
      }
    });
    return;
  case FieldType::Array:
    json.array([&] {
      for (const llvm::json::Value &element : *value.getAsArray())
        emitValue(json, element, spec.members.front());
    });
    return;
  case FieldType::Tuple:
    json.array([&] {
      const llvm::json::Array &elements = *value.getAsArray();
      for (std::size_t i = 0; i < elements.size(); ++i)
        emitValue(json, elements[i], spec.members[i]);
    });
    return;
  }
}

static std::string render(const llvm::json::Value &value,
                          const FieldSpec &spec) {
  std::string text;
  llvm::raw_string_ostream os(text);
  {
    llvm::json::OStream json(os);
    emitValue(json, value, spec);
  }
  return text;
}

//===----------------------------------------------------------------------===//
// The header and the empty payload
//===----------------------------------------------------------------------===//

/// JSON strings must be valid UTF-8; paths and arguments need not be.
static std::string utf8(llvm::StringRef text) {
  return llvm::json::isUTF8(text) ? text.str() : llvm::json::fixUTF8(text);
}

llvm::json::Value toJson(const RecordHeader &header) {
  llvm::json::Object producer;
  producer["name"] = utf8(header.producer.name);
  producer["version"] = utf8(header.producer.version);
  producer["revision"] = utf8(header.producer.revision);
  llvm::json::Array command;
  command.reserve(header.command.size());
  for (const std::string &argument : header.command)
    command.push_back(utf8(argument));
  llvm::json::Object config;
  config["checks"] = std::string(core::toString(header.config.checks));
  config["zeroInit"] = header.config.zeroInit;
  config["require"] = std::string(core::toString(header.config.require));
  config["budget"] = header.config.budget;
  llvm::json::Object described;
  described["path"] = utf8(header.object.path);
  described["digest"] = utf8(header.object.digest);
  llvm::json::Object fields;
  fields["producer"] = std::move(producer);
  fields["source"] = utf8(header.source);
  fields["cwd"] = utf8(header.cwd);
  fields["command"] = std::move(command);
  fields["target"] = utf8(header.target);
  fields["config"] = std::move(config);
  fields["object"] = std::move(described);
  return fields;
}

std::optional<RecordHeader> headerFromJson(const llvm::json::Value &value,
                                           std::string *error) {
  const auto fail = [&](std::string message) -> std::optional<RecordHeader> {
    if (error != nullptr)
      *error = std::move(message);
    return std::nullopt;
  };
  if (auto problem = validate(value, headerSchema(), "header"))
    return fail(*problem);
  const llvm::json::Object &fields = *value.getAsObject();
  const auto text = [](const llvm::json::Object &members, llvm::StringRef key) {
    return members.getString(key)->str();
  };
  RecordHeader header;
  const llvm::json::Object &producer = *fields.getObject("producer");
  header.producer = core::Producer{.name = text(producer, "name"),
                                   .version = text(producer, "version"),
                                   .revision = text(producer, "revision")};
  header.source = text(fields, "source");
  header.cwd = text(fields, "cwd");
  for (const llvm::json::Value &argument : *fields.getArray("command"))
    header.command.push_back(argument.getAsString()->str());
  header.target = text(fields, "target");
  const llvm::json::Object &config = *fields.getObject("config");
  const auto checks = core::parseChecksMode(*config.getString("checks"));
  if (!checks)
    return fail("header.config.checks: unknown mode '" +
                text(config, "checks") + "'");
  const auto require = core::parseRequireLevel(*config.getString("require"));
  if (!require)
    return fail("header.config.require: unknown level '" +
                text(config, "require") + "'");
  const llvm::json::Value &budget = *config.get("budget");
  std::optional<std::uint64_t> budgetValue = budget.getAsUINT64();
  if (!budgetValue) {
    const std::optional<std::int64_t> signedBudget = budget.getAsInteger();
    if (!signedBudget || *signedBudget < 0)
      return fail("header.config.budget: expected a non-negative integer");
    budgetValue = static_cast<std::uint64_t>(*signedBudget);
  }
  header.config = core::LedgerConfig{.checks = *checks,
                                     .zeroInit = *config.getBoolean("zeroInit"),
                                     .require = *require,
                                     .budget = *budgetValue};
  const llvm::json::Object &described = *fields.getObject("object");
  header.object = RecordObject{.path = text(described, "path"),
                               .digest = text(described, "digest")};
  return header;
}

static llvm::json::Value defaultValue(const FieldSpec &spec) {
  if (spec.nullable)
    return nullptr;
  switch (spec.type) {
  case FieldType::String:
    return "";
  case FieldType::Integer:
    return 0;
  case FieldType::Boolean:
    return false;
  case FieldType::Object: {
    llvm::json::Object members;
    for (const FieldSpec &member : spec.members)
      members[llvm::StringRef(member.name)] = defaultValue(member);
    return members;
  }
  case FieldType::Array:
    return llvm::json::Array();
  case FieldType::Tuple: {
    llvm::json::Array positions;
    for (const FieldSpec &member : spec.members)
      positions.push_back(defaultValue(member));
    return positions;
  }
  }
  return nullptr;
}

llvm::json::Object emptyPayload() {
  llvm::json::Value payload = defaultValue(payloadSchema());
  return std::move(*payload.getAsObject());
}

//===----------------------------------------------------------------------===//
// Framing
//===----------------------------------------------------------------------===//

static void appendLittleEndian(std::string &bytes, std::uint64_t value,
                               std::size_t size) {
  for (std::size_t i = 0; i < size; ++i)
    bytes += static_cast<char>((value >> (8 * i)) & 0xFFU);
}

static std::uint64_t readLittleEndian(llvm::StringRef bytes, std::size_t offset,
                                      std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i)
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes[offset + i]))
             << (8 * i);
  return value;
}

template <std::size_t N>
static void appendBytes(std::string &bytes,
                        const std::array<std::uint8_t, N> &data) {
  for (const std::uint8_t byte : data)
    bytes += static_cast<char>(byte);
}

template <std::size_t N>
static bool sameBytes(llvm::StringRef bytes,
                      const std::array<std::uint8_t, N> &data) {
  return bytes.size() == N &&
         std::equal(data.begin(), data.end(), bytes.bytes_begin());
}

std::optional<std::string> encode(const UnitRecord &record,
                                  std::string *error) {
  const auto fail = [&](std::string message) -> std::optional<std::string> {
    if (error != nullptr)
      *error = std::move(message);
    return std::nullopt;
  };
  const llvm::json::Value header = toJson(record.header);
  const llvm::json::Value payload(llvm::json::Object(record.payload));
  if (auto problem = validate(header, headerSchema(), "header"))
    return fail(*problem);
  if (auto problem = validate(payload, payloadSchema(), "payload"))
    return fail(*problem);
  const std::string headerText = render(header, headerSchema());
  const std::string payloadText = render(payload, payloadSchema());
  std::string bytes;
  bytes.reserve(PrefixSize + headerText.size() + payloadText.size() +
                DigestSize);
  appendBytes(bytes, Magic);
  appendLittleEndian(bytes, FormatVersion, 4);
  appendLittleEndian(bytes, 0, 4);
  appendBytes(bytes, schemaFingerprint());
  appendLittleEndian(bytes, headerText.size(), 8);
  appendLittleEndian(bytes, payloadText.size(), 8);
  bytes += headerText;
  bytes += payloadText;
  appendBytes(bytes, llvm::SHA256::hash(llvm::arrayRefFromStringRef(bytes)));
  return bytes;
}

/// Parses and checks one JSON part of a record.
static std::optional<llvm::json::Value> parsePart(llvm::StringRef text,
                                                  const FieldSpec &spec,
                                                  llvm::StringRef name,
                                                  std::string &staleReason) {
  llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(text);
  if (!parsed) {
    staleReason = name.str() + ": " + llvm::toString(parsed.takeError());
    return std::nullopt;
  }
  if (auto problem = validate(*parsed, spec, name)) {
    staleReason = std::move(*problem);
    return std::nullopt;
  }
  return std::move(*parsed);
}

std::optional<UnitRecord> decode(llvm::StringRef bytes,
                                 std::string &staleReason) {
  const auto stale = [&](std::string reason) -> std::optional<UnitRecord> {
    staleReason = std::move(reason);
    return std::nullopt;
  };
  if (bytes.size() < Magic.size() ||
      !sameBytes(bytes.take_front(Magic.size()), Magic))
    return stale("not a WeaveC record (bad magic)");
  if (bytes.size() < PrefixSize + DigestSize)
    return stale("truncated record (" + std::to_string(bytes.size()) +
                 " bytes)");
  const std::uint64_t format = readLittleEndian(bytes, 8, 4);
  if (format != FormatVersion)
    return stale("format " + std::to_string(format) + ", expected " +
                 std::to_string(FormatVersion));
  if (const std::uint64_t flags = readLittleEndian(bytes, 12, 4); flags != 0)
    return stale("unsupported flags 0x" + llvm::utohexstr(flags));
  if (!sameBytes(bytes.substr(16, DigestSize), schemaFingerprint()))
    return stale("schema fingerprint mismatch (written by another WeaveC)");
  const std::uint64_t headerSize = readLittleEndian(bytes, 48, 8);
  const std::uint64_t payloadSize = readLittleEndian(bytes, 56, 8);
  if (headerSize > bytes.size() || payloadSize > bytes.size() ||
      PrefixSize + headerSize + payloadSize + DigestSize != bytes.size())
    return stale("length mismatch (header " + std::to_string(headerSize) +
                 " and payload " + std::to_string(payloadSize) +
                 " bytes in a record of " + std::to_string(bytes.size()) + ")");
  const std::size_t body = PrefixSize + headerSize + payloadSize;
  if (!sameBytes(bytes.substr(body, DigestSize),
                 llvm::SHA256::hash(
                     llvm::arrayRefFromStringRef(bytes.take_front(body)))))
    return stale("digest mismatch");
  auto header = parsePart(bytes.substr(PrefixSize, headerSize), headerSchema(),
                          "header", staleReason);
  if (!header)
    return std::nullopt;
  auto payload = parsePart(bytes.substr(PrefixSize + headerSize, payloadSize),
                           payloadSchema(), "payload", staleReason);
  if (!payload)
    return std::nullopt;
  std::string error;
  std::optional<RecordHeader> typed = headerFromJson(*header, &error);
  if (!typed)
    return stale(error);
  return UnitRecord{.header = std::move(*typed),
                    .payload = std::move(*payload->getAsObject())};
}

//===----------------------------------------------------------------------===//
// Files
//===----------------------------------------------------------------------===//

std::string renderRecord(const UnitRecord &record) {
  const llvm::json::Value header = toJson(record.header);
  const llvm::json::Value payload = llvm::json::Object(record.payload);
  std::string text;
  llvm::raw_string_ostream os(text);
  {
    llvm::json::OStream json(os, /*IndentSize=*/2);
    json.object([&] {
      json.attribute("format", static_cast<std::int64_t>(FormatVersion));
      json.attributeBegin("header");
      emitValue(json, header, headerSchema());
      json.attributeEnd();
      json.attributeBegin("payload");
      emitValue(json, payload, payloadSchema());
      json.attributeEnd();
    });
  }
  text += '\n';
  return text;
}

core::Producer currentProducer() {
  core::Producer producer{.name = "weavec",
                          .version = WEAVEC_VERSION_STRING,
                          .revision = WEAVEC_GIT_REVISION};
  if (WEAVEC_GIT_DIRTY)
    producer.revision += "-dirty";
  return producer;
}

std::string recordPathFor(llvm::StringRef object) {
  return object.str() + ".weavec";
}

std::optional<std::string> fileDigest(llvm::StringRef path) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
      llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return std::nullopt;
  return "sha256:" + llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(
                                     (*buffer)->getBuffer())),
                                 /*LowerCase=*/true);
}

bool writeRecord(llvm::StringRef path, const UnitRecord &record,
                 std::string *error) {
  const std::optional<std::string> bytes = encode(record, error);
  return bytes && writeFileAtomically(path, *bytes, error);
}

std::optional<UnitRecord> readRecord(llvm::StringRef path,
                                     std::string &staleReason) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
      llvm::MemoryBuffer::getFile(path, /*IsText=*/false,
                                  /*RequiresNullTerminator=*/false);
  if (!buffer) {
    staleReason =
        "cannot read '" + path.str() + "': " + buffer.getError().message();
    return std::nullopt;
  }
  return decode((*buffer)->getBuffer(), staleReason);
}

} // namespace weavec::frontend::record
