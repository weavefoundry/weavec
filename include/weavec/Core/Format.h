//===- Format.h - Bounded output formats (RFC 0024) ------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_FORMAT_H
#define WEAVEC_CORE_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

inline constexpr std::size_t MaxFormatBytes = 4096;
inline constexpr std::size_t MaxFormatConversions = 64;
inline constexpr unsigned MaxFormatArguments = 128;

enum class FormatType : std::uint8_t {
  Int,
  UInt,
  Long,
  ULong,
  LongLong,
  ULongLong,
  IntMax,
  UIntMax,
  SignedSize,
  Size,
  Ptrdiff,
  UPtrdiff,
  Double,
  LongDouble,
  String,
  Pointer
};

struct FormatConversion {
  char conversion = 0;
  FormatType type = FormatType::Int;
  unsigned argument = 0;
  std::uint32_t width = 0;
  std::optional<std::uint32_t> precision;
  std::optional<unsigned> widthArgument;
  std::optional<unsigned> precisionArgument;
  bool alternate = false;
  bool sign = false;
  bool narrow = false;
  friend bool operator==(const FormatConversion &,
                         const FormatConversion &) = default;
};

struct OutputFormat {
  std::size_t literalBytes = 0;
  unsigned arguments = 0;
  std::vector<FormatConversion> conversions;
  std::string error;
  [[nodiscard]] bool valid() const { return error.empty(); }
  [[nodiscard]] static OutputFormat parse(std::string_view text);
};
[[nodiscard]] std::string encodeFormatLiteral(std::string_view text);
[[nodiscard]] std::optional<std::string>
decodeFormatLiteral(std::string_view text);

} // namespace weavec::core
#endif // WEAVEC_CORE_FORMAT_H
