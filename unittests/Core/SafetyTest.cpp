//===- SafetyTest.cpp - Checked contract algebra (RFC 0018) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/AnalysisStats.h"
#include "weavec/Core/CheckedIO.h"
#include "weavec/Core/SafetyEntryPool.h"

#include <gtest/gtest.h>

namespace weavec::core {

static SafetyObligation obligation(SafetyOutcome outcome, unsigned line = 1) {
  return {
      .property = SafetyProperty::Bounds,
      .outcome = outcome,
      .location = {.file = "test.c", .line = line, .column = 2, .opaque = 0},
      .function = "f",
      .subject = "access",
      .reason = "bounds",
      .calls = {}};
}
static CheckedRequirement extent(unsigned index, std::int64_t bytes = 4) {
  return {.kind = CheckedRequirementKind::Extent,
          .path = SummaryPath::param(index),
          .other = {},
          .begin = PathAffine::ofConstant(0),
          .end = PathAffine::ofConstant(bytes),
          .family = {}};
}
TEST(SafetyLedgerTest, SharedJoinMatchesCanonicalInsertionAcrossOutcomes) {
  SafetyEntryPool pool(nullptr, 4, 2);
  for (unsigned seed = 0; seed < 20; ++seed) {
    SafetyLedger left;
    SafetyLedger right;
    for (unsigned i = 0; i < 80; ++i) {
      auto value = obligation(static_cast<SafetyOutcome>((i + seed) % 5),
                              (((i * 13) + seed) % 31) + 1);
      value.reason = std::to_string((i + seed) % 7);
      value.calls.pushBack({.file = "origin.c",
                            .line = ((i + seed) % 11) + 1,
                            .column = 1,
                            .opaque = 0});
      (i % 3 ? left : right).add(std::move(value));
    }
    if (seed % 2)
      right.markLimited();
    left.shareSnapshot();
    auto expected = left;
    for (const auto &[key, value] : right.entries()) {
      (void)key;
      expected.add(value);
    }
    if (right.limited())
      expected.markLimited();
    left.join(right);
    EXPECT_TRUE(left.sameExplanationsAs(expected));
    EXPECT_EQ(left.complete(), expected.complete());
    EXPECT_EQ(left.trusted(), expected.trusted());
    EXPECT_EQ(left.violated(), expected.violated());
  }
}

TEST(SafetyLedgerTest, JoinsRetainNormalizedRowsWithoutAnInternPool) {
  SafetyEntryPool pool(nullptr, 0, 0);
  SafetyLedger left;
  SafetyLedger right;
  left.add({.property = SafetyProperty::Bounds,
            .outcome = SafetyOutcome::Proven,
            .location = {.file = "a.c", .line = 1, .column = 1, .opaque = 0},
            .function = "f",
            .subject = "a",
            .reason = "in bounds",
            .calls = {}});
  right.add({.property = SafetyProperty::Bounds,
             .outcome = SafetyOutcome::Unresolved,
             .location = {.file = "a.c", .line = 2, .column = 1, .opaque = 0},
             .function = "f",
             .subject = "b",
             .reason = "unknown extent",
             .calls = {}});
  const auto *row = &*right.entries().begin();
  const auto key = row->first;
  left.join(right);
  EXPECT_EQ(&*left.entries().find(key), row);
  auto weaker = row->second;
  weaker.outcome = SafetyOutcome::Violation;
  right.add(std::move(weaker));
  EXPECT_EQ(left.entries().find(key)->second.outcome,
            SafetyOutcome::Unresolved);
  left.join(right);
  EXPECT_EQ(&*left.entries().find(key), &*right.entries().begin());
  EXPECT_TRUE(left.violated());
  left.join(SafetyLedger{});
  EXPECT_TRUE(left.violated());
}

TEST(SafetyEntryPool, IndependentLedgersShareOnlyExactlyEqualEntries) {
  AnalysisStats stats;
  SafetyLedger first;
  SafetyLedger same;
  SafetyLedger differentReason;
  SafetyLedger differentOutcome;
  SafetyLedger differentPath;
  {
    SafetyEntryPool pool(&stats);
    first.add(obligation(SafetyOutcome::Unresolved));
    {
      SafetyEntryPool nested;
      same.add(obligation(SafetyOutcome::Unresolved));
    }
    auto entry = obligation(SafetyOutcome::Unresolved);
    entry.reason = "different";
    differentReason.add(entry);
    differentOutcome.add(obligation(SafetyOutcome::Violation));
    entry = obligation(SafetyOutcome::Unresolved);
    entry.calls.pushBack({});
    differentPath.add(entry);
    EXPECT_EQ(&*first.entries().begin(), &*same.entries().begin());
    EXPECT_NE(&*first.entries().begin(), &*differentReason.entries().begin());
    EXPECT_NE(&*first.entries().begin(), &*differentOutcome.entries().begin());
    EXPECT_NE(&*first.entries().begin(), &*differentPath.entries().begin());
  }
  EXPECT_EQ(stats.count("explanation_entry_hits"), 1U);
  EXPECT_EQ(stats.count("explanation_entry_misses"), 4U);
  EXPECT_EQ(first, same);
  EXPECT_EQ(differentReason.entries().begin()->second.reason, "different");
  SafetyEntryPool next;
  SafetyLedger independent;
  independent.add(obligation(SafetyOutcome::Unresolved));
  EXPECT_EQ(first, independent);
  EXPECT_NE(&*first.entries().begin(), &*independent.entries().begin());
}
TEST(SafetyEntryPool, WeakIndexDoesNotKeepDiscardedEntriesAlive) {
  AnalysisStats stats;
  {
    SafetyEntryPool pool(&stats);
    {
      SafetyLedger temporary;
      temporary.add(obligation(SafetyOutcome::Proven));
    }
    SafetyLedger next;
    next.add(obligation(SafetyOutcome::Proven));
  }
  EXPECT_EQ(stats.count("explanation_entry_hits"), 0U);
  EXPECT_EQ(stats.count("explanation_entry_misses"), 2U);
}
TEST(SafetyEntryPool, WholeSnapshotsShareIndexesAndPreserveDistinctRoutes) {
  AnalysisStats stats;
  SafetyLedger first;
  SafetyLedger same;
  SafetyLedger different;
  {
    SafetyEntryPool pool(&stats, 0);
    auto entry = obligation(SafetyOutcome::Unresolved);
    entry.calls.pushBack({.file = "first.c", .line = 7});
    first.add(entry);
    same.add(entry);
    EXPECT_NE(&first.entries(), &same.entries());
    first.shareSnapshot();
    same.shareSnapshot();
    EXPECT_EQ(&first.entries(), &same.entries());
    EXPECT_EQ(&first.propagation(), &same.propagation());
    // Route file names do not enter the hash: force a collision while
    // preserving identical identities, outcomes and route coordinates.
    entry.calls = {{.file = "other.c", .line = 7}};
    different.add(entry);
    different.shareSnapshot();
    EXPECT_EQ(first, different);
    EXPECT_NE(&first.entries(), &different.entries());
    EXPECT_FALSE(first.sameExplanationsAs(different));
    first.markLimited();
    EXPECT_FALSE(same.limited());
  }
  EXPECT_EQ(stats.count("explanation_snapshot_hits"), 1U);
  EXPECT_EQ(stats.count("explanation_snapshot_misses"), 2U);
  first.add(obligation(SafetyOutcome::Violation, 2));
  EXPECT_EQ(same.entries().size(), 1U);
  EXPECT_EQ(first.entries().size(), 2U);
}
TEST(SafetyEntryPool, SoleOwnerMutationCannotChangeAnIndexedSnapshot) {
  AnalysisStats stats;
  {
    SafetyEntryPool pool(&stats, 0);
    SafetyLedger first;
    first.add(obligation(SafetyOutcome::Unresolved));
    first.shareSnapshot();
    first.add(obligation(SafetyOutcome::Violation, 2));
    SafetyLedger oldContent;
    oldContent.add(obligation(SafetyOutcome::Unresolved));
    oldContent.shareSnapshot();
    EXPECT_EQ(oldContent.entries().size(), 1U);
    EXPECT_EQ(first.entries().size(), 2U);
    first.shareSnapshot();
  }
  EXPECT_EQ(stats.count("explanation_snapshot_hits"), 0U);
  EXPECT_EQ(stats.count("explanation_snapshot_misses"), 3U);
}
TEST(SafetyEntryPool, DecodedContractsShareOnlyIdenticalExplanationStorage) {
  SafetyEntryPool pool(nullptr, 0);
  CheckedContract contract;
  contract.computed = true;
  contract.obligations.add(obligation(SafetyOutcome::Unresolved));
  const GlobalNamer names = [](std::uint32_t) { return std::string("g"); };
  const GlobalResolver resolve = [](std::string_view) {
    return std::optional<std::uint32_t>(0);
  };
  const auto encoded = printCheckedContract(contract, names);
  auto first = parseCheckedContract(encoded, resolve);
  auto second = parseCheckedContract(encoded, resolve);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(&first->obligations.entries(), &second->obligations.entries());
  EXPECT_EQ(printCheckedContract(*first, names), encoded);
  first->obligations.add(obligation(SafetyOutcome::Violation, 2));
  EXPECT_EQ(printCheckedContract(*second, names), encoded);
}
TEST(SafetyEntryPool, SnapshotEvictionAndDisabledIndexPreserveContents) {
  AnalysisStats stats;
  SafetyLedger first;
  SafetyLedger second;
  SafetyLedger third;
  {
    SafetyEntryPool pool(&stats, 0, 2);
    first.add(obligation(SafetyOutcome::Proven, 1));
    second.add(obligation(SafetyOutcome::Unresolved, 2));
    third.add(obligation(SafetyOutcome::Trusted, 3));
    first.shareSnapshot();
    second.shareSnapshot();
    third.shareSnapshot();
  }
  EXPECT_EQ(stats.count("explanation_snapshot_resets"), 1U);
  EXPECT_TRUE(first.complete());
  EXPECT_FALSE(second.complete());
  EXPECT_TRUE(third.trusted());
  {
    SafetyEntryPool disabled(nullptr, 0, 0);
    SafetyLedger same;
    same.add(obligation(SafetyOutcome::Proven, 1));
    same.shareSnapshot();
    EXPECT_NE(&same.entries(), &first.entries());
    EXPECT_TRUE(same.sameExplanationsAs(first));
  }
}
TEST(SafetyEntryPool, BoundedIndexEvictionPreservesLiveLedgerFacts) {
  AnalysisStats stats;
  SafetyLedger ledger;
  {
    SafetyEntryPool pool(&stats, 2);
    for (unsigned line = 1; line <= 3; ++line)
      ledger.add(obligation(SafetyOutcome::Unresolved, line));
    SafetyLedger same;
    same.add(obligation(SafetyOutcome::Unresolved, 1));
    EXPECT_EQ(ledger.entries().begin()->second, same.entries().begin()->second);
  }
  EXPECT_EQ(stats.count("explanation_pool_resets"), 1U);
  EXPECT_EQ(stats.count("explanation_entry_misses"), 4U);
  EXPECT_EQ(ledger.entries().size(), 3U);
  EXPECT_FALSE(ledger.complete());
  AnalysisStats disabledStats;
  {
    SafetyEntryPool disabled(&disabledStats, 0);
    SafetyLedger same;
    same.add(obligation(SafetyOutcome::Unresolved, 1));
    EXPECT_EQ(ledger.entries().begin()->second, same.entries().begin()->second);
  }
  EXPECT_EQ(disabledStats.count("explanation_entry_hits"), 0U);
  EXPECT_EQ(disabledStats.count("explanation_entry_misses"), 1U);
}
TEST(SafetyLedger, WeakestOutcomeWinsOnEveryPermutation) {
  for (unsigned a = 0; a < 5; ++a)
    for (unsigned b = 0; b < 5; ++b) {
      SafetyLedger left;
      SafetyLedger right;
      left.add(obligation(static_cast<SafetyOutcome>(a)));
      right.add(obligation(static_cast<SafetyOutcome>(b)));
      auto reverse = right;
      reverse.join(left);
      left.join(right);
      EXPECT_EQ(left, reverse);
      EXPECT_EQ(left.entries().begin()->second.outcome,
                static_cast<SafetyOutcome>(std::max(a, b)));
      auto again = left;
      again.join(left);
      EXPECT_EQ(again, left);
      EXPECT_EQ(left.complete(), std::max(a, b) < 3);
    }
}
TEST(SafetyCallPath, CopiesShareNormalizedPathsAndEditsDetach) {
  const SourceLocation origin{.file = "origin.c", .line = 9, .opaque = 12};
  SafetyCallPath path{{.file = "caller.c", .line = 1, .opaque = 8}, origin};
  path.normalize();
  auto copy = path;
  const auto *shared = path.entries().data();
  EXPECT_EQ(shared, copy.entries().data());
  copy.normalize();
  EXPECT_EQ(shared, copy.entries().data());
  EXPECT_EQ(path.back().opaque, 0U);
  copy.insert(copy.begin(), copy.back());
  EXPECT_NE(shared, copy.entries().data());
  EXPECT_EQ(path.size(), 2U);
  EXPECT_EQ(copy.size(), 3U);
  copy.normalize();
  EXPECT_EQ(copy.size(), 1U);
  EXPECT_EQ(copy.back().line, origin.line);
  EXPECT_EQ(path.size(), 2U);
  auto extended = path;
  extended.resize(3);
  extended.pushBack({.file = "tail.c", .line = 17, .opaque = 99});
  extended.normalize();
  EXPECT_EQ(path.size(), 2U);
  EXPECT_EQ(extended.back().opaque, 0U);
}
TEST(SafetyCallPath, NormalizingACopyPreservesRawInputAndTheBoundedOrigin) {
  std::vector<SourceLocation> locations;
  locations.reserve(MaxSafetyCallDepth + 3);
  for (unsigned line = 1; line <= MaxSafetyCallDepth + 3; ++line)
    locations.push_back({.file = "route.c", .line = line, .opaque = line});
  SafetyCallPath raw(locations);
  auto normalized = raw;
  normalized.normalize();
  EXPECT_EQ(raw.entries(), locations);
  ASSERT_EQ(normalized.size(), MaxSafetyCallDepth);
  EXPECT_EQ(normalized.front().line, 1U);
  EXPECT_EQ(normalized.back().line, MaxSafetyCallDepth + 3);
  EXPECT_EQ(normalized.back().opaque, 0U);
  raw.normalize();
  EXPECT_EQ(raw, normalized);
  SafetyLedger ledger;
  auto entry = obligation(SafetyOutcome::Unresolved);
  entry.calls = normalized;
  ledger.add(entry);
  EXPECT_EQ(ledger.entries().begin()->second.calls.entries().data(),
            normalized.entries().data());
}
static void addCallsIndividually(SafetyLedger &ledger,
                                 const std::vector<SafetyObligation> &entries,
                                 const SourceLocation &location,
                                 const std::string &function,
                                 const std::string &callee, bool unsafe) {
  for (const auto &entry : entries) {
    const SafetyObligation origin{
        .property = SafetyProperty::Call,
        .location = entry.calls.empty() ? entry.location : entry.calls.back(),
        .function = callee,
        .subject = entry.reason,
        .reason = {},
        .calls = {}};
    ledger.add(
        {.property = SafetyProperty::Call,
         .outcome = unsafe ? SafetyOutcome::Trusted : entry.outcome,
         .location = location,
         .function = function,
         .subject = origin.identity(),
         .reason = unsafe ? "unsafe boundary: " + entry.reason : entry.reason,
         .calls = entry.calls});
  }
}
TEST(SafetyLedger, BulkCallOriginsMatchIndividualInsertionAndOwnBorrowedData) {
  for (const bool unsafe : {false, true}) {
    SafetyLedger individual;
    SafetyLedger bulk;
    {
      std::vector<SafetyObligation> entries;
      for (unsigned i = 0; i < 5; ++i) {
        auto entry = obligation(static_cast<SafetyOutcome>(i), i + 1);
        entry.calls = {{.file = "mid.c", .line = 8, .opaque = 17},
                       entry.location};
        entries.push_back(entry);
        entry.calls.insert(entry.calls.begin(), entry.calls.front());
        entries.push_back(entry);
      }
      const SourceLocation location{
          .file = "caller.c", .line = 17, .column = 5, .opaque = 42};
      addCallsIndividually(individual, entries, location, "caller", "callee",
                           unsafe);
      bulk.addCalls(entries, location, "caller", "callee", unsafe);
    }
    EXPECT_TRUE(individual.sameExplanationsAs(bulk));
    EXPECT_EQ(individual.complete(), bulk.complete());
    EXPECT_EQ(individual.trusted(), bulk.trusted());
  }
}
TEST(SafetyLedger, BulkCallOriginsPreserveCapUpdatesAndTruncation) {
  for (const bool unsafe : {false, true}) {
    SafetyLedger individual;
    SafetyLedger bulk;
    const SourceLocation location{.file = "caller.c", .line = 17};
    std::vector<SafetyObligation> entries;
    entries.reserve(MaxSafetyObligations + 2);
    for (unsigned i = 0; i < MaxSafetyObligations + 2; ++i)
      entries.push_back(obligation(SafetyOutcome::Unresolved, i + 1));
    addCallsIndividually(individual, entries, location, "caller", "callee",
                         unsafe);
    bulk.addCalls(entries, location, "caller", "callee", unsafe);
    EXPECT_TRUE(individual.sameExplanationsAs(bulk));
    ASSERT_TRUE(bulk.limited());
    entries.resize(1);
    entries.front().outcome = SafetyOutcome::Violation;
    entries.front().calls = {{.file = "mid.c", .line = 8},
                             entries.front().location};
    addCallsIndividually(individual, entries, location, "caller", "callee",
                         unsafe);
    bulk.addCalls(entries, location, "caller", "callee", unsafe);
    EXPECT_TRUE(individual.sameExplanationsAs(bulk));
    const std::string longName(65537, 'a');
    entries.front().reason = longName;
    addCallsIndividually(individual, entries, location, longName, longName,
                         unsafe);
    bulk.addCalls(entries, location, longName, longName, unsafe);
    EXPECT_TRUE(individual.sameExplanationsAs(bulk));
    SafetyLedger shortIndividual;
    SafetyLedger shortBulk;
    addCallsIndividually(shortIndividual, entries, location, longName, longName,
                         unsafe);
    shortBulk.addCalls(entries, location, longName, longName, unsafe);
    EXPECT_TRUE(shortIndividual.sameExplanationsAs(shortBulk));
    EXPECT_TRUE(shortBulk.limited());
  }
}
TEST(SafetyLedger, BulkInsertionKeepsItsOwnOriginProjectionAlive) {
  SafetyLedger bulk;
  bulk.add(obligation(SafetyOutcome::Unresolved));
  bulk.add(obligation(SafetyOutcome::Unresolved, 2));
  auto individual = bulk;
  const SourceLocation location{.file = "caller.c", .line = 17};
  const auto entries = individual.propagation().unresolved;
  addCallsIndividually(individual, entries, location, "caller", "callee",
                       false);
  // The copied ledger above changes before this call, leaving bulk the sole
  // owner of the original Storage and its cached origin vector.
  bulk.addCalls(bulk.propagation().unresolved, location, "caller", "callee",
                false);
  EXPECT_TRUE(bulk.sameExplanationsAs(individual));
}

TEST(SafetyEntryPool, PreparedOriginsKeepCallerAndUnsafeSemantics) {
  AnalysisStats stats;
  {
    SafetyEntryPool pool(&stats, 0, 0);
    SafetyLedger source;
    for (unsigned i = 0; i < 5; ++i) {
      auto entry = obligation(static_cast<SafetyOutcome>(i), i + 1);
      entry.reason = "quoted \"reason\"\\path\n" + std::to_string(i);
      entry.calls = {{.file = "mid.c", .line = 8, .opaque = 19},
                     entry.location};
      source.add(entry);
    }
    for (const bool trusted : {false, true}) {
      for (const bool unsafe : {false, true}) {
        for (unsigned caller = 0; caller < 2; ++caller) {
          const SourceLocation location{.file = "caller.c",
                                        .line = caller + 1,
                                        .column = 2,
                                        .opaque = caller + 10};
          const auto name = "caller" + std::to_string(caller);
          SafetyLedger expected;
          SafetyLedger actual;
          const auto &origins = source.propagation();
          addCallsIndividually(expected,
                               trusted ? origins.trusted : origins.unresolved,
                               location, name, "callee", unsafe);
          // An inner scope cannot change the outer scope's enabled cache.
          SafetyEntryPool inner(nullptr, 0, 0, 0, 0);
          actual.addCalls(source, trusted, location, name, "callee", unsafe);
          EXPECT_TRUE(actual.sameExplanationsAs(expected));
          EXPECT_EQ(actual.trusted(), expected.trusted());
        }
      }
    }
  }
  EXPECT_EQ(stats.count("explanation_call_hits"), 4U);
  EXPECT_EQ(stats.count("explanation_call_misses"), 4U);
}

TEST(SafetyEntryPool, PreparedOriginsPreserveCapOrderAndTruncationOnHits) {
  AnalysisStats stats;
  {
    SafetyEntryPool pool(&stats, 0, 0);
    SafetyLedger source;
    for (unsigned i = 0; i < 12; ++i) {
      auto entry = obligation(SafetyOutcome::Unresolved, i + 1);
      entry.calls = {{.file = "origin.c", .line = 12 - i}};
      source.add(entry);
    }
    auto longReason = obligation(SafetyOutcome::Unresolved, 30);
    longReason.reason.assign(65536, '"');
    source.add(longReason);
    const auto origins = source.propagation().unresolved;
    const SourceLocation location{.file = "caller.c", .line = 17};
    for (const bool unsafe : {false, true}) {
      SafetyLedger warm;
      warm.addCalls(source, false, location, "caller", "callee", unsafe);
      for (const unsigned remaining : {0U, 1U, 7U}) {
        SafetyLedger actual;
        for (unsigned i = remaining; i < MaxSafetyObligations; ++i)
          actual.add(obligation(SafetyOutcome::Proven, i + 100));
        auto expected = actual;
        addCallsIndividually(expected, origins, location, "caller", "callee",
                             unsafe);
        actual.addCalls(source, false, location, "caller", "callee", unsafe);
        EXPECT_TRUE(actual.sameExplanationsAs(expected));
        EXPECT_TRUE(actual.limited());
      }
      const std::string longCaller(65537, 'c');
      SafetyLedger expected;
      SafetyLedger actual;
      addCallsIndividually(expected, origins, location, longCaller, "callee",
                           unsafe);
      actual.addCalls(source, false, location, longCaller, "callee", unsafe);
      EXPECT_TRUE(actual.sameExplanationsAs(expected));
      EXPECT_TRUE(actual.limited());
    }
  }
  EXPECT_EQ(stats.count("explanation_call_misses"), 2U);
  EXPECT_EQ(stats.count("explanation_call_hits"), 8U);
}

TEST(SafetyEntryPool, PreparedOriginsHonorCapacityBytesAndDisabledScopes) {
  for (const std::size_t capacity : {0U, 1U}) {
    for (const std::size_t bytes : {1U, 1048576U}) {
      AnalysisStats stats;
      SafetyLedger source;
      source.add(obligation(SafetyOutcome::Unresolved));
      {
        SafetyEntryPool pool(&stats, 0, 0, capacity, bytes);
        for (const std::string name : {"first", "second", "third", "third"}) {
          SafetyLedger expected;
          SafetyLedger actual;
          const SourceLocation location{.file = "caller.c", .line = 17};
          addCallsIndividually(expected, source.propagation().unresolved,
                               location, "caller", name, false);
          actual.addCalls(source, false, location, "caller", name, false);
          EXPECT_TRUE(actual.sameExplanationsAs(expected));
        }
      }
      EXPECT_EQ(stats.count("explanation_call_hits"),
                capacity && bytes > 1 ? 1U : 0U);
      EXPECT_EQ(stats.count("explanation_call_resets"),
                capacity && bytes > 1 ? 2U : 0U);
      EXPECT_EQ(stats.count("explanation_call_rejections"),
                capacity && bytes == 1 ? 4U : 0U);
    }
  }
}

TEST(SafetyEntryPool, PreparedOriginsSurviveSelfInsertionAndSourceReplacement) {
  SafetyLedger retained;
  {
    SafetyEntryPool pool(nullptr, 0, 0, 4);
    for (unsigned i = 0; i < 32; ++i) {
      SafetyLedger source;
      source.add(obligation(SafetyOutcome::Unresolved, i + 1));
      source.add(obligation(SafetyOutcome::Trusted, i + 40));
      auto expected = source;
      const auto origins = source.propagation().unresolved;
      const SourceLocation location{.file = "caller.c", .line = 17};
      addCallsIndividually(expected, origins, location, "caller", "callee",
                           false);
      source.addCalls(source, false, location, "caller", "callee", false);
      EXPECT_TRUE(source.sameExplanationsAs(expected));
      retained = std::move(source);
    }
  }
  EXPECT_FALSE(retained.complete());
  EXPECT_TRUE(retained.trusted());
  EXPECT_EQ(retained.entries().size(), 3U);
  // The key-length bound bypasses preparation, not checking or truncation.
  const std::string longName(4097, 'x');
  SafetyEntryPool pool(nullptr, 0, 0);
  SafetyLedger expected;
  SafetyLedger actual;
  addCallsIndividually(expected, retained.propagation().unresolved, {},
                       "caller", longName, true);
  actual.addCalls(retained, false, {}, "caller", longName, true);
  EXPECT_TRUE(actual.sameExplanationsAs(expected));
}

TEST(SafetyLedgerTest, LinearJoinPreservesCapAndUpdatesExistingKeys) {
  SafetyEntryPool pool(nullptr, 0);
  SafetyLedger actual;
  for (unsigned i = 0; i < MaxSafetyObligations - 1; ++i)
    actual.add(obligation(SafetyOutcome::Proven, i + 100));
  actual.shareSnapshot();
  auto expected = actual;
  SafetyLedger incoming;
  incoming.add(obligation(SafetyOutcome::Unresolved, 1));
  incoming.add(obligation(SafetyOutcome::Trusted, 100));
  incoming.add(obligation(SafetyOutcome::Violation, 101));
  incoming.add(obligation(SafetyOutcome::Trusted, 9000));
  for (const auto &[key, entry] : incoming.entries()) {
    (void)key;
    expected.add(entry);
  }
  actual.join(incoming);
  EXPECT_TRUE(actual.sameExplanationsAs(expected));
  EXPECT_TRUE(actual.limited());
  EXPECT_TRUE(actual.violated());
  EXPECT_TRUE(actual.trusted());
}

TEST(SafetyLedger, LimitsFailClosed) {
  SafetyLedger ledger;
  for (unsigned i = 0; i <= MaxSafetyObligations; ++i)
    ledger.add(obligation(SafetyOutcome::Proven, i));
  EXPECT_EQ(ledger.entries().size(), MaxSafetyObligations);
  EXPECT_TRUE(ledger.limited());
  EXPECT_FALSE(ledger.complete());
  auto entry = obligation(SafetyOutcome::Trusted);
  entry.calls.resize(MaxSafetyCallDepth + 1);
  SafetyLedger chain;
  chain.add(entry);
  EXPECT_FALSE(chain.limited());
  EXPECT_TRUE(chain.complete());
}
TEST(SafetyLedger, FrontendHandlesDoNotAffectIdentity) {
  auto entry = obligation(SafetyOutcome::Proven);
  const auto identity = entry.identity();
  entry.location.opaque = 17;
  EXPECT_EQ(entry.identity(), identity);
  SafetyLedger ledger;
  ledger.add(entry);
  EXPECT_EQ(ledger.entries().begin()->second.location.opaque, 0U);
}
TEST(SafetyState, DisabledDomainHasNoFactsAndCopiesDoNotShareMutation) {
  AnalysisState ordinary;
  EXPECT_FALSE(ordinary.safety);
  AnalysisState checked;
  checked.safety.emplace();
  checked.safety->initialized.insert(PlaceId{1});
  auto copied = checked;
  copied.safety->initialized.clear();
  EXPECT_TRUE(checked.safety->initialized.contains(PlaceId{1}));
  EXPECT_TRUE(ordinary.join(checked));
  ASSERT_TRUE(ordinary.safety);
  EXPECT_TRUE(ordinary.safety->initialized.empty());
  EXPECT_TRUE(checked.join(ordinary));
  EXPECT_TRUE(checked.safety->initialized.empty());
}
TEST(SafetyState, IntersectionRetainsOnlyInitializedBytes) {
  SafetyState a;
  SafetyState b;
  const PlaceId p{1};
  a.initialized.insert(p);
  a.pointers.insert(p);
  a.initialize(p,
               {.begin = Affine::ofConstant(0), .end = Affine::ofConstant(8)});
  b.initialize(p,
               {.begin = Affine::ofConstant(4), .end = Affine::ofConstant(12)});
  EXPECT_TRUE(a.join(b));
  EXPECT_TRUE(a.initialized.empty());
  EXPECT_TRUE(a.pointers.empty());
  ASSERT_EQ(a.memory.at(p).size(), 1U);
  EXPECT_EQ(a.memory.at(p).front(),
            (InitializedRange{Affine::ofConstant(4), Affine::ofConstant(8)}));
  EXPECT_FALSE(a.join(b));
  EXPECT_TRUE(a.join(SafetyState{}));
  EXPECT_TRUE(a.memory.empty());
}
TEST(SafetyState, AdjacentWritesMergeWithoutFillingGaps) {
  SafetyState state;
  const PlaceId p{1};
  state.initialize(
      p, {.begin = Affine::ofConstant(0), .end = Affine::ofConstant(2)});
  state.initialize(
      p, {.begin = Affine::ofConstant(4), .end = Affine::ofConstant(6)});
  EXPECT_EQ(state.memory.at(p).size(), 2U);
  state.initialize(
      p, {.begin = Affine::ofConstant(2), .end = Affine::ofConstant(4)});
  ASSERT_EQ(state.memory.at(p).size(), 1U);
  EXPECT_EQ(state.memory.at(p).front().end, Affine::ofConstant(6));
  state.copyMemory(p, PlaceId{2});
  state.forget(p);
  EXPECT_FALSE(state.memory.contains(p));
  EXPECT_TRUE(state.memory.contains(PlaceId{2}));
}
TEST(CheckedContract, PreconditionsUnionAndPostconditionsIntersect) {
  CheckedContract a;
  CheckedContract b;
  a.computed = b.computed = true;
  a.require(extent(0));
  b.require(extent(1));
  a.establish(extent(0));
  b.establish(extent(0));
  b.establish(extent(1));
  a.join(b);
  EXPECT_EQ(a.requirements.size(), 2U);
  EXPECT_EQ(a.establishes.size(), 1U);
  EXPECT_TRUE(a.complete());
  for (unsigned i = 0; i <= MaxSafetyRequirements; ++i)
    a.require(extent(i));
  EXPECT_TRUE(a.limited);
  EXPECT_FALSE(a.complete());
}
TEST(CheckedContract, PortableRoundTripAndCorruption) {
  CheckedContract contract;
  contract.computed = contract.selected = true;
  contract.signature = "void (char *, unsigned int)";
  contract.require(extent(0));
  auto writable = extent(0);
  writable.kind = CheckedRequirementKind::Writable;
  contract.require(writable);
  contract.establish(extent(1));
  contract.obligations.add(obligation(SafetyOutcome::Required));
  const GlobalNamer names = [](std::uint32_t) { return std::string("g"); };
  const GlobalResolver resolve = [](std::string_view) {
    return std::optional<std::uint32_t>(0);
  };
  const auto text = printCheckedContract(contract, names);
  EXPECT_EQ(parseCheckedContract(text, resolve), contract);
  for (std::size_t i = 0; i < text.size(); ++i)
    EXPECT_FALSE(parseCheckedContract(text.substr(0, i), resolve));
  EXPECT_FALSE(parseCheckedContract(text + "00", resolve));
  EXPECT_FALSE(parseCheckedContract("zz", resolve));
  FunctionSummary summary;
  summary.checked = contract;
  EXPECT_EQ(parseSummary(printSummary(summary, names), resolve), summary);
}
TEST(CheckedContract, MissingGlobalsInvalidateProof) {
  FunctionSummary summary;
  summary.checked.computed = true;
  auto requirement = extent(0);
  requirement.path = SummaryPath::global(0);
  summary.checked.require(requirement);
  const auto remapped = remapGlobals(
      summary, [](std::uint32_t) { return std::optional<std::uint32_t>{}; });
  EXPECT_TRUE(remapped.checked.limited);
  EXPECT_FALSE(remapped.checked.complete());
}
TEST(SafetyJson, EscapesControlsAndPreservesUnicode) {
  EXPECT_EQ(safetyJsonString("a\n\"\\"), "\"a\\u000a\\\"\\\\\"");
  EXPECT_EQ(safetyJsonString("é"), "\"é\"");
}
// RFC 0019: conditional must-facts may survive a merge only when the other
// edge excludes their premise. A subsequent assignment destroys the evidence.
TEST(SafetyState, ConditionalInitializationExcludesTheOtherEdge) {
  const PlaceId bytes{1};
  const PlaceId flag{2};
  PlaceGuard yes;
  PlaceGuard no;
  yes.require(flag, ValueFact::of(Outcome::Positive));
  no.require(flag, ValueFact::of(Outcome::Zero));
  SafetyState written;
  written.initialize(bytes, {.begin = {}, .end = Affine::ofConstant(4)});
  const auto before = written;
  SafetyState empty;
  EXPECT_TRUE(written.join(empty, yes, no));
  ASSERT_EQ(written.memory.at(bytes).size(), 1U);
  EXPECT_EQ(written.memory.at(bytes).front().when, yes);
  auto reverse = empty;
  reverse.join(before, no, yes);
  EXPECT_EQ(written, reverse);
  EXPECT_FALSE(written.join(empty, yes, no));
  written.forgetDependency(flag);
  EXPECT_TRUE(written.memory.at(bytes).empty());
}

TEST(SafetyState, MissingBranchEvidenceCannotCreateConditionalFacts) {
  const PlaceId bytes{1};
  const PlaceId flag{2};
  PlaceGuard yes;
  yes.require(flag, ValueFact::of(Outcome::Positive));
  SafetyState a;
  a.initialize(bytes, {.begin = {}, .end = Affine::ofConstant(4)});
  a.join(SafetyState{}, yes, {});
  EXPECT_TRUE(a.memory.empty());
}

TEST(SafetyState, GuardLimitCannotWeakenMustFactPremises) {
  const PlaceId bytes{1};
  PlaceGuard branch;
  for (unsigned i = 0; i < MaxGuardConjuncts; ++i)
    branch.require(PlaceId{10 + i}, ValueFact::of(Outcome::Positive));
  PlaceGuard original;
  original.require(PlaceId{30}, ValueFact::of(Outcome::Positive));
  PlaceGuard other;
  other.require(PlaceId{10}, ValueFact::of(Outcome::Zero));
  SafetyState a;
  a.initialize(bytes,
               {.begin = {}, .end = Affine::ofConstant(4), .when = original});
  a.join(SafetyState{}, branch, other);
  EXPECT_TRUE(a.memory.empty());
}

TEST(SafetyState, ReplacingHolderDoesNotReplaceItsFormerObject) {
  SafetyState state;
  const PlaceId holder{1};
  const PlaceId copy{2};
  const PlaceId object{3};
  state.objects[holder] = object;
  state.objects[copy] = object;
  state.initialize(object, {.begin = {}, .end = Affine::ofConstant(4)});
  state.forget(holder);
  EXPECT_FALSE(state.objects.contains(holder));
  EXPECT_EQ(state.objects.at(copy), object);
  EXPECT_TRUE(state.memory.contains(object));
  auto unknown = state;
  unknown.objects.erase(copy);
  state.join(unknown);
  EXPECT_FALSE(state.objects.contains(copy));
}

TEST(PendingOutcome, InitializationRequiresEverySelectedReturnClass) {
  const PlaceId object{1};
  const InitializedRange bytes{.begin = {}, .end = Affine::ofConstant(4)};
  PendingOutcome call;
  call.consumedBy.try_emplace(Outcome::Zero);
  call.consumedBy.try_emplace(Outcome::Positive);
  call.initializedOn[Outcome::Positive].emplace_back(object, bytes);
  EXPECT_TRUE(call.initializedInAll().empty());
  auto success = call;
  success.select({Outcome::Positive});
  ASSERT_EQ(success.initializedInAll().size(), 1U);
  auto failure = call;
  failure.select({Outcome::Zero});
  EXPECT_TRUE(failure.initializedInAll().empty());
  EXPECT_TRUE(success.unite(failure));
  EXPECT_TRUE(success.initializedInAll().empty());
  auto changed = call;
  changed.initializedOn.clear();
  EXPECT_FALSE(call.unite(changed));
}

TEST(PendingOutcome, ReassignmentDropsPendingInitializationDependencies) {
  AnalysisState state;
  state.safety.emplace();
  const PlaceId result{1};
  const PlaceId object{2};
  const PlaceId length{3};
  auto &call = state.pending[result];
  call.consumedBy.try_emplace(Outcome::Positive);
  call.initializedOn[Outcome::Positive].push_back(
      {object, {.begin = {}, .end = Affine::ofPlace(length)}});
  state.dropGuardsOn(length);
  EXPECT_TRUE(call.initializedInAll().empty());
}

// RFC 0020: whole-place forgetting invalidates every dependent domain once.
TEST(PendingOutcome, ForgetPreservesOnlyIndependentInitializationEvidence) {
  AnalysisState state;
  state.safety.emplace();
  const PlaceId result{1};
  const PlaceId object{2};
  const PlaceId length{3};
  const PlaceId other{4};
  PlaceGuard guard;
  guard.require(length, ValueFact::of(Outcome::Positive));
  const InitializedRange dependent{
      .begin = {}, .end = Affine::ofConstant(4), .when = guard};
  const InitializedRange independent{.begin = Affine::ofConstant(4),
                                     .end = Affine::ofConstant(8)};
  state.safety->paths.push_back(guard);
  state.safety->initialized.insert(length);
  state.safety->initialized.insert(other);
  state.safety->initialize(object, dependent);
  state.safety->initialize(object, independent);
  state.safety->initialize(object,
                           {.begin = {}, .end = Affine::ofPlace(length)});
  auto &call = state.pending[result];
  call.consumedBy.try_emplace(Outcome::Positive);
  call.initializedOn[Outcome::Positive] = {{object, dependent},
                                           {object, independent}};
  call.factOn[Outcome::Positive] = {{length, ValueFact::ofConstant(4)},
                                    {other, ValueFact::ofConstant(8)}};

  state.forget(length);
  EXPECT_FALSE(state.safety->initialized.contains(length));
  EXPECT_TRUE(state.safety->initialized.contains(other));
  ASSERT_EQ(state.safety->paths.size(), 1U);
  EXPECT_TRUE(state.safety->paths.front().trivial());
  EXPECT_EQ(state.safety->memory.at(object),
            (std::vector<InitializedRange>{independent}));
  const auto initialized = call.initializedInAll();
  ASSERT_EQ(initialized.size(), 1U);
  EXPECT_EQ(initialized.front().first, object);
  EXPECT_EQ(initialized.front().second, independent);
  const auto facts = call.factsInAll();
  ASSERT_EQ(facts.size(), 1U);
  EXPECT_EQ(facts.front().first, other);
  EXPECT_EQ(facts.front().second, ValueFact::ofConstant(8));
}

TEST(CheckedIO, ConditionalNestedAndResultInitializationRoundTrip) {
  CheckedContract contract;
  contract.computed = true;
  contract.signature = "ptr(ptr,i32)";
  auto post = extent(0);
  post.kind = CheckedRequirementKind::Initialized;
  post.path = SummaryPath::result();
  post.on = Outcome::NonNull;
  post.when.require(SummaryPath::param(1), ValueFact::of(Outcome::Positive));
  contract.establish(post);
  post.kind = CheckedRequirementKind::Terminated;
  post.path = SummaryPath::param(0);
  post.on.reset();
  // RFC 0021 names the minimum witness index in begin; end is reserved.
  post.end = PathAffine::ofConstant(0);
  contract.require(post);
  const GlobalNamer names = [](std::uint32_t) { return std::string("global"); };
  const GlobalResolver resolve = [](std::string_view) {
    return std::optional<std::uint32_t>{1};
  };
  const auto encoded = printCheckedContract(contract, names);
  ASSERT_FALSE(encoded.empty());
  const auto decoded = parseCheckedContract(encoded, resolve);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, contract);
}

