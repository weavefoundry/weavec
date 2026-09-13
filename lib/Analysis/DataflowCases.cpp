//===- DataflowCases.cpp - Input proof cases (RFC 0025) -------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::discoverCheckedCases() {
  // Discovery names potential entry selectors, not initialized storage.
  // Normal CFG accesses must still discharge every property of a selector.
  std::vector<const Stmt *> worklist{function.getBody()};
  for (std::size_t i = 0; i < worklist.size() && worklist.size() <= 65536;
       ++i) {
    const auto *stmt = worklist[i];
    if (!stmt)
      continue;
    if (const auto *expr = dyn_cast<Expr>(stmt);
        expr &&
        (expr->getType()->isIntegerType() || expr->getType()->isPointerType()))
      if (const auto ref = builder.resolve(*expr))
        if (const auto path = builder.summaryPathOf(ref->place))
          inferred.checked.noteCaseInput(*path);
    if (const auto *trait = dyn_cast<UnaryExprOrTypeTraitExpr>(stmt);
        trait &&
        (trait->isArgumentType() ||
         !trait->getArgumentExpr()->getType()->isVariablyModifiedType()))
      continue;
    for (const auto *child : stmt->children())
      worklist.push_back(child);
  }
}

void FunctionDataflow::forwardCheckedCaseInputs(
    const CallExpr &call, const core::CheckedContract &contract) {
  if (!recording())
    return;
  for (const auto &input : contract.caseInputs)
    if (const auto ref = builder.resolveSummaryPath(input, call))
      if (const auto path = stableSummaryPathOf(ref->place))
        inferred.checked.noteCaseInput(*path);
}

} // namespace weavec::analysis
