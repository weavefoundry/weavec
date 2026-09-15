//===- Footprint.cpp - Exact footprint relations (RFC 0027) ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Footprint.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>
#include <utility>

namespace weavec::core {

static bool normalize(FootprintSum &value, FootprintSum &origin) {
  std::int64_t divisor = 0;
  for (auto *row : {&value, &origin}) {
    std::erase_if(*row, [](const auto &entry) { return entry.second == 0; });
    for (const auto &[place, coefficient] : *row) {
      (void)place;
      // gcd's absolute-value precondition excludes the minimum signed value.
      if (coefficient == std::numeric_limits<std::int64_t>::min())
        return false;
      divisor = std::gcd(divisor, coefficient);
    }
  }
  if (divisor == 0)
    return true;
  if ((!value.empty() ? value.begin()->second : origin.begin()->second) < 0)
    divisor = -divisor;
  for (auto *row : {&value, &origin})
    for (auto &[place, coefficient] : *row) {
      (void)place;
      coefficient /= divisor;
    }
  return true;
}

static bool combine(FootprintSum &row, std::int64_t scale,
                    const FootprintSum &subtract, std::int64_t factor) {
  FootprintSum result;
  for (const auto &[place, coefficient] : row)
    if (__builtin_mul_overflow(coefficient, scale, &result[place]))
      return false;
  for (const auto &[place, coefficient] : subtract) {
    std::int64_t product = 0;
    if (__builtin_mul_overflow(coefficient, factor, &product) ||
        __builtin_sub_overflow(result[place], product, &result[place]))
      return false;
  }
  row = std::move(result);
  return true;
}

static bool eliminate(FootprintSum &value, FootprintSum &origin,
                      const FootprintSum &pivot,
                      const FootprintSum &pivotOrigin, PlaceId column) {
  const auto term = value.find(column);
  if (term == value.end())
    return true;
  const auto a = term->second;
  const auto b = pivot.at(column);
  if (a == std::numeric_limits<std::int64_t>::min() ||
      b == std::numeric_limits<std::int64_t>::min())
    return false;
  const auto divisor = std::gcd(a, b);
  return combine(value, b / divisor, pivot, a / divisor) &&
         combine(origin, b / divisor, pivotOrigin, a / divisor) &&
         normalize(value, origin);
}

static bool reduce(FootprintSum &value,
                   const std::vector<FootprintSum> &basis) {
  FootprintSum ignored;
  if (!normalize(value, ignored))
    return false;
  for (const auto &pivot : basis)
    if (!eliminate(value, ignored, pivot, {}, pivot.begin()->first))
      return false;
  return true;
}

void FootprintRelations::fail() {
  rows.clear();
  exhausted = true;
}

bool FootprintRelations::canonicalize() {
  std::vector<FootprintSum> basis;
  std::set<PlaceId> variables;
  for (auto row : rows) {
    if (!reduce(row, basis))
      return false;
    if (row.empty())
      continue;
    const auto column = row.begin()->first;
    for (auto &previous : basis) {
      FootprintSum ignored;
      if (!eliminate(previous, ignored, row, {}, column))
        return false;
    }
    basis.insert(std::ranges::lower_bound(
                     basis, column, {},
                     [](const auto &entry) { return entry.begin()->first; }),
                 std::move(row));
  }
  for (const auto &row : basis)
    for (const auto &[place, coefficient] : row) {
      (void)coefficient;
      variables.insert(place);
    }
  if (variables.size() > MaxFootprintVariables ||
      basis.size() > MaxFootprintRelations)
    return false;
  rows = std::move(basis);
  return true;
}

bool FootprintRelations::constrain(FootprintSum equation) {
  if (equation.size() > MaxFootprintVariables || !reduce(equation, rows)) {
    fail();
    return false;
  }
  if (equation.empty())
    return true;
  rows.push_back(std::move(equation));
  if (!canonicalize()) {
    fail();
    return false;
  }
  return true;
}

bool FootprintRelations::entails(FootprintSum equation) const {
  return equation.size() <= MaxFootprintVariables && reduce(equation, rows) &&
         equation.empty();
}

bool FootprintRelations::equal(PlaceId first, PlaceId second) const {
  return first == second || entails({{first, 1}, {second, -1}});
}

bool FootprintRelations::empty(PlaceId place) const {
  return entails({{place, 1}});
}

void FootprintRelations::forget(PlaceId place) {
  const auto found = std::ranges::find_if(
      rows, [&](const auto &row) { return row.contains(place); });
  if (found == rows.end())
    return;
  auto pivot = std::move(*found);
  rows.erase(found);
  for (auto &row : rows) {
    FootprintSum ignored;
    if (!eliminate(row, ignored, pivot, {}, place)) {
      fail();
      return;
    }
  }
  if (!canonicalize())
    fail();
}

void FootprintRelations::assign(PlaceId destination, FootprintSum value) {
  // Do not normalize an assignment's RHS by itself: 2*y is not y.
  std::erase_if(value, [](const auto &entry) { return entry.second == 0; });
  const auto self = value.find(destination);
  if (self != value.end() && (self->second == 1 || self->second == -1)) {
    const auto coefficient = self->second;
    value.erase(self);
    for (auto &row : rows) {
      const auto term = row.find(destination);
      if (term == row.end())
        continue;
      const auto factor = term->second;
      row.erase(term);
      if (!combine(row, coefficient, value, factor)) {
        fail();
        return;
      }
      row[destination] = factor;
    }
  } else {
    // Noninvertible self assignments require retaining a temporary value.
    // This consumer only needs copies, zero and unit-coefficient updates;
    // lose an unsupported self relation conservatively.
    forget(destination);
    if (self != value.end())
      return;
    for (auto &[place, coefficient] : value) {
      (void)place;
      if (coefficient == std::numeric_limits<std::int64_t>::min()) {
        fail();
        return;
      }
      coefficient = -coefficient;
    }
    value[destination] = 1;
    rows.push_back(std::move(value));
  }
  if (!canonicalize())
    fail();
}

bool FootprintRelations::join(const FootprintRelations &other) {
  if (this == &other || *this == other)
    return false;
  const auto before = *this;
  exhausted |= other.exhausted;
  if (rows.empty() || other.rows.empty()) {
    rows.clear();
    return before != *this;
  }
  // Eliminate the union of the two bases while carrying the contribution
  // from the first space. Every dependency is an element of both spaces.
  struct Row {
    FootprintSum value;
    FootprintSum first;
  };
  std::map<PlaceId, Row> basis;
  std::vector<FootprintSum> common;
  const auto insert = [&](FootprintSum value, FootprintSum first) {
    for (const auto &[column, pivot] : basis)
      if (!eliminate(value, first, pivot.value, pivot.first, column))
        return false;
    if (value.empty()) {
      if (!first.empty())
        common.push_back(std::move(first));
    } else {
      const auto column = value.begin()->first;
      basis.emplace(column,
                    Row{.value = std::move(value), .first = std::move(first)});
    }
    return true;
  };
  for (const auto &row : rows)
    if (!insert(row, row)) {
      fail();
      return before != *this;
    }
  for (const auto &row : other.rows)
    if (!insert(row, {})) {
      fail();
      return before != *this;
    }
  rows = std::move(common);
  if (!canonicalize())
    fail();
  return before != *this;
}

void FootprintRelations::clear() {
  rows.clear();
}

} // namespace weavec::core
