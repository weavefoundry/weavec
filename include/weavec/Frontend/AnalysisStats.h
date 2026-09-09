//===- AnalysisStats.h - Invocation statistics output ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_FRONTEND_ANALYSISSTATS_H
#define WEAVEC_FRONTEND_ANALYSISSTATS_H
#include "weavec/Core/AnalysisStats.h"

#include <string_view>

namespace weavec::frontend {
bool writeAnalysisStats(std::string_view path, const core::AnalysisStats *stats,
                        bool final = true);
bool writeAtomicText(std::string_view path, std::string_view text);
} // namespace weavec::frontend
#endif // WEAVEC_FRONTEND_ANALYSISSTATS_H
