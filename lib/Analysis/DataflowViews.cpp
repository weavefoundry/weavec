//===- DataflowViews.cpp - Validate summary object views -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "weavec/Analysis/ProgramDatabase.h"

using namespace clang;

namespace weavec::analysis {

// RFC 0028: recover representation from the current value's positive facts.
// This does not establish memory validity or release permission.
std::string
FunctionDataflow::objectEvidenceView(core::PlaceId holder,
                                     const core::AnalysisState &state) {
  if (state.moves.recordOf(holder) || state.raw.isRaw(holder))
    return {};
  if (const auto view = state.objectViews.find(holder);
      view != state.objectViews.end())
    return view->second;
  return {};
}

bool FunctionDataflow::validateObjectPath(const core::SummaryPath &path,
                                          const CallExpr &call) {
  const auto cached = callSummaries.find(&call);
  if (!currentState || cached == callSummaries.end() || !cached->second ||
      path.steps.empty())
    return true;
  if (cached->second->objectViews.empty())
    return true;
  const auto summaryOwner = cached->second;
  if (const auto paths = validatedObjectPaths.find(&call);
      paths != validatedObjectPaths.end())
    if (const auto found = paths->second.find(path);
        found != paths->second.end() && found->second.lock() == summaryOwner)
      return true;
  const auto &views = summaryOwner->objectViews;
  bool typedPath = true;
  QualType type;
  const Expr *argument = nullptr;
  if (path.isParam() && path.index < call.getNumArgs()) {
    if (call.getArg(path.index)
            ->isNullPointerConstant(context, Expr::NPC_ValueDependentIsNotNull))
      return true;
    argument = call.getArg(path.index)->IgnoreParenCasts();
    type = argument->getType();
  } else if (path.isGlobal()) {
    if (const auto *global = summaries.globals().declFor(path.index))
      type = global->getType();
  }
  // RFC 0028: typed paths need layout checks only. Resolving and walking a
  // parallel place chain on every lookup is expensive in large call graphs.
  // Recover the identical holder lazily when actual erased evidence is needed.
  const auto evidenceHolder = [&](const core::SummaryPath &prefix) {
    std::optional<core::PlaceId> holder;
    if (const auto root = builder.resolveSummaryPath(path.rootPath(), call))
      holder = root->place;
    const auto addressed =
        argument ? builder.addressedPlace(*argument) : std::nullopt;
    std::optional<core::PlaceId> pointee;
    bool first = true;
    for (const auto &step : prefix.steps) {
      if (step.step == core::PathStep::Deref)
        pointee = holder;
      if (first && addressed && step.step == core::PathStep::Deref)
        holder = addressed->place;
      else if (holder)
        holder = places.child(*holder, step.step, step.field);
      first = false;
    }
    return pointee;
  };
  core::SummaryPath prefix = path.rootPath();
  for (const auto &step : path.steps) {
    if (const auto expected = views.find(prefix); expected != views.end()) {
      // RFC 0027: reuse a typed layout comparison, never the path walk.
      // Each later prefix still needs its own view, and erased recovery is
      // flow-dependent. An address is valid only with its live summary owner.
      const std::pair<const void *, const std::string *> key{
          type.getAsOpaquePtr(), &expected->second};
      const auto known = validatedObjectViews.find(key);
      const bool reused = known != validatedObjectViews.end() &&
                          known->second.lock() == summaryOwner;
      if (!reused) {
        std::string actual(summaries.objectView(type));
        const bool typed = !actual.empty();
        if (actual.empty()) {
          typedPath = false;
          if (const auto holder = evidenceHolder(prefix))
            actual = objectEvidenceView(*holder, *currentState);
          if (actual == expected->second) {
            const auto adapter = summaries.interfaceType(actual);
            if (adapter.isNull())
              actual.clear();
            else
              type = adapter;
          }
        }
        if (actual.empty() || actual != expected->second) {
          reportIncomplete("incompatible or unknown object view at call", call);
          return false;
        }
        if (typed) {
          if (validatedObjectViews.size() == 128)
            validatedObjectViews.clear();
          validatedObjectViews[key] = summaryOwner;
        }
      }
    }
    switch (step.step) {
    case core::PathStep::Deref:
      if (!type.isNull()) {
        // IgnoreParenCasts exposes an array before its argument decay.
        if (const auto *array = type->getAsArrayTypeUnsafe())
          type = array->getElementType();
        else
          type = type->isPointerType() ? type->getPointeeType() : QualType{};
      }
      break;
    case core::PathStep::Index:
      // A selected cell is below storage whose dereference/array-summary
      // step already selected its element type (RFC 0015).
      if (!step.field.empty())
        break;
      if (!type.isNull()) {
        if (const auto *array = type->getAsArrayTypeUnsafe())
          type = array->getElementType();
        else if (type->isPointerType())
          type = type->getPointeeType();
      }
      break;
    case core::PathStep::Field: {
      const RecordDecl *record =
          type.isNull() ? nullptr : type->getAsRecordDecl();
      type = QualType{};
      if (record) {
        for (const auto *field : record->fields())
          if (field->getName() == step.field) {
            type = field->getType();
            break;
          }
      }
      break;
    }
    }
    prefix.steps.pushBack(step);
  }
  // Every prefix passed using immutable C types. Only this exact call/path
  // and live summary can reuse the layout result. Opaque value evidence and
  // all flow-sensitive memory obligations remain outside this cache.
  if (typedPath) {
    if (validatedObjectPathCount == 1024) {
      validatedObjectPaths.clear();
      validatedObjectPathCount = 0;
    }
    auto &paths = validatedObjectPaths[&call];
    const auto [entry, inserted] = paths.try_emplace(path, summaryOwner);
    if (inserted)
      ++validatedObjectPathCount;
    else
      entry->second = summaryOwner;
  }
  return true;
}

} // namespace weavec::analysis
