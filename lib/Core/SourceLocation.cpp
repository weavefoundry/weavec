//===- SourceLocation.cpp - Frontend-neutral source positions -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/SourceLocation.h"

namespace weavec::core {

std::string SourceLocation::toString() const {
  if (!isValid())
    return "<unknown>";
  std::string result = file.empty() ? std::string("<input>") : file;
  result += ':';
  result += std::to_string(line);
  result += ':';
  result += std::to_string(column);
  return result;
}

} // namespace weavec::core
