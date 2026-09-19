//===- KindInferenceImpl.h - KindInference internals ------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The state `KindInference` builds and `KindInferenceResult` keeps: the
// facts one walk over the unit collects, the §7.3 fixpoint's nodes, and the
// helpers the must-access (KindInferenceMustAccess.cpp) and field
// (KindInferenceFields.cpp) parts share.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_KINDINFERENCEIMPL_H
#define WEAVEC_LIB_ANALYSIS_KINDINFERENCEIMPL_H

#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/KindTable.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/Analysis/Analyses/Dominators.h"
#include "clang/Analysis/CFG.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weavec::analysis {

/// The bytes a pointer value guarantees from where it points (a §7.1 lower
/// bound), as the Single-valid rules judge it syntactically.
struct ValueWidth {
  /// None: nothing is guaranteed. Zero: a valid object of unknown size,
  /// which is Single-valid only for an incomplete pointee.
  std::optional<std::uint64_t> bytes = std::nullopt;
  /// The null pointer constant: Single-valid, and no bound on the width.
  bool nullOnly = false;
  core::Nullability nullability = core::Nullability::Nullable;
  /// Why `bytes` is none.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string why = {};

  [[nodiscard]] static ValueWidth null() {
    return ValueWidth{.bytes = std::nullopt,
                      .nullOnly = true,
                      .nullability = core::Nullability::Nullable,
                      .why = {}};
  }
  [[nodiscard]] static ValueWidth unknown(std::string reason) {
    return ValueWidth{.bytes = std::nullopt,
                      .nullOnly = false,
                      .nullability = core::Nullability::Nullable,
                      .why = std::move(reason)};
  }
  [[nodiscard]] static ValueWidth
  of(std::uint64_t width,
     core::Nullability nullability = core::Nullability::Nullable) {
    return ValueWidth{.bytes = width,
                      .nullOnly = false,
                      .nullability = nullability,
                      .why = {}};
  }
  /// Single-valid for a pointer whose pointee needs `needed` bytes.
  [[nodiscard]] bool covers(std::uint64_t needed) const {
    return nullOnly || (bytes.has_value() && *bytes >= needed);
  }
};

/// The meet of two values that may both reach one place.
[[nodiscard]] ValueWidth meet(const ValueWidth &a, const ValueWidth &b);

/// §7.4: the object width of `pointee`: `sizeof`, or the offset of a
/// flexible trailing array member; 1 for `void`; 0 for an incomplete type.
[[nodiscard]] std::uint64_t objectWidth(clang::QualType pointee,
                                        const clang::ASTContext &context);
/// §7.4: `field` is a trailing array member the unit's
/// `-fstrict-flex-arrays` level treats as flexible.
[[nodiscard]] bool isFlexibleArrayMember(const clang::FieldDecl &field,
                                         const clang::ASTContext &context);
/// A pointer to an object (not a function pointer).
[[nodiscard]] bool isObjectPointer(clang::QualType type);
/// A character type (`char`, `signed char`, `unsigned char`).
[[nodiscard]] bool isCharacter(clang::QualType type);
/// `expr` without parentheses and conversions between object pointer types
/// (and to `void *`), for "the object's type" of a byte-wise write.
[[nodiscard]] const clang::Expr *stripPointerCasts(const clang::Expr *expr);
/// The function a call names, through `*`, `&` and parentheses.
[[nodiscard]] const clang::DeclRefExpr *
calleeReference(const clang::CallExpr &call);
/// The parameter `expr` reads (through parentheses and implicit casts).
[[nodiscard]] const clang::ParmVarDecl *parameterOf(const clang::Expr *expr);
/// The variable `expr` names (through parentheses and implicit casts).
[[nodiscard]] const clang::VarDecl *variableOf(const clang::Expr *expr);
/// `'<record>.<field>'`-style names for messages and dumps.
[[nodiscard]] std::string recordName(const clang::RecordDecl &record);
[[nodiscard]] std::string fieldName(const clang::FieldDecl &field);
/// The record type key of §7.6 and §13.1 (`struct buf`), or empty for a
/// record without a stable name.
[[nodiscard]] std::string recordKey(const clang::RecordDecl &record,
                                    const clang::ASTContext &context);

