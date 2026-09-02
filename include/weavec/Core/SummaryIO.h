//===- SummaryIO.h - Text form of function summaries -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The stable text form of a `FunctionSummary` (RFC 0005, *The summary text
// format*), used by the sidecar files that carry summaries between
// translation units and by the whole-program dump. One summary is one
// line-oriented record:
//
//   summary
//     effect <path> <flag>[,<flag>]*      flags: read written freed moved
//     store <path> <source>
//     return <source>
//     realloc-like
//   end
//
//   path   ::= param <i> [<steps>] | global <name> [<steps>]
//   steps  ::= ( '*' | '.' <field> | '[]' )+        (one token)
//   source ::= fresh | null | unknown | raw | copy <path> | borrow <path>
//
// Global roots are spelled by name; the caller supplies the mapping between
// the summary's global ids and names in both directions, so this file stays
// free of any frontend.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_SUMMARYIO_H
#define WEAVEC_CORE_SUMMARYIO_H

#include "weavec/Core/Summary.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace weavec::core {

/// Version of the record format; bumped when a record written by this
/// version cannot be read by the previous one.
inline constexpr unsigned SummaryFormatVersion = 1;

/// The name to print for a global root id.
using GlobalNamer = std::function<std::string(std::uint32_t)>;

/// The id to use for a global root name on reading, or `nullopt` to drop
/// every fact about that root (RFC 0005, *The program database*).
using GlobalResolver =
    std::function<std::optional<std::uint32_t>(std::string_view)>;

/// Spells `path` as in the record format (`param 0 *.data`).
[[nodiscard]] std::string printSummaryPath(const SummaryPath &path,
                                           const GlobalNamer &names);

/// Spells `source` as in the record format (`copy param 1`).
[[nodiscard]] std::string printValueSource(const ValueSource &source,
                                           const GlobalNamer &names);

/// Prints `summary` as one record, `summary\n ... end\n`, lines indented by
/// two spaces and in a deterministic order.
[[nodiscard]] std::string printSummary(const FunctionSummary &summary,
                                       const GlobalNamer &names);

/// Parses one record produced by `printSummary`. Leading and trailing blank
/// lines are ignored. Unknown line kinds are skipped; a malformed line fails
/// the record and, when `error` is given, describes why. A global root the
/// resolver declines is dropped: its effects vanish and a `copy`/`borrow` of
/// it becomes `unknown`.
[[nodiscard]] std::optional<FunctionSummary>
parseSummary(std::string_view record, const GlobalResolver &resolve,
             std::string *error = nullptr);

} // namespace weavec::core

#endif // WEAVEC_CORE_SUMMARYIO_H
