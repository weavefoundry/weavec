//===- CheckedArtifacts.cpp - Checked artifact identity (RFC 0018) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/CheckedArtifacts.h"

#include "weavec/Frontend/Sidecar.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#include <filesystem>

namespace weavec::frontend {
std::string checkedDigest(std::string_view bytes) {
  llvm::SHA256 hash;
  hash.update(bytes);
  return llvm::toHex(hash.final(), true);
}
std::optional<std::string> checkedFileDigest(std::string_view path) {
  const auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return std::nullopt;
  return checkedDigest((*buffer)->getBuffer());
}
std::string checkedCommandDigest(const std::vector<std::string> &command) {
  std::string bytes;
  for (const auto &arg : command) {
    bytes += std::to_string(arg.size()) + ':';
    bytes += arg;
  }
  return checkedDigest(bytes);
}
bool validateCheckedArtifact(const UnitRecord &record, std::string_view object,
                             std::string &error,
                             std::string_view preprocessing) {
  const auto fail = [&](const std::string &reason) {
    error = "checked artifact '" + std::string(object) + "': " + reason;
    return false;
  };
  if (record.objectDigest.empty() || record.commandDigest.empty() ||
      record.exports.checkedInputs.empty() || record.command.empty())
    return fail("missing build binding; rebuild with this WeaveC version");
  if (checkedFileDigest(object) != record.objectDigest)
    return fail("object contents changed; rebuild the object");
  if (checkedCommandDigest(record.command) != record.commandDigest)
    return fail("recorded compiler command changed; rebuild the object");
  for (const auto &[file, digest] : record.exports.checkedInputs) {
    auto path = std::filesystem::path(file);
    if (path.is_relative())
      path = std::filesystem::path(record.workingDirectory) / path;
    if (checkedFileDigest(path.string()) != digest)
      return fail("source input changed or is unavailable: " + file);
  }
  if (record.preprocessingDigest.empty() || preprocessing.empty())
    return fail("missing or unverifiable preprocessing binding; rebuild with "
                "supported inputs");
  if (record.preprocessingDigest != preprocessing)
    return fail("preprocessing changed; rebuild the object");
  return true;
}
} // namespace weavec::frontend
