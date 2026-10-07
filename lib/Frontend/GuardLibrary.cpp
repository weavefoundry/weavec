//===- GuardLibrary.cpp - C library calls ---------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §2.5. A direct call of a function of the library table
// (`LibrarySpec.txt`) that the unit does not define is guarded before the
// call: each pointer argument the row reads or writes over the bytes its
// term gives, evaluated on the call's arguments (`strlen` by the runtime,
// inside the string's addressable bytes), a string argument as a string,
// and the `%s` and `%n` arguments of a constant `printf` or `scanf` format.
// A row's `wrapper` redirects the call to the runtime's checked version
// (the calls whose bytes only the call computes, those that read less than
// their terms state, and `mmap` and `munmap`, whose versions clear the
// shadow of what they map); a call that only computes a string's length is
// replaced by the string guard that computes it.
//
//===----------------------------------------------------------------------===//

#include "GuardPassImpl.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <limits>
#include <optional>

namespace weavec::frontend::guard {

/// The C name of a callee: Darwin's `\01_name$VARIANT` asm labels and the
/// variant suffixes dropped.
static llvm::StringRef cName(llvm::StringRef name) {
  if (name.consume_front("\1"))
    name.consume_front("_");
  return name.split('$').first;
}

namespace {

/// One call being guarded.
struct LibraryCall {
  ModuleContext &context;
  llvm::Function &function;
  llvm::CallInst &call;
  const core::LibraryMatch &match;
  llvm::StringRef name;

  [[nodiscard]] ModuleContext &module() const { return context; }

  /// The call's argument for row argument `index`, or null.
  [[nodiscard]] llvm::Value *argument(unsigned index) const {
    const int at = match.callArgument(index);
    if (at < 0 || static_cast<unsigned>(at) >= call.arg_size())
      return nullptr;
    return call.getArgOperand(static_cast<unsigned>(at));
  }

  [[nodiscard]] llvm::Constant *site(bool write) const {
    return module().site(call.getDebugLoc(), write ? SiteWrite : 0);
  }

  unsigned record(llvm::StringRef reason, std::uint64_t bytes = 0) const {
    return module().row(function, call.getDebugLoc(), ("call:" + name).str(),
                        bytes, LedgerRow::Outcome::Guarded, reason);
  }

  /// The bytes of a `wchar_t` (the module's `wchar_size`, 4 by default).
  [[nodiscard]] std::uint64_t wcharSize() const {
    if (const auto *size = llvm::mdconst::extract_or_null<llvm::ConstantInt>(
            function.getParent()->getModuleFlag("wchar_size")))
      return size->getZExtValue();
    return 4;
  }

  /// Whether row argument `index` may be null.
  [[nodiscard]] bool nullOk(unsigned index) const {
    const std::vector<core::LibraryParam> &params = match.entry->params;
    return index < params.size() &&
           params[index].null != core::LibraryParam::Null::Forbidden;
  }

  /// The checked length of the string at `pointer`, at most `bound`; a
  /// literal's is a constant. `flags` are the marker's (`MarkerNullOk`).
  llvm::Value *stringLength(llvm::IRBuilder<> &builder, llvm::Value *pointer,
                            llvm::Value *bound, unsigned flags) const {
    llvm::StringRef literal;
    if (llvm::getConstantStringInfo(pointer, literal)) {
      std::uint64_t length = literal.size();
      if (const auto *constant =
              llvm::dyn_cast_or_null<llvm::ConstantInt>(bound))
        length = std::min(length, constant->getZExtValue());
      return llvm::ConstantInt::get(module().i64, length);
    }
    if (bound == nullptr)
      bound = llvm::ConstantInt::get(module().i64,
                                     std::numeric_limits<std::uint64_t>::max());
    return module().insertString(builder, pointer, bound, record("string"),
                                 flags);
  }

