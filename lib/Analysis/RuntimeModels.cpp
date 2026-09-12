//===- RuntimeModels.cpp - Checked runtime registry (RFC 0024) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "RuntimeModels.h"

#include "clang/AST/ASTContext.h"

#include <array>
namespace weavec::analysis {
static constexpr auto Models = std::to_array<RuntimeModel>({
    {.name = "fabs",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "fabsf",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "fabsl",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "floor",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "floorf",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "floorl",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "ceil",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "ceilf",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "ceill",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "trunc",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "truncf",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "truncl",
     .family = RuntimeFamily::Numeric,
     .parameters = "r",
     .result = 'r'},
    {.name = "memcmp",
     .family = RuntimeFamily::Compare,
     .parameters = "ppz",
     .result = 'i'},
    {.name = "strcmp",
     .family = RuntimeFamily::Compare,
     .parameters = "ss",
     .result = 'i'},
    {.name = "strncmp",
     .family = RuntimeFamily::Compare,
     .parameters = "ssz",
     .result = 'i'},
    {.name = "memchr",
     .family = RuntimeFamily::Search,
     .parameters = "piz",
     .result = 'p'},
    {.name = "strchr",
     .family = RuntimeFamily::Search,
     .parameters = "si",
     .result = 'p'},
    {.name = "strrchr",
     .family = RuntimeFamily::Search,
     .parameters = "si",
     .result = 'p'},
    {.name = "strstr",
     .family = RuntimeFamily::Search,
     .parameters = "ss",
     .result = 'p'},
    {.name = "strpbrk",
     .family = RuntimeFamily::Search,
     .parameters = "ss",
     .result = 'p'},
    {.name = "strspn",
     .family = RuntimeFamily::Span,
     .parameters = "ss",
     .result = 'z'},
    {.name = "strcspn",
     .family = RuntimeFamily::Span,
     .parameters = "ss",
     .result = 'z'},
    {.name = "read",
     .family = RuntimeFamily::Input,
     .parameters = "dpz",
     .result = 'n'},
    {.name = "fread",
     .family = RuntimeFamily::Input,
     .parameters = "pzzf",
     .result = 'z'},
    {.name = "fgets",
     .family = RuntimeFamily::Input,
     .parameters = "sif",
     .result = 'p'},
    {.name = "write",
     .family = RuntimeFamily::Output,
     .parameters = "dpz",
     .result = 'n'},
    {.name = "fwrite",
     .family = RuntimeFamily::Output,
     .parameters = "pzzf",
     .result = 'z'},
    {.name = "fopen",
     .family = RuntimeFamily::Open,
     .parameters = "ss",
     .result = 'p'},
    {.name = "fdopen",
     .family = RuntimeFamily::Open,
     .parameters = "ds",
     .result = 'p'},
    {.name = "tmpfile",
     .family = RuntimeFamily::Open,
     .parameters = "",
     .result = 'p'},
    {.name = "fflush",
     .family = RuntimeFamily::Stream,
     .parameters = "f",
     .result = 'i'},
    {.name = "puts",
     .family = RuntimeFamily::Stream,
     .parameters = "s",
     .result = 'i'},
    {.name = "fputs",
     .family = RuntimeFamily::Stream,
     .parameters = "sf",
     .result = 'i'},
    {.name = "fputc",
     .family = RuntimeFamily::Stream,
     .parameters = "if",
     .result = 'i'},
    {.name = "putchar",
     .family = RuntimeFamily::Stream,
     .parameters = "i",
     .result = 'i'},
    {.name = "printf",
     .family = RuntimeFamily::Format,
     .parameters = "s",
     .result = 'i',
     .variadic = true},
    {.name = "fprintf",
     .family = RuntimeFamily::Format,
     .parameters = "fs",
     .result = 'i',
     .variadic = true},
    {.name = "sprintf",
     .family = RuntimeFamily::Format,
     .parameters = "ss",
     .result = 'i',
     .variadic = true},
    {.name = "snprintf",
     .family = RuntimeFamily::Format,
     .parameters = "szs",
     .result = 'i',
     .variadic = true},
    {.name = "vprintf",
     .family = RuntimeFamily::Format,
     .parameters = "sa",
     .result = 'i'},
    {.name = "vfprintf",
     .family = RuntimeFamily::Format,
     .parameters = "fsa",
     .result = 'i'},
    {.name = "vsprintf",
     .family = RuntimeFamily::Format,
     .parameters = "ssa",
     .result = 'i'},
    {.name = "vsnprintf",
     .family = RuntimeFamily::Format,
     .parameters = "szsa",
     .result = 'i'},
});
const RuntimeModel *runtimeModel(std::string_view name) {
  if (name.starts_with("__builtin___") && name.ends_with("_chk"))
    name = name.substr(12, name.size() - 16);
  else if (name.starts_with("__builtin_"))
    name.remove_prefix(10);
  for (const auto &model : Models)
    if (model.name == name)
      return &model;
  return nullptr;
}
bool runtimeSignature(const RuntimeModel &model, const clang::CallExpr &call,
                      const clang::ASTContext &context) {
  using clang::ASTContext;
  auto roles = std::string(model.parameters);
  if (const auto *callee = call.getDirectCallee();
      callee && callee->getBuiltinID() &&
      callee->getName().starts_with("__builtin___") &&
      callee->getName().ends_with("_chk")) {
    if (model.name == "snprintf" || model.name == "vsnprintf")
      roles.insert(2, "iz");
    else if (model.name == "sprintf" || model.name == "vsprintf")
      roles.insert(1, "iz");
    else
      return false;
  }
  const auto count = roles.size();
  if (call.getNumArgs() < count ||
      (!model.variadic && call.getNumArgs() != count))
    return false;
  const auto matches = [&](char role, clang::QualType type) {
    switch (role) {
    case 's':
      return type->isPointerType() && type->getPointeeType()->isCharType();
    case 'p':
      return type->isPointerType() && !type->isFunctionPointerType();
    case 'f':
      return type->isPointerType() && type->getPointeeType()->isRecordType();
    case 'z':
      return ASTContext::hasSameUnqualifiedType(type, context.getSizeType());
    case 'n':
      return ASTContext::hasSameUnqualifiedType(type,
                                                context.getSignedSizeType());
    case 'i':
    case 'd':
      return ASTContext::hasSameUnqualifiedType(type, context.IntTy);
    case 'r': {
      const bool base = model.name == "fabs" || model.name == "floor" ||
                        model.name == "ceil" || model.name == "trunc";
      auto expected = context.DoubleTy;
      if (!base)
        expected =
            model.name.ends_with('f') ? context.FloatTy : context.LongDoubleTy;
      return ASTContext::hasSameUnqualifiedType(type, expected);
    }
    case 'a': {
      auto expected = context.getBuiltinVaListType();
      if (const auto *array = context.getAsArrayType(expected))
        expected = context.getPointerType(array->getElementType());
      return ASTContext::hasSameUnqualifiedType(type, expected);
    }
    default:
      return false;
    }
  };
  if (!matches(model.result, call.getType()))
    return false;
  for (unsigned index = 0; index < count; ++index)
    if (!matches(roles[index], call.getArg(index)->getType()))
      return false;
  const auto *pointer =
      call.getCallee()->getType()->getAs<clang::PointerType>();
  const auto *prototype =
      pointer ? pointer->getPointeeType()->getAs<clang::FunctionProtoType>()
              : nullptr;
  if (!prototype || prototype->getNumParams() != count ||
      prototype->isVariadic() != model.variadic ||
      !matches(model.result, prototype->getReturnType()))
    return false;
  for (unsigned index = 0; index < count; ++index)
    if (!matches(roles[index], prototype->getParamType(index)))
      return false;
  return true;
}
} // namespace weavec::analysis
