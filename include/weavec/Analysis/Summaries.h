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
// C standard library, or nothing (an unannotated external function). For a
// call through a function pointer (RFC 0004, *Boundaries*): annotations on
// the function-pointer type, then the join of every function of that type
// whose address is taken in the TU, or nothing.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_SUMMARIES_H
#define WEAVEC_ANALYSIS_SUMMARIES_H

#include "weavec/Analysis/Annotations.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
  /// Inferred from the callee's body in another unit of the program (RFC
  /// 0005, *The program database*).
  Program,
  /// The shipped table for the C standard library.
  Builtin,
};

class ProgramDatabase;

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

  /// True if the result or any parameter carries an ownership annotation
  /// (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT` or `WEAVEC_RAW`).
  [[nodiscard]] bool anyOwnership() const noexcept;
  /// True if the result or any parameter carries `WEAVEC_NULLABLE` or
  /// `WEAVEC_NONNULL` (RFC 0008).
  [[nodiscard]] bool anyNullness() const noexcept;
  /// True if any parameter carries `WEAVEC_SIZED_BY` (RFC 0011).
  [[nodiscard]] bool anySizedBy() const noexcept;
};

/// RFC 0011, *Annotation surface*: what `WEAVEC_SIZED_BY(n)` on parameter
/// `param` of `function` resolves to: the integer parameter `n` names and
/// the bytes per element of the pointer (1 for `void *` and incomplete
/// pointees). Nothing when the parameter is not annotated, or the annotation
/// is malformed (`n` is not an integer parameter, `param` not a pointer):
/// `FunctionAnalyzer` reports that as `invalid-annotation`.
struct SizedBy {
  const clang::ParmVarDecl *count = nullptr;
  std::int64_t unit = 1;
};
[[nodiscard]] std::optional<SizedBy>
sizedByOf(const clang::FunctionDecl &function, unsigned param);

/// Sets `summary.requiresExtent` from the `WEAVEC_SIZED_BY` annotations on
/// `function`'s parameters (authoritative per parameter).
void applySizedByAnnotations(core::FunctionSummary &summary,
                             const clang::FunctionDecl &function);

[[nodiscard]] SignatureAnnotations
collectAnnotations(const clang::FunctionDecl &function);

/// True if `function` or any of its parameters carries an ownership
/// annotation (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`,
/// `WEAVEC_RAW`).
[[nodiscard]] bool hasOwnershipAnnotations(const clang::FunctionDecl &function);

/// The summary implied by the annotations on `function`'s declaration alone
/// (RFC 0003, provider step 1). Empty if there are none.
[[nodiscard]] core::FunctionSummary
summaryFromAnnotations(const clang::FunctionDecl &function);

/// The function type an indirect call goes through, or null if the callee
/// expression's type is not a (pointer to) prototyped function.
[[nodiscard]] const clang::FunctionProtoType *
indirectCalleeType(const clang::CallExpr &call);

/// The declaration the callee expression of an indirect call names (a
/// variable, parameter or field of function-pointer type), or null when the
/// callee is not a place (`get_hook()(x)`).
[[nodiscard]] const clang::Decl *
indirectCalleeDecl(const clang::CallExpr &call);

/// The shipped summary for a C standard library function, matched by global
/// name as RFC 0002 matches allocators; null for anything else.
[[nodiscard]] const core::FunctionSummary *
builtinSummary(const clang::FunctionDecl &function);

/// The names in the builtin table, for documentation and tests.
[[nodiscard]] std::vector<llvm::StringRef> builtinNames();

