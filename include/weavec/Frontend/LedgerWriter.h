//===- LedgerWriter.h - Ledger JSON, SARIF and fingerprints ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §12: the writers of the ledger's two formats, over `llvm::json`
// and `llvm::SHA256` (Core may not use LLVM):
//
//   - JSON, schema `weavec-ledger` version 1 (§12.1): object keys in the
//     order the RFC shows, functions, sites and diagnostics in source order,
//     paths relative to the fingerprint root with `/` separators (absolute
//     when outside it);
//   - SARIF 2.1.0, one run (§12.2);
//   - fingerprints (§12.3), which RFC 0032's baselines key on;
//   - the fingerprint root, the `-fweavec-ledger` directory form (§16) and
//     the atomic write every ledger goes through.
//
// Three keys go beyond the §12.1 example. When `config.checks` is `verify`,
// every summary carries `verifyChecks` (the proven facets that received a
// verify check, §10.7) and `verifyCoverage`, the `{proven, checked}` pair
// per facet that gate G6 asks the ledger to report for the proven spatial
// and null facets. A `fixit` is the object
// `{file, line, column, insertion}`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_LEDGERWRITER_H
#define WEAVEC_FRONTEND_LEDGERWRITER_H

#include "weavec/Core/Ledger.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace weavec::frontend {

/// `-fweavec-ledger-format=` / `--ledger-format=`.
enum class LedgerFormat : std::uint8_t { Json, Sarif };

[[nodiscard]] std::string_view toString(LedgerFormat format) noexcept;
[[nodiscard]] std::optional<LedgerFormat>
parseLedgerFormat(std::string_view text);

struct LedgerWriteOptions {
  /// The directory relative paths in the ledger are relative to (the
  /// compile job's); empty means the process's working directory.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string workingDirectory = {};
  /// JSON indentation; 0 writes a single line.
  unsigned indent = 2;
  /// SARIF `invocations[0].executionSuccessful`: no internal error occurred.
  bool executionSuccessful = true;
};

/// §12.3: `hex(SHA-256(UTF-8("weavec-fp/1" ␟ key ␟ path ␟ function ␟
/// message ␟ ordinal)))[0:32]`, with ␟ = U+001F. `message` must already be
/// normalised.
[[nodiscard]] std::string computeFingerprint(llvm::StringRef key,
                                             llvm::StringRef path,
                                             llvm::StringRef function,
                                             llvm::StringRef message,
                                             std::size_t ordinal);

/// §12.3: every maximal run of ASCII digits becomes `0`, every run of
/// whitespace one space, and the result is trimmed.
[[nodiscard]] std::string normalizeFingerprintMessage(llvm::StringRef message);

/// The function component of a diagnostic's fingerprint at file scope.
inline constexpr llvm::StringLiteral FileScopeFunction = "<file-scope>";

/// Fills the fingerprint of every facet row and diagnostic (§12.3). The
/// writers use a filled fingerprint as it is and compute missing ones the
/// same way.
void assignFingerprints(core::Ledger &ledger,
                        const LedgerWriteOptions &options = {});

/// §12.3: the nearest ancestor directory of `sourceFile` that contains
/// `.git` (a directory, or a worktree's file), else `workingDirectory`.
/// Relative paths are taken against `workingDirectory` (the process's when
/// empty), and the result has no `.` or `..` components.
[[nodiscard]] std::string
detectFingerprintRoot(llvm::StringRef sourceFile,
                      llvm::StringRef workingDirectory);

/// §12.1: `path` relative to `root` with `/` separators, or absolute (with
/// `/` separators) when outside it. A relative `path` is first taken
/// against `workingDirectory` (the process's when empty). Empty stays empty.
[[nodiscard]] std::string pathRelativeToRoot(llvm::StringRef path,
                                             llvm::StringRef root,
                                             llvm::StringRef workingDirectory);

/// §12.1 JSON, terminated by a newline.
[[nodiscard]] std::string
renderLedgerJson(const core::Ledger &ledger,
                 const LedgerWriteOptions &options = {});
/// §12.2 SARIF 2.1.0, terminated by a newline.
[[nodiscard]] std::string
renderLedgerSarif(const core::Ledger &ledger,
                  const LedgerWriteOptions &options = {});
[[nodiscard]] std::string renderLedger(const core::Ledger &ledger,
                                       LedgerFormat format,
                                       const LedgerWriteOptions &options = {});

/// §16: a `-fweavec-ledger` value names a directory when it ends in a path
/// separator or names an existing directory.
[[nodiscard]] bool isLedgerDirectory(llvm::StringRef value);
/// §16: the file a `-fweavec-ledger` value writes for `artifact` (the
/// object at compile, the link output at link): `<dir>/<name>.ledger.json`,
/// with `<name>` the artifact's file name, for the directory form, and the
/// value itself otherwise.
[[nodiscard]] std::string ledgerPathFor(llvm::StringRef value,
                                        llvm::StringRef artifact);

/// Writes `contents` to a uniquely named temporary file next to `path` and
/// renames it into place, so parallel writers never interleave (§16).
bool writeFileAtomically(llvm::StringRef path, llvm::StringRef contents,
                         std::string *error = nullptr);
/// Renders `ledger` in `format` and writes it atomically to `path`.
bool writeLedger(llvm::StringRef path, const core::Ledger &ledger,
                 LedgerFormat format, const LedgerWriteOptions &options = {},
                 std::string *error = nullptr);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_LEDGERWRITER_H
