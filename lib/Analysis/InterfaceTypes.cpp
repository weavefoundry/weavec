//===- InterfaceTypes.cpp - Portable C storage adapters (RFC 0028) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "InterfaceTypes.h"

#include "weavec/Analysis/ProgramDatabase.h"

#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"

#include <array>
#include <functional>
#include <map>

using namespace clang;
namespace weavec::analysis {

/// The identity is `@weavec-state:<source>:<declaration>:<offset>:<name>`,
/// each field hex-encoded, with the macro expansion chain appended.
std::string privateStorageVariable(llvm::StringRef name) {
  llvm::SmallVector<llvm::StringRef, 8> fields;
  name.split(fields, ':');
  if (fields.size() < 5 || fields.front() != "@weavec-state")
    return {};
  const llvm::StringRef encoded = fields[4];
  if (encoded.empty() || (encoded.size() % 2) != 0 ||
      !llvm::all_of(encoded, [](char c) { return llvm::isHexDigit(c); }))
    return {};
  return llvm::fromHex(encoded);
}

std::string privateStorageName(const VarDecl &var) {
  const auto &sm = var.getASTContext().getSourceManager();
  const auto file = sm.getFileEntryRefForID(sm.getMainFileID());
  if (!file)
    return {};
  llvm::SmallString<256> source(file->getName());
  if (const auto real = file->getFileEntry().tryGetRealPathName();
      !real.empty())
    source = real;
  (void)sm.getFileManager().makeAbsolutePath(source);
  llvm::sys::path::remove_dots(source, true);
  const auto location =
      sm.getExpansionLoc(var.getCanonicalDecl()->getLocation());
  if (location.isInvalid())
    return {};
  llvm::SmallString<256> declaration(sm.getFilename(location));
  if (const auto owner = sm.getFileEntryRefForID(sm.getFileID(location)))
    if (const auto real = owner->getFileEntry().tryGetRealPathName();
        !real.empty())
      declaration = real;
  (void)sm.getFileManager().makeAbsolutePath(declaration);
  llvm::sys::path::remove_dots(declaration, true);
  std::string name = "@weavec-state:" + llvm::toHex(source, true) + ":" +
                     llvm::toHex(declaration, true) + ":" +
                     std::to_string(sm.getFileOffset(location)) + ":" +
                     llvm::toHex(var.getName(), true);
  auto expanded = var.getCanonicalDecl()->getLocation();
  for (unsigned depth = 0; expanded.isMacroID(); ++depth) {
    if (depth == core::MaxInterfaceNodes)
      return {};
    const auto spelling = sm.getSpellingLoc(expanded);
    name += ":" + llvm::toHex(sm.getFilename(spelling), true) + ":" +
            std::to_string(sm.getFileOffset(spelling));
    expanded = sm.getImmediateMacroCallerLoc(expanded);
  }
  return name;
}

std::optional<core::InterfaceType>
describeInterfaceType(QualType root, const ASTContext &context) {
  core::InterfaceType result;
  std::map<const void *, std::uint32_t> seen;
  bool valid = true;
  const auto add = [&](auto &&self, QualType type) -> std::uint32_t {
    if (type.isNull()) {
      valid = false;
      return 0;
    }
    type = type.getCanonicalType();
    if (const auto found = seen.find(type.getAsOpaquePtr());
        found != seen.end())
      return found->second;
    if (result.nodes.size() >= core::MaxInterfaceNodes) {
      valid = false;
      return 0;
    }
    const auto id = static_cast<std::uint32_t>(result.nodes.size());
    seen.emplace(type.getAsOpaquePtr(), id);
    result.nodes.emplace_back();
    core::InterfaceNode node;
    node.qualifiers = (type.isConstQualified() ? 1U : 0U) |
                      (type.isVolatileQualified() ? 2U : 0U) |
                      (type.isRestrictQualified() ? 4U : 0U);
    if (type->isAtomicType() || type->isVariablyModifiedType() ||
        type->isVectorType() || type->isComplexType()) {
      valid = false;
      return id;
    }
    if (!type->isIncompleteType() && !type->isFunctionType() &&
        !type->isVoidType()) {
      node.bytes = static_cast<std::uint64_t>(
          context.getTypeSizeInChars(type).getQuantity());
      node.alignment = static_cast<std::uint64_t>(
          context.getTypeAlignInChars(type).getQuantity());
    }
    if (type->isVoidType()) {
      node.kind = core::InterfaceKind::Void;
    } else if (type->isIntegerType() && !type->isEnumeralType()) {
      node.kind = core::InterfaceKind::Integer;
      node.name = type.getUnqualifiedType().getAsString();
    } else if (type->isRealFloatingType()) {
      node.kind = core::InterfaceKind::Floating;
      node.name = type.getUnqualifiedType().getAsString();
    } else if (type->isPointerType()) {
      node.kind = core::InterfaceKind::Pointer;
      node.element = self(self, type->getPointeeType());
    } else if (const auto *function = type->getAs<FunctionType>()) {
      node.kind = core::InterfaceKind::Function;
      node.element = self(self, function->getReturnType());
      if (function->getCallConv() != CC_C) {
        valid = false;
      } else if (const auto *prototype =
                     dyn_cast<FunctionProtoType>(function)) {
        node.variadic = prototype->isVariadic();
        for (const auto parameter : prototype->param_types())
          node.parameters.push_back(self(self, parameter));
      } else {
        node.prototype = false;
      }
    } else if (const auto *array = context.getAsConstantArrayType(type)) {
      node.kind = core::InterfaceKind::Array;
      node.count = array->getSize().getLimitedValue();
      node.element = self(self, array->getElementType());
    } else if (const auto *record = type->getAsRecordDecl()) {
      node.kind = core::InterfaceKind::Record;
      if (record->isUnion()) {
        valid = false;
      } else {
        node.name = record->getNameAsString();
        if (node.name.empty())
          if (const auto *alias = record->getTypedefNameForAnonDecl())
            node.typedefName = alias->getNameAsString();
        if (record->isCompleteDefinition()) {
          node.view = recordLayoutKey(type, context);
          const auto &layout = context.getASTRecordLayout(record);
          for (const auto *field : record->fields()) {
            if (field->isBitField() || field->getName().empty()) {
              valid = false;
              break;
            }
            node.fields.push_back(
                {.name = field->getNameAsString(),
                 .type = self(self, field->getType()),
                 .offset = layout.getFieldOffset(field->getFieldIndex()) /
                           context.getCharWidth()});
          }
        }
      }
    } else {
      valid = false;
    }
    result.nodes[id] = std::move(node);
    return id;
  };
  (void)add(add, root);
  return valid && result.valid() && !result.encode().empty()
             ? std::optional(std::move(result))
             : std::nullopt;
}

QualType materializeInterfaceType(const core::InterfaceType &description,
                                  ASTContext &context) {
  if (!description.valid())
    return {};
  std::vector<QualType> types(description.nodes.size());
  std::vector<RecordDecl *> records(description.nodes.size());
  std::vector<bool> active(description.nodes.size());
  bool valid = true;
  for (std::size_t i = 0; i < types.size(); ++i) {
    const auto &node = description.nodes[i];
    if (node.kind != core::InterfaceKind::Record)
      continue;
    records[i] = RecordDecl::Create(
        context, TagTypeKind::Struct, context.getTranslationUnitDecl(), {}, {},
        node.name.empty() ? nullptr : &context.Idents.get(node.name));
    records[i]->setImplicit();
    if (!node.typedefName.empty()) {
      auto *alias = TypedefDecl::Create(
          context, context.getTranslationUnitDecl(), {}, {},
          &context.Idents.get(node.typedefName),
          context.getTrivialTypeSourceInfo(
              context.getCanonicalTypeDeclType(records[i])));
      alias->setImplicit();
      records[i]->setTypedefNameForAnonDecl(alias);
    }
    Qualifiers qualifiers;
    if (node.qualifiers & 1U)
      qualifiers.addConst();
    if (node.qualifiers & 2U)
      qualifiers.addVolatile();
    if (node.qualifiers & 4U)
      qualifiers.addRestrict();
    types[i] = context.getQualifiedType(
        context.getCanonicalTypeDeclType(records[i]), qualifiers);
  }
  const auto build = [&](auto &&self, std::uint32_t id) -> QualType {
    if (!types[id].isNull())
      return types[id];
    if (active[id]) {
      valid = false;
      return {};
    }
    active[id] = true;
    const auto &node = description.nodes[id];
    QualType type;
    switch (node.kind) {
    case core::InterfaceKind::Void:
      type = context.VoidTy;
      break;
    case core::InterfaceKind::Integer:
    case core::InterfaceKind::Floating: {
      const std::array<QualType, 17> candidates = {
          context.BoolTy,       context.CharTy,
          context.SignedCharTy, context.UnsignedCharTy,
          context.ShortTy,      context.UnsignedShortTy,
          context.IntTy,        context.UnsignedIntTy,
          context.LongTy,       context.UnsignedLongTy,
          context.LongLongTy,   context.UnsignedLongLongTy,
          context.Int128Ty,     context.UnsignedInt128Ty,
          context.FloatTy,      context.DoubleTy,
          context.LongDoubleTy};
      for (const auto candidate : candidates)
        if (candidate.getAsString() == node.name &&
            (node.kind == core::InterfaceKind::Integer
                 ? candidate->isIntegerType()
                 : candidate->isRealFloatingType()))
          type = candidate;
      break;
    }
    case core::InterfaceKind::Pointer: {
      const auto element = self(self, node.element);
      if (!element.isNull())
        type = context.getPointerType(element);
      break;
    }
    case core::InterfaceKind::Array: {
      const auto element = self(self, node.element);
      if (!element.isNull())
        type =
            context.getConstantArrayType(element, llvm::APInt(64, node.count),
                                         nullptr, ArraySizeModifier::Normal, 0);
      break;
    }
    case core::InterfaceKind::Function: {
      const auto result = self(self, node.element);
      std::vector<QualType> parameters;
      for (const auto parameter : node.parameters) {
        const auto argument = self(self, parameter);
        if (argument.isNull())
          valid = false;
        parameters.push_back(argument);
      }
      if (!result.isNull() && valid) {
        if (node.prototype) {
          FunctionProtoType::ExtProtoInfo info;
          info.Variadic = node.variadic;
          type = context.getFunctionType(result, parameters, info);
        } else {
          type = context.getFunctionNoProtoType(result);
        }
      }
      break;
    }
    case core::InterfaceKind::Record:
      break;
    }
    if (type.isNull()) {
      valid = false;
    } else {
      Qualifiers qualifiers;
      if (node.qualifiers & 1U)
        qualifiers.addConst();
      if (node.qualifiers & 2U)
        qualifiers.addVolatile();
      if (node.qualifiers & 4U)
        qualifiers.addRestrict();
      type = context.getQualifiedType(type, qualifiers);
      types[id] = type;
    }
    active[id] = false;
    return type;
  };
  for (std::uint32_t i = 0; i < types.size(); ++i)
    (void)build(build, i);
  if (!valid)
    return {};
  for (std::size_t i = 0; i < types.size(); ++i) {
    const auto &node = description.nodes[i];
    if (!records[i] || !node.bytes)
      continue;
    records[i]->startDefinition();
    for (const auto &field : node.fields) {
      auto *decl = FieldDecl::Create(
          context, records[i], {}, {}, &context.Idents.get(field.name),
          types[field.type], nullptr, nullptr, false, ICIS_NoInit);
      decl->setImplicit();
      records[i]->addDecl(decl);
    }
    records[i]->completeDefinition();
  }
  for (std::size_t i = 0; i < types.size(); ++i) {
    const auto &node = description.nodes[i];
    if (!node.bytes)
      continue;
    if (types[i]->isIncompleteType() ||
        std::cmp_not_equal(context.getTypeSizeInChars(types[i]).getQuantity(),
                           node.bytes) ||
        std::cmp_not_equal(context.getTypeAlignInChars(types[i]).getQuantity(),
                           node.alignment))
      return {};
    if (records[i]) {
      if (recordLayoutKey(types[i], context) != node.view)
        return {};
      const auto &layout = context.getASTRecordLayout(records[i]);
      for (std::size_t j = 0; j < node.fields.size(); ++j)
        if (layout.getFieldOffset(static_cast<unsigned>(j)) /
                context.getCharWidth() !=
            node.fields[j].offset)
          return {};
    }
  }
  return types.front();
}
} // namespace weavec::analysis
