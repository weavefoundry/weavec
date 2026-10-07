//===- GuardPassImpl.h - The guard passes' parts ----------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared by GuardPass.cpp (the module's context, the markers, the
// registration), GuardInsert.cpp (accesses and their markers, pruning),
// GuardLibrary.cpp (C library calls), GuardLoops.cpp (loop ranges),
// GuardExpand.cpp (removal and inline checks), GuardFrames.cpp (stack
// objects and non-local exits) and GuardGlobals.cpp (global redzones). Not
// installed.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_FRONTEND_GUARDPASSIMPL_H
#define WEAVEC_LIB_FRONTEND_GUARDPASSIMPL_H

#include "weavec/Frontend/GuardPass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace weavec::frontend::guard {

/// The flags of a site record (runtime/weavec_rt.h).
inline constexpr unsigned SiteWrite = 1;
inline constexpr unsigned SiteReport = 2;
inline constexpr unsigned SiteProven = 4;
inline constexpr unsigned SiteNullOk = 8;

/// The markers GuardInsert puts before accesses and GuardExpand turns into
/// checks:
///
///   void __weavec.guard(ptr at, i64 width, i32 flags, ptr base, i32 row)
///   void __weavec.range(ptr at, i64 length, i32 flags, i32 row)
///   i64  __weavec.strlen(ptr string, i64 max, i32 flags, i32 row)
///
/// `base`, when not null, is tested for null (section 2.4); `row` is the
/// access's ledger row.
inline constexpr llvm::StringLiteral MarkerGuard = "__weavec.guard";
inline constexpr llvm::StringLiteral MarkerRange = "__weavec.range";
inline constexpr llvm::StringLiteral MarkerString = "__weavec.strlen";
/// `void __weavec.disjoint(ptr destination, ptr source, i64 length)`: a
/// copy whose operands must not overlap (section 2.5).
inline constexpr llvm::StringLiteral MarkerDisjoint = "__weavec.disjoint";
/// `void __weavec.scope()`: beside each lifetime marker until the frame is
/// laid out, a write of the runtime's memory that keeps the guards of an
/// object's accesses inside its scope (no pass hoists a guard above the
/// object's `lifetime.start`); removed once the lifetime marker is.
inline constexpr llvm::StringLiteral MarkerScope = "__weavec.scope";

/// Marker flags: the access writes; verify mode's monitor of a guard a rule
/// removed; the log2 of the access's alignment in bits 8 to 15.
inline constexpr unsigned MarkerWrite = 1;
inline constexpr unsigned MarkerProven = 2;
/// A string marker's string may be null (the call accepts it).
inline constexpr unsigned MarkerNullOk = 4;
inline constexpr unsigned MarkerAlignShift = 8;
/// A guard the loop ranges of rule 6.4 keep (the guarded copy of a
/// versioned loop's).
inline constexpr unsigned MarkerKeep = 1U << 16U;

/// The redzone between a frame's objects and after a global (section 3.2,
/// section 4): at least 16 bytes, one eighth of the object up to 4 KiB, so
/// that the object and its redzone end on a 32-byte boundary.
[[nodiscard]] std::uint64_t paddedSize(std::uint64_t size);

/// The size of an object whose extent the unit knows exactly: a static
/// alloca, or a global defined here that no other definition can replace.
[[nodiscard]] std::optional<std::uint64_t>
exactSize(const llvm::Value *object, const llvm::DataLayout &layout);

/// Whether `width` bytes at a constant `offset` lie inside `object`.
[[nodiscard]] bool insideAt(const llvm::Value *object, std::int64_t offset,
                            std::uint64_t width,
                            const llvm::DataLayout &layout);

/// Section 3.3: whether an instruction may run while a local is out of
/// scope, after one of its lifetime ends and before the next start. Rule
/// 6.1 removes the guard of an access to a local only when it cannot, so
/// that the guard catches a use after its scope.
class Scopes {
public:
  [[nodiscard]] bool mayBeOutOfScope(const llvm::AllocaInst &alloca,
                                     const llvm::Instruction &at);

private:
  /// Per alloca with lifetime ends: the blocks it may be out of scope at
  /// the start of.
  llvm::DenseMap<const llvm::AllocaInst *,
                 llvm::DenseSet<const llvm::BasicBlock *>>
      deadAtEntry;
};

/// What the passes need of the module: types, the markers, the runtime's
/// entry points, the shadow descriptor, the site records (deduplicated) and
/// the ledger.
class ModuleContext {
public:
  ModuleContext(llvm::Module &module, const GuardOptions &options);

  llvm::Module &module;
  const llvm::DataLayout &layout;
  llvm::LLVMContext &context;
  const GuardOptions &options;
  llvm::Type *i8;
  llvm::Type *i32;
  llvm::Type *i64;
  llvm::PointerType *ptr;

  [[nodiscard]] bool verify() const {
    return options.mode == GuardOptions::Mode::Verify;
  }