  /// A term's value as an unsigned 64-bit integer, saturated; null when a
  /// leaf has no value in the IR (`fmtlen`, a macro). `bound` bounds the
  /// `strlen` leaves (inside `min`).
  llvm::Value *evaluate(const core::LibTerm &term, llvm::IRBuilder<> &builder,
                        llvm::Value *bound = nullptr) const {
    using Kind = core::LibTerm::Kind;
    llvm::Type *i64 = module().i64;
    llvm::Value *max =
        llvm::ConstantInt::get(i64, std::numeric_limits<std::uint64_t>::max());
    switch (term.kind) {
    case Kind::Constant:
      return llvm::ConstantInt::get(
          i64,
          static_cast<std::uint64_t>(std::max<std::int64_t>(term.value, 0)));
    case Kind::Argument: {
      llvm::Value *value = argument(term.arg);
      if (value == nullptr || !value->getType()->isIntegerTy())
        return nullptr;
      return builder.CreateSExtOrTrunc(value, i64);
    }
    case Kind::StringLength: {
      // A wide string's length is in wchar_t elements, which the byte scan
      // of a string guard does not give.
      if (match.entry->wide)
        return nullptr;
      llvm::Value *pointer = argument(term.arg);
      if (pointer == nullptr || !pointer->getType()->isPointerTy())
        return nullptr;
      return stringLength(builder, pointer, bound,
                          nullOk(term.arg) ? MarkerNullOk : 0);
    }
    case Kind::FormatLength:
    case Kind::Macro:
      return nullptr;
    case Kind::Product:
    case Kind::Sum: {
      llvm::Value *left = evaluate(term.operands[0], builder, bound);
      llvm::Value *right = evaluate(term.operands[1], builder, bound);
      if (left == nullptr || right == nullptr)
        return nullptr;
      llvm::Value *result = builder.CreateBinaryIntrinsic(
          term.kind == Kind::Product ? llvm::Intrinsic::umul_with_overflow
                                     : llvm::Intrinsic::uadd_with_overflow,
          left, right);
      return builder.CreateSelect(builder.CreateExtractValue(result, 1), max,
                                  builder.CreateExtractValue(result, 0));
    }
    case Kind::Difference: {
      llvm::Value *left = evaluate(term.operands[0], builder, bound);
      if (left == nullptr)
        return nullptr;
      llvm::Value *subtrahend = llvm::ConstantInt::get(
          i64,
          static_cast<std::uint64_t>(std::max<std::int64_t>(term.value, 0)));
      return builder.CreateSelect(builder.CreateICmpULT(left, subtrahend),
                                  llvm::ConstantInt::get(i64, 0),
                                  builder.CreateSub(left, subtrahend));
    }
    case Kind::Min: {
      // `strlen` inside `min(t, …)` scans at most t elements.
      llvm::Value *left = evaluate(term.operands[0], builder, bound);
      if (left == nullptr)
        return nullptr;
      llvm::Value *right = evaluate(term.operands[1], builder, left);
      if (right == nullptr)
        return nullptr;
      return builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, left, right);
    }
    case Kind::Quotient: {
      llvm::Value *left = evaluate(term.operands[0], builder, bound);
      if (left == nullptr || term.value <= 0)
        return nullptr;
      return builder.CreateUDiv(
          left,
          llvm::ConstantInt::get(i64, static_cast<std::uint64_t>(term.value)));
    }
    }
    return nullptr;
  }

  /// Guards `bytes` (an i64) at `pointer` before the call.
  void guardBytes(llvm::Value *pointer, llvm::Value *bytes, bool write) const {
    const unsigned flags = write ? MarkerWrite : 0;
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(bytes)) {
      const std::uint64_t width = constant->getZExtValue();
      if (width == 0)
        return;
      module().insertGuard(&call, pointer, width, llvm::Align(1), flags,
                           nullptr, record("range", width));
      return;
    }
    module().insertRange(&call, pointer, bytes, flags, record("range"));
  }

  /// The arguments of a constant `printf` or `scanf` format (section 2.5).
  void guardFormat(const core::LibFormat &format) const;

  /// Guards each pointer argument the row reads or writes.
  void guardArguments() const;

