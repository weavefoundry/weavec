//===- DataflowRecursiveWriting.cpp - Recursive buffer outputs ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include <algorithm>

using namespace clang;
namespace weavec::analysis {

std::shared_ptr<const core::FunctionSummary>
FunctionDataflow::recursiveWriterCandidate(const FunctionDecl &callee) {
  const auto type = callee.getParamDecl(2)->getType()->getPointeeType();
  const auto *record = type->getAsRecordDecl();
  const auto shape = record ? discoverBufferShape(*record) : std::nullopt;
  if (!shape || shape->elementBytes != 1 || shape->pointerElements ||
      shape->reader || type.isConstQualified() || type.isVolatileQualified())
    return {};
  auto result = std::make_shared<core::FunctionSummary>();
  result->checked.computed = true;
  result->checked.signature = functionTypeKey(callee.getType(), context);
  const auto input = core::SummaryPath::param(0);
  const auto length = core::PathAffine::ofPath(core::SummaryPath::param(1));
  const auto header = core::SummaryPath::param(2);
  const auto object = header.deref();
  const auto data = object.field(shape->data.name);
  result->checked.require({.kind = core::CheckedRequirementKind::Valid,
                           .path = input,
                           .other = {},
                           .family = {}});
  for (const auto kind : {core::CheckedRequirementKind::Extent,
                          core::CheckedRequirementKind::Initialized})
    result->checked.require({.kind = kind,
                             .path = input,
                             .other = {},
                             .end = length,
                             .family = {}});
  result->checked.require({.kind = core::CheckedRequirementKind::Valid,
                           .path = header,
                           .other = {},
                           .family = {}});
  for (const auto kind : {core::CheckedRequirementKind::Extent,
                          core::CheckedRequirementKind::Initialized,
                          core::CheckedRequirementKind::Writable})
    result->checked.require(
        {.kind = kind,
         .path = header,
         .other = {},
         .end = core::PathAffine::ofConstant(
             context.getTypeSizeInChars(type).getQuantity()),
         .family = {}});
  for (const auto &other : {header, data})
    result->checked.require({.kind = core::CheckedRequirementKind::Separated,
                             .path = input,
                             .other = other,
                             .family = {}});
  result->checked.require({.kind = core::CheckedRequirementKind::Separated,
                           .path = header,
                           .other = data,
                           .family = {}});
  result->checked.require(
      {.kind = core::CheckedRequirementKind::Writable,
       .path = data,
       .other = {},
       .end = core::PathAffine::ofPath(object.field(shape->capacity.name)),
       .family = {}});
  result->checked.require({.kind = core::CheckedRequirementKind::Buffer,
                           .path = object,
                           .other = {},
                           .family = shape->encode()});
  result->checked.establish({.kind = core::CheckedRequirementKind::Buffer,
                             .path = object,
                             .other = {},
                             .family = shape->encode()});
  result->addEffect(input.deref(), {.read = true});
  result->addEffect(data.deref(), {.written = true});
  result->addEffect(object.field(shape->length.name), {.written = true});
  result->outcomes[core::Outcome::Zero] = {};
  result->outcomes[core::Outcome::Positive] = {};
  return result;
}

void FunctionDataflow::initializeRecursiveWriter(core::AnalysisState &state) {
  const auto candidate = recursiveWriterCandidate(function);
  if (!candidate)
    return;
  const auto header = builder.placeForVar(*function.getParamDecl(2));
  const auto object = places.deref(header);
  const auto type = function.getParamDecl(2)->getType()->getPointeeType();
  registerBuffer(object, *type->getAsRecordDecl());
  const auto found = bufferObjects.find(object);
  if (found == bufferObjects.end())
    return;
  const auto &shape = found->second;
  const auto data = places.field(object, shape.data.name);
  // Forwarding members may not themselves read a field. Their candidate still
  // has the same explicit buffer entry requirement as every other member.
  checkedInputObjects[data] = places.deref(data);
  bufferEntryBackings.insert(data);
  state.safety->buffers.set(
      data, {.shape = shape,
             .object = object,
             .length = places.field(object, shape.length.name),
             .capacity = places.field(object, shape.capacity.name),
             .initialized = true,
             .entryBacking = data});
  state.nulls.set(header, {.state = core::Nullness::NonNull,
                           .location = {},
                           .reason = core::NullReason::Declared});
  state.safety->pointers.insert(header);
  const auto bytes =
      core::Affine::ofConstant(context.getTypeSizeInChars(type).getQuantity());
  state.safety->accessible[object] = bytes;
  state.safety->initialize(object, {.begin = {}, .end = bytes});
  materializeBuffers(state);
}

void FunctionDataflow::verifyRecursiveWriter() {
  if (!recursiveInputUsed)
    return;
  const auto candidate = recursiveWriterCandidate(function);
  bool verified = candidate && recursiveInputRequirementsCovered(*candidate);
  if (candidate) {
    const auto &expected = *candidate->checked.establishes.begin();
    const auto shape = core::BufferShape::decode(expected.family);
    verified &= std::ranges::any_of(
        inferred.checked.establishes, [&](const auto &post) {
          const auto actual = core::BufferShape::decode(post.family);
          return post.kind == expected.kind && post.path == expected.path &&
                 post.when.trivial() && !post.on && !post.ifNonNull && actual &&
                 actual->entails(*shape);
        });
    for (const auto &[path, effect] : inferred.effects) {
      const auto allowed = candidate->effects.find(path);
      verified &= !path.isGlobal() && !effect.consumed() && !effect.escaped &&
                  !effect.replaced &&
                  (!effect.written || (allowed != candidate->effects.end() &&
                                       allowed->second.written));
    }
    for (const auto &[outcome, effects] : inferred.outcomes) {
      (void)effects;
      verified &=
          outcome == core::Outcome::Zero || outcome == core::Outcome::Positive;
    }
  }
  safetyObligation(
      core::SafetyProperty::Semantics,
      verified ? core::SafetyOutcome::Proven : core::SafetyOutcome::Unresolved,
      *function.getBody(), "recursive writer output",
      verified ? "recursive writer establishes its initialized buffer prefix"
               : "recursive writer does not establish its complete output "
                 "contract");
}

} // namespace weavec::analysis
