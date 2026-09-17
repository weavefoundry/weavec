//===- DataflowRecursiveExtension.cpp - Owned head construction ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include <algorithm>
#include <utility>

using namespace clang;

namespace weavec::analysis {

std::shared_ptr<const core::FunctionSummary>
FunctionDataflow::recursiveExtensionCandidate(const FunctionDecl &callee) {
  const auto type = callee.getParamDecl(0)->getType();
  const auto *discovered = containerShape(type);
  const auto *record = type->getPointeeType()->getAsRecordDecl();
  if (!discovered || !record)
    return {};
  auto input = *discovered;
  input.access = core::ContainerAccess::Release;
  input.family = "free";
  input.terminal = true;
  input.emptyLinks.clear();
  input.headValues.clear();
  for (const auto &payload : input.payloads)
    input.emptyPayloads.insert(payload.field.name);
  if (!input.singletonHead())
    return {};
  auto output = input;
  output.terminal = false;
  output.emptyPayloads.clear();
  const auto root = core::SummaryPath::param(0);
  auto result = std::make_shared<core::FunctionSummary>();
  result->checked.computed = true;
  result->checked.signature = functionTypeKey(callee.getType(), context);
  result->checked.require({.kind = core::CheckedRequirementKind::Valid,
                           .path = root,
                           .other = {},
                           .family = {}});
  result->checked.require({.kind = core::CheckedRequirementKind::Container,
                           .path = root,
                           .other = {},
                           .family = input.encode()});
  result->addEffect(root.deref(), {.read = true, .written = true});
  for (const auto *field : record->fields()) {
    const auto path = root.deref().field(field->getNameAsString());
    result->addEffect(path, {.written = true});
    if (field->getType()->isPointerType())
      result->addStore({.dest = path, .value = core::ValueSource::unknown()});
  }
  result->checked.establish(
      {.kind = core::CheckedRequirementKind::ContainerExtended,
       .path = root,
       .other = root,
       .family = output.encode()});
  return result;
}

void FunctionDataflow::initializeRecursiveExtension(
    core::AnalysisState &state) {
  const auto candidate = recursiveExtensionCandidate(function);
  if (!candidate)
    return;
  const auto root = core::SummaryPath::param(0);
  const auto holder = builder.placeForVar(*function.getParamDecl(0));
  for (const auto &requirement : candidate->checked.requirements) {
    inferred.checked.require(requirement);
    if (requirement.kind != core::CheckedRequirementKind::Container)
      continue;
    const auto shape = core::ContainerShape::decode(requirement.family);
    if (!shape)
      continue;
    initializeFootprint(holder, root, *shape, state);
    footprintEntries.at(root).shape = *shape;
    state.safety->containers.set(holder, containerInput(holder, root, *shape));
    state.nulls.set(holder, {.state = core::Nullness::NonNull,
                             .location = {},
                             .reason = core::NullReason::Declared});
    state.safety->pointers.insert(holder);
    const auto object = places.deref(holder);
    state.safety->accessible[object] = core::Affine::ofConstant(
        static_cast<std::int64_t>(shape->object.bytes));
    for (const auto &field : shape->initialized)
      state.safety->initialize(
          object, {.begin = core::Affine::ofConstant(
                       static_cast<std::int64_t>(field.offset)),
                   .end = core::Affine::ofConstant(
                       static_cast<std::int64_t>(field.offset + field.bytes))});
    const auto nullField = [&](const core::ContainerField &field) {
      const auto cell = places.field(object, field.name);
      state.nulls.set(cell, {.state = core::Nullness::Null,
                             .location = {},
                             .reason = core::NullReason::Declared});
      state.resources.markNull(cell);
      state.safety->footprints.constrain({{cell, 1}});
    };
    nullField(shape->link);
    for (const auto &child : shape->children)
      nullField(child);
    for (const auto &payload : shape->payloads)
      nullField(payload.field);
    state.safety->footprints.constrain(
        {{footprintHead(holder), 1}, {footprintEntries.at(root).identity, -1}});
  }
}

std::shared_ptr<const core::FunctionSummary>
FunctionDataflow::recursiveExtensionCall(const CallExpr &call,
                                         core::AnalysisState &state) {
  const auto *callee = call.getDirectCallee();
  if (!callee || call.getNumArgs() != 2)
    return {};
  auto candidate = recursiveExtensionCandidate(*callee);
  auto count = integerAffineOf(*call.getArg(1), state);
  if (!candidate || !count)
    return {};
  count = foldAffine(*count, state);
  const auto parameter = builder.placeForVar(*function.getParamDecl(1));
  const auto saved = numericEntryValues.find(parameter);
  const auto entry = core::Affine::ofPlace(
      saved == numericEntryValues.end() ? parameter : saved->second);
  if (!checkedAtMost({}, *count, state) || !checkedAtMost(*count, entry, state))
    return {};
  const auto next = count->shifted(1);
  const bool strict = next && checkedAtMost(*next, entry, state);
  recursiveInputUsed = true;
  recursiveContractCalls.emplace(callee->getCanonicalDecl(), strict);
  safetyObligation(
      core::SafetyProperty::Call, core::SafetyOutcome::Required, call,
      "recursive construction count",
      strict ? "recursive construction decreases its immutable entry count"
             : "recursive forwarding requires progress on every group cycle");
  return candidate;
}

void FunctionDataflow::verifyRecursiveExtension() {
  if (!recursiveInputUsed)
    return;
  const auto candidate = recursiveExtensionCandidate(function);
  bool verified = candidate != nullptr;
  if (candidate) {
    const auto root = core::SummaryPath::param(0);
    const auto input = std::ranges::find_if(
        candidate->checked.requirements, [](const auto &pre) {
          return pre.kind == core::CheckedRequirementKind::Container;
        });
    const auto shape = core::ContainerShape::decode(input->family).value();
    for (auto pre : inferred.checked.requirements) {
      pre.when.clear();
      if (candidate->checked.requirements.contains(pre))
        continue;
      bool covered = false;
      if (pre.path == root &&
          pre.kind == core::CheckedRequirementKind::Container)
        if (const auto needed = core::ContainerShape::decode(pre.family))
          covered = shape.entails(*needed);
      if (pre.path == root && pre.begin.isConstant() && pre.end.isConstant() &&
          pre.begin.constant >= 0 && pre.end.constant >= pre.begin.constant &&
          std::cmp_less_equal(pre.end.constant, shape.object.bytes)) {
        covered |= pre.kind == core::CheckedRequirementKind::Extent ||
                   pre.kind == core::CheckedRequirementKind::Writable;
        if (pre.kind == core::CheckedRequirementKind::Initialized)
          covered |=
              std::ranges::any_of(shape.initialized, [&](const auto &field) {
                return std::cmp_less_equal(field.offset, pre.begin.constant) &&
                       std::cmp_less_equal(pre.end.constant,
                                           field.offset + field.bytes);
              });
      }
      verified &= covered;
    }
    for (const auto &[path, effect] : inferred.effects)
      verified &= !path.isGlobal() && !effect.consumed() && !effect.escaped &&
                  !effect.replaced &&
                  (!effect.written ||
                   (path.isParam() && path.index == 0 && path.hasDeref()));
    for (const auto &expected : candidate->checked.establishes)
      verified &=
          std::ranges::any_of(inferred.checked.establishes, [&](auto actual) {
            const auto established =
                core::ContainerShape::decode(actual.family);
            const auto required = core::ContainerShape::decode(expected.family);
            actual.family = expected.family;
            return actual == expected && established && required &&
                   established->entails(*required);
          });
  }
  safetyObligation(
      core::SafetyProperty::Semantics,
      verified ? core::SafetyOutcome::Proven : core::SafetyOutcome::Unresolved,
      *function.getBody(), "recursive construction output",
      verified ? "recursive construction preserves its head and accounts for "
                 "every fresh descendant"
               : "recursive construction does not establish its complete "
                 "output contract");
}

} // namespace weavec::analysis
