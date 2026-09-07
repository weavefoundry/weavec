//===- CallTargets.h - Bounded function pointer values --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_CALLTARGETS_H
#define WEAVEC_CORE_CALLTARGETS_H

#include <compare>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace weavec::core {

inline constexpr std::size_t MaxCallTargets = 32;
inline constexpr std::size_t MaxCallbackContexts = 32;

/// RFC 0014: function symbols reaching a pointer, including unknown/null.
/// Missing information is unknown; the empty set is only the join identity.
struct CallTargets {
  std::set<std::string> functions;
  bool unknown = false;
  bool null = false;

  static CallTargets any() { return {.functions = {}, .unknown = true}; }
  static CallTargets function(std::string symbol) {
    return {.functions = {std::move(symbol)}};
  }
  [[nodiscard]] bool empty() const {
    return functions.empty() && !unknown && !null;
  }
  [[nodiscard]] bool resolved() const {
    return !unknown && !null && !functions.empty();
  }
  bool join(const CallTargets &other);
  /// Stable one-token encoding, including internal symbols with spaces.
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<CallTargets> parse(std::string_view text);
  friend bool operator==(const CallTargets &, const CallTargets &) = default;
  friend auto operator<=>(const CallTargets &, const CallTargets &) = default;
};

} // namespace weavec::core
#endif
