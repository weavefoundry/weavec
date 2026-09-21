//===- BypassedDeclarations.cpp - Jumps past a declaration (RFC 0030) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/BypassedDeclarations.h"

#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// Whether an object of `type` can hold a pointer: -ftrivial-auto-var-init
/// does not initialise a bypassed one.
static bool mayHoldPointer(clang::QualType type, unsigned depth = 0) {
  if (depth > 8 || type.isNull())
    return false;
  type = type.getCanonicalType();
  if (type->isPointerType() || type->isBlockPointerType())
    return true;
  if (const auto *array = type->getAsArrayTypeUnsafe())
    return mayHoldPointer(array->getElementType(), depth + 1);
  if (const auto *record = type->getAsRecordDecl()) {
    const clang::RecordDecl *definition = record->getDefinition();
    return definition != nullptr &&
           llvm::any_of(definition->fields(),
                        [depth](const clang::FieldDecl *field) {
                          return mayHoldPointer(field->getType(), depth + 1);
                        });
  }
  return false;
}

namespace {

/// §11, A5: the pointer locals of one body whose declaration a `goto`, a
/// computed `goto` or a `switch` can jump past.
class BypassFinder {
public:
  explicit BypassFinder(const clang::Stmt &body) { number(&body, nullptr); }

  [[nodiscard]] std::vector<const clang::VarDecl *> result() const {
    std::vector<const clang::VarDecl *> bypassed;
    for (const Declaration &declaration : declarations) {
      const auto inRegion = [&](unsigned at) {
        return at >= declaration.begin && at <= declaration.end;
      };
      if (llvm::any_of(jumps, [&](const std::pair<unsigned, unsigned> &jump) {
            return inRegion(jump.second) && !inRegion(jump.first);
          }))
        llvm::append_range(bypassed, declaration.variables);
    }
    return bypassed;
  }

private:
  struct Declaration {
    /// The pre-order indices after the declaration within its parent.
    unsigned begin = 0;
    unsigned end = 0;
    std::vector<const clang::VarDecl *> variables;
  };

  unsigned next = 0;
  llvm::DenseMap<const clang::Stmt *, unsigned> order;
  llvm::DenseMap<const clang::LabelDecl *, unsigned> labels;
  llvm::DenseSet<const clang::LabelDecl *> addressTaken;
  std::vector<std::pair<unsigned, const clang::LabelDecl *>> gotos;
  std::vector<unsigned> indirectGotos;
  std::vector<std::pair<unsigned, unsigned>> jumps;
  std::vector<Declaration> declarations;

  /// Numbers `node`'s subtree in pre-order; returns its last index.
  unsigned number(const clang::Stmt *node, const clang::Stmt *parent) {
    const unsigned at = next++;
    order[node] = at;
    if (const auto *label = llvm::dyn_cast<clang::LabelStmt>(node))
      labels[label->getDecl()] = at;
    else if (const auto *jump = llvm::dyn_cast<clang::GotoStmt>(node))
      gotos.emplace_back(at, jump->getLabel());
    else if (llvm::isa<clang::IndirectGotoStmt>(node))
      indirectGotos.push_back(at);
    else if (const auto *address = llvm::dyn_cast<clang::AddrLabelExpr>(node))
      addressTaken.insert(address->getLabel());
    unsigned last = at;
    std::vector<std::pair<const clang::DeclStmt *, unsigned>> pending;
    for (const clang::Stmt *child : node->children()) {
      if (child == nullptr || llvm::isa<clang::BlockExpr>(child))
        continue;
      last = number(child, node);
      if (const auto *statement = llvm::dyn_cast<clang::DeclStmt>(child))
        pending.emplace_back(statement, last);
    }
    for (const auto &[statement, end] : pending) {
      std::vector<const clang::VarDecl *> variables;
      for (const clang::Decl *decl : statement->decls())
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(decl);
            variable != nullptr && variable->hasLocalStorage() &&
            !variable->getType()->isVariablyModifiedType() &&
            mayHoldPointer(variable->getType()))
          variables.push_back(variable);
      if (!variables.empty() && end < last)
        declarations.push_back(Declaration{
            .begin = end + 1, .end = last, .variables = std::move(variables)});
    }
    if (const auto *switchStmt = llvm::dyn_cast<clang::SwitchStmt>(node))
      for (const clang::SwitchCase *label = switchStmt->getSwitchCaseList();
           label != nullptr; label = label->getNextSwitchCase())
        if (const auto found = order.find(label); found != order.end())
          jumps.emplace_back(at, found->second);
    if (parent == nullptr) {
      for (const auto &[from, label] : gotos)
        if (const auto target = labels.find(label); target != labels.end())
          jumps.emplace_back(from, target->second);
      for (const unsigned from : indirectGotos)
        for (const clang::LabelDecl *label : addressTaken)
          if (const auto target = labels.find(label); target != labels.end())
            jumps.emplace_back(from, target->second);
    }
    return last;
  }
};

} // namespace

std::vector<const clang::VarDecl *>
bypassedDeclarations(const clang::Stmt &body) {
  return BypassFinder(body).result();
}

} // namespace weavec::analysis
