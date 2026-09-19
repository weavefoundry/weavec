//===- KindTable.cpp - Pointer kinds by declaration (RFC 0030) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/KindTable.h"

#include "clang/AST/Type.h"

namespace weavec::analysis {

std::string_view toString(KindLevel level) noexcept {
  switch (level) {
  case KindLevel::Annotation:
    return "annotation";
  case KindLevel::Ecosystem:
    return "ecosystem";
  case KindLevel::LibrarySpec:
    return "library-spec";
  case KindLevel::SystemHeader:
    return "system-header";
  }
  return "annotation";
}

bool KindEntry::hasShape() const noexcept {
  return kind.shape != core::PointerShape::Unknown;
}

bool KindEntry::isNonnull() const noexcept {
  return kind.nullability == core::Nullability::Nonnull;
}

bool KindEntry::shapeFromSystemHeader() const noexcept {
  return shapeLevel == KindLevel::SystemHeader;
}

bool KindEntry::nullabilityFromSystemHeader() const noexcept {
  return nullabilityLevel == KindLevel::SystemHeader;
}

bool KindEntry::isCheckOperand() const noexcept {
  return extentClass.has_value() && core::isCheckOperand(*extentClass) &&
         !shapeFromSystemHeader();
}

const KindEntry *KindTable::param(const clang::FunctionDecl &function,
                                  unsigned index) const {
  const auto found = params.find({function.getCanonicalDecl(), index});
  return found == params.end() ? nullptr : &found->second;
}

const KindEntry *KindTable::result(const clang::FunctionDecl &function) const {
  const auto found = results.find(function.getCanonicalDecl());
  return found == results.end() ? nullptr : &found->second;
}

const KindEntry *KindTable::field(const clang::FieldDecl &field) const {
  const auto found = fields.find(field.getCanonicalDecl());
  return found == fields.end() ? nullptr : &found->second;
}

const KindEntry *KindTable::variable(const clang::VarDecl &variable) const {
  const auto found = variables.find(variable.getCanonicalDecl());
  return found == variables.end() ? nullptr : &found->second;
}

bool KindTable::governedByLibrary(const clang::FunctionDecl &function) const {
  return library.contains(function.getCanonicalDecl());
}

void KindTable::setParam(const clang::FunctionDecl &function, unsigned index,
                         KindEntry entry) {
  params.insert_or_assign({function.getCanonicalDecl(), index},
                          std::move(entry));
}

void KindTable::setResult(const clang::FunctionDecl &function,
                          KindEntry entry) {
  results.insert_or_assign(function.getCanonicalDecl(), std::move(entry));
}

void KindTable::setField(const clang::FieldDecl &field, KindEntry entry) {
  fields.insert_or_assign(field.getCanonicalDecl(), std::move(entry));
}

void KindTable::setVariable(const clang::VarDecl &variable, KindEntry entry) {
  variables.insert_or_assign(variable.getCanonicalDecl(), std::move(entry));
}

void KindTable::setGovernedByLibrary(const clang::FunctionDecl &function) {
  library.insert(function.getCanonicalDecl());
}

void KindTable::addProblem(KindProblem problem) {
  problemList.push_back(std::move(problem));
}

std::size_t KindTable::size() const noexcept {
  return params.size() + results.size() + fields.size() + variables.size();
}

static core::LibraryParam::Type classOf(clang::QualType type) {
  if (type->isFunctionPointerType())
    return core::LibraryParam::Type::Function;
  if (type->isPointerType() || type->isArrayType())
    return core::LibraryParam::Type::Pointer;
  if (type->isIntegralOrEnumerationType())
    return core::LibraryParam::Type::Int;
  return core::LibraryParam::Type::Other;
}

std::optional<core::LibSignature>
librarySignatureOf(const clang::FunctionDecl &function) {
  const auto *prototype = function.getType()->getAs<clang::FunctionProtoType>();
  if (prototype == nullptr)
    return std::nullopt;
  core::LibSignature signature;
  signature.params.reserve(prototype->getNumParams());
  for (const clang::QualType param : prototype->getParamTypes())
    signature.params.push_back(classOf(param));
  signature.variadic = prototype->isVariadic();
  signature.pointerResult = prototype->getReturnType()->isPointerType();
  return signature;
}

std::optional<core::LibraryMatch>
governingLibraryEntry(const clang::FunctionDecl &function,
                      const core::LibrarySpec &library) {
  // §8: only when the program does not define the function; per unit, the
  // program is the unit (the link step decides across units, §13.2).
  if (function.getIdentifier() == nullptr || function.isDefined())
    return std::nullopt;
  const llvm::StringRef name = function.getName();
  const std::string_view callee(name.data(), name.size());
  if (const auto signature = librarySignatureOf(function))
    return library.lookup(callee, *signature);
  return library.lookup(callee);
}

} // namespace weavec::analysis
