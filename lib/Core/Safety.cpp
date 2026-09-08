//===- Safety.cpp - Checked obligations and positive evidence ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Safety.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>

namespace weavec::core {

static constexpr std::array<std::string_view, 5> Outcomes{
    "proven", "required", "trusted", "unresolved", "violation"};
static constexpr std::array<std::string_view, 9> Properties{
    "bounds", "validity",  "initialization", "release", "aliasing",
    "call",   "semantics", "arithmetic",     "resource"};

std::string_view toString(SafetyOutcome value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  return index < Outcomes.size() ? std::span(Outcomes)[index] : "unresolved";
}
std::string_view toString(SafetyProperty value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  return index < Properties.size() ? std::span(Properties)[index] : "semantics";
}
std::optional<SafetyOutcome> parseSafetyOutcome(std::string_view value) {
  for (std::size_t i = 0; i < Outcomes.size(); ++i)
    if (Outcomes.at(i) == value)
      return static_cast<SafetyOutcome>(i);
  return std::nullopt;
}
std::optional<SafetyProperty> parseSafetyProperty(std::string_view value) {
  for (std::size_t i = 0; i < Properties.size(); ++i)
    if (Properties.at(i) == value)
      return static_cast<SafetyProperty>(i);
  return std::nullopt;
}

std::string SafetyObligation::identity() const {
  // Length-delimited JSON strings cannot collide on embedded separators.
  return safetyJsonString(location.file) + ":" + std::to_string(location.line) +
         ":" + std::to_string(location.column) + ":" +
         safetyJsonString(function) + ":" + std::string(toString(property)) +
         ":" + safetyJsonString(subject);
}

void SafetyLedger::add(SafetyObligation obligation) {
  obligation.location.opaque = 0;
  for (auto &call : obligation.calls)
    call.opaque = 0;
  if (obligation.calls.size() > MaxSafetyCallDepth) {
    obligation.calls.resize(MaxSafetyCallDepth);
    exhausted = true;
  }
  const auto bound = [&](std::string &value) {
    if (value.size() > 65536) {
      value.resize(65536);
      exhausted = true;
    }
  };
  bound(obligation.location.file);
  bound(obligation.function);
  bound(obligation.subject);
  bound(obligation.reason);
  const auto provenance = [](const SafetyObligation &entry) {
    std::string result;
    for (const auto &call : entry.calls)
      result += safetyJsonString(call.file) + ":" + std::to_string(call.line) +
                ":" + std::to_string(call.column) + ";";
    return result;
  };
  const auto key = obligation.identity();
  const auto found = obligations.find(key);
  if (found == obligations.end()) {
    if (obligations.size() == MaxSafetyObligations) {
      exhausted = true;
      return;
    }
    obligations.emplace(key, std::move(obligation));
  } else if (obligation.outcome > found->second.outcome ||
             (obligation.outcome == found->second.outcome &&
              (obligation.reason < found->second.reason ||
               (obligation.reason == found->second.reason &&
                provenance(obligation) < provenance(found->second))))) {
    found->second = std::move(obligation);
  }
}

void SafetyLedger::join(const SafetyLedger &other) {
  exhausted |= other.exhausted;
  for (const auto &[key, obligation] : other.obligations) {
    (void)key;
    add(obligation);
  }
}
bool SafetyLedger::complete() const {
  return !exhausted && std::ranges::none_of(obligations, [](const auto &entry) {
    return entry.second.outcome >= SafetyOutcome::Unresolved;
  });
}
bool SafetyLedger::violated() const {
  return std::ranges::any_of(obligations, [](const auto &entry) {
    return entry.second.outcome == SafetyOutcome::Violation;
  });
}
bool SafetyLedger::trusted() const {
  return std::ranges::any_of(obligations, [](const auto &entry) {
    return entry.second.outcome == SafetyOutcome::Trusted;
  });
}

void SafetyState::initialize(PlaceId storage, InitializedRange range) {
  if (range.begin == range.end)
    return;
  auto &ranges = memory[storage];
  if (std::ranges::find(ranges, range) != ranges.end())
    return;
  if (range.begin.isConstant() && range.end.isConstant()) {
    if (range.begin.constant > range.end.constant)
      return;
    // Coalesce only known adjacent/overlapping intervals, never gaps.
    std::erase_if(ranges, [&](const InitializedRange &old) {
      if (!old.begin.isConstant() || !old.end.isConstant() ||
          old.end.constant < range.begin.constant ||
          range.end.constant < old.begin.constant)
        return false;
      range.begin.constant = std::min(range.begin.constant, old.begin.constant);
      range.end.constant = std::max(range.end.constant, old.end.constant);
      return true;
    });
  }
  // Losing initialization evidence is safe; the later read stays unresolved.
  if (ranges.size() < MaxInitializedRanges) {
    ranges.push_back(range);
    std::ranges::sort(ranges);
  }
}
void SafetyState::copyMemory(PlaceId source, PlaceId destination) {
  if (source == destination)
    return;
  const auto it = memory.find(source);
  if (it == memory.end())
    memory.erase(destination);
  else
    memory[destination] = it->second;
}
void SafetyState::forget(PlaceId place) {
  initialized.erase(place);
  pointers.erase(place);
  deferred.erase(place);
  memory.erase(place);
}
bool SafetyState::join(const SafetyState &other) {
  const auto before = *this;
  deferred.insert(other.deferred.begin(), other.deferred.end());
  std::erase_if(initialized,
                [&](PlaceId p) { return !other.initialized.contains(p); });
  std::erase_if(pointers,
                [&](PlaceId p) { return !other.pointers.contains(p); });
  for (auto it = memory.begin(); it != memory.end();) {
    const auto theirs = other.memory.find(it->first);
    if (theirs == other.memory.end()) {
      it = memory.erase(it);
      continue;
    }
    std::vector<InitializedRange> common;
    for (const auto &a : it->second) {
      for (const auto &b : theirs->second) {
        if (a == b) {
          common.push_back(a);
        } else if (a.begin.isConstant() && a.end.isConstant() &&
                   b.begin.isConstant() && b.end.isConstant()) {
          const auto first = std::max(a.begin.constant, b.begin.constant);
          const auto last = std::min(a.end.constant, b.end.constant);
          if (first < last)
            common.push_back({.begin = Affine::ofConstant(first),
                              .end = Affine::ofConstant(last)});
        }
      }
    }
    std::ranges::sort(common);
    common.erase(std::ranges::unique(common).begin(), common.end());
    if (common.size() > MaxInitializedRanges)
      common.resize(MaxInitializedRanges);
    if (common.empty()) {
      it = memory.erase(it);
    } else {
      it->second = std::move(common);
      ++it;
    }
  }
  return *this != before;
}

std::string safetyJsonString(std::string_view value) {
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
  return result + '"';
}

} // namespace weavec::core
