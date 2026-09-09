//===- SafetyEntryPool.h - Shared entries (RFC 0020) -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_CORE_SAFETYENTRYPOOL_H
#define WEAVEC_CORE_SAFETYENTRYPOOL_H

#include "weavec/Core/Safety.h"

namespace weavec::core {

struct AnalysisStats;

/// Caller-independent strings derived from one immutable origin sequence.
struct PreparedSafetyOrigins {
  struct Entry {
    std::string subject;
    std::string escapedSubject;
    std::string reason;
    bool limited = false;
  };
  std::vector<Entry> entries;
  std::size_t bytes = 0;
};

/// RFC 0020: shares exactly equal immutable entries across context analyses.
/// The outermost scope owns a bounded weak index. Nested scopes reuse it;
/// summaries retain their own entries after the pool has gone away. The
/// capacity controls reuse only and never limits a ledger's facts.
class SafetyEntryPool {
public:
  explicit SafetyEntryPool(AnalysisStats *stats = nullptr,
                           std::size_t capacity = 262144,
                           std::size_t snapshotCapacity = 8192,
                           std::size_t callCapacity = 1024,
                           std::size_t callBytes = std::size_t{64} * 1024 *
                                                   1024);
  ~SafetyEntryPool();
  SafetyEntryPool(const SafetyEntryPool &) = delete;
  SafetyEntryPool &operator=(const SafetyEntryPool &) = delete;

private:
  struct CallKey {
    const SafetyPropagation *source;
    std::string callee;
    bool trusted;
    bool unsafe;
    friend bool operator==(const CallKey &, const CallKey &) = default;
  };
  static bool cachesCalls();
  static std::shared_ptr<const PreparedSafetyOrigins>
  findCalls(const CallKey &key,
            const std::shared_ptr<const SafetyPropagation> &source);
  static void saveCalls(CallKey key,
                        const std::shared_ptr<const SafetyPropagation> &source,
                        std::shared_ptr<const PreparedSafetyOrigins> prepared);
  struct Storage;
  std::unique_ptr<Storage> owned;
  static thread_local Storage *active;
  static std::shared_ptr<const SafetyEntries::Row>
  intern(std::string key, SafetyObligation obligation);
  static std::shared_ptr<SafetyLedger::Storage>
  internSnapshot(std::shared_ptr<SafetyLedger::Storage> snapshot,
                 const SafetyEntries &entries);
  friend class SafetyEntries;
  friend class SafetyLedger;
};

} // namespace weavec::core
#endif // WEAVEC_CORE_SAFETYENTRYPOOL_H
