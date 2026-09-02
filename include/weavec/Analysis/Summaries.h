//===- Summaries.h - Function summaries for Clang declarations -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Resolves a callee to its `core::FunctionSummary` (RFC 0003, *The
// translation-unit driver*), in this order: annotations on the declaration,
// the summary inferred from its body in this TU, the shipped table for the
// C standard library, or nothing (an unannotated external function).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_SUMMARIES_H
#define WEAVEC_ANALYSIS_SUMMARIES_H

#include "weavec/Analysis/Annotations.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/Decl.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace weavec::analysis {

/// Interns the globals that appear as summary roots in one translation unit,
/// so a summary can name a global by a small integer (Core is Clang-free).
class GlobalTable {
public:
  /// The id for `var`, allocated on first use. `var` must have global
  /// storage (a file-scope variable or a `static` local).
  [[nodiscard]] std::uint32_t idFor(const clang::VarDecl &var);

  /// The variable behind `id`, or null if `id` was never allocated.
  [[nodiscard]] const clang::VarDecl *declFor(std::uint32_t id) const noexcept;

  /// Display name for `id`, or `<global>` if unknown.
  [[nodiscard]] llvm::StringRef nameOf(std::uint32_t id) const;

  [[nodiscard]] std::size_t size() const noexcept { return decls.size(); }

private:
  llvm::DenseMap<const clang::VarDecl *, std::uint32_t> ids;
  std::vector<const clang::VarDecl *> decls;
};

/// Where a callee's summary came from.
enum class SummarySource : std::uint8_t {
  /// Derived (at least in part) from `WEAVEC_*` annotations on the
  /// declaration.
  Annotation,
  /// Inferred from the callee's body in this translation unit.
  Inferred,
  /// The shipped table for the C standard library.
  Builtin,
};

/// A summary together with its provenance.
struct ResolvedSummary {
  const core::FunctionSummary *summary = nullptr;
  SummarySource source = SummarySource::Inferred;
};

/// Annotations on a function's signature, collected over every
/// redeclaration so a prototype in a header annotates the definition in the
/// source file.
struct SignatureAnnotations {
  AnnotationSet result;
  std::vector<AnnotationSet> params;
  bool unsafe = false;

  /// True if the result or any parameter carries an ownership annotation.
  [[nodiscard]] bool anyOwnership() const noexcept;
};

[[nodiscard]] SignatureAnnotations
collectAnnotations(const clang::FunctionDecl &function);

/// True if `function` or any of its parameters carries an ownership
/// annotation (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`).
[[nodiscard]] bool hasOwnershipAnnotations(const clang::FunctionDecl &function);

/// The summary implied by the annotations on `function`'s declaration alone
/// (RFC 0003, provider step 1). Empty if there are none.
[[nodiscard]] core::FunctionSummary
summaryFromAnnotations(const clang::FunctionDecl &function);

/// The shipped summary for a C standard library function, matched by global
/// name as RFC 0002 matches allocators; null for anything else.
[[nodiscard]] const core::FunctionSummary *
builtinSummary(const clang::FunctionDecl &function);

/// The names in the builtin table, for documentation and tests.
[[nodiscard]] std::vector<llvm::StringRef> builtinNames();

/// Holds inferred summaries for one translation unit and answers callee
/// lookups by combining them with annotations and the builtin table.
class SummaryStore {
public:
  /// Records the summary inferred for `function`'s body, replacing any
  /// previous one. Returns true if the summary changed.
  bool setInferred(const clang::FunctionDecl &function,
                   core::FunctionSummary summary);

  /// The inferred summary for `function`, or null if none was recorded.
  [[nodiscard]] const core::FunctionSummary *
  inferredFor(const clang::FunctionDecl &function) const;

  /// Resolves `callee` (RFC 0003 order). Returns an empty optional for a
  /// callee nothing is known about: no annotations, no body analysed here,
  /// not in the builtin table.
  [[nodiscard]] std::optional<ResolvedSummary>
  lookup(const clang::FunctionDecl &callee);

  [[nodiscard]] GlobalTable &globals() noexcept { return globalTable; }
  [[nodiscard]] const GlobalTable &globals() const noexcept {
    return globalTable;
  }

  /// Records that a call to `callee`, which `lookup` could not resolve, was
  /// seen in reported code. Returns true the first time for this callee.
  bool noteUnknownCallee(const clang::FunctionDecl &callee);

private:
  // Node-based maps: `lookup` hands out pointers into them that must stay
  // valid while further lookups insert.
  std::map<const clang::FunctionDecl *, core::FunctionSummary> inferred;
  std::map<const clang::FunctionDecl *, core::FunctionSummary> merged;
  llvm::DenseSet<const clang::FunctionDecl *> unknownCallees;
  GlobalTable globalTable;

  [[nodiscard]] static const clang::FunctionDecl *
  key(const clang::FunctionDecl &function) noexcept {
    return function.getCanonicalDecl();
  }
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_SUMMARIES_H
