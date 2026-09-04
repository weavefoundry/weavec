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
//     never-returns                        no path reaches the exit
//     effect <path> <flag>[,<flag>]* [<guard>]
//                                          flags: read written freed moved
//                                          freed(<family>) moved(<family>)
//                                          replaced element (qualify a
//                                          consume)
//     store <path> <source> [<guard>]
//     return <source> [<guard>]
//     outcome <class>                      the class is a possible result
//     outcome <class> <path> <flag>[,<flag>]* [<guard>]
//     null <class> <path>                  the place is null on every path
//                                          returning the class
//     notnull <class> <path>               the place is non-null on every
//                                          path returning the class
//     requires <i>                         parameter <i> must not be null
//   end
//
//   path   ::= param <i> [<steps>] | global <name> [<steps>]
//            | result [<steps>]
//   steps  ::= ( '*' | '.' <field> | '[]' )+        (one token)
//   source ::= fresh | fresh(<family>) | null | unknown | raw | copy <path>
//            | interior <path> | borrow <path>
//   class  ::= null | nonnull | zero | positive | negative
//   family ::= an identifier naming the canonical releaser (free, fclose)
//   guard  ::= when <path> <fact> ( and <path> <fact> )*
//   fact   ::= =<integer> | <class> ( '|' <class> )*
//
// Version 2 (RFC 0006) added `outcome` and `interior` and dropped
// `realloc-like`. Version 3 (RFC 0007) added the optional release family on
// `fresh`, `freed` and `moved` (the bare spellings mean "unknown family")
// and the `null` line. Version 4 (RFC 0008) added the `replaced` and
// `element` flags, the `notnull` and `requires` lines and the `result` root.
// Version 5 (RFC 0009) added the `never-returns` line and the optional guard
// on `effect`, `outcome`, `store` and `return` lines: the effect, store or
// alternative holds only when every conjunct does.
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
inline constexpr unsigned SummaryFormatVersion = 5;

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

/// Spells `effect`'s flags as in the record format (`read,freed(free)`).
[[nodiscard]] std::string printFlags(const PlaceEffect &effect);

/// Spells `guard` as in the record format, with a leading space (` when
/// param 3 zero and param 2 nonnull`); empty for a trivial guard.
[[nodiscard]] std::string printGuard(const PathGuard &guard,
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