TEST(PendingOutcome, OverwritingOutputDiscardsNumericPostcondition) {
  AnalysisState state;
  const PlaceId result{1};
  const PlaceId output{2};
  auto &call = state.pending[result];
  call.consumedBy.try_emplace(Outcome::Zero);
  call.factOn[Outcome::Zero].emplace_back(output, ValueFact::ofConstant(16));
  ASSERT_EQ(call.factsInAll().size(), 1U);
  state.dropGuardsOn(output);
  EXPECT_TRUE(call.factsInAll().empty());
}

TEST(SafetyState, CopiedIncomingEvidenceIsNotAnInitializedRange) {
  const PlaceId object{1};
  const PlaceId source{2};
  SafetyState copied;
  copied.initialize(
      object, {.begin = {}, .end = Affine::ofConstant(16), .source = source});
  copied.initialize(object, {.begin = {}, .end = Affine::ofConstant(4)});
  ASSERT_EQ(copied.memory.at(object).size(), 2U);
  auto empty = copied;
  empty.memory.clear();
  copied.join(empty);
  EXPECT_TRUE(copied.memory.empty());
}

TEST(CheckedIO, RejectsOutputFactsInAnInputContract) {
  const auto names = [](std::uint32_t) { return std::string("global"); };
  const auto resolve = [](std::string_view) { return std::optional(1U); };
  CheckedContract contract;
  contract.computed = true;
  auto requirement = extent(0);
  requirement.kind = CheckedRequirementKind::Copied;
  contract.require(requirement);
  EXPECT_FALSE(
      parseCheckedContract(printCheckedContract(contract, names), resolve));
  contract.requirements.clear();
  requirement.kind = CheckedRequirementKind::Initialized;
  requirement.on = Outcome::Zero;
  contract.require(requirement);
  EXPECT_FALSE(
      parseCheckedContract(printCheckedContract(contract, names), resolve));
}

