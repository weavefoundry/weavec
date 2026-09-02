//===- Builtins.cpp - Shipped summaries for the C standard library --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0003, provider step 3: summaries for the ISO C `<stdlib.h>`,
// `<string.h>` and `<stdio.h>` functions that take or return pointers, so
// calling libc from checked code neither warns nor hides a bug. Entries are
// matched by global name, as RFC 0002 matched its allocator list, and the
// list subsumes it.
//
// Each entry is a compact spec:
//
//   params  one character per parameter
//             'r'  reads through the pointer (shared borrow for the call)
//             'w'  writes through the pointer (mutable borrow for the call)
//             'f'  releases the pointer (`free`, `fclose`)
//             'm'  moves the pointer to the callee (`realloc`'s argument)
//             '.'  not a pointer, or no ownership effect
//   result  'F'  fresh owned allocation
//           '0'..'9'  a copy of that argument (`strchr` returns into `s`)
//           '-'  not a pointer, or nothing known (`getenv` returns static
//                storage, which this RFC spells as unknown)
//
// The few functions whose behaviour the spec cannot express (`realloc`,
// `strtol`'s end pointer) are patched by hand below.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Summaries.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <array>
#include <cassert>

using namespace clang;

namespace weavec::analysis {

namespace {

struct BuiltinSpec {
  llvm::StringLiteral name;
  llvm::StringLiteral params;
  char result;