  /// Guards the first byte of each string argument that must not be null
  /// and is not a literal.
  void guardFirstBytes() const {
    const core::LibraryEntry &entry = *match.entry;
    for (unsigned index = 0; index < entry.params.size(); ++index) {
      const core::LibraryParam &param = entry.params[index];
      llvm::Value *pointer = argument(index);
      llvm::StringRef literal;
      if (!param.string || entry.wide ||
          param.null != core::LibraryParam::Null::Forbidden ||
          pointer == nullptr || !pointer->getType()->isPointerTy() ||
          llvm::getConstantStringInfo(pointer, literal))
        continue;
      guardBytes(pointer, llvm::ConstantInt::get(module().i64, 1), false);
    }
  }
};

} // namespace

void LibraryCall::guardArguments() const {
  const core::LibraryEntry &entry = *match.entry;
  for (unsigned index = 0; index < entry.params.size(); ++index) {
    const core::LibraryParam &param = entry.params[index];
    if (param.type != core::LibraryParam::Type::Pointer ||
        param.access == core::LibraryParam::Access::None)
      continue;
    // What `free` and `realloc` release the allocator validates; a block
    // they take may have no bytes (`malloc(0)`).
    if (param.effect == core::LibraryParam::Effect::Release ||
        param.effect == core::LibraryParam::Effect::Realloc)
      continue;
    llvm::Value *pointer = argument(index);
    if (pointer == nullptr || !pointer->getType()->isPointerTy() ||
        pointer->getType()->getPointerAddressSpace() != 0)
      continue;
    const bool write = param.access == core::LibraryParam::Access::Write ||
                       param.access == core::LibraryParam::Access::ReadWrite;
    llvm::IRBuilder<> builder(&call);
    if (param.string && !entry.wide)
      (void)stringLength(builder, pointer, nullptr,
                         nullOk(index) ? MarkerNullOk : 0);
    const std::optional<core::LibTerm> &term =
        param.bytes ? param.bytes : param.count;
    // `min(t, strlen(a<self>) + 1)`: the bytes a bounded scan of the string
    // reads, which its string guard checks.
    if (term && !entry.wide && term->kind == core::LibTerm::Kind::Min &&
        term->operands.size() == 2 &&
        term->operands[1] ==
            core::LibTerm::sum(core::LibTerm::stringLength(index),
                               core::LibTerm::constant(1))) {
      if (llvm::Value *bound = evaluate(term->operands[0], builder))
        (void)stringLength(builder, pointer, bound,
                           nullOk(index) ? MarkerNullOk : 0);
      continue;
    }
    llvm::Value *bytes = nullptr;
    // A wide row counts wchar_t elements.
    if (term && !param.bytes && entry.wide)
      bytes = evaluate(core::LibTerm::product(
                           *term, core::LibTerm::constant(
                                      static_cast<std::int64_t>(wcharSize()))),
                       builder);
    else if (term)
      bytes = evaluate(*term, builder);
    else if (!param.string)
      bytes = llvm::ConstantInt::get(module().i64, 1);
    if (bytes == nullptr)
      continue;
    // A null the call accepts needs no bytes.
    if (param.null == core::LibraryParam::Null::Allowed)
      bytes =
          builder.CreateSelect(builder.CreateIsNull(pointer),
                               llvm::ConstantInt::get(module().i64, 0), bytes);
    guardBytes(pointer, bytes, write);
  }
  // The operands a copy must not overlap.
  for (const core::LibDisjoint &disjoint : entry.disjoint) {
    llvm::Value *first = argument(disjoint.first);
    llvm::Value *second = argument(disjoint.second);
    if (first == nullptr || second == nullptr ||
        !first->getType()->isPointerTy() || !second->getType()->isPointerTy())
      continue;
    llvm::IRBuilder<> builder(&call);
    if (llvm::Value *length = evaluate(disjoint.length, builder))
      module().insertDisjoint(&call, first, second, length);
  }
}