TEST(CheckedIO, MissingOutputGuardGlobalsInvalidateProof) {
  FunctionSummary summary;
  summary.checked.computed = true;
  auto post = extent(0);
  post.kind = CheckedRequirementKind::Initialized;
  post.path = SummaryPath::result().field("bytes");
  post.on = Outcome::NonNull;
  post.when.require(SummaryPath::global(1), ValueFact::of(Outcome::Positive));
  summary.checked.establish(post);
  const auto names = [](std::uint32_t) { return std::string("flag"); };
  const auto missing = [](std::string_view) {
    return std::optional<std::uint32_t>{};
  };
  EXPECT_FALSE(parseCheckedContract(
      printCheckedContract(summary.checked, names), missing));
  const auto remapped = remapGlobals(
      summary, [](std::uint32_t) { return std::optional<std::uint32_t>{}; });
  EXPECT_TRUE(remapped.checked.limited);
  EXPECT_FALSE(remapped.checked.complete());
  EXPECT_TRUE(remapped.checked.establishes.empty());
}

TEST(CheckedIO, BytePreservationAndZeroesCannotBecomeInputAssumptions) {
  const auto names = [](std::uint32_t) { return std::string("global"); };
  const auto resolve = [](std::string_view) { return std::optional(1U); };
  for (const auto kind :
       {CheckedRequirementKind::Copied, CheckedRequirementKind::Zeroed}) {
    CheckedContract contract;
    contract.computed = true;
    auto post = extent(0);
    post.kind = kind;
    post.path = SummaryPath::result();
    post.on = Outcome::NonNull;
    contract.establish(post);
    const auto encoded = printCheckedContract(contract, names);
    EXPECT_EQ(parseCheckedContract(encoded, resolve), contract);
    for (std::size_t i = 0; i < encoded.size(); ++i)
      EXPECT_FALSE(parseCheckedContract(encoded.substr(0, i), resolve));
    contract.establishes.clear();
    post.path = SummaryPath::param(0);
    post.on.reset();
    contract.require(post);
    EXPECT_FALSE(
        parseCheckedContract(printCheckedContract(contract, names), resolve));
  }
}

