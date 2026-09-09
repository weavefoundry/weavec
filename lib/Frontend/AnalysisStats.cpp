//===- AnalysisStats.cpp - Invocation statistics output (RFC 0020) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Frontend/AnalysisStats.h"

#include "weavec/Core/Safety.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>

namespace weavec::frontend {
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
      out << core::safetyJsonString(name) << ':' << value;
    }
  out << "},\"nanoseconds\":{";
  first = true;
  if (stats)
    for (const auto &[name, value] : stats->nanoseconds) {
      if (!first)
        out << ',';
      first = false;
      out << core::safetyJsonString(name) << ':' << value;
    }
  out << "},\"final\":" << (final ? "true" : "false") << "}\n";
  if (writeAtomicText(path, out.str()))
    return true;
  llvm::errs() << "weavec: error: cannot write analysis statistics '" << path
               << "'\n";
  return false;
}
} // namespace weavec::frontend
