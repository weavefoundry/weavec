//===- AnalysisStats.cpp - Invocation statistics output (RFC 0020) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/AnalysisStats.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>
#include <string>
#include <string_view>

namespace weavec::frontend {

/// A JSON string: valid UTF-8 sequences are kept, quotes and backslashes
/// escaped, and control bytes and invalid UTF-8 written as `\u00XX`.
static std::string jsonString(std::string_view value) {
  static constexpr std::string_view Hex = "0123456789abcdef";
  std::string result = "\"";
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto byte = static_cast<unsigned char>(value[i]);
    if (byte >= 0x80) {
      unsigned length = 0;
      if (byte >= 0xc2 && byte <= 0xdf)
        length = 2;
      else if (byte >= 0xe0 && byte <= 0xef)
        length = 3;
      else if (byte >= 0xf0 && byte <= 0xf4)
        length = 4;
      bool valid = length != 0 && i + length <= value.size();
      for (unsigned j = 1; valid && j < length; ++j)
        valid = (static_cast<unsigned char>(value[i + j]) & 0xc0U) == 0x80U;
      if (valid) {
        const auto next = static_cast<unsigned char>(value[i + 1]);
        valid = (byte != 0xe0 || next >= 0xa0) &&
                (byte != 0xed || next < 0xa0) &&
                (byte != 0xf0 || next >= 0x90) && (byte != 0xf4 || next < 0x90);
      }
      if (valid) {
        result.append(value.substr(i, length));
        i += length - 1;
        continue;
      }
    }
    if (byte == '"' || byte == '\\') {
      result += '\\';
      result += value[i];
    } else if (byte < 0x20 || byte >= 0x80) {
      result += "\\u00";
      result += Hex[byte >> 4U];
      result += Hex[byte & 15U];
    } else {
      result += value[i];
    }
  }
  result += '"';
  return result;
}

bool writeAtomicText(std::string_view path, std::string_view text) {
  llvm::SmallString<256> temporary;
  int descriptor = -1;
  auto error = llvm::sys::fs::createUniqueFile(
      std::string(path) + ".tmp-%%%%%%", descriptor, temporary);
  if (!error) {
    llvm::raw_fd_ostream stream(descriptor, true);
    stream << text;
    stream.close();
    if (stream.has_error()) {
      error = stream.error();
      stream.clear_error();
    }
    if (!error)
      error = llvm::sys::fs::rename(temporary, path);
  }
  if (error && !temporary.empty())
    std::ignore = llvm::sys::fs::remove(temporary);
  return !error;
}

bool writeAnalysisStats(std::string_view path, const core::AnalysisStats *stats,
                        bool final) {
  if (path.empty())
    return true;
  std::ostringstream out;
  out << R"({"version":1,"counters":{)";
  bool first = true;
  if (stats)
    for (const auto &[name, value] : stats->counters) {
      if (!first)
        out << ',';
      first = false;
      out << jsonString(name) << ':' << value;
    }
  out << "},\"nanoseconds\":{";
  first = true;
  if (stats)
    for (const auto &[name, value] : stats->nanoseconds) {
      if (!first)
        out << ',';
      first = false;
      out << jsonString(name) << ':' << value;
    }
  out << "},\"final\":" << (final ? "true" : "false") << "}\n";
  if (writeAtomicText(path, out.str()))
    return true;
  llvm::errs() << "weavec: error: cannot write analysis statistics '" << path
               << "'\n";
  return false;
}
} // namespace weavec::frontend