TEST(SafetyState, ZeroBytesImplyInitializationAndAreInvalidatedByWrites) {
  const PlaceId bytes{1};
  SafetyState state;
  state.initialize(bytes,
                   {.begin = {}, .end = Affine::ofConstant(4), .zeroed = true});
  ASSERT_EQ(state.memory.at(bytes).size(), 2U);
  const auto initialized = std::ranges::find_if(
      state.memory.at(bytes), [](const auto &range) { return !range.zeroed; });
  ASSERT_NE(initialized, state.memory.at(bytes).end());
  auto other = state;
  other.forgetZeros();
  ASSERT_EQ(other.memory.at(bytes).size(), 1U);
  state.join(other);
  EXPECT_EQ(state, other);
}

TEST(PendingOutcome, AWriteCannotReapplyAnEarlierTerminatorFact) {
  AnalysisState state;
  state.safety.emplace();
  const PlaceId result{1};
  auto &call = state.pending[result];
  call.consumedBy.try_emplace(Outcome::Zero);
  call.initializedOn[Outcome::Zero].push_back(
      {PlaceId{2},
       {.begin = {}, .end = Affine::ofConstant(1), .zeroed = true}});
  state.forgetZeroedMemory();
  EXPECT_TRUE(call.initializedInAll().empty());
}

