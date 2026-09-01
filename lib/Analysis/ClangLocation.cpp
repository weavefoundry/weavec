//===- ClangLocation.cpp - clang <-> core source location bridging --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/ClangLocation.h"

namespace weavec::analysis {

core::SourceLocation toCoreLocation(const clang::SourceManager &sm,
                                    clang::SourceLocation loc) {
  core::SourceLocation result;
  if (loc.isInvalid())
    return result;

  const clang::SourceLocation expansion = sm.getExpansionLoc(loc);
  const clang::PresumedLoc presumed = sm.getPresumedLoc(expansion);
  if (presumed.isValid()) {
    result.file = presumed.getFilename();
    result.line = presumed.getLine();
    result.column = presumed.getColumn();
  }
  result.opaque = expansion.getRawEncoding();
  return result;
}

clang::SourceLocation toClangLocation(const core::SourceLocation &loc) {
  if (loc.opaque == 0)
    return {};
  return clang::SourceLocation::getFromRawEncoding(
      static_cast<clang::SourceLocation::UIntTy>(loc.opaque));
}

} // namespace weavec::analysis
