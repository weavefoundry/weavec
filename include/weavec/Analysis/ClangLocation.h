//===- ClangLocation.h - clang <-> core source location bridging -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_CLANGLOCATION_H
#define WEAVEC_ANALYSIS_CLANGLOCATION_H

#include "weavec/Core/SourceLocation.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"

namespace weavec::analysis {

/// Resolves `loc` (through macro expansions) into a core location. The raw
/// Clang encoding is preserved in `opaque` so the frontend can report at the
/// exact original position.
[[nodiscard]] core::SourceLocation
toCoreLocation(const clang::SourceManager &sm, clang::SourceLocation loc);

/// Recovers the Clang location stored by `toCoreLocation`, or an invalid
/// location if the core location did not originate from Clang.
[[nodiscard]] clang::SourceLocation
toClangLocation(const core::SourceLocation &loc);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_CLANGLOCATION_H
