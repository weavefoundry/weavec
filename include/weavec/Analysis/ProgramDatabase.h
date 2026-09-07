//===- ProgramDatabase.h - Summaries across translation units --*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What one translation unit exports to the rest of the program and the
// database that collects those exports (RFC 0005, *Programs, units and
// exports* and *The program database*). Summaries in exports and in the
// database name globals through a `GlobalNames` table rather than a unit's
// `GlobalTable`, so they mean the same thing in every unit.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_PROGRAMDATABASE_H
#define WEAVEC_ANALYSIS_PROGRAMDATABASE_H

#include "weavec/Core/CallContext.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace weavec::analysis {

class GlobalTable;

/// Interns global variable names for the summaries of an export set or a
/// database, mirroring `GlobalTable` without a Clang declaration behind
/// each id.
class GlobalNames {
public:
  [[nodiscard]] std::uint32_t idFor(llvm::StringRef name);
  [[nodiscard]] std::optional<std::uint32_t> find(llvm::StringRef name) const;
  [[nodiscard]] llvm::StringRef nameOf(std::uint32_t id) const;
  [[nodiscard]] std::size_t size() const noexcept { return names.size(); }

  /// If one table is a prefix of the other (ids agree wherever both have
  /// them), makes this the longer one and returns true; otherwise leaves it
  /// unchanged and returns false.
  bool extendTo(const GlobalNames &other);

  friend bool operator==(const GlobalNames &, const GlobalNames &) = default;

private:
  std::vector<std::string> names;
  std::map<std::string, std::uint32_t, std::less<>> ids;
};

/// One function a unit exports.
struct ExportedFunction {
  /// The summary a caller in the exporting unit would see (annotations
  /// applied), with globals numbered by the unit's `GlobalNames`.
  core::FunctionSummary summary;
  std::map<core::CallbackBindings, core::FunctionSummary> specializations;
  /// `functionTypeKey` of the definition; empty if the type has no stable
  /// spelling (an anonymous record is involved).
  std::string typeKey;
  /// External linkage: callable by name from another unit.
  bool external = true;
  /// Its address is taken somewhere in the unit: reachable through a
  /// pointer of its type from another unit.
  bool addressTaken = false;
  bool acceptsCallbacks = false;
  bool acceptsMemoryContexts = false;
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::map<core::CallContext, core::FunctionSummary> memorySpecializations = {};

  friend bool operator==(const ExportedFunction &,
                         const ExportedFunction &) = default;
};

/// RFC 0012, *Sized fields*: one function's evidence that the pointer field
/// `field` (a count-field key, `struct buf.data`) is as long as the sibling
/// integer field `count` says, in units of `scale` bytes.
struct SizedFieldWitness {
  std::string field;
  std::string count;
  std::int64_t scale = 1;

  friend auto operator<=>(const SizedFieldWitness &,
                          const SizedFieldWitness &) = default;
};

/// RFC 0012: a `(field, count)` pair some function's writes contradict.
struct UnsizedPair {
  std::string field;
  std::string count;

  friend auto operator<=>(const UnsizedPair &, const UnsizedPair &) = default;
};

/// RFC 0012, *Sized fields*: what a unit (or the database) knows about
/// which pointer fields are counted by which sibling fields.
struct SizedFieldFacts {
  std::set<SizedFieldWitness> witnesses;
  /// Pointer fields some function stores a value into whose extent is no
  /// sibling's value: never sized.
  std::set<std::string> unsizedFields;
  /// Pairs whose count is written without the pointer being stored.
  std::set<UnsizedPair> unsizedPairs;

  void merge(const SizedFieldFacts &other);
  void clear();
  [[nodiscard]] bool empty() const noexcept {
    return witnesses.empty() && unsizedFields.empty() && unsizedPairs.empty();
  }
  /// The count and scale `field` is confirmed sized by: its witnesses are
  /// exactly one `(count, scale)`, and no refutation names it.
  [[nodiscard]] std::optional<std::pair<std::string, std::int64_t>>
  confirmed(std::string_view field) const;
  /// Every confirmed pair, as witnesses.
  [[nodiscard]] std::set<SizedFieldWitness> confirmedPairs() const;

  friend bool operator==(const SizedFieldFacts &,
                         const SizedFieldFacts &) = default;
};

/// Everything one translation unit contributes to, and needs from, the
/// program.
struct UnitExports {
  /// The main source file, for messages and the dump.
  std::string source;
  std::map<std::string, std::set<core::CallContext>> memoryRequests;
  std::map<std::string, std::set<core::CallbackBindings>> callbackRequests;
  std::map<std::string, core::CallTargets> callbackGlobals;
  /// Exported definitions by linkage name.
  std::map<std::string, ExportedFunction> functions;
  /// Names the summaries above use for global roots.
  GlobalNames globals;
  /// External-linkage callees with no definition in the unit.
  std::set<std::string> imports;
  /// Type keys of the unit's indirect calls.
  std::set<std::string> indirectTypes;
  /// Callees `imports` contains for which the unit had no summary at all
  /// (the boundary of RFC 0003), plus indirect type keys with no
  /// candidates: what `annotation-required` would have warned about.
  std::set<std::string> unknownCallees;
  std::set<std::string> unknownIndirectTypes;
  /// RFC 0010, *Leaks of shares*: the count-field keys (`struct obj.rc`)
  /// some function of the unit releases a share through, or that are
  /// annotated `WEAVEC_REFCOUNT`. Sidecar line `count-field <key>`.
  std::set<std::string> countFields;
  /// RFC 0012, *Sized fields*: the unit's witnesses and refutations.
  /// Sidecar lines `sized-field <f> <g> <scale>` and `unsized-field <f>
  /// [<g>]`.
  SizedFieldFacts sizedFields;
  /// RFC 0012, *Sized fields*: the keys of the unannotated pointer fields
  /// some bounds check of the unit looked up the extent of. A pair the
  /// program later confirms for one of them means the unit is analysed once
  /// more. Sidecar line `loads-field <key>`.
  std::set<std::string> sizedFieldLoads;

