//===- KindInference.h - Inferred pointer kinds (RFC 0030) ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §7.3–§7.6, before the engine runs: everything `KindTable` holds
// beyond the declared kinds, from the syntax alone. No engine fact is used,
// so there is no circularity; the engine refines what is left (a static
// parameter's argument proven to have an element, the `reliesOnSingle`
// flags, the §7.6 rounds).
//
//   - Parameter defaults (§7.3): a parameter of a `static` function whose
//     address is never taken gets the join of its arguments at the direct
//     calls, judged by the Single-valid rules; every other parameter is
//     `single nullable` by A1, with `reliesOnSingle` set when some use of
//     the parameter's value in the body may rest on that default (a
//     syntactic over-approximation: every use but comparisons, conversions
//     to integers, pointer arithmetic and non-zero subscripts).
//   - `argv` of `main` (§7.3).
//   - Results (§7.3): the join over the unit's `return`s for a function the
//     unit defines, the A3 default for one it only declares.
//   - Slot kinds (§7.3): every pointer field and pointer variable with
//     static storage starts `single` and is demoted to `unknown`, as a
//     greatest fixpoint, by a store whose value is not Single-valid and by
//     the stores the syntax does not show: a store through `*q` (through a
//     `void *` or character lvalue: of any type) into an address-taken slot,
//     where every slot counts as address-taken once a pointer to pointers is
//     made from anything but a fresh allocation; its address passed to a
//     callee that may store through it, or converted to a pointer to
//     non-pointers; a byte-wise write into its object (a zero fill or a copy
//     from the same type excepted); a store to another member of its union;
//     its object made from a byte buffer. Elements of arrays of pointers
//     are no slots: a load from one is never Single-valid.
//   - Must-access requirements R1–R5 (§7.5), from the function's CFG and
//     post-dominator tree, with the `static`/exported enforcement.
//   - Store groups (§7.4 rule 7) and the §7.6 candidates, with the
//     disqualifications before round 1.
//
// Single-valid values (§7.3): null; `&object`, an array decaying to its
// first element and a string literal; an allocation whose `LibrarySpec`
// extent is at least the object width with constant operands; a parameter
// under its A1 default or its static join; a call result whose kind is
// `single` (A3 for functions outside the unit); a load from a slot that is
// still `single`. Two sound extensions: a local that is never incremented
// or address-taken is Single-valid when every value assigned to it is
// (locals carry the least width stored, so `void *m = malloc(sizeof(T))`
// then `(T *)m` is Single-valid), and a conversion is Single-valid when the
// source's width covers the object width of the new pointee (§7.4 rule 4).
//
// "Known to return" (§7.5) for a function the unit defines is a syntactic
// greatest fixpoint: every call in its body is known to return (a
// `LibrarySpec` row without `noreturn` or `exits`, a C library, POSIX or
// platform declaration without `noreturn`, a compiler builtin, or another
// such function of the unit). Indirect and unknown callees never are.
//
// Like every §14 component outside the engine, this one depends on no part
// of `FunctionDataflow`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_KINDINFERENCE_H
#define WEAVEC_ANALYSIS_KINDINFERENCE_H

#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/LedgerAdapter.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Core/LibrarySpec.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace weavec::analysis {

class SlotCollection;

struct KindInferenceOptions {
  /// §7.5 must-access requirements.
  bool mustAccess = true;
  /// §7.6 candidates (the designated first cut).
  bool fieldCandidates = true;
  /// Drop the candidates of a pointer and an integer field that no function
  /// of the unit mentions together. Dropping a candidate only loses an
  /// invariant, so this is sound; it keeps the engine's store verdicts
  /// proportional to the fields the code relates.
  bool pruneUnrelatedCandidates = true;
  /// §9.3's slots for the unit and their solution: the result of an
  /// indirect call through a closed slot is Single-valid when every target's
  /// result is `single`, and through an open slot it is the A3 default.
  /// Without them, indirect call results are never Single-valid.
  const SlotCollection *slots = nullptr;
  const core::SlotSolution *slotSolution = nullptr;
};