TEST(SafetyState, HeapCellSnapshotsKeepTheReferentIdentity) {
  SafetyState state;
  const PlaceId holder{1};
  const PlaceId saved{2};
  const PlaceId object{3};
  state.objects[holder] = object;
  state.initialize(object, {.begin = {}, .end = Affine::ofConstant(4)});
  state.copyMemory(holder, saved);
  state.forget(holder);
  ASSERT_TRUE(state.objects.contains(saved));
  EXPECT_EQ(state.objects.at(saved), object);
  ASSERT_TRUE(state.memory.contains(object));
  EXPECT_EQ(state.memory.at(object).front().end, Affine::ofConstant(4));
}

// RFC 0020: sharing changes ownership of storage, never the lattice.
TEST(SafetyLedger, CopyOnWriteKeepsSnapshotsAndDistinctOrigins) {
  SafetyLedger original;
  original.add(obligation(SafetyOutcome::Proven));
  auto copy = original;
  EXPECT_EQ(&original.entries(), &copy.entries());
  copy.add(obligation(SafetyOutcome::Violation));
  copy.add(obligation(SafetyOutcome::Unresolved, 2));
  EXPECT_NE(&original.entries(), &copy.entries());
  EXPECT_TRUE(original.complete());
  EXPECT_FALSE(copy.complete());
  EXPECT_EQ(original.entries().size(), 1U);
  EXPECT_EQ(copy.entries().size(), 2U);
  auto joined = original;
  joined.join(copy);
  EXPECT_EQ(joined, copy);
  EXPECT_TRUE(original.complete());
}

