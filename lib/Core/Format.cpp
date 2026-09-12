//===- Format.cpp - Bounded output formats (RFC 0024) --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Format.h"

#include <limits>

namespace weavec::core {

OutputFormat OutputFormat::parse(std::string_view text) {
  OutputFormat result;
  const auto fail = [&](const char *reason) {
    result.error = reason;
    return result;
  };
  if (text.size() > MaxFormatBytes)
    return fail("format byte limit reached");
  text = text.substr(0, text.find('\0'));
  std::size_t position = 0;
  const auto number = [&](std::uint32_t &value) {
    while (position < text.size() && text[position] >= '0' &&
           text[position] <= '9') {
      const auto digit = static_cast<unsigned>(text[position++] - '0');
      if (value > (std::numeric_limits<std::int32_t>::max() - digit) / 10)
        return false;
      value = (value * 10) + digit;
    }
    return true;
  };
  while (position < text.size()) {
    if (text[position++] != '%') {
      ++result.literalBytes;
      continue;
    }
    if (position < text.size() && text[position] == '%') {
      ++position;
      ++result.literalBytes;
      continue;
    }
    if (result.conversions.size() == MaxFormatConversions)
      return fail("format conversion limit reached");
    FormatConversion conversion;
    bool zeroPadding = false;
    while (position < text.size() &&
           std::string_view("-+ #0").find(text[position]) !=
               std::string_view::npos) {
      conversion.alternate |= text[position] == '#';
      conversion.sign |= text[position] == '+' || text[position] == ' ';
      zeroPadding |= text[position] == '0';
      ++position;
    }
    if (position < text.size() && text[position] == '*') {
      ++position;
      conversion.widthArgument = result.arguments++;
    } else if (!number(conversion.width)) {
      return fail("format width is not representable");
    }
    if (position < text.size() && text[position] == '.') {
      ++position;
      if (position < text.size() && text[position] == '*') {
        ++position;
        conversion.precisionArgument = result.arguments++;
      } else {
        std::uint32_t precision = 0;
        if (!number(precision))
          return fail("format precision is not representable");
        conversion.precision = precision;
      }
    }
    std::string_view length;
    const auto start = position;
    if (position < text.size() &&
        std::string_view("hljztL").find(text[position]) !=
            std::string_view::npos) {
      const auto first = text[position++];
      if ((first == 'h' || first == 'l') && position < text.size() &&
          text[position] == first)
        ++position;
      length = text.substr(start, position - start);
    }
    if (position == text.size())
      return fail("incomplete format conversion");
    const char code = text[position++];
    conversion.conversion = code;
    const bool signedInteger = code == 'd' || code == 'i';
    const bool unsignedInteger =
        code == 'u' || code == 'o' || code == 'x' || code == 'X';
    conversion.narrow = length == "h" || length == "hh";
    if (signedInteger || unsignedInteger) {
      if (length.empty() || length == "h" || length == "hh")
        conversion.type = signedInteger ? FormatType::Int : FormatType::UInt;
      else if (length == "l")
        conversion.type = signedInteger ? FormatType::Long : FormatType::ULong;
      else if (length == "ll")
        conversion.type =
            signedInteger ? FormatType::LongLong : FormatType::ULongLong;
      else if (length == "j")
        conversion.type =
            signedInteger ? FormatType::IntMax : FormatType::UIntMax;
      else if (length == "z")
        conversion.type =
            signedInteger ? FormatType::SignedSize : FormatType::Size;
      else if (length == "t")
        conversion.type =
            signedInteger ? FormatType::Ptrdiff : FormatType::UPtrdiff;
      else
        return fail("unsupported integer format length");
    } else if (std::string_view("aAeEfFgG").find(code) !=
               std::string_view::npos) {
      if (!length.empty() && length != "l" && length != "L")
        return fail("unsupported floating format length");
      conversion.type =
          length == "L" ? FormatType::LongDouble : FormatType::Double;
    } else if ((code == 's' || code == 'c' || code == 'p') && length.empty()) {
      if (code != 's' && (conversion.precision || conversion.precisionArgument))
        return fail("unsupported precision on character or pointer format");
      conversion.type = FormatType::Int;
      if (code == 's')
        conversion.type = FormatType::String;
      else if (code == 'p')
        conversion.type = FormatType::Pointer;
    } else {
      return fail("unsupported format conversion");
    }
    if ((code == 's' || code == 'c' || code == 'p') &&
        (conversion.alternate || conversion.sign || zeroPadding))
      return fail("unsupported format flags");
    if (conversion.alternate && (signedInteger || code == 'u'))
      return fail("unsupported alternate form");
    conversion.argument = result.arguments++;
    if (result.arguments > MaxFormatArguments)
      return fail("format argument limit reached");
    result.conversions.push_back(conversion);
  }
  return result;
}

std::string encodeFormatLiteral(std::string_view text) {
  static constexpr std::string_view Hex = "0123456789abcdef";
  std::string result = "literal:";
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    result.push_back(Hex[byte >> 4U]);
    result.push_back(Hex[byte & 15U]);
  }
  return result;
}
std::optional<std::string> decodeFormatLiteral(std::string_view text) {
  if (!text.starts_with("literal:"))
    return {};
  text.remove_prefix(8);
  if (text.size() % 2 || text.size() > MaxFormatBytes * 2)
    return {};
  static constexpr std::string_view Hex = "0123456789abcdef";
  std::string result;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const auto a = Hex.find(text[i]);
    const auto b = Hex.find(text[i + 1]);
    if (a == std::string_view::npos || b == std::string_view::npos)
      return {};
    result.push_back(static_cast<char>((a * 16) + b));
  }
  return result;
}

} // namespace weavec::core
