//===- UnitRecordTest.cpp - Tests for the format-28 unit record -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/UnitRecord.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>

namespace weavec::frontend::record {

/// A value of `spec` with one element in every array and no nulls, so that
/// every key of the table appears.
static llvm::json::Value exampleValue(const FieldSpec &spec) {
  switch (spec.type) {
  case FieldType::String:
    return "x";
  case FieldType::Integer:
    return 1;
  case FieldType::Boolean:
    return true;
  case FieldType::Object: {
    llvm::json::Object members;
    for (const FieldSpec &member : spec.members)
      members[llvm::StringRef(member.name)] = exampleValue(member);
    return members;
  }
  case FieldType::Array:
    return llvm::json::Array{exampleValue(spec.members.front())};
  case FieldType::Tuple: {
    llvm::json::Array positions;
    for (const FieldSpec &member : spec.members)
      positions.push_back(exampleValue(member));
    return positions;
  }
  }
  return nullptr;
}

static UnitRecord sampleRecord() {
  UnitRecord record;
  record.header =
      RecordHeader{.producer = {.name = "weavec",
                                .version = "0.11.0",
                                .revision = "abc1234"},
                   .source = "src/cJSON.c",
                   .cwd = "/work/build",
                   .command = {"-triple", "arm64-apple-macosx15.0", "-emit-obj",
                               "-o", "cJSON.o"},
                   .target = "arm64-apple-macosx15.0",
                   .config = {.checks = core::ChecksMode::Report,
                              .zeroInit = false,
                              .require = core::RequireLevel::Checked,
                              .budget = 50000},
                   .object = {.path = "cJSON.o", .digest = "sha256:00"}};
  record.payload = emptyPayload();
  llvm::json::Object sites;
  sites["function"] = "cJSON_Delete";
  sites["file"] = "";
  sites["line"] = 253;
  sites["linkage"] = "external";
  sites["rows"] = llvm::json::Array{
      llvm::json::Array{0, "deref", 258, 21, "proven", "checked",
                        "unresolved/unknown-callee", nullptr}};
  record.payload["sites"] = llvm::json::Array{std::move(sites)};
  record.payload["unknown"] = llvm::json::Array{"blob_open"};
  record.payload["definesAllocator"] = true;
  return record;
}

static std::uint64_t readLittleEndian(llvm::StringRef bytes, std::size_t at,
                                      std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i)
    value |=
        static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at + i]))
        << (8 * i);
  return value;
}

/// The header and payload texts of a record, by the §13.1 layout.
static std::pair<std::string, std::string> parts(llvm::StringRef bytes) {
  const std::uint64_t header = readLittleEndian(bytes, 48, 8);
  const std::uint64_t payload = readLittleEndian(bytes, 56, 8);
  return {bytes.substr(PrefixSize, header).str(),
          bytes.substr(PrefixSize + header, payload).str()};
}

/// Frames raw JSON texts by the §13.1 layout, with a correct digest.
static std::string frame(llvm::StringRef header, llvm::StringRef payload,
                         std::uint32_t format = FormatVersion,
                         std::uint32_t flags = 0) {
  std::string bytes(Magic.begin(), Magic.end());
  const auto put = [&](std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
      bytes += static_cast<char>((value >> (8 * i)) & 0xFFU);
  };
  put(format, 4);
  put(flags, 4);
  const auto schema = schemaFingerprint();
  bytes.append(schema.begin(), schema.end());
  put(header.size(), 8);
  put(payload.size(), 8);
  bytes += header;
  bytes += payload;
  const auto digest = llvm::SHA256::hash(llvm::arrayRefFromStringRef(bytes));
  bytes.append(digest.begin(), digest.end());
  return bytes;
}

static std::string compact(const llvm::json::Value &value) {
  std::string text;
  llvm::raw_string_ostream os(text);
  os << value;
  return text;
}