TEST(SafetyLedger, DetachingSharesUnchangedRowsAndOwnsReplacedKeys) {
  SafetyLedger original;
  original.add(obligation(SafetyOutcome::Unresolved, 1));
  original.add(obligation(SafetyOutcome::Proven, 2));
  const auto firstKey = obligation(SafetyOutcome::Unresolved, 1).identity();
  const auto secondKey = obligation(SafetyOutcome::Proven, 2).identity();
  const auto *first = &*original.entries().find(firstKey);
  const auto *second = &*original.entries().find(secondKey);
  auto copy = original;
  copy.add(obligation(SafetyOutcome::Violation, 1));
  EXPECT_NE(&original.entries(), &copy.entries());
  EXPECT_EQ(&*original.entries().find(firstKey), first);
  EXPECT_NE(&*copy.entries().find(firstKey), first);
  EXPECT_EQ(&*copy.entries().find(secondKey), second);
  original = SafetyLedger{};
  // The only owners of both key views now live in the detached index. Repeat
  // replacement and ordered lookup after the original index has been freed.
  auto changed = obligation(SafetyOutcome::Violation, 1);
  changed.reason = "a preferred explanation";
  copy.add(changed);
  EXPECT_EQ(copy.entries().find(firstKey)->second.reason, changed.reason);
  EXPECT_EQ(&*copy.entries().find(secondKey), second);
  SafetyLedger joined;
  joined.join(copy);
  EXPECT_TRUE(joined.sameExplanationsAs(copy));
  EXPECT_EQ(std::distance(copy.entries().begin(), copy.entries().end()), 2);
}

