//===- PreludeTest.cpp - Tests for the check prelude ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Prelude.h"

#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"

#include <gtest/gtest.h>

#include <string>

namespace weavec::frontend {

static std::string prelude(CheckMode mode, bool zeroInit = true,
                           PreludeForm form = PreludeForm::Inline) {
  PreludeOptions options;
  options.mode = mode;
  options.zeroInit = zeroInit;
  options.usableSize = UsableSizeQuery::MallocSize;
  options.form = form;
  return buildCheckPrelude(options);
}

TEST(PreludeTest, ModeNamesRoundTrip) {
  for (const CheckMode mode :
       {CheckMode::Trap, CheckMode::Report, CheckMode::Verify, CheckMode::None})
    EXPECT_EQ(parseCheckMode(checkModeName(mode)), mode);
  EXPECT_FALSE(parseCheckMode("abort").has_value());
}

TEST(PreludeTest, ChoosesTheUsableSizeQueryByTarget) {
  const auto query = [](llvm::StringRef triple) {
    return usableSizeQueryFor(llvm::Triple(triple));
  };
  EXPECT_EQ(query("arm64-apple-macosx14.0.0"), UsableSizeQuery::MallocSize);
  EXPECT_EQ(query("arm64-apple-ios17.0"), UsableSizeQuery::MallocSize);
  EXPECT_EQ(query("x86_64-unknown-linux-gnu"),
            UsableSizeQuery::MallocUsableSize);
  EXPECT_EQ(query("aarch64-unknown-linux-musl"),
            UsableSizeQuery::MallocUsableSize);
  EXPECT_EQ(query("aarch64-unknown-linux-android34"),
            UsableSizeQuery::MallocUsableSizeConst);
  EXPECT_EQ(query("x86_64-unknown-freebsd14.0"),
            UsableSizeQuery::MallocUsableSizeConst);
  EXPECT_EQ(query("x86_64-pc-windows-msvc"), UsableSizeQuery::None);
}

TEST(PreludeTest, ModeNoneHasNoPrelude) {
  EXPECT_TRUE(prelude(CheckMode::None).empty());
}

TEST(PreludeTest, TrapModeTrapsWithEveryTemplate) {
  const std::string text = prelude(CheckMode::Trap);
  // RFC 0034 §1.1: `live` is only declared; the backend and the runtime
  // check it.
  EXPECT_NE(text.find("extern void *__weavec_chk_live(const volatile void *);"),
            std::string::npos);
  EXPECT_NE(text.find("extern void *__weavec_chk_object(const volatile void "
                      "*, long long, unsigned long long,"),
            std::string::npos);
  for (const llvm::StringLiteral name : checkTemplates())
    if (name != "live")
      EXPECT_NE(text.find("__builtin_verbose_trap(\"weavec\", \"" + name.str() +
                          "\")"),
                std::string::npos)
          << name.str();
  EXPECT_NE(text.find("static __inline__ __attribute__((always_inline, "
                      "nodebug, unused)) void *__weavec_chk_nonnull("),
            std::string::npos);
  EXPECT_EQ(text.find("__weavec_prv_"), std::string::npos);
  EXPECT_EQ(text.find("__weavec_rt_report"), std::string::npos);
  // RFC 0034 §5.2: the checked wrappers, which compute the need at run
  // time; sprintf's forwards to vsprintf's, through the bounded writer.
  EXPECT_NE(text.find("extern unsigned long long __weavec_rt_room(const void "
                      "*);"),
            std::string::npos);
  EXPECT_NE(text.find("char *__weavec_chk_strcpy(char *d, const char *s, "
                      "unsigned long long cap,\n                    unsigned "
                      "what) {"),
            std::string::npos);
  EXPECT_NE(text.find("static __attribute__((unused, nodebug)) int "
                      "__weavec_chk_sprintf(char *d, const char *f, unsigned "
                      "long long cap,\n                   unsigned what, ...) "
                      "{"),
            std::string::npos);
  EXPECT_NE(text.find("__builtin_va_start(ap, what);"), std::string::npos);
  EXPECT_NE(text.find("r = __builtin_vsnprintf(d, room, f, ap);"),
            std::string::npos);
  EXPECT_NE(text.find("__builtin_verbose_trap(\"weavec\", \"disjoint\"))"),
            std::string::npos);
  // RFC 0034 §5.3: with the runtime, zero-initialisation asks the runtime
  // for a block's size.
  EXPECT_NE(text.find("extern __typeof__(sizeof 0) __weavec_rt_size(const void "
                      "*);"),
            std::string::npos);
}

TEST(PreludeTest, UsesNoMacrosNorLineComments) {
  // Preprocessed input expands no macro, and C89 has no `//` comments.
  for (const CheckMode mode :
       {CheckMode::Trap, CheckMode::Report, CheckMode::Verify}) {
    const std::string text = prelude(mode);
    EXPECT_EQ(text.find("#define"), std::string::npos);
    EXPECT_EQ(text.find("__SIZE_TYPE__"), std::string::npos);
    EXPECT_EQ(text.find("//"), std::string::npos);
    EXPECT_EQ(text.find('$'), std::string::npos) << "an unexpanded marker";
    // Every helper is a static always-inline function, but for the static
    // non-inline wrappers whose address a program takes (section 11) and
    // the variadic checked wrapper (RFC 0034 section 5.2), which cannot be
    // inlined.
    llvm::SmallVector<llvm::StringRef, 256> lines;
    llvm::StringRef(text).split(lines, '\n');
    for (const llvm::StringRef line : lines)
      if (!line.starts_with(" ") && line.contains("__weavec_") &&
          line.ends_with("{"))
        EXPECT_TRUE(line.starts_with("static __inline__ __attribute__") ||
                    (line.starts_with("static __attribute__((unused, "
                                      "nodebug))") &&
                     (line.contains("_zero_fn(") ||
                      line.contains("__weavec_chk_sprintf("))))
            << line.str();
  }
}

TEST(PreludeTest, ReportModeTakesTheSiteAndCallsTheRuntime) {
  const std::string text = prelude(CheckMode::Report);
  EXPECT_NE(text.find("extern void __weavec_rt_report(const char *, const "
                      "char *, unsigned, unsigned);"),
            std::string::npos);
  EXPECT_NE(text.find("__weavec_chk_index(unsigned long long i, unsigned long "
                      "long n, const char *file, unsigned line, unsigned "
                      "column)"),
            std::string::npos);
  // RFC 0034 §6.4: no unconditional trap.
  EXPECT_EQ(text.find("__weavec_chk_violation"), std::string::npos);
  EXPECT_NE(text.find("__weavec_rt_report(\"span\", file, line, column);"),
            std::string::npos);
  EXPECT_EQ(text.find("__builtin_verbose_trap"), std::string::npos);
  EXPECT_EQ(text.find("__builtin_trap"), std::string::npos);
  // RFC 0034 §5.2: a variadic wrapper takes the site before its `...`.
  EXPECT_NE(text.find("unsigned what, const char *file, unsigned line, "
                      "unsigned column, ...) {"),
            std::string::npos);
  EXPECT_NE(text.find("__builtin_va_start(ap, column);"), std::string::npos);
  EXPECT_NE(text.find("r = __weavec_chk_vsprintf(d, f, ap, cap, what, file, "
                      "line, column);"),
            std::string::npos);
}

TEST(PreludeTest, VerifyModeAddsTheProvenFamily) {
  const std::string text = prelude(CheckMode::Verify);
  EXPECT_NE(text.find("__weavec_chk_index("), std::string::npos);
  EXPECT_NE(text.find("__weavec_prv_index("), std::string::npos);
  EXPECT_NE(text.find("__builtin_verbose_trap(\"weavec.proven\", \"index\")"),
            std::string::npos);
  // A lowered violation is never a proven facet.
  EXPECT_EQ(text.find("__weavec_prv_violation"), std::string::npos);
}

TEST(PreludeTest, ZeroInitNeedsTheSwitchAndAQuery) {
  EXPECT_NE(prelude(CheckMode::Trap).find("__weavec_realloc_zero"),
            std::string::npos);
  EXPECT_EQ(prelude(CheckMode::Trap, /*zeroInit=*/false)
                .find("__weavec_realloc_zero"),
            std::string::npos);
  PreludeOptions options;
  options.usableSize = UsableSizeQuery::None;
  EXPECT_EQ(buildCheckPrelude(options).find("__weavec_malloc_zero"),
            std::string::npos);
  options.usableSize = UsableSizeQuery::MallocUsableSize;
  EXPECT_NE(buildCheckPrelude(options).find(
                "extern __typeof__(sizeof 0) __weavec_rt_size(const void *);"),
            std::string::npos);
  {
    PreludeOptions plain = options;
    plain.runtime = false;
    EXPECT_NE(buildCheckPrelude(plain).find(
                  "extern __typeof__(sizeof 0) malloc_usable_size(void *);"),
              std::string::npos);
  }
  options.verboseTrap = false;
  // RFC 0033 §6.1: with the runtime a trap first makes sure it ends the
  // program.
  EXPECT_NE(buildCheckPrelude(options).find(
                "(__weavec_rt_trapping(), __builtin_trap());"),
            std::string::npos);
  options.runtime = false;
  EXPECT_NE(buildCheckPrelude(options).find("__builtin_trap();"),
            std::string::npos);
  EXPECT_EQ(buildCheckPrelude(options).find("__weavec_rt_trapping"),
            std::string::npos);
}

TEST(PreludeTest, OutOfLineFormDefinesExternalHelpers) {
  const std::string checks =
      prelude(CheckMode::Verify, true, PreludeForm::OutOfLine);
  EXPECT_EQ(checks.find("static"), std::string::npos);
  EXPECT_EQ(checks.find("#pragma"), std::string::npos);
  EXPECT_NE(
      checks.find("\nvoid *__weavec_chk_nonnull(const volatile void *p) {"),
      std::string::npos);
  EXPECT_NE(checks.find("WEAVEC_CHK_TRAP(\"weavec.proven\", \"len\")"),
            std::string::npos);
  EXPECT_NE(checks.find("#ifdef WEAVEC_CHK_USABLE"), std::string::npos);
  EXPECT_NE(checks.find("WEAVEC_CHK_USABLE(p)"), std::string::npos);
  // One archive holds both families, so the report one is renamed.
  const std::string report =
      prelude(CheckMode::Report, true, PreludeForm::OutOfLine);
  EXPECT_NE(report.find("__weavec_chk_nonnull_report(const volatile void *p, "
                        "const char *file"),
            std::string::npos);
  EXPECT_NE(report.find("__weavec_strnlen_report("), std::string::npos);
  EXPECT_NE(report.find("\nint __weavec_chk_sprintf_report(char *d"),
            std::string::npos);
  EXPECT_EQ(report.find("__weavec_need_add"), std::string::npos);
}

// Section 10.2: warning free under -std=c89 -pedantic-errors -Weverything
// -Werror and every later standard, included or as preprocessed input.
TEST(PreludeTest, CompilesUnderEveryStandardWithEveryWarning) {
  for (const CheckMode mode : {CheckMode::Trap, CheckMode::Report,
                               CheckMode::Verify, CheckMode::None}) {
    for (const bool zeroInit : {true, false}) {
      const std::string unit =
          prelude(mode, zeroInit) + "int main(void) { return 0; }\n";
      for (const char *standard :
           {"-std=c89", "-std=c99", "-std=c11", "-std=c17", "-std=c2x"}) {
        for (const char *file : {"unit.c", "unit.i"}) {
          EXPECT_TRUE(clang::tooling::runToolOnCodeWithArgs(
              std::make_unique<clang::SyntaxOnlyAction>(), unit,
              {standard, "-pedantic-errors", "-Weverything", "-Werror",
               "-target", "arm64-apple-macosx14.0.0"},
              file))
              << checkModeName(mode).str() << " " << standard << " " << file;
        }
      }
    }
  }
  // The `__builtin_trap()` form, for compilers without
  // `__builtin_verbose_trap`, which `weavec-cc -fweavec-print-prelude` never
  // prints.
  PreludeOptions plain;
  plain.zeroInit = false;
  plain.verboseTrap = false;
  const std::string unit =
      buildCheckPrelude(plain) + "int main(void) { return 0; }\n";
  for (const char *standard : {"-std=c89", "-std=c2x"}) {
    EXPECT_TRUE(clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<clang::SyntaxOnlyAction>(), unit,
        {standard, "-pedantic-errors", "-Weverything", "-Werror", "-target",
         "arm64-apple-macosx14.0.0"},
        "unit.c"))
        << "plain " << standard;
  }
}

} // namespace weavec::frontend
