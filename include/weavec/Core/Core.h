//===- Core.h - Umbrella header for the WeaveC core model -----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The core model is intentionally independent of Clang and LLVM. It contains
// the ownership lattice, lifetime constraints, borrow tracking, move tracking
// and a frontend-neutral diagnostics interface. Everything in this library
// operates on abstract `PlaceId`s and `SourceLocation`s that the frontend
// layer produces from Clang's AST.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_CORE_H
#define WEAVEC_CORE_CORE_H

#include "weavec/Core/Borrow.h"     // IWYU pragma: export
#include "weavec/Core/Diagnostic.h" // IWYU pragma: export
#include "weavec/Core/Lifetime.h"   // IWYU pragma: export
#include "weavec/Core/Moves.h"      // IWYU pragma: export
#include "weavec/Core/Ownership.h"  // IWYU pragma: export
#include "weavec/Core/Place.h"      // IWYU pragma: export
#include "weavec/Core/Summary.h"    // IWYU pragma: export

#endif // WEAVEC_CORE_CORE_H
