//===- CallTargets.cpp - Bounded function pointer values -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/CallTargets.h"

namespace weavec::core {

bool CallTargets::join(const CallTargets &other) {
  const CallTargets before = *this;
  unknown |= other.unknown;
  null |= other.null;
  functions.insert(other.functions.begin(), other.functions.end());
  if (functions.size() > MaxCallTargets) {
    unknown = true;
    // Deterministic retained prefix: known effects still apply.
    while (functions.size() > MaxCallTargets)
      functions.erase(std::prev(functions.end()));
  }
  return *this != before;
}

std::string CallTargets::toString() const {
  std::string result = unknown ? "?" : "-";
  if (null)
    result += '0';
  constexpr std::string_view Hex = "0123456789abcdef";
  for (const auto &symbol : functions) {
    result += ':';
    for (const char character : symbol) {
      const auto byte = static_cast<unsigned char>(character);
      result += Hex[byte >> 4U];
      result += Hex[byte & 15U];
    }
  }
  return result;
}

std::optional<CallTargets> CallTargets::parse(std::string_view text) {
  if (text.empty() || (text.front() != '?' && text.front() != '-'))
    return std::nullopt;
  CallTargets result;
  result.unknown = text.front() == '?';
  text.remove_prefix(1);
  if (text.starts_with('0')) {
    result.null = true;
    text.remove_prefix(1);
  }
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  while (!text.empty()) {
    if (text.front() != ':')
      return std::nullopt;
    text.remove_prefix(1);
    const auto end = text.find(':');
    const auto token = text.substr(0, end);
    if (token.empty() || token.size() % 2 != 0)
      return std::nullopt;
    std::string symbol;
    for (std::size_t i = 0; i < token.size(); i += 2) {
      const int hi = digit(token[i]);
      const int lo = digit(token[i + 1]);
      const int byte = (hi * 16) + lo;
      if (hi < 0 || lo < 0 || byte == 0 || byte == '\n')
        return std::nullopt;
      symbol += static_cast<char>(byte);
    }
    if (!result.functions.insert(std::move(symbol)).second ||
        result.functions.size() > MaxCallTargets)
      return std::nullopt;
    if (end == std::string_view::npos)
      break;
    text.remove_prefix(end);
  }
  return result;
}

} // namespace weavec::core
