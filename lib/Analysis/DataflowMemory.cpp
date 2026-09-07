//===- DataflowMemory.cpp - Complete object-representation copies --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "weavec/Analysis/Allocators.h"

#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::reportIncomplete(const std::string &reason,
                                        const Stmt &at) {
  if (!recording())
    return;
  inferred.incomplete.insert(reason);
  if (!emitDiagnostics || inUnsafe ||
      !incompleteReports.emplace(&at, reason).second)
    return;
  report(core::Diagnostic{.severity = core::Severity::Warning,
                          .id = core::diag::AnalysisIncomplete,
                          .message = "analysis is incomplete: " + reason,
                          .location = locate(at),
                          .notes = {},
                          .fixits = {}});
}

static bool containsPointer(QualType type, unsigned depth = 0) {
  if (type.isNull() || depth > core::MaxHeapPathDepth)
    return false;
  if (type->isPointerType())
    return true;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return containsPointer(array->getElementType(), depth + 1);
  if (const RecordDecl *record = type->getAsRecordDecl()) {
    if (!record->isCompleteDefinition())
      return false;
    for (const FieldDecl *field : record->fields())
      if (containsPointer(field->getType(), depth + 1))
        return true;
  }
  return false;
}

bool FunctionDataflow::handleMemoryCopy(const CallExpr &call,
                                        const CallEffects &effects,
                                        core::AnalysisState &state) {
  const FunctionDecl *callee = call.getDirectCallee();
  if (!callee || effects.source != SummarySource::Builtin ||
      call.getNumArgs() < 3)
    return false;
  llvm::StringRef name = callee->getName();
  if (name.starts_with("__builtin___")) {
    name = name.drop_front(12);
    name.consume_back("_chk");
  } else {
    name.consume_front("__builtin_");
  }
  if (name != "memcpy" && name != "memmove")
    return false;

  const Expr &destExpr = *call.getArg(0)->IgnoreParenImpCasts();
  const Expr &sourceExpr = *call.getArg(1)->IgnoreParenImpCasts();
  const auto objectType = [](const Expr &expr) -> QualType {
    QualType type = expr.getType();
    if (const auto *array = type->getAsArrayTypeUnsafe())
      return array->getElementType();
    return type->isPointerType() ? type->getPointeeType() : QualType{};
  };
  const QualType destType = objectType(destExpr);
  const QualType sourceType = objectType(sourceExpr);
  if (!containsPointer(destType) && !containsPointer(sourceType))
    return false;
  const auto storage = [&](const Expr &expr) -> std::optional<PlaceRef> {
    if (auto addressed = builder.addressedPlace(expr))
      return addressed;
    if (auto pointer = builder.resolvePointerValue(expr)) {
      pointer->addDeref(pointer->place, &expr);
      pointer->place = places.deref(pointer->place);
      return pointer;
    }
    return std::nullopt;
  };
  const auto dest = storage(destExpr);
  const auto source = storage(sourceExpr);
  auto bytes = builder.affineOf(*call.getArg(2));
  if (bytes)
    bytes = foldAffine(*bytes, state);
  const auto size = byteSizeOf(sourceType, context);
  if (bytes && bytes->isConstant() && bytes->constant == 0)
    return false;
  const bool compatible =
      !destType.isNull() && !sourceType.isNull() &&
      ASTContext::hasSameUnqualifiedType(destType, sourceType);
  const bool complete =
      dest && source && dest->element.isWhole() && source->element.isWhole() &&
      size && bytes && bytes->isConstant() && bytes->constant == *size &&
      compatible && (sourceType->isPointerType() || sourceType->isRecordType());
  if (!complete) {
    if (dest) {
      for (const auto cell : storageOf(dest->place)) {
        escape(cell, state);
        state.forget(cell);
      }
      state.incompleteHeap.insert(dest->place);
      if (destType->isFunctionPointerType())
        state.callTargets[dest->place] = core::CallTargets::any();
    }
    reportIncomplete("unsupported memory copy of pointer-containing storage",
                     call);
    return false;
  }

  checkRequiredArguments(call, *effects.summary, state);
  checkRequiredExtents(call, *effects.summary, state);
  doMutationCheck(dest->place, call, state);
  checkAnnotationOnWrite(*dest, call, state);
  recordAccess(source->place, false, state);
  recordAccess(dest->place, true, state);
  if (source->place == dest->place)
    return true;

  auto it = memorySnapshots.find(&call);
  if (it == memorySnapshots.end())
    it = memorySnapshots.emplace(&call, places.create("copy-input")).first;
  const core::PlaceId snapshot = it->second;
  pointerSnapshots.insert(snapshot);
  if (sourceType->isPointerType()) {
    copyHeapValue(source->place, snapshot, state);
    ValueOrigin origin;
    origin.kind = ValueOrigin::Kind::Copy;
    origin.place = PlaceRef{.place = snapshot,
                            .derefs = {},
                            .derefExprs = {},
                            .derefElements = {},
                            .element = {}};
    noteRewritten(dest->place, state);
    noteOverwritten(dest->place, state);
    applyPointerAssign(dest->place, origin, call,
                       sourceType->getPointeeType().isConstQualified(), state);
    applyHeapValue(dest->place, origin, state);
  } else {
    copyRecordPlaces(snapshot, source->place, state);
    const auto storagePlaces = storageOf(dest->place);
    const std::set<core::PlaceId> going(storagePlaces.begin(),
                                        storagePlaces.end());
    checkLeaks(
        storagePlaces, [&going](core::PlaceId p) { return going.contains(p); },
        LeakForm::Overwritten, locate(call), state);
    noteRewritten(dest->place, state);
    noteOverwritten(dest->place, state);
    copyRecordPlaces(dest->place, snapshot, state);
    for (const auto field : storageOf(dest->place)) {
      if (field == dest->place)
        continue;
      if (const auto input = state.incoming.find(field);
          input != state.incoming.end())
        noteCalleeStore(field, call, state);
    }
  }
  // Output capture uses the normal heap machinery, with immutable source
  // identities retained while this copy's scratch holders are retired.
  for (const auto child : places.descendants(snapshot))
    state.forget(child);
  state.forget(snapshot);
  return true;
}

} // namespace weavec::analysis