  /// True if the exported summaries (and count fields, and sized-field
  /// facts) are the same; the fixpoint test of RFC 0005's whole-program
  /// algorithm.
  [[nodiscard]] bool sameSummariesAs(const UnitExports &other) const;
};

/// The canonical spelling of a function type that identifies indirect-call
/// candidates across units, or an empty string when the type involves an
/// anonymous record and so has no stable spelling.
[[nodiscard]] std::string functionTypeKey(clang::QualType type,
                                          const clang::ASTContext &context);

/// The same for a record type (`struct obj`), the first half of an RFC 0010
/// count-field key; empty for a non-record or an anonymous one.
[[nodiscard]] std::string recordLayoutKey(clang::QualType type,
                                          const clang::ASTContext &context);

[[nodiscard]] std::string recordTypeKey(clang::QualType type,
                                        const clang::ASTContext &context);

/// The exports of every unit of a program except the one being analysed.
class ProgramDatabase {
public:
  /// Adds a unit's exports. A name defined by more than one unit gets the
  /// join of the definitions' summaries (RFC 0005, *Accepted false
  /// positives*). Summaries numbered by a table this one extends, or that
  /// extends this one (see `renumbered`), are copied rather than renumbered.
  void add(const UnitExports &unit);
  void addCallbackInformation(const UnitExports &unit);
  [[nodiscard]] const core::FunctionSummary *
  findMemorySpecialization(std::string_view symbol,
                           const core::CallContext &context) const;
  [[nodiscard]] const std::set<core::CallContext> &
  memoryRequestsFor(std::string_view symbol) const;
  [[nodiscard]] std::optional<core::CallContext>
  importContext(const core::CallContext &input,
                const clang::ASTContext &context, GlobalTable &table) const;
  [[nodiscard]] std::optional<core::CallContext>
  exportContext(const core::CallContext &input, const GlobalTable &table) const;
  std::map<std::string, core::CallTargets> callbackGlobals;
  [[nodiscard]] const core::FunctionSummary *
  findCallable(std::string_view symbol) const;
  [[nodiscard]] const core::FunctionSummary *
  findSpecialization(std::string_view symbol,
                     const core::CallbackBindings &bindings) const;
  [[nodiscard]] const std::set<core::CallbackBindings> &
  requestsFor(std::string_view symbol) const;

  /// `unit` with its summaries numbered by this database's table, which is
  /// extended with any names it did not have; the result's `globals` is a
  /// copy of `globals()`. Rebuilding a database from such exports is a copy
  /// per summary instead of a renumbering, which is what the whole-program
  /// fixpoint does once per changed member (RFC 0005, *Performance*).
  [[nodiscard]] UnitExports renumbered(const UnitExports &unit);
  void clear();
  [[nodiscard]] bool empty() const noexcept { return functions.empty(); }

  /// Whether some unit defines `name` with external linkage.
  [[nodiscard]] bool defines(llvm::StringRef name) const;

  /// The joined summary of `name`'s external definitions, with globals
  /// numbered by `globals()`; null if no unit defines it.
  [[nodiscard]] const core::FunctionSummary *find(llvm::StringRef name) const;

  /// The joined summary of every address-taken function of type `typeKey`,
  /// or null if there is none.
  [[nodiscard]] const core::FunctionSummary *
  candidates(llvm::StringRef typeKey) const;

  [[nodiscard]] const GlobalNames &globals() const noexcept {
    return globalNames;
  }

  /// RFC 0010: whether some unit lists `key` among its count fields.
  [[nodiscard]] bool isKnownCount(llvm::StringRef key) const {
    return countFields.contains(key);
  }
  [[nodiscard]] const std::set<std::string, std::less<>> &
  knownCounts() const noexcept {
    return countFields;
  }

  /// RFC 0012: the sized-field facts of every unit, unioned.
  [[nodiscard]] const SizedFieldFacts &sizedFieldFacts() const noexcept {
    return sizedFields;
  }

  /// Rewrites a database summary for use in the unit `context` describes:
  /// each global root becomes the unit's external-linkage variable of that
  /// name, interned in `table`, or is dropped if the unit declares none.
  [[nodiscard]] core::FunctionSummary
  importInto(const core::FunctionSummary &summary,
             const clang::ASTContext &context, GlobalTable &table) const;

  /// Sorted names of every exported function, then every type key with
  /// candidates, in the RFC 0003 dump spelling (for `--dump-analysis`).
  void dump(llvm::raw_ostream &os) const;

private:
  std::map<std::pair<std::string, core::CallContext>, core::FunctionSummary>
      memorySummaries;
  std::map<std::string, std::set<core::CallContext>, std::less<>>
      memoryRequests;
  std::map<std::string, core::FunctionSummary, std::less<>> functions;
  std::map<std::string, core::FunctionSummary, std::less<>> callableSummaries;
  std::map<std::pair<std::string, core::CallbackBindings>,
           core::FunctionSummary>
      contextSummaries;
  std::map<std::string, std::set<core::CallbackBindings>, std::less<>>
      callbackRequests;

  std::map<std::string, core::FunctionSummary, std::less<>> candidateSummaries;
  GlobalNames globalNames;
  std::set<std::string, std::less<>> countFields;
  SizedFieldFacts sizedFields;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_PROGRAMDATABASE_H
