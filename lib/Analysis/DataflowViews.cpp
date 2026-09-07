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

bool FunctionDataflow::validateObjectPath(const core::SummaryPath &path,
                                          const CallExpr &call) {
  const auto cached = callSummaries.find(&call);
  if (!currentState || cached == callSummaries.end() || !cached->second ||
      path.steps.empty())
    return true;
  const auto &views = cached->second->objectViews;
  if (views.empty())
    return true;
  QualType type;
  std::string recovered;
  if (path.isParam() && path.index < call.getNumArgs()) {
    if (call.getArg(path.index)
            ->isNullPointerConstant(context, Expr::NPC_ValueDependentIsNotNull))
      return true;
    const Expr *arg = call.getArg(path.index)->IgnoreParenCasts();
    type = arg->getType();
    if (const auto ref = builder.resolve(*arg)) {
      const auto found = currentState->objectViews.find(ref->place);
      if (found != currentState->objectViews.end())
        recovered = found->second;
    }
  } else if (path.isGlobal()) {
    if (const auto *global = summaries.globals().declFor(path.index))
      type = global->getType();
  }
  core::SummaryPath prefix = path.rootPath();
  for (const auto &step : path.steps) {
    if (const auto expected = views.find(prefix); expected != views.end()) {
      std::string actual(summaries.objectView(type));
      if (actual.empty() && !recovered.empty()) {
        actual = recovered;
        recovered.clear();
      }
      if (actual.empty() || actual != expected->second) {
        reportIncomplete("incompatible or unknown object view at call", call);
        return false;
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
    prefix.steps.push_back(step);
  }
  return true;
}

} // namespace weavec::analysis
