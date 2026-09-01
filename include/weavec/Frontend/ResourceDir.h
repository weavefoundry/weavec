//===- ResourceDir.h - Locating WeaveC's resource directory ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_RESOURCEDIR_H
#define WEAVEC_FRONTEND_RESOURCEDIR_H

#include <string>

namespace weavec::frontend {

/// Finds the directory containing `weavec.h`, the C-facing annotation header.
///
/// Resolution order:
///   1. `$WEAVEC_RESOURCE_DIR/include` if the variable is set.
///   2. `<exe-dir>/../lib/weavec/include` (installed layout).
///   3. The source-tree resources directory baked in at build time.
///
/// Returns an empty string if none exists.
std::string findResourceIncludeDir(const char *argv0, void *mainAddr);

/// The Clang resource directory (builtin headers such as `<stddef.h>`) of the
/// Clang installation WeaveC was built against, or an empty string if it is
/// not available on this machine.
std::string getClangResourceDir();

/// A default system root for the host, used when the command line does not
/// specify one. On Apple platforms this is `$SDKROOT` or the SDK reported by
/// `xcrun --show-sdk-path`; elsewhere it is empty (the driver's defaults are
/// correct there).
std::string getDefaultSysroot();

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_RESOURCEDIR_H
