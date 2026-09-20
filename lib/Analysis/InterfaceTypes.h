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
/// The declared name of the variable a private-storage identity stands for
/// (RFC 0028 §2 encodes it in the identity). Empty when `name` is not one.
/// Diagnostics about another unit's private storage name it this way: the
/// proxy the analysis builds for it is internal and must never be named in
/// a message.
[[nodiscard]] std::string privateStorageVariable(llvm::StringRef name);
} // namespace weavec::analysis
#endif
