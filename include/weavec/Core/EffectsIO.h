//===- EffectsIO.h - Summary format 30 text, joins, renumbering -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0031 §6.1, §7: the text a unit record carries for a function's
// summary, and the operations the program database needs on summaries: the
// join of several definitions or indirect-call candidates, and renumbering
// the global roots between units.
//
// One item per line, tokens separated by one space:
//
//   returns always|may|never
//   incomplete <text>
//   effect release|move|unknown|escape|share-|share+ <path> when=<case>
//          [family=<f>] [may] [lossy] [offset=<n>] [elements=<t>,<t>]
//   store <path> when=<case> [may] [elements=<t>,<t>] :: <value>
//   result classes=<c>,... [param=<i>=0|<i>!=0] :: <value>
//   nonnull-on <class> <path>
//   reads <path> | writes <path>
//
//   <path>  ::= (p<i> | g<i> | r) ( '*' | '.' <name> | '[' <name>? ']' )*
//   <case>  ::= <c>,...|- ':' (<i>=0|<i>!=0|-)
//   <t>     ::= <n> | <path>@<scale>@<constant>
//   <value> ::= null|fresh|path|static|int|dangling|unknown
//               [family=<f>] [extent=<t>] [zeroed] [path=<path>]
//               [offset=<n>] [lo=<n>] [hi=<n>] [maybe-null] [object=<n>]
//               [many]
//
// Names and families are percent-encoded outside `[A-Za-z0-9_#-]`. Every
// field round-trips: `parseEffects(printEffects(e)) == e`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_EFFECTSIO_H
#define WEAVEC_CORE_EFFECTSIO_H

#include "weavec/Core/Effects.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace weavec::core {

/// The summary format this file reads and writes (RFC 0031 §6.1).
inline constexpr unsigned EffectsFormatVersion = 30;

/// The summary as format-30 lines.
[[nodiscard]] std::string printEffects(const FunctionEffects &effects);

/// Reads `printEffects`' text, or returns none with `error` naming the line
/// and what is wrong.
[[nodiscard]] std::optional<FunctionEffects>
parseEffects(std::string_view text, std::string *error = nullptr);

/// A path's token (`p0*.next`), and back.
[[nodiscard]] std::string printPath(const SummaryPath &path);
[[nodiscard]] std::optional<SummaryPath> parsePath(std::string_view text);

/// A summary sound for a call that may reach either function: effects on
/// one side only become possible ones, results are the alternatives of
/// both, a non-null guarantee holds only where both give it.
[[nodiscard]] FunctionEffects joinEffects(const FunctionEffects &left,
                                          const FunctionEffects &right);

/// RFC 0031 §6.4: the widening of a recursive function's summary `previous`
/// by the next round's `next`: their join, where the integer results of one
/// case merge into one whose bounds that moved from `previous` are dropped,
/// so a component's rounds stop changing.
[[nodiscard]] FunctionEffects widenEffects(const FunctionEffects &previous,
                                           const FunctionEffects &next);

/// The id a global root has in another numbering, or none when that side
/// has no such global.
using GlobalRenumbering =
    std::function<std::optional<std::uint32_t>(std::uint32_t)>;

/// `effects` with every global root renumbered. On the other side a global
/// it does not have cannot be named: a store into one is dropped, a value
/// read from one is unknown, and an effect on an object reached through one
/// makes the summary incomplete (RFC 0030 §5.5), since the object may be
/// reachable there through other pointers.
[[nodiscard]] FunctionEffects renumberGlobals(const FunctionEffects &effects,
                                              const GlobalRenumbering &map);

} // namespace weavec::core

#endif // WEAVEC_CORE_EFFECTSIO_H