/// One value that reaches a §7.3 node.
struct Incoming {
  /// Judged in every round; or
  const clang::Expr *value = nullptr;
  /// a fixed contribution (a parameter's A1 entry value, a `LibrarySpec`
  /// out value); or
  std::optional<ValueWidth> fixed = std::nullopt;
  /// the load value of another node (a static parameter's entry).
  std::optional<unsigned> node = std::nullopt;
  /// The store, initialiser, call or `return` it arrives by.
  const clang::Stmt *store = nullptr;
};

/// A node of the §7.3 fixpoint.
struct KindNode {
  enum class Kind : std::uint8_t {
    /// A pointer field (field-based: every object of the record).
    Field,
    /// A pointer variable with static storage.
    Global,
    /// The result of a function the unit defines.
    Result,
    /// A static function's parameter at entry: the join of its arguments.
    ParamEntry,
    /// The value of a local or parameter variable wherever it is read: the
    /// meet of every value assigned to it.
    Value,
  };
  Kind kind = Kind::Field;
  const clang::Decl *decl = nullptr;
  clang::QualType pointee;
  /// `objectWidth(pointee)`.
  std::uint64_t needed = 0;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Incoming> incoming = {};
  /// Stores the syntax does not show (§7.3), which demote unconditionally.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<KindDemotion> forced = {};

  // The fixpoint state.
  bool demoted = false;
  /// Value nodes: the least width assigned; `Top` when only null is.
  std::uint64_t least = Top;
  /// Every value that reaches the node is nonnull.
  bool nonnull = true;
  /// The first reason the node was demoted.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string why = {};

  static constexpr std::uint64_t Top =
      std::numeric_limits<std::uint64_t>::max();
};

/// A store through `*q` whose target the syntax does not name (§7.3): it
/// reaches every address-taken slot of a compatible type.
struct IndirectStore {
  const clang::Expr *value = nullptr;
  /// The lvalue's pointer type (`T *` for `*q = v` with `q` a `T **`).
  clang::QualType type;
  const clang::Stmt *store = nullptr;
  /// `++*q` and friends: pointer arithmetic, never Single-valid.
  bool arithmetic = false;
};

/// A call argument that is the address of a slot, or a pointer that may
/// point to one (§7.3 "a slot whose address is passed to a callee").
struct SlotAddressArgument {
  const clang::CallExpr *call = nullptr;
  unsigned index = 0;
  /// The slot's node, or none when the argument is a `T **` value that may
  /// point to any address-taken slot of a compatible type.
  std::optional<unsigned> node = std::nullopt;
  /// Argument `index`'s type.
  clang::QualType type;
};

/// A function's CFG (every expression an element, in evaluation order) with
/// its post-dominator tree and where each statement sits (§7.4 rule 7,
/// §7.5).
struct FunctionCfg {
  std::unique_ptr<clang::CFG> cfg;
  std::unique_ptr<clang::CFGPostDomTree> postDominators;
  /// Statement to (block, element index).
  llvm::DenseMap<const clang::Stmt *,
                 std::pair<const clang::CFGBlock *, unsigned>>
      where;
  /// Loop statement to the block whose terminator it is (its header).
  llvm::DenseMap<const clang::Stmt *, const clang::CFGBlock *> headers;

  [[nodiscard]] bool postDominates(const clang::CFGBlock *a,
                                   const clang::CFGBlock *b) const {
    return postDominators->dominates(a, b);
  }
};

