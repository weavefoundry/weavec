//===- RuntimeContractsTest.cpp - Checked runtime effects (RFC 0024) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
static core::CheckedContract runtimeCheck(const std::string &code) {
  static constexpr const char *Declarations = R"c(
    typedef __builtin_va_list va_list;
    #define va_start(a,p) __builtin_va_start(a,p)
    #define va_copy(a,b) __builtin_va_copy(a,b)
    #define va_end(a) __builtin_va_end(a)
    size_t strlen(const char *);
    int strcmp(const char *,const char *);
    int strncmp(const char *,const char *,size_t);
    void *memchr(const void *,int,size_t);
    void *memset(void *,int,size_t);
    void *memcpy(void *,const void *,size_t);
    int memcmp(const void *,const void *,size_t);
    int printf(const char *,...);
    int sprintf(char *,const char *,...);
    int snprintf(char *,size_t,const char *,...);
    int vprintf(const char *,va_list);
    int vsnprintf(char *,size_t,const char *,va_list);
    long read(int,void *,size_t);
    long write(int,const void *,size_t);
  )c";
  AnalysisOptions options;
  options.checkedFunctions.insert("f");
  const auto result = test::analyze(std::string(Declarations) + code, options);
  if (!result.ast || !result.summary("f")) {
    ADD_FAILURE() << "could not analyze runtime test";
    return {};
  }
  return result.summary("f")->checked;
}
TEST(RuntimeContracts, ReadOnlyOperationsRequireInitializedInputs) {
  for (const auto *body :
       {R"(int f(void){return strcmp("a","b");})",
        "int f(void){char a[2]={1,2};return strncmp(a,\"ab\",2);}",
        "int f(void){char a[2]={1,2};return memcmp(a,a,2);}",
        "int f(void){char a[2]={1,2};char*p=memchr(a,2,2);return p?*p:0;}",
        "int f(void){return write(1,\"abc\",3)<0;}"}) {
    const auto contract = runtimeCheck(body);
    EXPECT_TRUE(contract.complete()) << body;
    EXPECT_TRUE(contract.requirements.empty()) << body;
  }
  for (const auto *body :
       {"int f(void){char a[2];return memcmp(a,\"ab\",2);}",
        "int f(void){char a[2]={1,2};return strcmp(a,\"ab\");}",
        "int f(void){char a[2];return write(1,a,2)<0;}"})
    EXPECT_FALSE(runtimeCheck(body).complete()) << body;
}
TEST(RuntimeContracts,
     PartialInputAndResultCopiesPreserveOnlyTheReturnedPrefix) {
  EXPECT_TRUE(runtimeCheck("int f(void){char a[4];long "
                           "n=read(0,a,4);if(n<=0)return 0;return a[0];}")
                  .complete());
  EXPECT_TRUE(
      runtimeCheck("int f(void){char a[4];long "
                   "n=read(0,a,4),m=n;n=0;if(m<=0)return 0;return a[0];}")
          .complete());
  EXPECT_FALSE(runtimeCheck("int f(void){char a[4];long "
                            "n=read(0,a,1);if(n<=0)return 0;return a[3];}")
                   .complete());
  EXPECT_FALSE(runtimeCheck("int f(void){char a[4];long "
                            "n=read(0,a,4);n=1;if(n<=0)return 0;return a[0];}")
                   .complete());
  EXPECT_FALSE(runtimeCheck("int f(void){char a[4];long "
                            "n=read(0,a,4);if(n<0)return a[0];return 0;}")
                   .complete());
}
TEST(RuntimeContracts, FormatTypesPrecisionAndOverlap) {
  for (const auto *body :
       {"int f(void){char b[8];int n=snprintf(b,8,\"%d\",42);if(n<0)return "
        "0;return strlen(b);}",
        "int f(void){char s[3]={1,2,3},b[8];int "
        "n=snprintf(b,8,\"%.*s\",3,s);if(n<0)return 0;return strlen(b);}",
        "int f(void){char b[4];int n=snprintf(b,4,\"abcdef\");if(n<0)return "
        "0;return b[3];}",
        "int f(void){return snprintf(0,0,\"%d\",42);}"})
    EXPECT_TRUE(runtimeCheck(body).complete()) << body;
  for (const auto *body :
       {"int f(void){return printf(\"%s\",7);}",
        "int f(void){return printf(\"%d\",1L);}",
        R"(int f(void){return printf("%*s",1L,"a");})",
        R"(int f(void){return printf("%s %d","a");})",
        "int f(void){int n;return printf(\"%n\",&n);}",
        R"(int f(void){char b[4]="abc";return snprintf(b,4,"%s",b);})",
        R"(int f(void){char b[4]="abc";return sprintf(b,"%s",b);})",
        "int f(double x){char b[32];if(snprintf(b,32,\"%f\",x)<0)return "
        "0;return snprintf(b,32,\"%s\",b);}",
        "int f(void){char b[2];return sprintf(b,\"%u\",(unsigned)-1);}",
        "int f(void){char b[8];return "
        "__builtin___snprintf_chk(b,8,0,2,\"abc\");}",
        "int f(void){char b[16];int n=snprintf(b,16,\"%d\",1);if(n<0)return "
        "0;return b[15];}"})
    EXPECT_FALSE(runtimeCheck(body).complete()) << body;
}
TEST(RuntimeContracts, FormattedCountsNameOnlyTheWrittenPrefix) {
  const auto check = [](const char *tail) {
    std::string program =
        R"(int f(double x){char b[8];int n=snprintf(b,8,"%f",x);)";
    program += tail;
    program += '}';
    return runtimeCheck(program).complete();
  };
  for (const auto *tail :
       {"if(n>=0&&n<8)return b[n];return 0;", "if(n>=8)return b[7];return 0;"})
    EXPECT_TRUE(check(tail)) << tail;
  for (const auto *tail :
       {"if(n>=0&&n<7)return b[n+1];return 0;", "if(n<0)return b[0];return 0;",
        "n=7;return b[n];", "if(n>=8)return b[8];return 0;"})
    EXPECT_FALSE(check(tail)) << tail;
}
TEST(RuntimeContracts, VariadicLifecycleAndIndependentCopies) {
  const std::string caller = R"(int f(void){return out("%s","ok");})";
  EXPECT_TRUE(runtimeCheck("static int out(const char *fmt,...){va_list "
                           "a,b;va_start(a,fmt);va_copy(b,a);vprintf(fmt,a);"
                           "vprintf(fmt,b);va_end(a);va_end(b);return 0;}" +
                           caller)
                  .complete());
  for (const auto *body :
       {"va_list a;va_start(a,fmt);vprintf(fmt,a);vprintf(fmt,a);va_end(a);",
        "va_list a;va_start(a,fmt);va_end(a);vprintf(fmt,a);",
        "va_list a;vprintf(fmt,a);",
        "va_list a;va_start(a,fmt);vprintf(fmt,a);",
        "va_list a;va_start(a,fmt);va_start(a,fmt);va_end(a);",
        "{va_list a;va_start(a,fmt);}",
        "va_list a;va_start(a,fmt);memset(&a,0,sizeof "
        "a);vprintf(fmt,a);va_end(a);",
        "va_list a,b;va_start(a,fmt);memcpy(&b,&a,sizeof "
        "a);vprintf(fmt,b);va_end(a);",
        "va_list a;va_start(a,fmt);int "
        "n=__builtin_va_arg(a,int);va_end(a);(void)n;"})
    EXPECT_FALSE(
        runtimeCheck(std::string("static int out(const char *fmt,...){") +
                     body + "return 0;}" + caller)
            .complete())
        << body;
}
TEST(RuntimeContracts, PredictionHintsRetainBothOutcomes) {
  EXPECT_TRUE(runtimeCheck("int f(int n){char "
                           "a[4]={0};if(__builtin_expect_with_probability(n>=0&"
                           "&n<4,1,0.99))return a[n];return 0;}")
                  .complete());
  EXPECT_FALSE(
      runtimeCheck(
          "int f(void){int *p=0;if(__builtin_expect(1,0))return *p;return 0;}")
          .complete());
}
TEST(RuntimeContracts, VariadicStartNeedsItsValidLastParameter) {
  for (const auto *declaration : {"const char *fmt, int last", "float fmt",
                                  "char fmt", "register int fmt"}) {
    const std::string code = std::string("static void out(") + declaration +
                             ",...){va_list a;va_start(a,fmt);va_end(a);}";
    const std::string call =
        std::string(declaration) == "const char *fmt, int last"
            ? "out(\"%d\",1,2);"
            : "out(1,2);";
    std::string program = code;
    program += "int f(void){";
    program += call;
    program += "return 0;}";
    EXPECT_FALSE(runtimeCheck(program).complete());
  }
  EXPECT_TRUE(runtimeCheck("static void out(int last,...){va_list "
                           "a;va_start(a,last);vprintf(\"\",a);va_end(a);}int "
                           "f(void){out(0);return 0;}")
                  .complete());
}
TEST(RuntimeContracts, SourceDefinitionOverridesTheLibrarySpelling) {
  EXPECT_FALSE(
      runtimeCheck("int strcmp(const char*a,const char*b){return *(int*)0;}int "
                   "f(void){return strcmp(\"a\",\"b\");}")
          .complete());
}
TEST(RuntimeContracts, ImplicitOutputCarriesItsStreamThroughHelpers) {
  const std::string declarations = R"c(
# 1 "runtime-stdio.h" 1 3
    typedef struct FILE FILE;
    extern FILE *stdout;
    int fclose(FILE *);
    int fputs(const char *,FILE *);
    int puts(const char *);
    int putchar(int);
    int printf(const char *,...);
# 1 "input.c" 2
    static int emit(void){return puts("ok");}
    static void close_output(void){fclose(stdout);}
  )c";
  AnalysisOptions options;
  options.checkedFunctions.insert("main");
  for (const auto *body :
       {"puts(\"ok\");", "putchar(10);", "printf(\"%d\",42);", "emit();",
        "fputs(\"ok\",stdout);", "int (*output)(void)=emit;output();"}) {
    for (const auto *prefix :
         {"", "fclose(stdout);", "close_output();", "stdout=0;"}) {
      const auto result = test::analyze(declarations + "int main(void){" +
                                            prefix + body + "return 0;}",
                                        options);
      ASSERT_NE(result.summary("main"), nullptr);
      EXPECT_EQ(result.summary("main")->checked.complete(),
                std::string_view(prefix).empty())
          << prefix << body;
      ASSERT_NE(result.summary("emit"), nullptr);
      EXPECT_TRUE(std::ranges::any_of(
          result.summary("emit")->checked.requirements, [](const auto &entry) {
            return entry.kind == core::CheckedRequirementKind::StandardStream &&
                   entry.family == "stdout";
          }));
    }
  }
}
} // namespace weavec::analysis
