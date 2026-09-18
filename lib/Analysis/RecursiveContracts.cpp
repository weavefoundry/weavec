//===- RecursiveContracts.cpp - Atomic recursive proofs (RFC 0029) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Dataflow.h"
#include "weavec/Analysis/TranslationUnitAnalysis.h"
#include "weavec/Core/Induction.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace clang;
namespace weavec::analysis {

static std::pair<const FieldDecl *, const FieldDecl *>
copiedReaderFields(const RecordDecl &record,
                   const std::vector<const FunctionDecl *> &members) {
  std::set<const FieldDecl *> data;
  std::set<const FieldDecl *> counts;
  const auto memberField = [&](const Expr *expression) {
    const auto *member =
        expression ? dyn_cast<MemberExpr>(expression->IgnoreParenImpCasts())
                   : nullptr;
    const auto *field =
        member ? dyn_cast<FieldDecl>(member->getMemberDecl()) : nullptr;
    return field && field->getParent() == &record ? field : nullptr;
  };
  for (const auto *member : members) {
    std::vector<const Stmt *> work{member->getBody()};
    for (std::size_t i = 0; i < work.size(); ++i) {
      if (work.size() > 65536 || data.size() > 16 || counts.size() > 16)
        return {};
      const auto *statement = work[i];
      if (!statement)
        continue;
      const Expr *read = nullptr;
      const Expr *decrement = nullptr;
      if (const auto *index = dyn_cast<ArraySubscriptExpr>(statement))
        read = index->getBase();
      if (const auto *unary = dyn_cast<UnaryOperator>(statement)) {
        if (unary->getOpcode() == UO_Deref)
          read = unary->getSubExpr();
        if (unary->isDecrementOp())
          decrement = unary->getSubExpr();
      }
      if (const auto *binary = dyn_cast<BinaryOperator>(statement);
          binary && binary->getOpcode() == BO_SubAssign)
        decrement = binary->getLHS();
      if (const auto *field = memberField(read);
          field && !field->isBitField() && field->getType()->isPointerType() &&
          !field->getType().isVolatileQualified() &&
          field->getType()->getPointeeType().isConstQualified() &&
          !field->getType()->getPointeeType().isVolatileQualified() &&
          field->getType()->getPointeeType()->isCharType())
        data.insert(field);
      if (const auto *field = memberField(decrement);
          field && !field->isBitField() &&
          field->getType()->isUnsignedIntegerType() &&
          !field->getType()->isBooleanType() &&
          !field->getType().isVolatileQualified() &&
          !field->getType()->isAtomicType())
        counts.insert(field);
      for (const auto *child : statement->children()) {
        if (work.size() >= 65536)
          return {};
        work.push_back(child);
      }
    }
  }
  return data.size() == 1 && counts.size() == 1
             ? std::pair{*data.begin(), *counts.begin()}
             : std::pair<const FieldDecl *, const FieldDecl *>{};
}

bool TranslationUnitAnalyzer::verifyRecursiveContractGroup(
    const std::vector<unsigned> &component) {
  if (component.empty() || component.size() > 32 ||
      !store.activeRecursiveContracts.members.empty())
    return false;
  std::map<const FunctionDecl *, unsigned> members;
  QualType pointerType;
  const auto *first = definitions[component.front()];
  const bool extendsHead =
      first->getNumParams() == 2 && first->getReturnType()->isIntegerType() &&
      first->getParamDecl(0)->getType()->isPointerType() &&
      first->getParamDecl(0)->getType()->getPointeeType()->isRecordType() &&
      first->getParamDecl(1)->getType()->isUnsignedIntegerType() &&
      !first->getParamDecl(1)->getType()->isBooleanType();
  const bool mutableReader = first->getNumParams() == 1 &&
                             first->getParamDecl(0)->getType()->isPointerType();
  QualType readerType;
  if (first->getNumParams() == 1)
    readerType = mutableReader
                     ? first->getParamDecl(0)->getType()->getPointeeType()
                     : first->getParamDecl(0)->getType();
  const auto *reader =
      readerType.isNull() ? nullptr : readerType->getAsRecordDecl();
  const bool copiedReader =
      reader != nullptr && !reader->isUnion() &&
      first->getReturnType()->isPointerType() &&
      first->getReturnType()->getPointeeType()->getAsRecordDecl() != nullptr;
  const bool outputSlot =
      first->getNumParams() == 3 && first->getReturnType()->isIntegerType() &&
      first->getParamDecl(2)->getType()->isPointerType() &&
      first->getParamDecl(2)->getType()->getPointeeType()->isPointerType();
  const bool writes =
      !outputSlot && first->getNumParams() == 3 &&
      first->getReturnType()->isIntegerType() &&
      first->getParamDecl(2)->getType()->isPointerType() &&
      first->getParamDecl(2)->getType()->getPointeeType()->getAsRecordDecl() !=
          nullptr;
  const bool constructs =
      extendsHead || outputSlot || copiedReader ||
      (first->getNumParams() == 2 && first->getReturnType()->isPointerType() &&
       first->getReturnType()->getPointeeType()->getAsRecordDecl() != nullptr);
  for (const unsigned index : component) {
    const auto *definition = definitions[index];
    if (extendsHead || copiedReader) {
      if (!ASTContext::hasSameType(first->getType(), definition->getType()))
        return false;
    } else if (constructs || writes) {
      if (!ASTContext::hasSameType(first->getType(), definition->getType()) ||
          !definition->getParamDecl(0)->getType()->isPointerType() ||
          !definition->getParamDecl(0)
               ->getType()
               ->getPointeeType()
               ->isCharType() ||
          !definition->getParamDecl(0)
               ->getType()
               ->getPointeeType()
               .isConstQualified() ||
          !definition->getParamDecl(1)->getType()->isUnsignedIntegerType() ||
          definition->getParamDecl(1)->getType()->isBooleanType())
        return false;
    } else if (definition->getNumParams() != 1 ||
               !(definition->getReturnType()->isVoidType() ||
                 definition->getReturnType()->isIntegerType())) {
      return false;
    }
    auto type = constructs ? definition->getReturnType()
                           : definition->getParamDecl(0)->getType();
    if (extendsHead)
      type = definition->getParamDecl(0)->getType();
    if (writes)
      type = definition->getParamDecl(2)->getType();
    if (outputSlot)
      type = definition->getParamDecl(2)->getType()->getPointeeType();
    if (!type->isPointerType() || !type->getPointeeType()->getAsRecordDecl())
      return false;
    if (pointerType.isNull())
      pointerType = type;
    else if (!ASTContext::hasSameType(pointerType, type))
      return false;
    members.emplace(definition->getCanonicalDecl(),
                    static_cast<unsigned>(members.size()));
  }
  std::pair<const FieldDecl *, const FieldDecl *> readerFields;
  if (copiedReader) {
    std::vector<const FunctionDecl *> definitionsInGroup;
    definitionsInGroup.reserve(component.size());
    for (const auto index : component)
      definitionsInGroup.push_back(definitions[index]);
    readerFields = copiedReaderFields(*reader, definitionsInGroup);
    if (!readerFields.first || !readerFields.second) {
      for (const auto &[member, memberIndex] : members) {
        (void)memberIndex;
        store.failedRecursiveOutputs.insert_or_assign(member, writes);
      }
      return false;
    }
  }
  // Keep the group rule closed under effects. Construction may compose a
  // completed helper: the normal call transfer checks its requirements and
  // effects, and construction validation covers every resulting premise and
  // output. An unavailable helper or private global cannot disappear behind
  // the induction hypothesis.
  bool releases = false;
  for (const unsigned index : component) {
    std::vector<const Stmt *> pending{definitions[index]->getBody()};
    for (std::size_t i = 0; i < pending.size(); ++i) {
      if (pending.size() > 65536)
        return false;
      const auto *statement = pending[i];
      if (!statement)
        continue;
      if (const auto *reference = dyn_cast<DeclRefExpr>(statement))
        if (const auto *var = dyn_cast<VarDecl>(reference->getDecl());
            var && var->hasGlobalStorage())
          return false;
      if (const auto *call = dyn_cast<CallExpr>(statement)) {
        const auto *callee = call->getDirectCallee();
        releases |= callee != nullptr && callee->getName() == "free" &&
                    !callee->hasBody();
        bool allowed = callee != nullptr &&
                       (members.contains(callee->getCanonicalDecl()) ||
                        (callee->getName() == "free" && !callee->hasBody() &&
                         call->getNumArgs() == 1) ||
                        (constructs && !callee->hasBody() &&
                         (callee->getName() == "malloc" ||
                          callee->getName() == "calloc")));
        if (!allowed && (constructs || writes) && callee)
          if (const auto helper = store.lookup(*callee);
              helper && helper->summary->checked.complete())
            allowed = true;
        if (!allowed)
          return false;
      }
      for (const auto *child : statement->children())
        pending.push_back(child);
    }
  }
  if (!constructs && releases &&
      std::ranges::any_of(component, [&](unsigned index) {
        return !definitions[index]->getReturnType()->isVoidType();
      }))
    return false;
  store.activeRecursiveContracts.releases = releases && !constructs;
  store.activeRecursiveContracts.constructs = constructs;
  store.activeRecursiveContracts.extendsHead = extendsHead;
  store.activeRecursiveContracts.writes = writes;
  store.activeRecursiveContracts.mutableReader = copiedReader && mutableReader;
  store.activeRecursiveContracts.readerData = readerFields.first;
  store.activeRecursiveContracts.readerCount = readerFields.second;
  for (const auto &[function, index] : members) {
    (void)index;
    store.activeRecursiveContracts.members.insert(function);
  }
  const auto clear =
      llvm::scope_exit([&] { store.activeRecursiveContracts = {}; });
  std::map<const FunctionDecl *, core::FunctionSummary> candidates;
  std::vector<core::InductionEdge> edges;
  for (const unsigned index : component) {
    const auto &definition = *definitions[index];
    core::DiagnosticCollector collected;
    FunctionAnalyzer validator(context, collected, options);
    validator.validate(definition);
    if (std::ranges::any_of(collected.diagnostics(), [](const auto &entry) {
          return entry.severity == core::Severity::Error ||
                 entry.id == core::diag::InvalidAnnotation;
        }))
      return false;
    FunctionDataflow analysis(context, definition, collected, options, store,
                              false);
    analysis.run();
    if (!analysis.verifiedRecursiveContracts()) {
      if ((constructs || writes) && !analysis.recursiveContractCalls.empty())
        for (const auto &[member, memberIndex] : members) {
          (void)memberIndex;
          store.failedRecursiveOutputs.insert_or_assign(member, writes);
        }
      if (options.dumpStream) {
        *options.dumpStream << "  rejected recursive contract for "
                            << definition.getNameAsString() << ":\n";
        for (const auto &[identity, obligation] :
             analysis.summary().checked.obligations.entries()) {
          (void)identity;
          if (obligation.outcome >= core::SafetyOutcome::Unresolved)
            *options.dumpStream << "    " << obligation.reason << "\n";
        }
      }
      return false;
    }
    for (const auto &[callee, strict] : analysis.recursiveContractCalls) {
      const auto target = members.find(callee);
      if (target == members.end())
        return false;
      edges.push_back({.caller = members.at(definition.getCanonicalDecl()),
                       .callee = target->second,
                       .strict = strict});
    }
    auto summary = std::move(analysis).summary();
    if (std::ranges::any_of(summary.effects, [](const auto &entry) {
          return entry.first.isGlobal();
        }))
      return false;
    candidates.emplace(definition.getCanonicalDecl(), std::move(summary));
  }
  if (!core::validInductionProgress(static_cast<unsigned>(members.size()),
                                    edges)) {
    for (const auto &[function, index] : members) {
      (void)index;
      store.failedRecursiveProgress.insert(function);
    }
    return false;
  }
  // No candidate has been published or used as a completed summary. All
  // members proved their local operations and complete outputs; publication
  // below performs no intervening analysis or callback into another member.
  for (auto &[function, summary] : candidates) {
    store.failedRecursiveOutputs.erase(function);
    store.verifiedRecursiveContracts[function] = store.activeRecursiveContracts;
    store.setInferred(*function, std::move(summary), false, true);
  }
  if (options.stats) {
    const char *counter = "recursive_traversal_groups_verified";
    if (writes)
      counter = "recursive_writer_groups_verified";
    else if (constructs)
      counter = "recursive_construction_groups_verified";
    else if (releases)
      counter = "recursive_cleanup_groups_verified";
    options.stats->add(counter);
  }
  return true;
}

} // namespace weavec::analysis