/// RFC 0010, *Retaining*: the key of a count field, the canonical spelling
/// of the record type `object` followed by the field path (`struct obj.rc`,
/// `struct obj.base.refs`; `struct obj` alone when `fields` is empty and the
/// object stands for its own count). Empty when `object` is not a named
/// record type or `fields` is not a chain of fields of it.
[[nodiscard]] std::string countFieldKey(clang::QualType object,
                                        llvm::ArrayRef<core::PathElem> fields,
                                        const clang::ASTContext &context);

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

  /// Resolves the callee of an indirect `call` (RFC 0004, *Signatures for
  /// function pointers*): annotations on the function-pointer type, else
  /// the join of the summaries of every address-taken function of that
  /// type. Returns an empty optional when neither applies.
  [[nodiscard]] std::optional<ResolvedSummary>
  lookupIndirect(const clang::CallExpr &call);

  /// Records that `function`'s name is used as a value somewhere in the
  /// translation unit, so it may be the target of an indirect call.
  void addAddressTaken(const clang::FunctionDecl &function);

  /// Whether `addAddressTaken` was called for `function`.
  [[nodiscard]] bool isAddressTaken(const clang::FunctionDecl &function) const;

  /// The address-taken functions whose type matches `call`'s callee type.
  [[nodiscard]] std::vector<const clang::FunctionDecl *>
  candidatesFor(const clang::CallExpr &call) const;

  /// The unit being analysed; needed to spell type keys and to name the
  /// unit's globals when importing program summaries.
  void setContext(const clang::ASTContext *unitContext) noexcept {
    context = unitContext;
  }

  /// Attaches the exports of the other units of the program (RFC 0005):
  /// `lookup` consults them for a callee with external linkage and no body
  /// here, `lookupIndirect` joins their candidates with this unit's.
  void setDatabase(const ProgramDatabase *program) noexcept {
    database = program;
  }
  [[nodiscard]] const ProgramDatabase *programDatabase() const noexcept {
    return database;
  }

  /// The callees `noteUnknownCallee` recorded and the type keys
  /// `noteUnknownIndirect` recorded, sorted (RFC 0005 exports).
  [[nodiscard]] std::vector<std::string> unknownCalleeNames() const;
  [[nodiscard]] std::vector<std::string> unknownIndirectTypeKeys() const;

  [[nodiscard]] GlobalTable &globals() noexcept { return globalTable; }
  [[nodiscard]] const GlobalTable &globals() const noexcept {
    return globalTable;
  }

  /// Records that a call to `callee`, which `lookup` could not resolve, was
  /// seen in reported code. Returns true the first time for this callee.
  bool noteUnknownCallee(const clang::FunctionDecl &callee);

  /// The same for an indirect call `lookupIndirect` could not resolve,
  /// once per function type.
  bool noteUnknownIndirect(const clang::CallExpr &call);

  // -- Count fields (RFC 0010, *Leaks of shares*) -----------------------------

  /// The count-field key of the summary path `path` of `function` (`param 0
  /// *.rc` with `struct obj *` as parameter 0 is `struct obj.rc`), if the
  /// path is one dereference of a parameter or global followed by fields.
  [[nodiscard]] std::optional<std::string>
  countKeyOf(const clang::FunctionDecl &function,
             const core::SummaryPath &path) const;
  /// Records `key` as a known count (a `WEAVEC_REFCOUNT` field, or a field
  /// some analysed function releases a share through).
  void addKnownCount(std::string key);
  /// Whether `key` is a known count here or in the program database.
  [[nodiscard]] bool isKnownCount(llvm::StringRef key) const;
  /// The keys known in this unit (for `UnitExports::countFields`).
  [[nodiscard]] const std::set<std::string> &knownCountKeys() const noexcept;

private:
  // Node-based maps: `lookup` hands out pointers into them that must stay
  // valid while further lookups insert.
  std::map<const clang::FunctionDecl *, core::FunctionSummary> inferred;
  std::map<const clang::FunctionDecl *, core::FunctionSummary> merged;
  std::map<const clang::FunctionDecl *, SummarySource> mergedSource;
  /// Indirect summaries, keyed by the canonical function type and the
  /// declaration whose annotations were applied (null if none).
  std::map<std::pair<const clang::Type *, const clang::Decl *>,
           core::FunctionSummary>
      mergedIndirect;
  std::vector<const clang::FunctionDecl *> addressTaken;
  llvm::DenseSet<const clang::FunctionDecl *> addressTakenSet;
  llvm::DenseSet<const clang::FunctionDecl *> unknownCallees;
  llvm::DenseSet<const clang::Type *> unknownIndirect;
  GlobalTable globalTable;
  const ProgramDatabase *database = nullptr;
  const clang::ASTContext *context = nullptr;
  std::set<std::string> knownCounts;

  /// The database summary for `callee`, imported into this unit, if the
  /// program defines it elsewhere.
  [[nodiscard]] std::optional<core::FunctionSummary>
  programSummaryFor(const clang::FunctionDecl &callee);

  [[nodiscard]] static const clang::FunctionDecl *
  key(const clang::FunctionDecl &function) noexcept {
    return function.getCanonicalDecl();
  }
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_SUMMARIES_H
