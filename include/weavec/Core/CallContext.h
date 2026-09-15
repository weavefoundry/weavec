//===- CallContext.h - Caller identity for contextual checking -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_CALLCONTEXT_H
#define WEAVEC_CORE_CALLCONTEXT_H

#include "weavec/Core/SummaryIO.h"

namespace weavec::core {

inline constexpr std::size_t MaxCallContextPaths = 32;
inline constexpr std::size_t MaxCallContextFacts = 64;
inline constexpr std::size_t MaxMemoryContexts = 32;
inline constexpr std::size_t MaxCallContextDepth = 8;

/// RFC 0016: first = second + offset. A may relationship is not evidence
/// of definite equality, and different shares still reach one allocation.
struct ContextAlias {
  SummaryPath first;
  SummaryPath second;
  PointerOffset offset;
  bool definite = true;
  bool sameShare = true;
  friend auto operator<=>(const ContextAlias &, const ContextAlias &) = default;
};

struct CallContext {
  bool reportDiagnostics = true;
  CallbackBindings callbacks;
  std::set<ContextAlias> aliases;
  std::set<std::pair<SummaryPath, SummaryPath>> separations;
  /// RFC 0021: first <= second in one byte array at call entry. These
  /// directed facts do not assert an exact displacement or ownership share.
  std::set<std::pair<SummaryPath, SummaryPath>> orders;
  std::map<SummaryPath, ValueFact> facts;

  [[nodiscard]] bool empty() const noexcept {
    return callbacks.empty() && aliases.empty() && separations.empty() &&
           orders.empty() && facts.empty();
  }
  /// Canonicalizes the pair, without weakening a conflicting existing fact.
  /// False means the context cannot be represented; callers must not use it.
  bool addAlias(ContextAlias alias);
  [[nodiscard]] bool valid() const;
  friend auto operator<=>(const CallContext &, const CallContext &) = default;
};

/// Unlike summary remapping, losing any premise invalidates the whole context.
[[nodiscard]] std::optional<CallContext>
remapCallContext(const CallContext &context, const GlobalIdMap &map);

/// Pointer value paths whose identities can affect a call's memory effects.
/// Analysis additionally validates the types and resolves actual input values.
/// RFC 0028: nullopt means more than MaxCallContextFacts distinct inputs.
/// An over-limit prefix must never be used as a complete footprint.
[[nodiscard]] std::optional<std::set<SummaryPath>>
callMemoryFootprint(const FunctionSummary &summary);

/// Per-function preparation of immutable summary inputs; no caller state.
class CallMemoryFootprintCache {
public:
  static constexpr std::size_t Capacity = 64;
  [[nodiscard]] const std::optional<std::set<SummaryPath>> &
  get(const std::shared_ptr<const FunctionSummary> &summary);

private:
  struct Entry {
    std::weak_ptr<const FunctionSummary> owner;
    std::optional<std::set<SummaryPath>> paths;
  };
  std::map<const FunctionSummary *, Entry> entries;
};

/// Single-token format. Paths and facts are hex encoded so user field/global
/// spellings cannot introduce record delimiters. Global names use the ordinary
/// summary namespace; every declined global invalidates the context.
[[nodiscard]] std::string printCallContext(const CallContext &context,
                                           const GlobalNamer &names);
[[nodiscard]] std::optional<CallContext>
parseCallContext(std::string_view text, const GlobalResolver &resolve);

} // namespace weavec::core

#endif // WEAVEC_CORE_CALLCONTEXT_H
