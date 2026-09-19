//===- UnitRecord.h - The format-28 unit record (RFC 0030) -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §13.1: the unit record `weavec-cc` writes to `<object>.weavec`.
// The file holds exactly one self-delimiting record, so that RFC 0032 can
// place it verbatim into an object section:
//
//   offset    size  field
//   0         8     magic 89 57 56 43 0D 0A 1A 0A ("\x89WVC\r\n\x1a\n")
//   8         4     format, little-endian u32 = 28
//   12        4     flags, u32 = 0 (readers reject non-zero)
//   16        32    schema fingerprint (below)
//   48        8     header length H, u64
//   56        8     payload length P, u64
//   64        H     header: UTF-8 JSON
//   64+H      P     payload: UTF-8 JSON
//   64+H+P    32    SHA-256 of bytes [0, 64+H+P)
//
// The codec's *field table* declares every key of the header and the
// payload, its type and its nesting. The encoder and the decoder both walk
// it: the encoder refuses a value the table does not describe and emits
// keys in table order, and the decoder rejects missing, unknown and
// mistyped keys. The schema fingerprint is the SHA-256 of the table's
// canonical text, so any schema change changes it, and records written
// under another schema are stale. Anything a reader cannot accept is a
// *stale record*, with a reason, and the input is treated as having none;
// there are no legacy readers.
//
// The header is typed (`RecordHeader`). The payload is carried as a
// generic `llvm::json::Object` checked against the table; S8 fills in its
// producers and consumers.
//
// The namespace keeps these names apart from the RFC 0005 sidecar's
// `frontend::UnitRecord` until S8 deletes it.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_UNITRECORD_H
#define WEAVEC_FRONTEND_UNITRECORD_H

#include "weavec/Core/Ledger.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::frontend::record {

inline constexpr std::array<std::uint8_t, 8> Magic{0x89, 'W',  'V',  'C',
                                                   '\r', '\n', 0x1A, '\n'};
inline constexpr std::uint32_t FormatVersion = 28;
/// Bytes before the header: magic, format, flags, schema fingerprint and the
/// two lengths.
inline constexpr std::size_t PrefixSize = 64;
/// SHA-256 sizes: the schema fingerprint and the trailing digest.
inline constexpr std::size_t DigestSize = 32;

/// The JSON type of one field of the table.
enum class FieldType : std::uint8_t {
  String,
  Integer,
  Boolean,
  /// Named members, all required, no others.
  Object,
  /// Any number of elements of one spec.
  Array,
  /// A fixed number of positions, one spec each (the compact `sites` rows).
  Tuple,
};

[[nodiscard]] std::string_view toString(FieldType type) noexcept;

/// One node of the field table.
struct FieldSpec {
  /// The key in the enclosing object; empty for array elements, tuple
  /// positions and the two roots.
  std::string_view name;
  FieldType type = FieldType::String;
  /// Null is also accepted.
  bool nullable = false;
  /// Object: its members in order. Array: exactly one element spec. Tuple:
  /// one spec per position.
  std::span<const FieldSpec> members;
};

/// The roots of the field table.
[[nodiscard]] const FieldSpec &headerSchema() noexcept;
[[nodiscard]] const FieldSpec &payloadSchema() noexcept;

/// The canonical text of the field table: each field as
/// `<name>:<type>[?]`, objects as `object{...}`, arrays as `array<...>`,
/// tuples as `tuple(...)`, members separated by commas, `?` for nullable.
[[nodiscard]] std::string schemaText();
/// SHA-256 of `schemaText()`: bytes 16 to 48 of every record.
[[nodiscard]] std::array<std::uint8_t, DigestSize> schemaFingerprint();

/// Checks `value` against `spec`. The error names the offending path, for
/// example `payload.functions[2]: missing key 'typeKey'`.
[[nodiscard]] std::optional<std::string>
validate(const llvm::json::Value &value, const FieldSpec &spec,
         std::string_view path);

/// Every node path the table describes, sorted: `header.producer.name`,
/// `payload.functions[].kinds.params[]`. Array elements and tuple positions
/// both read `[]`.
[[nodiscard]] std::vector<std::string> schemaPaths();
/// Every node path present in `value`, rooted at `root`, sorted and in the
/// notation of `schemaPaths`.
[[nodiscard]] std::vector<std::string>
valuePaths(const llvm::json::Value &value, std::string_view root);

/// `object: {path, digest}`: the object the record describes, and
/// `sha256:<hex>` of its bytes.
struct RecordObject {
  std::string path;
  std::string digest;

  friend bool operator==(const RecordObject &, const RecordObject &) = default;
};

/// The header (§13.1).
struct RecordHeader {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::Producer producer = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string source = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string cwd = {};
  /// The `-cc1` arguments.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<std::string> command = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string target = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  core::LedgerConfig config = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  RecordObject object = {};

  friend bool operator==(const RecordHeader &, const RecordHeader &) = default;
};

[[nodiscard]] llvm::json::Value toJson(const RecordHeader &header);
/// Reads a header that `validate` accepted; fails on an unknown `checks` or
/// `require` spelling.
[[nodiscard]] std::optional<RecordHeader>
headerFromJson(const llvm::json::Value &value, std::string *error = nullptr);

struct UnitRecord {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  RecordHeader header = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  llvm::json::Object payload = {};
};

/// A payload with every key the table requires and nothing in it: empty
/// arrays, `false`, zero, empty strings, and null where allowed.
[[nodiscard]] llvm::json::Object emptyPayload();

/// The record's bytes, or none (with `error` set) when the header or the
/// payload does not match the field table.
[[nodiscard]] std::optional<std::string> encode(const UnitRecord &record,
                                                std::string *error = nullptr);
/// The record in `bytes`, or none with `staleReason` saying why the record
/// is stale: a bad magic, another format, non-zero flags, another schema,
/// wrong lengths, a wrong digest, or JSON the table does not describe.
[[nodiscard]] std::optional<UnitRecord> decode(llvm::StringRef bytes,
                                               std::string &staleReason);

/// `<object>.weavec`.
[[nodiscard]] std::string recordPathFor(llvm::StringRef object);
/// `sha256:<hex>` of a file's bytes, for `object.digest`.
[[nodiscard]] std::optional<std::string> fileDigest(llvm::StringRef path);
/// Encodes `record` and writes it atomically to `path`.
bool writeRecord(llvm::StringRef path, const UnitRecord &record,
                 std::string *error = nullptr);
/// Reads and decodes the record at `path`; an unreadable file is stale too.
[[nodiscard]] std::optional<UnitRecord> readRecord(llvm::StringRef path,
                                                   std::string &staleReason);

} // namespace weavec::frontend::record

#endif // WEAVEC_FRONTEND_UNITRECORD_H