/// §7.4 rule 7: stores that update a pointer and its count, per a declared
/// or candidate counted relation, in one basic block with no intervening
/// call, loop or access through the object. The obligation (a declared
/// kind's check, or a §7.6 candidate's store verdict) is decided once,
/// after the last store. A single store is a group of one.
struct StoreGroup {
  const clang::FunctionDecl *function = nullptr;
  const clang::RecordDecl *record = nullptr;
  /// The base of the first store's member access (`b` in `b->data = ...`).
  const clang::Expr *object = nullptr;
  /// The assignments (and increments) of relation fields, in evaluation
  /// order.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<const clang::Expr *> stores = {};
  /// The relation fields the group writes, in first-store order.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<const clang::FieldDecl *> fields = {};

  /// The store after which the obligation is decided.
  [[nodiscard]] const clang::Expr *last() const {
    return stores.empty() ? nullptr : stores.back();
  }
};

/// A §7.6 candidate with the declarations it names.
struct ResolvedCandidate {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  FieldCandidate candidate = {};
  const clang::RecordDecl *record = nullptr;
  const clang::FieldDecl *pointer = nullptr;
  const clang::FieldDecl *count = nullptr;
};

/// §7.6: a struct whose candidates were all dropped before round 1.
struct DisqualifiedRecord {
  const clang::RecordDecl *record = nullptr;
  /// The record type key (`struct buf`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string key = {};
  /// The first reason found, and where.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string reason = {};
  const clang::Stmt *where = nullptr;
};

class KindInferenceState;

/// What `KindInference::infer` found besides the table.
class KindInferenceResult {
public:
  KindInferenceResult();
  ~KindInferenceResult();
  KindInferenceResult(KindInferenceResult &&) noexcept;
  KindInferenceResult &operator=(KindInferenceResult &&) noexcept;
  KindInferenceResult(const KindInferenceResult &) = delete;
  KindInferenceResult &operator=(const KindInferenceResult &) = delete;

  /// §7.6: the candidates assumed at entry in round 1
  /// (`EngineInput::fieldAssumptions`), sorted.
  [[nodiscard]] std::vector<FieldCandidate> fieldCandidates() const;
  [[nodiscard]] const std::vector<ResolvedCandidate> &
  resolvedCandidates() const noexcept;
  [[nodiscard]] const std::vector<DisqualifiedRecord> &
  disqualified() const noexcept;
  /// §7.4 rule 7, per function in source order.
  [[nodiscard]] const std::vector<StoreGroup> &storeGroups() const noexcept;

  /// §7.3: whether `value` is Single-valid for a pointer to `pointee` under
  /// the final slot kinds (§13.1 `imports.calls`, the §7.3 Call-site row).
  [[nodiscard]] bool isSingleValid(const clang::Expr &value,
                                   clang::QualType pointee) const;
  /// The same for argument `index` of `call`, against the parameter's
  /// pointee (the argument's own pointee past the prototype).
  [[nodiscard]] bool argumentIsSingleValid(const clang::CallExpr &call,
                                           unsigned index) const;
  /// §7.5: whether `function`, defined in the unit, always returns.
  [[nodiscard]] bool alwaysReturns(const clang::FunctionDecl &function) const;
  /// §7.5: whether `call` is known to return.
  [[nodiscard]] bool knownToReturn(const clang::CallExpr &call) const;

private:
  friend class KindInference;
  std::unique_ptr<KindInferenceState> state;
};

/// Completes a unit's `KindTable`.
class KindInference {
public:
  KindInference(clang::ASTContext &ctx, const core::LibrarySpec &spec,
                KindInferenceOptions inferenceOptions = {})
      : context(ctx), library(spec), options(inferenceOptions) {}

  /// Fills `kinds`, which holds the declared kinds `AttributeReader` read,
  /// with the defaults, inferred kinds, requirements and reliance flags of
  /// every function the unit defines or calls and every slot, and returns
  /// the rest. Declared shapes and nullabilities are kept; each entry's
  /// `kind.source` and `extentClass` describe its shape. The result refers
  /// to `kinds`, the AST and `KindInferenceOptions::slots`: keep them alive
  /// while it is used.
  [[nodiscard]] KindInferenceResult infer(KindTable &kinds);

private:
  clang::ASTContext &context;
  const core::LibrarySpec &library;
  KindInferenceOptions options;
};

/// `weavec --dump-kinds`: every entry of `kinds`, the problems and
/// suggestions, the store groups and the candidates, one per line in source
/// order (a debugging aid; the format is unstable).
void dumpKinds(const KindTable &kinds, const KindInferenceResult &inferred,
               clang::ASTContext &context, llvm::raw_ostream &os);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_KINDINFERENCE_H
