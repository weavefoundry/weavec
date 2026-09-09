//===- AnalysisStats.h - Analysis work accounting (RFC 0020) ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_CORE_ANALYSISSTATS_H
#define WEAVEC_CORE_ANALYSISSTATS_H

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace weavec::core {

/// Invocation-owned measurements. Never part of an abstract state or proof.
struct AnalysisStats {
  std::map<std::string, std::uint64_t, std::less<>> counters;
  std::map<std::string, std::uint64_t, std::less<>> nanoseconds;
  void add(std::string_view name, std::uint64_t count = 1) {
    counters[std::string(name)] += count;
  }
  [[nodiscard]] std::uint64_t count(std::string_view name) const {
    const auto it = counters.find(name);
    return it == counters.end() ? 0 : it->second;
  }
};

class AnalysisTimer {
public:
  AnalysisTimer(AnalysisStats *stats, std::string_view name)
      : elapsed(stats ? &stats->nanoseconds[std::string(name)] : nullptr),
        start(stats ? Clock::now() : Clock::time_point{}) {}
  ~AnalysisTimer() {
    if (elapsed)
      *elapsed += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                               start)
              .count());
  }
  AnalysisTimer(const AnalysisTimer &) = delete;
  AnalysisTimer &operator=(const AnalysisTimer &) = delete;

private:
  using Clock = std::chrono::steady_clock;
  std::uint64_t *elapsed;
  Clock::time_point start;
};

} // namespace weavec::core
#endif // WEAVEC_CORE_ANALYSISSTATS_H
