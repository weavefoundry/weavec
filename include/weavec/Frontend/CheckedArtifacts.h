//===- CheckedArtifacts.h - Bind proofs to build inputs (RFC 0018) -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_FRONTEND_CHECKEDARTIFACTS_H
#define WEAVEC_FRONTEND_CHECKEDARTIFACTS_H
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace weavec::frontend {
struct UnitRecord;
[[nodiscard]] std::string checkedDigest(std::string_view bytes);
[[nodiscard]] std::optional<std::string>
checkedFileDigest(std::string_view path);
[[nodiscard]] std::string
checkedCommandDigest(const std::vector<std::string> &command);
[[nodiscard]] bool validateCheckedArtifact(const UnitRecord &record,
                                           std::string_view object,
                                           std::string &error);
} // namespace weavec::frontend
#endif // WEAVEC_FRONTEND_CHECKEDARTIFACTS_H
