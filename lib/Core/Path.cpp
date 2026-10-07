//===- Path.cpp - Places relative to a function's interface ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Path.h"

#include <algorithm>

namespace weavec::core {

SummaryPath SummaryPath::deref() const {
  SummaryPath result = *this;
  result.steps.pushBack(PathElem{.step = PathStep::Deref, .field = {}});
  return result;
}

SummaryPath SummaryPath::field(std::string_view name) const {
  SummaryPath result = *this;
  result.steps.pushBack(
      PathElem{.step = PathStep::Field, .field = std::string(name)});
  return result;
}

SummaryPath SummaryPath::indexed(std::string_view selector) const {
  // Indexing every element of an index or a dereference collapses onto it.
  if (selector.empty() && !steps.empty() &&
      ((steps.back().step == PathStep::Index && steps.back().field.empty()) ||
       steps.back().step == PathStep::Deref))
    return *this;
  SummaryPath result = *this;
  result.steps.pushBack(
      PathElem{.step = PathStep::Index, .field = std::string(selector)});
  return result;
}

bool SummaryPath::isProperPrefixOf(const SummaryPath &other) const {
  if (root != other.root || index != other.index ||
      steps.size() >= other.steps.size())
    return false;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (steps[i] != other.steps[i])
      return false;
  }
  return true;
}

std::string SummaryPath::toString(std::string_view rootName) const {
  std::string name(rootName);
  std::size_t i = 0;
  while (i < steps.size()) {
    switch (steps[i].step) {
    case PathStep::Deref:
      if (i + 1 < steps.size() && steps[i + 1].step == PathStep::Index &&
          !steps[i + 1].field.empty()) {
        name += "[" + steps[i + 1].field + "]";
        i += 2;
        continue;
      }
      // `(*p).f` is spelled `p->f`; a trailing or non-field-followed deref
      // is spelled `*p`.
      if (i + 1 < steps.size() && steps[i + 1].step == PathStep::Field) {
        name += "->" + steps[i + 1].field;
        i += 2;
        continue;
      }
      name.insert(0, 1, '*');
      break;
    case PathStep::Field:
      name += "." + steps[i].field;
      break;
    case PathStep::Index:
      name += "[" + (steps[i].field.empty() ? "*" : steps[i].field) + "]";
      break;
    }
    ++i;
  }
  return name;
}

} // namespace weavec::core