TEST(UnitRecord, FramingFollowsTheLayout) {
  std::string error;
  const auto bytes = encode(sampleRecord(), &error);
  ASSERT_TRUE(bytes) << error;
  const llvm::StringRef view = *bytes;
  EXPECT_EQ(view.take_front(8), llvm::StringRef("\x89WVC\r\n\x1a\n", 8));
  EXPECT_EQ(readLittleEndian(view, 8, 4), 28U);
  EXPECT_EQ(readLittleEndian(view, 12, 4), 0U);
  const auto schema = schemaFingerprint();
  EXPECT_TRUE(std::equal(schema.begin(), schema.end(),
                         view.substr(16, 32).bytes_begin()));
  const auto [header, payload] = parts(view);
  EXPECT_EQ(PrefixSize + header.size() + payload.size() + DigestSize,
            view.size());
  const auto digest = llvm::SHA256::hash(
      llvm::arrayRefFromStringRef(view.drop_back(DigestSize)));
  EXPECT_TRUE(std::equal(digest.begin(), digest.end(),
                         view.take_back(DigestSize).bytes_begin()));
  // Keys follow the table.
  EXPECT_EQ(header.rfind(R"({"producer":{"name":"weavec","version":"0.11.0",)"
                         R"("revision":"abc1234"},"source":"src/cJSON.c",)"
                         R"("cwd":"/work/build","command":["-triple",)",
                         0),
            0U);
  EXPECT_NE(header.find(R"("config":{"checks":"report","zeroInit":false,)"
                        R"("require":"checked","budget":50000},)"
                        R"("object":{"path":"cJSON.o","digest":"sha256:00"}})"),
            std::string::npos);
  EXPECT_EQ(payload.rfind(R"({"functions":[],"globals":[],"imports":[],)", 0),
            0U);
  EXPECT_NE(payload.find(R"("sites":[{"function":"cJSON_Delete","file":"",)"
                         R"("line":253,"linkage":"external","rows":[[0,)"
                         R"("deref",258,21,"proven","checked",)"
                         R"("unresolved/unknown-callee",null]]}],)"
                         R"("reported":[],"definesAllocator":true,"a5":{)"),
            std::string::npos);
}

TEST(UnitRecord, RoundTrips) {
  const UnitRecord record = sampleRecord();
  std::string reason;
  const auto decoded = decode(*encode(record), reason);
  ASSERT_TRUE(decoded) << reason;
  EXPECT_EQ(decoded->header, record.header);
  EXPECT_EQ(decoded->payload, record.payload);
}

TEST(UnitRecord, EmittedKeysMatchTheFieldTable) {
  UnitRecord record = sampleRecord();
  record.payload = std::move(*exampleValue(payloadSchema()).getAsObject());
  std::string error;
  const auto bytes = encode(record, &error);
  ASSERT_TRUE(bytes) << error;
  const auto [header, payload] = parts(*bytes);
  std::vector<std::string> emitted =
      valuePaths(llvm::cantFail(llvm::json::parse(header)), "header");
  const std::vector<std::string> payloadPaths =
      valuePaths(llvm::cantFail(llvm::json::parse(payload)), "payload");
  emitted.insert(emitted.end(), payloadPaths.begin(), payloadPaths.end());
  std::ranges::sort(emitted);
  EXPECT_EQ(emitted, schemaPaths());
  for (const std::string path :
       {"header.config.budget", "payload.sites[].rows[]",
        "payload.imports[].calls[].args[]", "payload.invariants[].store.line",
        "payload.functions[].contexts.memory[].summary",
        "payload.slotRules.escapedStatics[]", "payload.a5.allocator"})
    EXPECT_TRUE(std::ranges::binary_search(schemaPaths(), path)) << path;
}

TEST(UnitRecord, SchemaFingerprintIsTheTableHash) {
  const std::string text = schemaText();
  const auto expected = llvm::SHA256::hash(llvm::arrayRefFromStringRef(text));
  EXPECT_EQ(schemaFingerprint(), expected);
  EXPECT_EQ(text.rfind("weavec-record-schema\nsummary-format:27\n"
                       "header:object{producer:object{"
                       "name:string,version:string,revision:string},"
                       "source:string,cwd:string,command:array<string>,"
                       "target:string,config:object{checks:string,"
                       "zeroInit:boolean,require:string,budget:integer},"
                       "object:object{path:string,digest:string}}\n"
                       "payload:object{functions:array<object{name:string,",
                       0),
            0U);
  EXPECT_NE(text.find("sites:array<object{function:string,file:string,"
                      "line:integer,linkage:string,rows:array<tuple("
                      "integer,string,integer,integer,string?,string?,"
                      "string?,string?)>}>"),
            std::string::npos);
  EXPECT_NE(text.find("store:object{file:string,line:integer,column:integer}?"),
            std::string::npos);
  EXPECT_TRUE(llvm::StringRef(text).ends_with(
      "definesAllocator:boolean,a5:object{loweredAllocations:integer,"
      "nonLoweredAllocations:integer,bypassedDeclarations:integer,"
      "allocator:string?}}\n"));
}