  /// The markers, declared with the attributes that keep them.
  llvm::FunctionCallee markerGuard();
  llvm::FunctionCallee markerRange();
  llvm::FunctionCallee markerString();
  llvm::FunctionCallee markerDisjoint();
  llvm::FunctionCallee markerScope();
  /// Whether `call` is a scope barrier.
  [[nodiscard]] static bool isScope(const llvm::CallBase &call);
  /// The marker `call` is, if any.
  [[nodiscard]] static std::optional<llvm::StringRef>
  markerOf(const llvm::CallBase &call);

  /// Inserts a guard marker of `width` bytes at `pointer` before `before`.
  llvm::CallInst *insertGuard(llvm::Instruction *before, llvm::Value *pointer,
                              std::uint64_t width, llvm::Align alignment,
                              unsigned flags, llvm::Value *nullBase,
                              unsigned row);
  /// Inserts a range marker of `length` (an integer) bytes.
  llvm::CallInst *insertRange(llvm::Instruction *before, llvm::Value *pointer,
                              llvm::Value *length, unsigned flags,
                              unsigned row);
  /// Inserts a disjointness marker of a copy of `length` bytes.
  void insertDisjoint(llvm::Instruction *before, llvm::Value *destination,
                      llvm::Value *source, llvm::Value *length);
  /// Inserts a string marker; its result is the string's length, at most
  /// `max` (an i64).
  llvm::Value *insertString(llvm::IRBuilder<> &builder, llvm::Value *pointer,
                            llvm::Value *max, unsigned row, unsigned flags);

  /// A call of a slow path (`guardFunction`, `nullFunction`) with its
  /// calling convention.
  static llvm::CallInst *callSlow(llvm::IRBuilder<> &builder,
                                  llvm::FunctionCallee callee,
                                  llvm::ArrayRef<llvm::Value *> args);

  /// `__weavec_rt_<name>` with the given type, declared once.
  llvm::FunctionCallee runtime(llvm::StringRef name, llvm::FunctionType *type);
  llvm::FunctionCallee guardFunction();
  llvm::FunctionCallee nullFunction();
  llvm::FunctionCallee rangeFunction();
  llvm::FunctionCallee rangeOkFunction();
  llvm::FunctionCallee strlenFunction();

  /// `__weavec_rt_shadow`.
  llvm::GlobalVariable *shadowDescriptor();

  /// The site record of an access at `location` (section 2.3), with the
  /// mode's flags added to `flags`.
  llvm::Constant *site(const llvm::DebugLoc &location, unsigned flags);

  /// The source location of an instruction, for the ledger and the unsafe
  /// regions: its file (absolute where the debug information says), line
  /// and column.
  struct Position {
    std::string file;
    unsigned line = 0;
    unsigned column = 0;
  };
  [[nodiscard]] static Position positionOf(const llvm::DebugLoc &location);

  /// Whether `location` lies in a `WEAVEC_UNSAFE` region.
  [[nodiscard]] bool isUnsafe(const llvm::DebugLoc &location) const;

  /// Adds a ledger row for an access of `function`; its index.
  unsigned row(const llvm::Function &function, const llvm::DebugLoc &location,
               llvm::StringRef operation, std::uint64_t bytes,
               LedgerRow::Outcome outcome, llvm::StringRef reason);
  /// A copy of `row`'s guard stays, or a rule removed it with `reason`.
  void copy(unsigned row, bool stays, llvm::StringRef reason);

  /// Whether `name` is the runtime's or a guard's own function.
  [[nodiscard]] static bool isRuntimeName(llvm::StringRef name);

private:
  llvm::StringMap<llvm::Constant *> files;
  std::map<std::tuple<llvm::Constant *, unsigned, unsigned, unsigned>,
           llvm::Constant *>
      sites;
  llvm::StructType *siteType = nullptr;
  llvm::GlobalVariable *descriptor = nullptr;
};

/// The shadow descriptor's fields, loaded once per function.
struct ShadowValues {
  llvm::Value *base = nullptr;
  llvm::Value *mask = nullptr;
};

/// One function's state while GuardExpand runs on it.
class FunctionContext {
public:
  FunctionContext(ModuleContext &module, llvm::Function &function);
  ~FunctionContext();
  FunctionContext(const FunctionContext &) = delete;
  FunctionContext &operator=(const FunctionContext &) = delete;

  ModuleContext &module;
  llvm::Function &function;

  /// The descriptor's fields, loaded at the function's entry (and removed
  /// again if nothing used them).
  ShadowValues shadow();
  /// Where code that must run first at entry goes: right after the
  /// descriptor's loads.
  [[nodiscard]] llvm::Instruction *entry() const;

  /// The shadow byte address of `address` (an i64), at `builder`.
  llvm::Value *shadowAddress(llvm::IRBuilder<> &builder, llvm::Value *address);