void LibraryCall::guardFormat(const core::LibFormat &format) const {
  const llvm::Value *formatArgument = argument(format.format);
  llvm::StringRef text;
  if (formatArgument == nullptr ||
      !llvm::getConstantStringInfo(formatArgument, text))
    return;
  const int first = match.callArgument(format.first);
  if (first < 0)
    return;
  const bool scanf = format.kind == core::LibFormat::Kind::Scanf;
  auto next = static_cast<unsigned>(first);
  auto take = [&]() -> llvm::Value * {
    return next < call.arg_size() ? call.getArgOperand(next++) : nullptr;
  };
  llvm::IRBuilder<> builder(&call);
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (text[at] != '%')
      continue;
    ++at;
    if (at < text.size() && text[at] == '%')
      continue;
    bool suppressed = false;
    if (scanf && at < text.size() && text[at] == '*') {
      suppressed = true;
      ++at;
    }
    while (at < text.size() && llvm::StringRef("-+ #0'").contains(text[at]))
      ++at;
    std::optional<std::uint64_t> width;
    if (at < text.size() && text[at] == '*') {
      (void)take();
      ++at;
    } else {
      std::uint64_t digits = 0;
      bool any = false;
      while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        digits = (digits * 10) + static_cast<std::uint64_t>(text[at] - '0');
        any = true;
        ++at;
      }
      if (at < text.size() && text[at] == '$')
        return;
      if (any)
        width = digits;
    }
    std::optional<std::uint64_t> precision;
    bool dynamicPrecision = false;
    if (!scanf && at < text.size() && text[at] == '.') {
      ++at;
      if (at < text.size() && text[at] == '*') {
        (void)take();
        dynamicPrecision = true;
        ++at;
      } else {
        std::uint64_t digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
          digits = (digits * 10) + static_cast<std::uint64_t>(text[at] - '0');
          ++at;
        }
        precision = digits;
      }
    }
    bool wide = false;
    while (at < text.size() && llvm::StringRef("hljztLqI").contains(text[at])) {
      wide = wide || text[at] == 'l';
      ++at;
    }
    if (at >= text.size())
      return;
    const char conversion = text[at];
    if (scanf && conversion == '[') {
      while (at + 1 < text.size() && text[at + 1] != ']')
        ++at;
      ++at;
    }
    if (suppressed)
      continue;
    llvm::Value *value = take();
    if (value == nullptr || !value->getType()->isPointerTy())
      continue;
    if (conversion == 'n') {
      guardBytes(value, llvm::ConstantInt::get(module().i64, 1), true);
      continue;
    }
    if (wide)
      continue;
    if (!scanf && conversion == 's' && !dynamicPrecision) {
      (void)stringLength(builder, value,
                         precision
                             ? llvm::ConstantInt::get(module().i64, *precision)
                             : nullptr,
                         // `printf("%s", NULL)` prints "(null)" in the
                         // C libraries a drop-in build meets.
                         MarkerNullOk);
      continue;
    }
    if (scanf && (conversion == 's' || conversion == '[') && width) {
      guardBytes(value, llvm::ConstantInt::get(module().i64, *width + 1), true);
      continue;
    }
    if (scanf && conversion == 'c')
      guardBytes(value, llvm::ConstantInt::get(module().i64, width.value_or(1)),
                 true);
  }
}

/// Whether the call only reads, and reads exactly a constant of at most 113
/// bytes (an inline check) through every pointer that is not a literal:
/// then its guards are cheaper than the runtime's version.
static bool cheapToGuard(const LibraryCall &call) {
  const core::LibraryEntry &entry = *call.match.entry;
  auto small = [&](const core::LibTerm &term) {
    using Kind = core::LibTerm::Kind;
    if (term.kind == Kind::Constant)
      return term.value >= 0 && term.value <= 113;
    if (term.kind != Kind::Argument)
      return false;
    const auto *value =
        llvm::dyn_cast_or_null<llvm::ConstantInt>(call.argument(term.arg));
    return value != nullptr && value->getValue().ule(113);
  };
  bool checked = false;
  for (unsigned index = 0; index < entry.params.size(); ++index) {
    const core::LibraryParam &param = entry.params[index];
    if (param.type != core::LibraryParam::Type::Pointer ||
        param.access == core::LibraryParam::Access::None)
      continue;
    // A write's term may only bound what the call writes (snprintf's
    // size): its wrapper computes the bytes.
    if (param.access != core::LibraryParam::Access::Read)
      return false;
    checked = true;
    llvm::StringRef literal;
    if (const llvm::Value *pointer = call.argument(index);
        pointer != nullptr && llvm::getConstantStringInfo(pointer, literal))
      continue;
    const std::optional<core::LibTerm> &term =
        param.bytes ? param.bytes : param.count;
    if (!term || entry.wide)
      return false;
    // Only a constant: `min(t, strlen(..) + 1)` bounds a scan the call may
    // stop earlier (strncmp at a difference, memchr at its byte), so a guard
    // of t bytes could fail where the call reads nothing wrong.
    if (!small(*term))
      return false;
  }
  return checked;
}

