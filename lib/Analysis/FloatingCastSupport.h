//===- FloatingCastSupport.h - Target conversion proof (RFC 0029) -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_ANALYSIS_FLOATINGCASTSUPPORT_H
#define WEAVEC_ANALYSIS_FLOATINGCASTSUPPORT_H

namespace clang {
class ASTContext;
class CastExpr;
class FunctionDecl;
} // namespace clang

namespace weavec::analysis {
/// A local sufficient proof, never a floating arithmetic or return summary.
[[nodiscard]] bool finiteFloatingCast(const clang::CastExpr &cast,
                                      const clang::FunctionDecl &function,
                                      clang::ASTContext &context,
                                      bool excludesNan = false);
} // namespace weavec::analysis
#endif