  /// One access a merged guard covers, checked on its own on the slow
  /// path so that a failure names the access that fails.
  struct Member {
    llvm::Value *pointer;
    std::uint64_t width;
    llvm::Constant *site;
  };

  /// The guard of `width` constant bytes at `pointer`, before `before`:
  /// inline where the width allows, the runtime's range guard otherwise.
  /// `nullBase`, when set, is tested for null first (section 2.4). The slow
  /// path of a merged guard checks its `members` in order.
  void emitGuard(llvm::Instruction *before, llvm::Value *pointer,
                 std::uint64_t width, llvm::Align alignment,
                 llvm::Constant *site, llvm::Value *nullBase,
                 llvm::ArrayRef<Member> members = {});
  /// A range guard of `length` (an integer) bytes: inline when it is short.
  void emitRange(llvm::Instruction *before, llvm::Value *pointer,
                 llvm::Value *length, llvm::Constant *site);
  /// A bounded string guard of at most 113 bytes at `pointer`: inline when
  /// every byte of the bound is addressable, the runtime's scan otherwise.
  void chunkCheckString(llvm::Instruction *before, llvm::Value *pointer,
                        std::uint64_t bound, llvm::Constant *site);
  /// The inline check of up to 113 bytes (`width`, an i64) at `address` (an
  /// i64), whose shadow byte address is `shadowAt` if known. Its slow path
  /// is `slow` with `(address, width, site)`, the guard's by default.
  void chunkCheck(llvm::Instruction *before, llvm::Value *address,
                  llvm::Value *width, llvm::Constant *site,
                  llvm::Value *nullTest, llvm::Value *shadowAt,
                  llvm::FunctionCallee slow = {},
                  llvm::ArrayRef<Member> members = {});

  /// Writes `bytes` (shadow values, one per granule) over the shadow of the
  /// granules from `pointer` (16-aligned), before `before`, when the shadow
  /// exists.
  void writeShadow(llvm::Instruction *before, llvm::Value *pointer,
                   llvm::ArrayRef<std::uint8_t> bytes);

private:
  /// The slow path of a failed inline check: the runtime's guard of the
  /// bytes, or of each member.
  void slowGuard(llvm::IRBuilder<> &builder, llvm::Value *address,
                 llvm::Value *width, llvm::Constant *site,
                 llvm::ArrayRef<Member> members);

  ShadowValues loaded;
  llvm::Instruction *anchor = nullptr;
};

/// Section 2.5: guards and redirects the calls of the library table in
/// `function`, adding their ledger rows.
void guardLibraryCalls(ModuleContext &module, llvm::Function &function,
                       bool late);

/// Section 2.6: disables the `array-bounds` checks in unsafe regions.
void dropUnsafeBoundsChecks(const ModuleContext &module,
                            llvm::Function &function);

/// Section 3: lays out the tracked static allocas in one frame with
/// redzones, tracks their scopes, poisons the dynamic ones, and clears the
/// frame at each return. `tracked` lists the static and dynamic allocas
/// to track. Runs before any guard is expanded; returns what replaced each
/// alloca.
llvm::DenseMap<llvm::Value *, llvm::Value *>
layoutFrame(FunctionContext &context,
            llvm::ArrayRef<llvm::AllocaInst *> tracked);

/// Section 3.5: clears the stack's shadow before each call that does not
/// return.
void unpoisonBeforeNoReturn(FunctionContext &context);

/// Section 4: gives the module's globals redzones and registers them.
void instrumentGlobals(ModuleContext &module);

/// GuardInsert and GuardPrune on one function.
void insertGuards(ModuleContext &module, llvm::Function &function);
void pruneGuards(ModuleContext &module, llvm::Function &function);
/// Rule 6.4: versions the loops whose guards one range per access covers;
/// whether it changed the function.
bool versionLoops(ModuleContext &module, llvm::Function &function,
                  llvm::FunctionAnalysisManager &manager);
/// Whether `instruction` may end an object's lifetime (a call that may
/// free, a lifetime marker, a stack restore), so that no guard before it
/// covers an access after it.
[[nodiscard]] bool killsGuards(const llvm::Instruction &instruction);
/// Puts a scope barrier beside each lifetime marker that has none.
void addScopes(ModuleContext &module, llvm::Function &function);
/// Removes the scope barriers, or with `orphansOnly` those in a block with
/// no lifetime marker left.
void dropScopes(llvm::Function &function, bool orphansOnly);
/// GuardExpand on one function.
void expandGuards(ModuleContext &module, llvm::Function &function,
                  llvm::FunctionAnalysisManager &analyses);

/// Whether the passes leave `function` alone: a declaration, a naked
/// function, or the runtime's.
[[nodiscard]] bool skips(const llvm::Function &function);

} // namespace weavec::frontend::guard

#endif // WEAVEC_LIB_FRONTEND_GUARDPASSIMPL_H
