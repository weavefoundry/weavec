//===- Offset.cpp - Where a pointer points within its object --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Offset.h"

#include <charconv>

namespace weavec::core {

PointerOffset PointerOffset::plus(const PointerOffset &other) const {
  if (isZero())
    return other;
  if (other.isZero())
    return *this;
  if (isInside() || other.isInside())
    return inside();
  const auto elementLike = [](const PointerOffset &o) {
    return o.isElements() || o.isUnknown();
  };
  if (elementLike(*this) && elementLike(other) &&
      (isUnknown() || other.isUnknown()))
    return unknown();
  if (isElements() && other.isElements()) {
    // Saturate rather than wrap: an offset past what an int64 holds is
    // unknown, not negative.
    const std::int64_t sum = elements + other.elements;
    const bool overflow = (other.elements > 0 && sum < elements) ||
                          (other.elements < 0 && sum > elements);
    return overflow ? unknown() : ofElements(sum);
  }
  if (isField() && other.isField() && field == other.field &&
      negative != other.negative)
    return zero();
  return inside();
}

PointerOffset PointerOffset::negated() const {
  switch (kind) {
  case Kind::Zero:
  case Kind::Unknown:
  case Kind::Inside:
    return *this;
  case Kind::Elements:
    return elements == INT64_MIN ? unknown() : ofElements(-elements);
  case Kind::Field:
    return ofField(field, !negative);
  }
  return unknown();
}

bool PointerOffset::join(const PointerOffset &other) {
  if (*this == other || isInside())
    return false;
  // Zero is an element offset too: `if (c) p++;` leaves `p` at an unknown
  // element, still within the array `*p` stands for.
  const auto elementLike = [](const PointerOffset &o) {
    return o.isZero() || o.isElements() || o.isUnknown();
  };
  if (elementLike(*this) && elementLike(other)) {
    if (isUnknown())
      return false;
    *this = unknown();
    return true;
  }
  *this = inside();
  return true;
}

std::string PointerOffset::toString() const {
  switch (kind) {
  case Kind::Zero:
    return "0";
  case Kind::Unknown:
    return "?";
  case Kind::Inside:
    return "~";
  case Kind::Elements:
    return (elements > 0 ? "+" : "") + std::to_string(elements);
  case Kind::Field:
    return (negative ? "-" : "+") + field;
  }
  return "?";
}

std::optional<PointerOffset> PointerOffset::parse(std::string_view text) {
  if (text == "0")
    return zero();
  if (text == "?")
    return unknown();
  if (text == "~")
    return inside();
  if (text.size() < 2 || (text.front() != '+' && text.front() != '-'))
    return std::nullopt;
  const bool negative = text.front() == '-';
  const std::string_view rest = text.substr(1);
  std::int64_t count = 0;
  const auto [end, error] =
      std::from_chars(rest.data(), rest.data() + rest.size(), count);
  if (error == std::errc{} && end == rest.data() + rest.size()) {
    if (negative) {
      if (count == INT64_MIN)
        return std::nullopt;
      count = -count;
    }
    return ofElements(count);
  }
  return ofField(std::string(rest), negative);
}

} // namespace weavec::core