/// Replaces the call with the runtime's checked version its row names
/// (section 2.5): the call's arguments and the site, first when the call is
/// variadic. False when the row names none or the call does not fit it.
static bool redirect(LibraryCall &call, bool late) {
  const core::LibraryEntry &entry = *call.match.entry;
  if (!entry.wrapper)
    return false;
  ModuleContext &module = call.module();
  llvm::SmallVector<llvm::Value *, 8> arguments;
  llvm::SmallVector<llvm::Type *, 8> types;
  const auto named = static_cast<unsigned>(entry.params.size());
  for (unsigned index = 0; index < named; ++index) {
    llvm::Value *value = call.argument(index);
    if (value == nullptr)
      return false;
    arguments.push_back(value);
    types.push_back(value->getType());
  }
  // A read's site: the wrapper marks what it writes.
  llvm::Constant *site = call.site(false);
  if (entry.variadic) {
    arguments.insert(arguments.begin(), site);
    types.insert(types.begin(), module.ptr);
    // The variadic arguments follow the row's last named one.
    const int last = named == 0 ? -1 : call.match.callArgument(named - 1);
    if (named != 0 && last < 0)
      return false;
    for (auto at = static_cast<unsigned>(last + 1); at < call.call.arg_size();
         ++at)
      arguments.push_back(call.call.getArgOperand(at));
  } else {
    arguments.push_back(site);
    types.push_back(module.ptr);
  }
  llvm::FunctionCallee callee = module.runtime(
      entry.wrapper->name,
      llvm::FunctionType::get(call.call.getType(), types, entry.variadic));
  llvm::IRBuilder<> builder(&call.call);
  llvm::CallInst *replacement = builder.CreateCall(callee, arguments);
  replacement->setDebugLoc(call.call.getDebugLoc());
  call.call.replaceAllUsesWith(replacement);
  const unsigned row = call.record("checked-call");
  // A row recorded after the optimiser has its one copy already.
  if (late)
    module.copy(row, true, "checked-call");
  call.call.eraseFromParent();
  return true;
}

/// Whether the row only computes its `value` from what it reads: pointer
/// arguments read or untouched and kept by nothing, no clause and no
/// format. Such a call (`strlen`) is replaced by what its value term
/// evaluates to, the string guards included.
static bool computesItsValue(const core::LibraryEntry &entry) {
  if (!entry.result.value || entry.wide || entry.variadic || entry.format ||
      entry.wrapper || !entry.disjoint.empty() || !entry.copies.empty() ||
      !entry.fills.empty() || !entry.writesString.empty() ||
      !entry.invalidates.empty() || !entry.reads.empty() || entry.noreturn)
    return false;
  return llvm::all_of(entry.params, [](const core::LibraryParam &param) {
    return param.type != core::LibraryParam::Type::Pointer ||
           ((param.access == core::LibraryParam::Access::Read ||
             param.access == core::LibraryParam::Access::None) &&
            param.effect == core::LibraryParam::Effect::Borrow && !param.out);
  });
}

/// Whether the row's call writes through no pointer and has no format: a
/// search, a comparison or a mapping, whose redirection waits until the
/// optimiser has folded what it can (`strcmp` of two constants).
static bool onlyReads(const core::LibraryEntry &entry) {
  return !entry.format &&
         llvm::all_of(entry.params, [](const core::LibraryParam &param) {
           return param.type != core::LibraryParam::Type::Pointer ||
                  param.access == core::LibraryParam::Access::Read ||
                  param.access == core::LibraryParam::Access::None;
         });
}