/// Everything `KindInference` computes; `KindInferenceResult` keeps it.
class KindInferenceState {
public:
  KindInferenceState(clang::ASTContext &ctx, const core::LibrarySpec &spec,
                     KindInferenceOptions inferenceOptions, KindTable &table)
      : context(ctx), library(spec), options(inferenceOptions), kinds(table) {}

  void run();

  // -- Queries (valid after `run`) -------------------------------------------
  [[nodiscard]] ValueWidth judge(const clang::Expr *expr) const;
  [[nodiscard]] bool alwaysReturns(const clang::FunctionDecl &function) const;
  [[nodiscard]] bool knownToReturn(const clang::CallExpr &call) const;

  clang::ASTContext &context;
  const core::LibrarySpec &library;
  KindInferenceOptions options;
  KindTable &kinds;

  // -- What the walk collects -------------------------------------------------
  struct FunctionFacts {
    const clang::FunctionDecl *definition = nullptr;
    bool addressTaken = false;
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::vector<const clang::CallExpr *> calls = {};
  };
  /// By canonical declaration.
  llvm::DenseMap<const clang::FunctionDecl *, FunctionFacts> functions;
  /// Every function the unit defines, in source order.
  std::vector<const clang::FunctionDecl *> definitions;
  /// Every function the unit declares and references, in source order.
  std::vector<const clang::FunctionDecl *> referenced;
  /// The calls in each definition's body.
  llvm::DenseMap<const clang::FunctionDecl *,
                 std::vector<const clang::CallExpr *>>
      callsIn;

  struct VariableUse {
    bool assigned = false;
    /// Incremented, decremented, `+=`/`-=`, or its address taken.
    bool modified = false;
    bool addressTaken = false;
  };
  /// Parameters and locals, by declaration.
  llvm::DenseMap<const clang::VarDecl *, VariableUse> variableUses;

  std::vector<KindNode> nodes;
  llvm::DenseMap<const clang::Decl *, unsigned> nodeOf;
  /// Static parameters' entry nodes, by the definition's parameter.
  llvm::DenseMap<const clang::ParmVarDecl *, unsigned> entryOf;

  std::vector<IndirectStore> indirectStores;
  std::vector<SlotAddressArgument> slotAddressArguments;
  /// Slots whose address is taken.
  llvm::DenseSet<unsigned> addressTaken;
  /// A pointer to pointers was made from a pointer to something else (other
  /// than a fresh allocation): it may point into any object, so every slot
  /// counts as address-taken.
  bool everySlotTaken = false;
  /// A byte-wise write into an object of `record` (§7.3; §7.6): a
  /// `LibrarySpec` argument with `w` or `rw` access, or a character-typed
  /// store. A zero fill or a copy from the same type leaves the pointer
  /// fields Single-valid (§7.3) but still disqualifies §7.6 candidates.
  struct ByteWrite {
    const clang::RecordDecl *record = nullptr;
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    KindDemotion demotion = {};
    bool keepsPointers = false;
  };
  std::vector<ByteWrite> byteWrites;
  /// Union members stored, by union.
  llvm::DenseMap<
      const clang::RecordDecl *,
      std::vector<std::pair<const clang::FieldDecl *, const clang::Stmt *>>>
      unionStores;

  // §7.6 inputs.
  /// Fields whose address is taken, with where.
  llvm::DenseMap<const clang::FieldDecl *, const clang::Stmt *> fieldAddresses;
  /// A pointer to `record` converted from a pointer to another object type
  /// (§7.6); `fromBytes` when that is a character buffer, whose bytes no
  /// store into the record's fields wrote (§7.3).
  struct Conversion {
    const clang::RecordDecl *record = nullptr;
    const clang::CastExpr *where = nullptr;
    bool fromBytes = false;
  };
  std::vector<Conversion> conversions;
  /// References to each parameter of a function the unit defines, for the
  /// reliance and confinement classifications.
  llvm::DenseMap<const clang::ParmVarDecl *,
                 std::vector<const clang::DeclRefExpr *>>
      parameterReferences;
  /// Records defined by the unit, in source order.
  std::vector<const clang::RecordDecl *> records;
  /// Pairs of fields some function mentions together.
  llvm::DenseSet<std::pair<const clang::FieldDecl *, const clang::FieldDecl *>>
      related;