TEST(SafetyLedger, ExplanationRoutesDoNotRestartSemanticConvergence) {
  auto entry = obligation(SafetyOutcome::Unresolved);
  SafetyLedger first;
  first.add(entry);
  entry.reason = "another route to the same failed operation";
  entry.calls.pushBack(
      {.file = "caller.c", .line = 9, .column = 1, .opaque = 0});
  SafetyLedger second;
  second.add(entry);
  EXPECT_EQ(first, second);
  EXPECT_FALSE(first.sameExplanationsAs(second));
  second.add(obligation(SafetyOutcome::Violation));
  EXPECT_NE(first, second);
}

TEST(SafetyLedger, PropagationSharesOriginsAndInvalidatesOnlyChangedSnapshots) {
  SafetyLedger ledger;
  const SourceLocation origin{
      .file = "origin.c", .line = 7, .column = 3, .opaque = 0};
  auto first = obligation(SafetyOutcome::Unresolved, 12);
  first.calls = {origin};
  ledger.add(first);
  auto second = first;
  second.location.line = 11;
  second.outcome = SafetyOutcome::Violation;
  ledger.add(second);
  auto trusted = first;
  trusted.location.line = 13;
  trusted.outcome = SafetyOutcome::Trusted;
  ledger.add(trusted);
  auto copy = ledger;
  const auto &projection = ledger.propagation();
  EXPECT_EQ(&projection, &copy.propagation());
  ASSERT_EQ(projection.unresolved.size(), 1U);
  ASSERT_EQ(projection.trusted.size(), 1U);
  EXPECT_EQ(projection.unresolved.front().outcome, SafetyOutcome::Unresolved);
  EXPECT_EQ(projection.unresolved.front().location, origin);
  EXPECT_EQ(projection.unresolved.front().calls.entries(),
            (std::vector<SourceLocation>{second.location, origin}));
  EXPECT_EQ(projection.trusted.front().calls.entries(),
            (std::vector<SourceLocation>{trusted.location, origin}));
  copy.add(obligation(SafetyOutcome::Unresolved, 20));
  EXPECT_NE(&projection, &copy.propagation());
  EXPECT_EQ(ledger.propagation().unresolved.size(), 1U);
  EXPECT_EQ(copy.propagation().unresolved.size(), 2U);
  auto differentReason = first;
  differentReason.location.line = 21;
  differentReason.reason = "initialization";
  copy.add(differentReason);
  EXPECT_EQ(copy.propagation().unresolved.size(), 3U);
  EXPECT_EQ(ledger.entries().size(), 3U);
}