void guardLibraryCalls(ModuleContext &module, llvm::Function &function,
                       bool late) {
  const core::LibrarySpec &spec = core::LibrarySpec::shipped();
  std::vector<llvm::CallInst *> calls;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (const llvm::Function *callee = call->getCalledFunction();
            callee && (callee->isDeclaration() && !callee->isIntrinsic() &&
                       !ModuleContext::isRuntimeName(callee->getName())))
          calls.push_back(call);
  for (llvm::CallInst *call : calls) {
    const llvm::StringRef name = cName(call->getCalledFunction()->getName());
    if (module.isUnsafe(call->getDebugLoc()))
      continue;
    const std::optional<core::LibraryMatch> match = spec.lookup(name);
    if (!match || match->entry == nullptr)
      continue;
    LibraryCall library{.context = module,
                        .function = function,
                        .call = *call,
                        .match = *match,
                        .name = match->entry->name};
    // A read-only call with a wrapper is the late pass's (GuardExpand, after
    // the optimiser), which redirects it or guards it inline; the late pass
    // touches nothing else.
    const bool deferred = match->entry->wrapper && onlyReads(*match->entry);
    if (deferred && !late) {
      // What the optimiser may make of the call before then (`strcmp(s,
      // "")` becomes a load of `s[0]`): a string has at least its
      // terminator, so its first byte is guarded now.
      library.guardFirstBytes();
      continue;
    }
    if (deferred != late)
      continue;
    if (late) {
      if (cheapToGuard(library))
        library.guardArguments();
      else
        (void)redirect(library, /*late=*/true);
      continue;
    }
    // The arguments a constant format reads (a redirected call's wrapper
    // checks them itself).
    if (match->entry->format && !match->entry->format->vaList &&
        !match->entry->wrapper)
      library.guardFormat(*match->entry->format);
    if (!cheapToGuard(library) && redirect(library, /*late=*/false))
      continue;
    // A call that computes its value from strings (`strlen`): the string
    // guards compute it, and the string is scanned once.
    if (match->alias == nullptr && computesItsValue(*match->entry) &&
        call->getType()->isIntegerTy(64)) {
      llvm::IRBuilder<> builder(call);
      if (llvm::Value *value =
              library.evaluate(*match->entry->result.value, builder);
          value != nullptr && value->getType() == call->getType()) {
        call->replaceAllUsesWith(value);
        call->eraseFromParent();
        continue;
      }
    }
    library.guardArguments();
  }
}

void dropUnsafeBoundsChecks(const ModuleContext &module,
                            llvm::Function &function) {
  if (!module.options.unsafe || module.options.unsafe->empty())
    return;
  std::vector<llvm::BasicBlock *> handlers;
  for (llvm::BasicBlock &block : function)
    for (llvm::Instruction &instruction : block)
      if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (const llvm::Function *callee = call->getCalledFunction();
            callee &&
            callee->getName().starts_with("__ubsan_handle_out_of_bounds") &&
            module.isUnsafe(call->getDebugLoc())) {
          handlers.push_back(&block);
          break;
        }
  for (llvm::BasicBlock *handler : handlers) {
    for (llvm::BasicBlock *predecessor :
         llvm::to_vector(llvm::predecessors(handler))) {
      auto *branch =
          llvm::dyn_cast<llvm::CondBrInst>(predecessor->getTerminator());
      if (branch == nullptr)
        continue;
      llvm::BasicBlock *other = branch->getSuccessor(0) == handler
                                    ? branch->getSuccessor(1)
                                    : branch->getSuccessor(0);
      if (other == handler)
        continue;
      handler->removePredecessor(predecessor);
      // The block owns the branch it is inserted in.
      // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
      llvm::UncondBrInst::Create(other, branch->getIterator());
      branch->eraseFromParent();
      // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    }
    if (llvm::pred_empty(handler))
      llvm::DeleteDeadBlock(handler);
  }
}

} // namespace weavec::frontend::guard