TEST(UnitRecord, AnythingElseIsStale) {
  const std::string good = *encode(sampleRecord());
  const auto [header, payload] = parts(good);
  const auto reason = [](llvm::StringRef bytes) {
    std::string why;
    EXPECT_FALSE(decode(bytes, why));
    return why;
  };
  EXPECT_EQ(reason(""), "not a WeaveC record (bad magic)");
  std::string magic = good;
  magic[1] = 'X';
  EXPECT_EQ(reason(magic), "not a WeaveC record (bad magic)");
  EXPECT_EQ(reason(llvm::StringRef(good).take_front(80)),
            "truncated record (80 bytes)");
  EXPECT_EQ(reason(frame(header, payload, 27)), "format 27, expected 28");
  EXPECT_EQ(reason(frame(header, payload, 28, 1)), "unsupported flags 0x1");
  std::string schema = good;
  schema[20] = static_cast<char>(static_cast<unsigned char>(schema[20]) ^ 1U);
  EXPECT_EQ(reason(schema),
            "schema fingerprint mismatch (written by another WeaveC)");
  EXPECT_EQ(reason(good + "x").rfind("length mismatch (header ", 0), 0U);
  std::string corrupt = good;
  corrupt[PrefixSize + 3] = static_cast<char>(
      static_cast<unsigned char>(corrupt[PrefixSize + 3]) ^ 1U);
  EXPECT_EQ(reason(corrupt), "digest mismatch");
  EXPECT_EQ(reason(frame(header, "{")).rfind("payload: ", 0), 0U);

  // JSON the table does not describe, framed with a correct digest.
  const auto edited = [&](auto edit) {
    llvm::json::Value value = llvm::cantFail(llvm::json::parse(payload));
    edit(*value.getAsObject());
    return frame(header, compact(value));
  };
  EXPECT_EQ(reason(edited([](llvm::json::Object &o) { o["extra"] = 1; })),
            "payload: unknown key 'extra'");
  EXPECT_EQ(reason(edited(
                [](llvm::json::Object &o) { o.erase("definesAllocator"); })),
            "payload: missing key 'definesAllocator'");
  EXPECT_EQ(
      reason(edited([](llvm::json::Object &o) { o["definesAllocator"] = 1; })),
      "payload.definesAllocator: expected a boolean");
  EXPECT_EQ(reason(edited([](llvm::json::Object &o) {
              (*(*o.getArray("sites"))[0].getAsObject()->getArray("rows"))[0]
                  .getAsArray()
                  ->pop_back();
            })),
            "payload.sites[0].rows[0]: expected an array of 8");
  std::string badMode = header;
  badMode.replace(badMode.find("\"report\""), 8, "\"fast\"");
  EXPECT_EQ(reason(frame(badMode, payload)),
            "header.config.checks: unknown mode 'fast'");
}

TEST(UnitRecord, EncodeRejectsWhatTheTableDoesNotDescribe) {
  std::string error;
  UnitRecord extra = sampleRecord();
  extra.payload["extra"] = 1;
  EXPECT_FALSE(encode(extra, &error));
  EXPECT_EQ(error, "payload: unknown key 'extra'");
  UnitRecord missing = sampleRecord();
  missing.payload.erase("slots");
  EXPECT_FALSE(encode(missing, &error));
  EXPECT_EQ(error, "payload: missing key 'slots'");
  UnitRecord nested = sampleRecord();
  llvm::json::Object function;
  function["name"] = "f";
  nested.payload["functions"] = llvm::json::Array{std::move(function)};
  EXPECT_FALSE(encode(nested, &error));
  EXPECT_EQ(error, "payload.functions[0]: missing key 'linkage'");
  EXPECT_FALSE(
      validate(llvm::json::Value(emptyPayload()), payloadSchema(), "payload"));
  EXPECT_EQ(emptyPayload().size(), 20U);
  const FieldSpec nullable{
      .name = "x", .type = FieldType::String, .nullable = true, .members = {}};
  EXPECT_FALSE(validate(nullptr, nullable, "x"));
  const FieldSpec required{
      .name = "x", .type = FieldType::String, .nullable = false, .members = {}};
  EXPECT_EQ(validate(nullptr, required, "x"), "x: expected string, not null");
}

TEST(UnitRecord, FilesRoundTrip) {
  llvm::SmallString<256> directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("weavec-record", directory));
  const std::string path = recordPathFor(directory.str().str() + "/a.o");
  EXPECT_TRUE(llvm::StringRef(path).ends_with("/a.o.weavec"));
  std::string error;
  ASSERT_TRUE(writeRecord(path, sampleRecord(), &error)) << error;
  std::string reason;
  const auto read = readRecord(path, reason);
  ASSERT_TRUE(read) << reason;
  EXPECT_EQ(read->header, sampleRecord().header);
  EXPECT_FALSE(readRecord(path + ".missing", reason));
  EXPECT_EQ(reason.rfind("cannot read '", 0), 0U);
  const std::string abc = directory.str().str() + "/abc";
  {
    std::error_code ec;
    llvm::raw_fd_ostream out(abc, ec);
    out << "abc";
  }
  EXPECT_EQ(fileDigest(abc), "sha256:ba7816bf8f01cfea414140de5dae2223b00361a3"
                             "96177a9cb410ff61f20015ad");
  EXPECT_FALSE(fileDigest(abc + ".missing"));
  std::ignore = llvm::sys::fs::remove_directories(directory);
}

} // namespace weavec::frontend::record
