//===- KindTable.cpp - Pointer kinds by declaration (RFC 0030) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/KindTable.h"

#include "weavec/Analysis/ClangLocation.h"

#include "clang/AST/Type.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

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

std::string_view toString(MustAccessRule rule) noexcept {
  switch (rule) {
  case MustAccessRule::R1:
    return "R1";
  case MustAccessRule::R2:
    return "R2";
  case MustAccessRule::R3:
    return "R3";
  case MustAccessRule::R4:
    return "R4";
  case MustAccessRule::R5:
    return "R5";
  }
  return "R1";
}

std::string_view toString(RequirementEnforcement enforcement) noexcept {
  switch (enforcement) {
  case RequirementEnforcement::CallSites:
    return "call-sites";
  case RequirementEnforcement::CallerContract:
    return "caller-contract";
  }
  return "call-sites";
}

std::string RequirementGuard::toString() const {
  return lhs.toString() +
         (relation == Relation::Less ? std::string(" < ")
                                     : std::string(" <= ")) +
         rhs.toString();
}

std::string MustAccessRequirement::toString() const {
  std::string text;
  if (guard)
    text = guard->toString() + " -> ";
  return text + kind.toString() + " (" + std::string(analysis::toString(rule)) +
         ")";
}

bool KindEntry::hasShape() const noexcept {
  return kind.shape != core::PointerShape::Unknown;
}

bool KindEntry::hasDeclaredShape() const noexcept {
  return shapeLevel.has_value() && hasShape();
}

bool KindEntry::isNonnull() const noexcept {
  return kind.nullability == core::Nullability::Nonnull;
}

bool KindEntry::declaresNonnull() const noexcept {
  return nullabilityLevel.has_value() && isNonnull();
}

bool KindEntry::hasEnforcedRequirement() const noexcept {
  return enforcement == RequirementEnforcement::CallSites &&
         !mustAccess.empty();
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

bool KindTable::requireSafe(const clang::FunctionDecl &function) const {
  return safe.contains(function.getCanonicalDecl());
}

void KindTable::setRequireSafe(const clang::FunctionDecl &function) {
  safe.insert(function.getCanonicalDecl());
}

const OwnershipContract *
KindTable::ownership(const clang::FunctionDecl &function) const {
  const auto found = contracts.find(function.getCanonicalDecl());
  return found == contracts.end() ? nullptr : &found->second;
}

void KindTable::setOwnership(const clang::FunctionDecl &function,
                             OwnershipContract contract) {
  contracts.insert_or_assign(function.getCanonicalDecl(), std::move(contract));
}

void KindTable::addSuggestion(KindSuggestion suggestion) {
  suggestionList.push_back(std::move(suggestion));
}

std::vector<core::Diagnostic>
kindProblemDiagnostics(const KindTable &table, const clang::SourceManager &sm) {
  std::vector<const KindProblem *> ordered;
  ordered.reserve(table.problems().size());
  for (const KindProblem &problem : table.problems())
    ordered.push_back(&problem);
  std::ranges::stable_sort(
      ordered, [&sm](const KindProblem *a, const KindProblem *b) {
        return a->location != b->location &&
               sm.isBeforeInTranslationUnit(a->location, b->location);
      });
  std::vector<core::Diagnostic> out;
  for (const KindProblem *problem : ordered) {
    // A redeclaration repeats its annotations; report each problem once.
    const bool repeated = llvm::any_of(out, [&](const core::Diagnostic &seen) {
      return seen.message == problem->message &&
             seen.location == toCoreLocation(sm, problem->location);
    });
    if (repeated)
      continue;
    out.push_back(core::Diagnostic{
        .severity = core::Severity::Warning,
        .certainty = core::Certainty::Definite,
        .id = core::diag::InvalidAnnotation,
        .message = problem->message,
        .location = toCoreLocation(sm, problem->location),
        .notes = {},
        .fixits = {},
    });
  }
  return out;
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
