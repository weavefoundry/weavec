//===- UnsafeRegions.h - WEAVEC_UNSAFE source ranges ------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §2.6. `WEAVEC_UNSAFE` before a function definition or a compound
// statement makes its source range unsafe: the guard pass leaves the
// accesses whose instructions' locations lie in one unguarded and removes
// their `array-bounds` checks. The collector runs over the AST before code
// generation, and only when the unit expanded the macro or spelled the
// annotation.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_UNSAFEREGIONS_H
#define WEAVEC_FRONTEND_UNSAFEREGIONS_H

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace clang {
class ASTConsumer;
class CompilerInstance;
} // namespace clang

namespace weavec::frontend {

/// The unsafe ranges of one unit, by file.
class UnsafeRegions {
public:
  struct Range {
    unsigned beginLine = 0;
    unsigned beginColumn = 0;
    unsigned endLine = 0;
    unsigned endColumn = 0;
  };

  /// `file` is a normalised absolute path.
  void add(llvm::StringRef file, Range range);
  /// Whether `line:column` of `file` lies in a range; column 0 matches a
  /// range anywhere on the line.
  [[nodiscard]] bool contains(llvm::StringRef file, unsigned line,
                              unsigned column) const;
  [[nodiscard]] bool empty() const { return ranges.empty(); }

  /// The normalised absolute form of `file`, relative to `directory`.
  [[nodiscard]] static std::string normalise(llvm::StringRef file,
                                             llvm::StringRef directory);

private:
  llvm::StringMap<std::vector<Range>> ranges;
};

/// The collector: a consumer to run before code generation, which fills
/// `out` at the end of the unit. It also watches the preprocessor, and
/// walks the AST only when the unit mentions `WEAVEC_UNSAFE` or
/// `weavec.unsafe`.
[[nodiscard]] std::unique_ptr<clang::ASTConsumer>
createUnsafeRegionCollector(clang::CompilerInstance &compiler,
                            std::shared_ptr<UnsafeRegions> out);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_UNSAFEREGIONS_H
