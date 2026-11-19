//===- RuleFix.h - Rule errors as data, and their fixes ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0049 §1. Every error of Safe C's and Safe C++'s rules is recorded as
// data: where it is, what it says, the declarations whose stated facts the
// rule read (its subjects), and the fixes the compiler can write. The
// checker fills them, the enforcement ledger carries them, and
// `weavec-cc --weavec-fix` and `--weavec-audit` read them.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_RULEFIX_H
#define WEAVEC_FRONTEND_RULEFIX_H

#include <cstdint>
#include <string>
#include <vector>

namespace weavec::frontend {

/// An edit of a source file: `text` inserted at `offset` (a fix only adds).
struct FixEdit {
  /// An absolute path.
  std::string file;
  unsigned offset = 0;
  std::string text;
  /// The file does not define WeaveC's macros where the edit is (no
  /// `weavec.h` before it): the fixer adds the include with it.
  bool needsInclude = false;
};

/// A fix of an error, or (§5) what a unit's evidence says about a
/// declaration other units want.
struct RuleFix {
  /// `certain`, `likely` or `choice` (§1).
  std::string level;
  /// The declaration, by `<file>:<line>:<column>` of its first
  /// declaration's name (a template's, for an instantiation's).
  std::string declaration;
  /// Its name (`Class::member` for a member), which an error's message may
  /// quote.
  std::string name;
  /// What the fix states of the declaration, for merging: `bound`,
  /// `escapes`, `borrowed`, `const`, `lifetimebound`, `aliased`, `copy`;
  /// and the fact itself (`counted_by(size_)`, `escapes`).
  std::string aspect;
  std::string fact;
  /// What to write, for the note.
  std::string text;
  /// A likely fix's evidence (§3.2): its kind and where.
  std::string evidence;
  /// Applied together, or not at all; empty for a choice.
  std::vector<FixEdit> edits;
};

/// One rule error.
struct RuleError {
  std::string rule;
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
  std::string message;
  /// The function (or, `global`, the global) whose code has the error.
  std::string owner;
  std::string ownerFile;
  unsigned ownerLine = 0;
  bool global = false;
  /// Under -fweavec-safe-survey: of code that is not safe yet, not printed.
  bool surveyed = false;
  /// The declarations whose stated facts the rule read.
  std::vector<std::string> subjects;
  std::vector<RuleFix> fixes;
};

/// A file of the project a unit read (§6): to refuse to edit one that
/// changed since.
struct ReadFile {
  std::string path;
  std::uint64_t size = 0;
  std::string hash;
};

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_RULEFIX_H
