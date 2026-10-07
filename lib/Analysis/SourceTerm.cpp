//===- SourceTerm.cpp - Quantities spelled over C at a site ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/SourceTerm.h"

#include <string>
#include <utility>

namespace weavec::analysis {

SourceTerm::Step SourceTerm::Step::member(std::string name) {
  return Step{.kind = Kind::Member, .field = std::move(name)};
}

SourceTerm::Step SourceTerm::Step::deref() {
  return Step{.kind = Kind::Deref};
}

SourceTerm SourceTerm::ofConstant(std::int64_t value) {
  SourceTerm term;
  term.kind = Kind::Constant;
  term.constant = value;
  return term;
}

SourceTerm SourceTerm::ofPlace(const clang::ValueDecl &decl,
                               std::vector<Step> path) {
  SourceTerm term;
  term.kind = Kind::Place;
  term.decl = &decl;
  term.path = std::move(path);
  return term;
}

SourceTerm SourceTerm::ofExpr(const clang::Expr &expr) {
  SourceTerm term;
  term.kind = Kind::Expr;
  term.expr = &expr;
  return term;
}

static SourceTerm binaryTerm(SourceTerm::Kind kind, SourceTerm lhs,
                             SourceTerm rhs) {
  SourceTerm term;
  term.kind = kind;
  term.operands.push_back(std::move(lhs));
  term.operands.push_back(std::move(rhs));
  return term;
}

SourceTerm SourceTerm::add(SourceTerm lhs, SourceTerm rhs) {
  return binaryTerm(Kind::Add, std::move(lhs), std::move(rhs));
}

SourceTerm SourceTerm::sub(SourceTerm lhs, SourceTerm rhs) {
  return binaryTerm(Kind::Sub, std::move(lhs), std::move(rhs));
}

SourceTerm SourceTerm::mul(SourceTerm lhs, SourceTerm rhs) {
  return binaryTerm(Kind::Mul, std::move(lhs), std::move(rhs));
}

SourceTerm SourceTerm::div(SourceTerm lhs, std::int64_t divisor) {
  return binaryTerm(Kind::Div, std::move(lhs), ofConstant(divisor));
}

SourceTerm SourceTerm::strLen(SourceTerm pointer) {
  SourceTerm term;
  term.kind = Kind::StrLen;
  term.operands.push_back(std::move(pointer));
  return term;
}

std::string SourceTerm::toString() const {
  switch (kind) {
  case Kind::Constant:
    return std::to_string(constant);
  case Kind::Place: {
    std::string text = decl != nullptr ? decl->getNameAsString() : "<null>";
    for (const Step &step : path) {
      if (step.kind == Step::Kind::Deref) {
        text.insert(0, "(*");
        text += ')';
      } else {
        text += '.';
        text += step.field;
      }
    }
    return text;
  }
  case Kind::Add:
  case Kind::Sub:
  case Kind::Mul:
  case Kind::Div: {
    if (operands.size() != 2)
      return "<malformed>";
    const char *op = " * ";
    if (kind == Kind::Add)
      op = " + ";
    else if (kind == Kind::Sub)
      op = " - ";
    else if (kind == Kind::Div)
      op = " / ";
    std::string text = "(" + operands[0].toString();
    text += op;
    text += operands[1].toString();
    text += ')';
    return text;
  }
  case Kind::StrLen:
    return "strlen(" +
           (operands.empty() ? std::string("<malformed>")
                             : operands.front().toString()) +
           ")";
  case Kind::Expr:
    return "<expr>";
  }
  return "<malformed>";
}

} // namespace weavec::analysis
