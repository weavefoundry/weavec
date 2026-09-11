//===- DataflowObjectTypes.cpp - Checked opaque pointers (RFC 0022) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/ObjectType.h"

#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <utility>

using namespace clang;

namespace weavec::analysis {

static std::optional<core::ObjectType> objectTypeOf(QualType type,
                                                    const ASTContext &context) {
  if (type.isNull() || type->isIncompleteType() || type->isFunctionType() ||
      type->isVoidType() || type->isVariablyModifiedType())
    return std::nullopt;
  while (const auto *array = context.getAsArrayType(type))
    type = array->getElementType();
  // Typedef attributes can strengthen alignment without changing canonical
  // type identity. Read the target layout before discarding that sugar.
  const auto bytes = context.getTypeSizeInChars(type).getQuantity();
  const auto alignment = context.getTypeAlignInChars(type).getQuantity();
  type = type.getCanonicalType().getUnqualifiedType();
  core::ObjectType result{.bytes = static_cast<std::uint64_t>(bytes),
                          .alignment = static_cast<std::uint64_t>(alignment),
                          .identity = type->isRecordType()
                                          ? recordLayoutKey(type, context)
                                          : type.getAsString()};
  if (const auto *record = type->getAsRecordDecl();
      record && (!record->getDeclContext()->isTranslationUnit() ||
                 record->getName().empty())) {
    const auto &sm = context.getSourceManager();
    const auto location =
        sm.getExpansionLoc(record->getCanonicalDecl()->getLocation());
    if (location.isInvalid())
      return std::nullopt;
    result.identity += ":scope:" + llvm::toHex(sm.getFilename(location), true) +
                       ":" + std::to_string(sm.getFileOffset(location));
  }
  return result.valid() ? std::optional(result) : std::nullopt;
}

void FunctionDataflow::checkedObjectRequirement(const CheckedMemory &memory,
                                                std::string_view descriptor,
                                                const Stmt &at,
                                                core::AnalysisState &state) {
  const auto wanted = core::ObjectType::parse(descriptor);
  const auto offset = foldAffine(memory.begin, state);
  bool proved = false;
  bool incompatible = false;
  if (wanted && offset.isConstant()) {
    if (const auto *decl =
            dyn_cast_or_null<ValueDecl>(builder.declFor(memory.storage))) {
      if (const auto actual = objectTypeOf(decl->getType(), context)) {
        proved =
            actual->accepts(*wanted, offset.constant) &&
            std::cmp_greater_equal(context.getDeclAlign(decl).getQuantity(),
                                   wanted->alignment);
        incompatible = !proved;
      }
    } else if (const auto view = state.safety->objectTypes.find(memory.storage);
               view != state.safety->objectTypes.end()) {
      if (const auto actual = core::ObjectType::parse(view->second)) {
        proved = actual->accepts(*wanted, offset.constant);
        incompatible = !proved;
      } else if (view->second.empty()) {
        const auto resource =
            state.resources.recordOf(memory.holder.value_or(memory.storage));
        proved =
            resource && resource->origin == core::ResourceOrigin::Allocated &&
            resource->family == "free" && offset.constant >= 0 &&
            static_cast<std::uint64_t>(offset.constant) % wanted->alignment ==
                0 &&
            wanted->alignment <= context.getTargetInfo().getSuitableAlign() /
                                     context.getCharWidth();
        if (proved)
          view->second = wanted->toString();
      }
    }
  }
  const bool required =
      wanted && !proved && !incompatible && memory.input &&
      checkedRequire(core::CheckedRequirementKind::ObjectType, memory, at,
                     state, std::string(descriptor));
  if (required) {
    if (state.safety->objectTypes.size() < core::MaxSafetyRequirements ||
        state.safety->objectTypes.contains(memory.storage))
      state.safety->objectTypes[memory.storage] = std::string(descriptor);
    else if (recording())
      inferred.checked.limited = true;
  }
  safetyObligation(
      core::SafetyProperty::Semantics,
      incompatible ? core::SafetyOutcome::Violation
                   : core::safetyOutcome(proved, required),
      at, "object view",
      incompatible
          ? "pointer recovery has an incompatible object type or alignment"
          : "pointer recovery requires a compatible object type and alignment");
}

std::string FunctionDataflow::checkedObjectType(QualType type) {
  const auto result = objectTypeOf(type, context);
  return result ? result->toString() : std::string{};
}

void FunctionDataflow::checkedObjectCast(const CastExpr &cast,
                                         core::AnalysisState &state) {
  if (cast.getCastKind() != CK_BitCast || !cast.getType()->isPointerType() ||
      !cast.getSubExpr()->getType()->isPointerType())
    return;
  const auto from = cast.getSubExpr()->getType()->getPointeeType();
  const auto to = cast.getType()->getPointeeType();
  if ((!from->isVoidType() && !from->isCharType()) || to->isVoidType() ||
      to->isCharType() || ASTContext::hasSameUnqualifiedType(from, to))
    return;
  const auto origin = builder.classifyValue(*cast.getSubExpr());
  if (origin.kind == ValueOrigin::Kind::Null)
    return;
  const auto type = objectTypeOf(to, context);
  // A wrapper can return a fresh allocation whose view was established in
  // its body. Fresh ownership does not erase that object's type evidence.
  if (type)
    if (const auto *call =
            dyn_cast<CallExpr>(cast.getSubExpr()->IgnoreParenImpCasts()))
      if (const auto summary = resolveCall(*call))
        for (const auto &post : summary->summary->checked.establishes)
          if (post.kind == core::CheckedRequirementKind::ObjectType &&
              post.path.isResult() && post.path.isRoot()) {
            const auto actual = core::ObjectType::parse(post.family);
            if (actual && !actual->accepts(*type, 0)) {
              safetyObligation(core::SafetyProperty::Semantics,
                               core::SafetyOutcome::Violation, cast,
                               "object view",
                               "pointer recovery has an incompatible object "
                               "type or alignment");
              return;
            }
          }
  // Allocation expression results have no holder until assignment. Their
  // declared library alignment still bounds which view can be recovered.
  const auto fresh = [](const ValueOrigin &value) {
    return value.kind == ValueOrigin::Kind::Null ||
           (value.kind == ValueOrigin::Kind::Alloc && value.family == "free" &&
            value.offset.isZero());
  };
  if (type &&
      type->alignment <=
          context.getTargetInfo().getSuitableAlign() / context.getCharWidth() &&
      (fresh(origin) || (origin.kind == ValueOrigin::Kind::Conditional &&
                         !origin.alternatives.empty() &&
                         std::ranges::all_of(origin.alternatives, fresh)))) {
    safetyObligation(core::SafetyProperty::Semantics,
                     core::SafetyOutcome::Proven, cast, "object view",
                     "allocation supports target object alignment");
    return;
  }
  const auto memory = checkedMemory(*cast.getSubExpr(), {}, {}, state);
  if (type && memory) {
    checkedObjectRequirement(*memory, type->toString(), cast, state);
    return;
  }
  safetyObligation(
      core::SafetyProperty::Semantics, core::SafetyOutcome::Unresolved, cast,
      "object view",
      "pointer recovery requires a represented object type and alignment");
}

} // namespace weavec::analysis