  // -- Results ----------------------------------------------------------------
  std::vector<ResolvedCandidate> candidates;
  std::vector<DisqualifiedRecord> disqualified;
  std::vector<StoreGroup> groups;
  mutable llvm::DenseMap<const clang::FunctionDecl *, bool> returns;

  // -- Steps ------------------------------------------------------------------
  void collect();
  void addHiddenStores();
  void solve();
  void fillTable();
  void inferMustAccess();
  void inferFieldCandidates();
  void collectStoreGroups();

  // Helpers shared by the steps.
  [[nodiscard]] std::optional<unsigned>
  slotNode(const clang::Expr *lvalue) const;
  [[nodiscard]] ValueWidth loadOf(unsigned index) const;
  [[nodiscard]] ValueWidth judgeLoad(const clang::Expr *lvalue) const;
  [[nodiscard]] ValueWidth judgeAddress(const clang::Expr *lvalue) const;
  [[nodiscard]] ValueWidth judgeArray(const clang::Expr *array) const;
  [[nodiscard]] ValueWidth judgeCall(const clang::CallExpr &call) const;
  [[nodiscard]] ValueWidth declaredWidth(const KindEntry &entry,
                                         clang::QualType pointee,
                                         const clang::CallExpr *call) const;
  [[nodiscard]] std::optional<std::int64_t>
  constantOf(const clang::Expr *expr) const;
  /// §7.3: `function` is `main(int argc, char **argv[, ...])` and neither
  /// `argc` nor `argv` is ever assigned.
  [[nodiscard]] bool isMainArgv(const clang::FunctionDecl &definition) const;
  /// Whether the static function `function` gets the §7.3 join.
  [[nodiscard]] bool joinsArguments(const clang::FunctionDecl &function) const;
  /// Whether `param` (of a function the unit defines) only ever has
  /// `*param` read or written and is compared, never copied, passed on,
  /// returned or modified: a callee whose stores through it the
  /// indirect-store rule already sees.
  [[nodiscard]] bool confinedParameter(const clang::ParmVarDecl &param) const;
  /// §7.3 `reliesOnSingle`: some use of `param`'s value may rest on its
  /// Single default (every use but comparisons, conversions to integers,
  /// pointer arithmetic, non-zero subscripts and assignments to it).
  [[nodiscard]] bool reliesOnDefault(const clang::ParmVarDecl &param) const;
  /// The value of a `LibrarySpec` result or out value at `call`.
  [[nodiscard]] ValueWidth libraryValue(const core::LibraryResult &result,
                                        const core::LibraryMatch &match,
                                        const clang::CallExpr &call,
                                        clang::QualType pointee) const;
  /// The CFG of a function the unit defines, built on first use; null when
  /// Clang cannot build one.
  [[nodiscard]] const FunctionCfg *
  cfgOf(const clang::FunctionDecl &function) const;
  mutable llvm::DenseMap<const clang::FunctionDecl *,
                         std::unique_ptr<FunctionCfg>>
      cfgs;
  /// The parent map of a function's body, built on first use.
  [[nodiscard]] const clang::ParentMap &
  parentsOf(const clang::FunctionDecl &function) const;
  mutable llvm::DenseMap<const clang::FunctionDecl *,
                         std::unique_ptr<clang::ParentMap>>
      parentMaps;
  [[nodiscard]] std::string nodeName(unsigned index) const;
  unsigned addNode(KindNode::Kind kind, const clang::Decl *decl,
                   clang::QualType pointee);
  void demote(unsigned index, const clang::Stmt *store, std::string reason);
};

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_KINDINFERENCEIMPL_H
