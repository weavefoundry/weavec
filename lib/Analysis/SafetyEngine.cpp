//===- SafetyEngine.cpp - The engine interface (RFC 0030) -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/SafetyEngine.h"

namespace weavec::analysis {

SafetyEngine::~SafetyEngine() = default;

} // namespace weavec::analysis
