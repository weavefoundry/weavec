//===- ObjectEngine.h - The RFC 0031 engine behind the seam -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031: `SafetyEngine` implemented by the object engine, whose facts live
// on abstract objects and symbolic values (docs/rfcs/0031-object-engine.md).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_OBJECTENGINE_H
#define WEAVEC_ANALYSIS_OBJECTENGINE_H

#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Analysis/SafetyEngine.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

#include "llvm/Support/raw_ostream.h"

#include <memory>

namespace weavec::analysis {

namespace engine {
class UnitRun;
} // namespace engine

class ObjectEngine final : public SafetyEngine {
public:
  ObjectEngine();
  ~ObjectEngine() override;
  ObjectEngine(const ObjectEngine &) = delete;
  ObjectEngine &operator=(const ObjectEngine &) = delete;
  ObjectEngine(ObjectEngine &&) = delete;
  ObjectEngine &operator=(ObjectEngine &&) = delete;

  void analyzeUnit(const EngineInput &input, LedgerAdapter &out) override;
  [[nodiscard]] UnitExports exports() override;
  void dump(const clang::FunctionDecl &function,
            llvm::raw_ostream &os) override;
  /// RFC 0005 discovery: the unit's definitions, imports and indirect types
  /// with empty summaries, without analysing anything.
  [[nodiscard]] static UnitExports discover(const EngineInput &input);
  /// After `analyzeUnit`: the summary of a definition of the unit, or null.
  [[nodiscard]] const core::FunctionEffects *
  summaryOf(const clang::FunctionDecl &function) const;

private:
  std::unique_ptr<EngineInput> input;
  std::unique_ptr<engine::UnitRun> unit;
  UnitExports exported;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_OBJECTENGINE_H