  // The constructor keeps the table below terse; designated initialisers
  // would triple its width.
  constexpr BuiltinSpec(llvm::StringLiteral fn, llvm::StringLiteral args,
                        char ret)
      : name(fn), params(args), result(ret) {}
};

} // namespace

// clang-format off
static constexpr std::array<BuiltinSpec, 85> Specs = {{
    // <stdlib.h>
    {"malloc",        ".",      'F'},
    {"calloc",        "..",     'F'},
    {"realloc",       "m.",     'F'},
    {"aligned_alloc", "..",     'F'},
    {"free",          "f",      '-'},
    {"strdup",        "r",      'F'},
    {"strndup",       "r.",     'F'},
    {"atoi",          "r",      '-'},
    {"atol",          "r",      '-'},
    {"atoll",         "r",      '-'},
    {"atof",          "r",      '-'},
    {"strtol",        "rw.",    '-'},
    {"strtoll",       "rw.",    '-'},
    {"strtoul",       "rw.",    '-'},
    {"strtoull",      "rw.",    '-'},
    {"strtod",        "rw",     '-'},
    {"strtof",        "rw",     '-'},
    {"strtold",       "rw",     '-'},
    {"getenv",        "r",      '-'},
    {"system",        "r",      '-'},
    {"qsort",         "w...",   '-'},
    {"bsearch",       "rr...",  '1'},
    // <string.h>
    {"memcpy",        "wr.",    '0'},
    {"memmove",       "wr.",    '0'},
    {"memset",        "w..",    '0'},
    {"memcmp",        "rr.",    '-'},
    {"memchr",        "r..",    '0'},
    {"strcpy",        "wr",     '0'},
    {"strncpy",       "wr.",    '0'},
    {"strcat",        "wr",     '0'},
    {"strncat",       "wr.",    '0'},
    {"strcmp",        "rr",     '-'},
    {"strncmp",       "rr.",    '-'},
    {"strcoll",       "rr",     '-'},
    {"strxfrm",       "wr.",    '-'},
    {"strchr",        "r.",     '0'},
    {"strrchr",       "r.",     '0'},
    {"strstr",        "rr",     '0'},
    {"strpbrk",       "rr",     '0'},
    {"strspn",        "rr",     '-'},
    {"strcspn",       "rr",     '-'},
    {"strlen",        "r",      '-'},
    {"strnlen",       "r.",     '-'},
    {"strtok",        "wr",     '0'},
    {"strerror",      ".",      '-'},
    // <stdio.h>
    {"fopen",         "rr",     'F'},
    {"fdopen",        ".r",     'F'},
    {"tmpfile",       "",       'F'},
    {"fclose",        "f",      '-'},
    {"fflush",        "w",      '-'},
    {"printf",        "r",      '-'},
    {"fprintf",       "wr",     '-'},
    {"sprintf",       "wr",     '-'},
    {"snprintf",      "w.r",    '-'},
    {"vprintf",       "r.",     '-'},
    {"vfprintf",      "wr.",    '-'},
    {"vsprintf",      "wr.",    '-'},
    {"vsnprintf",     "w.r.",   '-'},
    {"scanf",         "r",      '-'},
    {"fscanf",        "wr",     '-'},
    {"sscanf",        "rr",     '-'},
    {"puts",          "r",      '-'},
    {"fputs",         "rw",     '-'},
    {"fputc",         ".w",     '-'},
    {"putc",          ".w",     '-'},
    {"fgetc",         "w",      '-'},
    {"getc",          "w",      '-'},
    {"ungetc",        ".w",     '-'},
    {"fgets",         "w.w",    '0'},
    {"fread",         "w..w",   '-'},
    {"fwrite",        "r..w",   '-'},
    {"fseek",         "w..",    '-'},
    {"ftell",         "r",      '-'},
    {"rewind",        "w",      '-'},
    {"fgetpos",       "rw",     '-'},
    {"fsetpos",       "wr",     '-'},
    {"feof",          "r",      '-'},
    {"ferror",        "r",      '-'},
    {"clearerr",      "w",      '-'},
    {"perror",        "r",      '-'},
    {"remove",        "r",      '-'},
    {"rename",        "rr",     '-'},
    {"setvbuf",       "ww..",   '-'},
    {"setbuf",        "ww",     '-'},
    {"fileno",        "r",      '-'},
}};
// clang-format on

static core::FunctionSummary fromSpec(const BuiltinSpec &spec) {
  core::FunctionSummary summary;
  for (unsigned i = 0; i < spec.params.size(); ++i) {
    const core::SummaryPath root = core::SummaryPath::param(i);
    switch (spec.params[i]) {
    case 'r':
      summary.addEffect(root.deref(), core::PlaceEffect{.read = true});
      break;
    case 'w':
      summary.addEffect(root.deref(), core::PlaceEffect{.written = true});
      break;
    case 'f':
      summary.addEffect(root, core::PlaceEffect{.freed = true});
      break;
    case 'm':
      summary.addEffect(root, core::PlaceEffect{.moved = true});
      break;
    case '.':
      break;
    default:
      assert(false && "unknown builtin parameter spec");
    }
  }
  if (spec.result == 'F')
    summary.addReturn(core::ValueSource::fresh());
  else if (spec.result >= '0' && spec.result <= '9')
    summary.addReturn(core::ValueSource::copy(
        core::SummaryPath::param(static_cast<unsigned>(spec.result - '0'))));
  return summary;
}

static llvm::StringMap<core::FunctionSummary> buildTable() {
  llvm::StringMap<core::FunctionSummary> table;
  for (const BuiltinSpec &spec : Specs)
    table[spec.name] = fromSpec(spec);

  table["realloc"].reallocLike = true;

  // `strto*(s, &end, ...)` store a pointer into `s` through `end`.
  for (const llvm::StringLiteral name :
       {llvm::StringLiteral("strtol"), llvm::StringLiteral("strtoll"),
        llvm::StringLiteral("strtoul"), llvm::StringLiteral("strtoull"),
        llvm::StringLiteral("strtod"), llvm::StringLiteral("strtof"),
        llvm::StringLiteral("strtold")}) {
    table[name].addStore(core::Store{
        .dest = core::SummaryPath::param(1).deref(),
        .value = core::ValueSource::copy(core::SummaryPath::param(0))});
  }
  return table;
}

static const llvm::StringMap<core::FunctionSummary> &table() {
  static const llvm::StringMap<core::FunctionSummary> Table = buildTable();
  return Table;
}

const core::FunctionSummary *builtinSummary(const FunctionDecl &function) {
  // Only global functions count: a static helper called `free` in some file
  // is the user's business, not libc's.
  if (!function.isGlobal())
    return nullptr;
  const IdentifierInfo *ident = function.getIdentifier();
  if (ident == nullptr)
    return nullptr;
  const auto it = table().find(ident->getName());
  return it == table().end() ? nullptr : &it->second;
}

std::vector<llvm::StringRef> builtinNames() {
  std::vector<llvm::StringRef> names;
  names.reserve(Specs.size());
  for (const BuiltinSpec &spec : Specs)
    names.push_back(spec.name);
  return names;
}

} // namespace weavec::analysis
