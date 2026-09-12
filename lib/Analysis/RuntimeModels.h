//===- RuntimeModels.h - Checked runtime dispatch (RFC 0024) ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_ANALYSIS_RUNTIMEMODELS_H
#define WEAVEC_ANALYSIS_RUNTIMEMODELS_H
#include "clang/AST/Expr.h"

#include <cstdint>
#include <string_view>
namespace weavec::analysis {
enum class RuntimeFamily : std::uint8_t {
  Numeric,
  Compare,
  Search,
  Span,
  Input,
  Output,
  Open,
  Stream,
  Format
};
struct RuntimeModel {
  std::string_view name;
  RuntimeFamily family;
  // s: narrow string, p: memory, f: stream, z: size_t, i: int, d: fd,
  // r: real scalar, a: va_list. Result:
  // i:int,z:size_t,n:ssize_t,p:pointer,r:real.
  std::string_view parameters;
  char result;
  bool variadic = false;
};
[[nodiscard]] const RuntimeModel *runtimeModel(std::string_view name);
[[nodiscard]] bool runtimeSignature(const RuntimeModel &model,
                                    const clang::CallExpr &call,
                                    const clang::ASTContext &context);
} // namespace weavec::analysis
#endif // WEAVEC_ANALYSIS_RUNTIMEMODELS_H
