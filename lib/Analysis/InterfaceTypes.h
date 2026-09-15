//===- InterfaceTypes.h - Internal storage adapters (RFC 0028) -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_LIB_ANALYSIS_INTERFACETYPES_H
#define WEAVEC_LIB_ANALYSIS_INTERFACETYPES_H

#include "weavec/Core/Interface.h"

#include "clang/AST/ASTContext.h"

namespace weavec::analysis {
[[nodiscard]] std::optional<core::InterfaceType>
describeInterfaceType(clang::QualType root, const clang::ASTContext &context);
/// Internal types only: never completes declarations in the source program.
[[nodiscard]] clang::QualType
materializeInterfaceType(const core::InterfaceType &description,
                         clang::ASTContext &context);
[[nodiscard]] std::string privateStorageName(const clang::VarDecl &var);
} // namespace weavec::analysis
#endif