TEST(SafetyLedger, CachedOutcomeQueriesFollowReplacementAndSharedJoins) {
  SafetyLedger a;
  a.add(obligation(SafetyOutcome::Trusted));
  EXPECT_TRUE(a.trusted());
  EXPECT_TRUE(a.complete());
  auto b = a;
  b.add(obligation(SafetyOutcome::Unresolved));
  EXPECT_FALSE(b.trusted());
  EXPECT_FALSE(b.complete());
  EXPECT_FALSE(b.violated());
  EXPECT_TRUE(a.trusted());
  SafetyLedger empty;
  empty.join(b);
  EXPECT_FALSE(empty.trusted());
  EXPECT_FALSE(empty.complete());
  b.add(obligation(SafetyOutcome::Violation, 2));
  empty.join(b);
  EXPECT_TRUE(empty.violated());
  EXPECT_TRUE(a.complete());
  auto moved = std::move(empty);
  EXPECT_TRUE(moved.violated());
  // The custom move explicitly restores an empty ledger and its derived counts.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(empty.complete());
  empty.add(obligation(SafetyOutcome::Trusted));
  EXPECT_TRUE(empty.complete());
  EXPECT_TRUE(empty.trusted());
}

TEST(SafetyJson, PlainRunsPreserveMixedEscapesAndInvalidUnicodeBytes) {
  EXPECT_EQ(safetyJsonString("plain ascii with spaces"),
            "\"plain ascii with spaces\"");
  EXPECT_EQ(safetyJsonString("before\t\"é\\after"),
            "\"before\\u0009\\\"é\\\\after\"");
  EXPECT_EQ(safetyJsonString(std::string("before") + char(0xff) + "after"),
            "\"before\\u00ffafter\"");
}

TEST(SafetyState,
     CommonInitializationDoesNotAccumulateRedundantBranchPremises) {
  const PlaceId object{1};
  const PlaceId flag{2};
  PlaceGuard yes;
  PlaceGuard no;
  yes.require(flag, ValueFact::of(Outcome::Positive));
  no.require(flag, ValueFact::of(Outcome::Zero));
  SafetyState a;
  a.initialize(object, {.begin = {}, .end = Affine::ofConstant(4)});
  const auto b = a;
  a.join(b, yes, no);
  ASSERT_EQ(a.memory.at(object).size(), 1U);
  EXPECT_TRUE(a.memory.at(object).front().when.trivial());
  a.join(b, yes, no);
  ASSERT_EQ(a.memory.at(object).size(), 1U);
  EXPECT_TRUE(a.memory.at(object).front().when.trivial());
}

} // namespace weavec::core
