//===- Core.h - Umbrella header for the WeaveC core model -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The core model is independent of Clang and LLVM (RFC 0031 §2): the
// abstract domain the engine runs over (`Heap`, `Zone`), summaries and their
// text (`Effects`, `EffectsIO`), the ledger, pointer kinds, the library
// table, function-pointer slots and the diagnostics interface. Programs are
// named through opaque handles and `SourceLocation`s the Analysis layer
// produces from Clang's AST.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_CORE_H
#define WEAVEC_CORE_CORE_H

#include "weavec/Core/Diagnostic.h"  // IWYU pragma: export
#include "weavec/Core/Effects.h"     // IWYU pragma: export
#include "weavec/Core/EffectsIO.h"   // IWYU pragma: export
#include "weavec/Core/FnSlots.h"     // IWYU pragma: export
#include "weavec/Core/Heap.h"        // IWYU pragma: export
#include "weavec/Core/Ledger.h"      // IWYU pragma: export
#include "weavec/Core/LibrarySpec.h" // IWYU pragma: export
#include "weavec/Core/Ownership.h"   // IWYU pragma: export
#include "weavec/Core/Path.h"        // IWYU pragma: export
#include "weavec/Core/PointerKind.h" // IWYU pragma: export
#include "weavec/Core/Zone.h"        // IWYU pragma: export

#endif // WEAVEC_CORE_CORE_H
