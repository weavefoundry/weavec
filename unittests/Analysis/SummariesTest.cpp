//===- SummariesTest.cpp - Tests for summary resolution -------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `SummaryStore::lookup` order (RFC 0003, provider steps 1-4), the
// annotation-derived summaries, and the shipped library table.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Summaries.h"

#include "TestUtils.h"

#include "clang/AST/RecursiveASTVisitor.h"

#include "llvm/ADT/STLExtras.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace weavec::analysis {
namespace {

using core::PlaceEffect;
using core::SummaryPath;
using core::ValueSource;

struct Parsed {
  std::unique_ptr<clang::ASTUnit> ast;

  [[nodiscard]] const clang::FunctionDecl *fn(llvm::StringRef name) const {
    for (const clang::Decl *decl :
         ast->getASTContext().getTranslationUnitDecl()->decls()) {
      const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (function != nullptr && function->getName() == name)
        return function;
    }
    return nullptr;
  }
};

} // namespace

static Parsed parse(const std::string &code) {
  Parsed parsed;
  parsed.ast = clang::tooling::buildASTFromCodeWithArgs(
      std::string(weavec::test::Prelude) + code, {"-std=c17", "-x", "c", "-w"},
      "input.c");
  return parsed;
}

namespace {

TEST(Summaries, AnnotationsDeriveASummary) {
  const auto parsed = parse(R"c(
    struct s;
    struct s *OWNED make(int n, struct s *OWNED a, const struct s *BORROWED b,
                         struct s *MUT c, struct s *d);
    struct s *BORROWED peekat(struct s *BORROWED s);
    void plain(struct s *p);
  )c");
  ASSERT_TRUE(parsed.ast);

  const clang::FunctionDecl *make = parsed.fn("make");
  ASSERT_NE(make, nullptr);
  EXPECT_TRUE(hasOwnershipAnnotations(*make));
  const core::FunctionSummary summary = summaryFromAnnotations(*make);
  EXPECT_FALSE(summary.consumes(0)) << "n is not a pointer";
  EXPECT_TRUE(summary.consumes(1));
  EXPECT_FALSE(summary.frees(1)) << "moved, not released";
  EXPECT_EQ(summary.borrowKind(2), core::BorrowKind::Shared);
  EXPECT_EQ(summary.borrowKind(3), core::BorrowKind::Mutable);
  EXPECT_EQ(summary.inferredKind(4), core::OwnershipKind::Unknown);
  EXPECT_EQ(summary.returns, std::set<ValueSource>{ValueSource::fresh()});

  const clang::FunctionDecl *peekat = parsed.fn("peekat");
  ASSERT_NE(peekat, nullptr);
  EXPECT_EQ(summaryFromAnnotations(*peekat).returns,
            std::set<ValueSource>{ValueSource::unknown()})
      << "a borrowed result's source is not known from the signature";

  const clang::FunctionDecl *plain = parsed.fn("plain");
  ASSERT_NE(plain, nullptr);
  EXPECT_FALSE(hasOwnershipAnnotations(*plain));
  EXPECT_TRUE(summaryFromAnnotations(*plain).empty());
}

TEST(Summaries, AnnotationsOnAnyRedeclarationCount) {
  const auto parsed = parse(R"c(
    void take(void *OWNED p);
    void take(void *p) { }
  )c");
  ASSERT_TRUE(parsed.ast);
  // The definition is the last redeclaration; the prototype carried the
  // annotation.
  const clang::FunctionDecl *definition = nullptr;
  for (const clang::Decl *decl :
       parsed.ast->getASTContext().getTranslationUnitDecl()->decls()) {
    const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (function != nullptr && function->getName() == "take" &&
        function->doesThisDeclarationHaveABody())
      definition = function;
  }
  ASSERT_NE(definition, nullptr);
  EXPECT_TRUE(hasOwnershipAnnotations(*definition));
  EXPECT_TRUE(summaryFromAnnotations(*definition).consumes(0));
  EXPECT_TRUE(collectAnnotations(*definition).params[0].owned);
}

TEST(Summaries, LookupOrder) {
  const auto parsed = parse(R"c(
    void *calloc(size_t, size_t);
    void take(void *OWNED p);
    void mystery(void *p);
    static void helper(void *p) {}
    __attribute__((annotate("weavec.unsafe"))) void raw(void *p) {}
  )c");
  ASSERT_TRUE(parsed.ast);
  SummaryStore store;

  const auto builtin = store.lookup(*parsed.fn("calloc"));
  ASSERT_TRUE(builtin);
  EXPECT_EQ(builtin->source, SummarySource::Builtin);
  EXPECT_TRUE(builtin->summary->returns.contains(ValueSource::fresh("free")));

  const auto annotated = store.lookup(*parsed.fn("take"));
  ASSERT_TRUE(annotated);
  EXPECT_EQ(annotated->source, SummarySource::Annotation);
  EXPECT_TRUE(annotated->summary->consumes(0));

  EXPECT_FALSE(store.lookup(*parsed.fn("mystery"))) << "nothing known";
  EXPECT_FALSE(store.lookup(*parsed.fn("helper"))) << "not analysed yet";

  // An unsafe definition's signature is its contract: known, and empty.
  const auto unsafe = store.lookup(*parsed.fn("raw"));
  ASSERT_TRUE(unsafe);
  EXPECT_TRUE(unsafe->summary->empty());

  // Once inferred, the helper resolves; setting the same summary again
  // reports no change.
  core::FunctionSummary inferred;
  inferred.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  EXPECT_TRUE(store.setInferred(*parsed.fn("helper"), inferred));
  EXPECT_FALSE(store.setInferred(*parsed.fn("helper"), inferred));
  const auto resolved = store.lookup(*parsed.fn("helper"));
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, SummarySource::Inferred);
  EXPECT_TRUE(resolved->summary->frees(0));

  // A user definition shadows the library table.
  core::FunctionSummary userCalloc;
  EXPECT_TRUE(store.setInferred(*parsed.fn("calloc"), userCalloc));
  const auto shadowed = store.lookup(*parsed.fn("calloc"));
  ASSERT_TRUE(shadowed);
  EXPECT_EQ(shadowed->source, SummarySource::Inferred);
  EXPECT_FALSE(shadowed->summary->returns.contains(ValueSource::fresh()));

  EXPECT_TRUE(store.noteUnknownCallee(*parsed.fn("mystery")));
  EXPECT_FALSE(store.noteUnknownCallee(*parsed.fn("mystery")))
      << "reported once per callee";
}

TEST(Summaries, AnnotationsOverrideInferredRootsOnly) {
  const auto parsed = parse(R"c(
    struct b { char *data; };
    void init(struct b *MUT b, char *p);
  )c");
  ASSERT_TRUE(parsed.ast);
  const clang::FunctionDecl *init = parsed.fn("init");
  ASSERT_NE(init, nullptr);

  // Pretend the body freed both `b` and `p` and stored into `b->data`.
  core::FunctionSummary inferred;
  inferred.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  inferred.addEffect(SummaryPath::param(1), PlaceEffect{.freed = true});
  inferred.addStore(
      core::Store{.dest = SummaryPath::param(0).deref().field("data"),
                  .value = ValueSource::fresh()});
  SummaryStore store;
  store.setInferred(*init, inferred);

  const auto resolved = store.lookup(*init);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, SummarySource::Annotation);
  EXPECT_FALSE(resolved->summary->consumes(0)) << "MUT says borrowed";
  EXPECT_EQ(resolved->summary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_TRUE(resolved->summary->consumes(1)) << "unannotated root: inferred";
  EXPECT_EQ(resolved->summary->stores.size(), 1U)
      << "stores through a borrowed parameter are kept";
}

// -- Function-pointer types (RFC 0004, "Signatures for function pointers") ----

TEST(Summaries, RawAnnotationDerivesASummary) {
  const auto parsed = parse(R"c(
    void *RAW lookup(int key, void *RAW ctx, void *OWNED taken);
  )c");
  ASSERT_TRUE(parsed.ast);
  const clang::FunctionDecl *lookup = parsed.fn("lookup");
  ASSERT_NE(lookup, nullptr);
  EXPECT_TRUE(hasOwnershipAnnotations(*lookup));
  const SignatureAnnotations annotations = collectAnnotations(*lookup);
  EXPECT_TRUE(annotations.result.raw);
  EXPECT_TRUE(annotations.params[1].raw);
  EXPECT_FALSE(annotations.params[1].safeKind().has_value());
  EXPECT_EQ(annotations.params[2].safeKind(), core::OwnershipKind::Owned);
  EXPECT_STREQ(macroSpelling(annotations.result), "WEAVEC_RAW");
  EXPECT_STREQ(macroSpelling(annotations.params[2]), "WEAVEC_OWNED");
  EXPECT_EQ(macroSpelling(annotations.params[0]), nullptr);

  const core::FunctionSummary summary = summaryFromAnnotations(*lookup);
  EXPECT_EQ(summary.returns, std::set<ValueSource>{ValueSource::raw()});
  EXPECT_EQ(summary.inferredReturnKind(), core::OwnershipKind::Raw);
  EXPECT_FALSE(summary.consumes(1)) << "a raw parameter is neither moved";
  EXPECT_FALSE(summary.borrowKind(1).has_value()) << "nor borrowed";
  EXPECT_TRUE(summary.consumes(2));
}

TEST(Summaries, FunctionTypeAnnotationsAreCollected) {
  const auto parsed = parse(R"c(
    struct node;
    typedef void (*dtor_t)(void *OWNED);
    typedef OWNED struct node *(*maker_t)(void);
    typedef RAW void *(*lookup_t)(int);
    struct ops {
      dtor_t drop;
      void *(*OWNED alloc)(size_t);
      void (*release)(void *OWNED);
      int (*plain)(int);
    };
    void user(dtor_t d, maker_t m, lookup_t l, struct ops *o,
              void (*inline_param)(void *OWNED), int (*cmp)(const void *, const void *),
              void (*table[2])(void *OWNED));
  )c");
  ASSERT_TRUE(parsed.ast);
  const clang::FunctionDecl *user = parsed.fn("user");
  ASSERT_NE(user, nullptr);

  // Through a typedef: annotations on the prototype's parameters.
  FunctionTypeAnnotations dtor =
      collectFunctionTypeAnnotations(*user->getParamDecl(0));
  ASSERT_NE(dtor.prototype, nullptr);
  ASSERT_EQ(dtor.params.size(), 1U);
  EXPECT_TRUE(dtor.params[0].owned);
  EXPECT_TRUE(dtor.anyOwnership());

  // An annotation on the typedef describes the result.
  FunctionTypeAnnotations maker =
      collectFunctionTypeAnnotations(*user->getParamDecl(1));
  ASSERT_NE(maker.prototype, nullptr);
  EXPECT_TRUE(maker.result.owned);
  EXPECT_TRUE(
      collectFunctionTypeAnnotations(*user->getParamDecl(2)).result.raw);

  // Inline on a parameter declarator, and on an array of callbacks.
  EXPECT_TRUE(
      collectFunctionTypeAnnotations(*user->getParamDecl(4)).params[0].owned);
  EXPECT_TRUE(
      collectFunctionTypeAnnotations(*user->getParamDecl(6)).params[0].owned);

  // Nothing on `cmp`.
  FunctionTypeAnnotations cmp =
      collectFunctionTypeAnnotations(*user->getParamDecl(5));
  ASSERT_NE(cmp.prototype, nullptr);
  EXPECT_FALSE(cmp.anyOwnership());

  // Fields: the declarator's own annotation is the result, the prototype's
  // parameters are the parameters.
  const clang::RecordDecl *ops = nullptr;
  for (const clang::Decl *decl :
       parsed.ast->getASTContext().getTranslationUnitDecl()->decls()) {
    if (const auto *record = llvm::dyn_cast<clang::RecordDecl>(decl);
        record != nullptr && record->getName() == "ops")
      ops = record;
  }
  ASSERT_NE(ops, nullptr);
  std::vector<const clang::FieldDecl *> fields;
  for (const clang::FieldDecl *field : ops->fields())
    fields.push_back(field);
  ASSERT_EQ(fields.size(), 4U);
  EXPECT_TRUE(collectFunctionTypeAnnotations(*fields[0]).params[0].owned)
      << "through the typedef";
  EXPECT_TRUE(collectFunctionTypeAnnotations(*fields[1]).result.owned);
  EXPECT_TRUE(collectFunctionTypeAnnotations(*fields[2]).params[0].owned);
  EXPECT_FALSE(collectFunctionTypeAnnotations(*fields[3]).anyOwnership());

  // A declaration that is not of function-pointer type has no prototype.
  EXPECT_EQ(collectFunctionTypeAnnotations(*user->getParamDecl(3)).prototype,
            nullptr);
}

TEST(Summaries, IndirectLookupOrder) {
  const auto result = weavec::test::analyze(R"c(
    struct node { int v; };
    typedef void (*dtor_t)(struct node *OWNED);
    static void node_free(struct node *n) { free(n); }
    static void node_peek(struct node *n) { use(n); }
    static void (*hook)(struct node *) = node_free;
    void f(dtor_t d, void (*cb)(struct node *), int (*cmp)(int, int),
           struct node *a, struct node *b, struct node *c) {
      d(a);
      cb(b);
      hook(c);
      cmp(1, 2);
      (void)node_peek;
    }
  )c");
  ASSERT_TRUE(result.ast);
  SummaryStore &store = result.analyzer->summaries();
  const clang::FunctionDecl *f = result.function("f");
  ASSERT_NE(f, nullptr);

  std::vector<const clang::CallExpr *> calls;
  struct Collector : clang::RecursiveASTVisitor<Collector> {
    std::vector<const clang::CallExpr *> *out = nullptr;
    // NOLINTNEXTLINE(readability-identifier-naming,bugprone-derived-method-shadowing-base-method,readability-make-member-function-const)
    bool VisitCallExpr(clang::CallExpr *call) {
      out->push_back(call);
      return true;
    }
  } collector;
  collector.out = &calls;
  collector.TraverseStmt(f->getBody());
  ASSERT_EQ(calls.size(), 4U);

  // 1. Type annotations win.
  const auto viaType = store.lookupIndirect(*calls[0]);
  ASSERT_TRUE(viaType);
  EXPECT_EQ(viaType->source, SummarySource::Annotation);
  EXPECT_TRUE(viaType->summary->consumes(0));
  EXPECT_FALSE(viaType->summary->frees(0)) << "moved, per the annotation";

  // 2. The join of address-taken functions of the type: `node_free` (in an
  //    initialiser) and `node_peek` (as a discarded value).
  EXPECT_EQ(store.candidatesFor(*calls[1]).size(), 2U);
  const auto viaJoin = store.lookupIndirect(*calls[1]);
  ASSERT_TRUE(viaJoin);
  EXPECT_EQ(viaJoin->source, SummarySource::Inferred);
  EXPECT_TRUE(viaJoin->summary->frees(0)) << "may free";
  EXPECT_EQ(viaJoin->summary->effectOf(SummaryPath::param(0).deref()).read,
            true)
      << "may read";
  EXPECT_TRUE(store.lookupIndirect(*calls[2]));

  // 3. Nothing for `cmp`; the boundary is noted once per type.
  EXPECT_TRUE(store.candidatesFor(*calls[3]).empty());
  EXPECT_FALSE(store.lookupIndirect(*calls[3]));
  EXPECT_TRUE(store.noteUnknownIndirect(*calls[3]));
  EXPECT_FALSE(store.noteUnknownIndirect(*calls[3]));

  EXPECT_EQ(indirectCalleeDecl(*calls[0]), f->getParamDecl(0));
  EXPECT_EQ(indirectCalleeDecl(*calls[2])->getKind(), clang::Decl::Var);
}

TEST(Summaries, GlobalTableInternsCanonicalDecls) {
  const auto parsed = parse(R"c(
    extern int g;
    int g;
    int h;
  )c");
  ASSERT_TRUE(parsed.ast);
  std::vector<const clang::VarDecl *> vars;
  for (const clang::Decl *decl :
       parsed.ast->getASTContext().getTranslationUnitDecl()->decls()) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
      vars.push_back(var);
  }
  ASSERT_EQ(vars.size(), 3U);
  GlobalTable table;
  const std::uint32_t g1 = table.idFor(*vars[0]);
  const std::uint32_t g2 = table.idFor(*vars[1]);
  const std::uint32_t h = table.idFor(*vars[2]);
  EXPECT_EQ(g1, g2) << "redeclarations share an id";
  EXPECT_NE(g1, h);
  EXPECT_EQ(table.size(), 2U);
  EXPECT_EQ(table.nameOf(g1), "g");
  EXPECT_EQ(table.nameOf(h), "h");
  EXPECT_EQ(table.nameOf(99), "<global>");
  EXPECT_EQ(table.declFor(99), nullptr);
}

// -- Builtins -----------------------------------------------------------------

TEST(Builtins, TableCoversTheAllocatorList) {
  const std::vector<llvm::StringRef> names = builtinNames();
  for (const char *expected :
       {"malloc", "calloc", "realloc", "free", "strdup", "strndup",
        "aligned_alloc", "fopen", "fclose", "strchr", "memcpy", "strtol"})
    EXPECT_TRUE(llvm::is_contained(names, expected)) << expected;
}

TEST(Builtins, Entries) {
  const auto parsed = parse(R"c(
    typedef struct FILE FILE;
    FILE *fopen(const char *, const char *);
    int fclose(FILE *);
    char *strchr(const char *, int);
    void *memcpy(void *, const void *, size_t);
    long strtol(const char *, char **, int);
    size_t strlen(const char *);
    void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
    char *getenv(const char *);
    static char *strdup(const char *s) { return 0; }
  )c");
  ASSERT_TRUE(parsed.ast);

  const auto *fopenSummary = builtinSummary(*parsed.fn("fopen"));
  ASSERT_NE(fopenSummary, nullptr);
  EXPECT_TRUE(fopenSummary->returns.contains(ValueSource::fresh("fclose")));
  EXPECT_EQ(fopenSummary->borrowKind(0), core::BorrowKind::Shared);

  const auto *fcloseSummary = builtinSummary(*parsed.fn("fclose"));
  ASSERT_NE(fcloseSummary, nullptr);
  EXPECT_TRUE(fcloseSummary->frees(0));

  const auto *strchrSummary = builtinSummary(*parsed.fn("strchr"));
  ASSERT_NE(strchrSummary, nullptr);
  EXPECT_EQ(
      strchrSummary->returns,
      std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(0))});
  EXPECT_EQ(strchrSummary->borrowKind(0), core::BorrowKind::Shared);

  const auto *memcpySummary = builtinSummary(*parsed.fn("memcpy"));
  ASSERT_NE(memcpySummary, nullptr);
  EXPECT_EQ(memcpySummary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(memcpySummary->borrowKind(1), core::BorrowKind::Shared);
  EXPECT_EQ(memcpySummary->returns,
            std::set<ValueSource>{ValueSource::copy(SummaryPath::param(0))});

  const auto *strtolSummary = builtinSummary(*parsed.fn("strtol"));
  ASSERT_NE(strtolSummary, nullptr);
  EXPECT_TRUE(strtolSummary->returns.empty());
  ASSERT_EQ(strtolSummary->stores.size(), 1U);
  EXPECT_EQ(strtolSummary->stores.begin()->dest, SummaryPath::param(1).deref());
  EXPECT_EQ(strtolSummary->stores.begin()->value,
            ValueSource::interiorCopy(SummaryPath::param(0)));

  const auto *strlenSummary = builtinSummary(*parsed.fn("strlen"));
  ASSERT_NE(strlenSummary, nullptr);
  EXPECT_EQ(strlenSummary->borrowKind(0), core::BorrowKind::Shared);
  EXPECT_TRUE(strlenSummary->returns.empty());

  const auto *bsearchSummary = builtinSummary(*parsed.fn("bsearch"));
  ASSERT_NE(bsearchSummary, nullptr);
  EXPECT_EQ(
      bsearchSummary->returns,
      std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(1))});

  const auto *getenvSummary = builtinSummary(*parsed.fn("getenv"));
  ASSERT_NE(getenvSummary, nullptr);
  EXPECT_TRUE(getenvSummary->returns.empty()) << "static storage: unknown";

  EXPECT_EQ(builtinSummary(*parsed.fn("strdup")), nullptr)
      << "a file-local function is not libc";
}

// -- POSIX / GNU / BSD entries (RFC 0004, "The library table") ----------------

TEST(Builtins, TableHasNoDuplicatesAndCoversPosix) {
  std::vector<llvm::StringRef> names = builtinNames();
  EXPECT_GE(names.size(), 200U);
  std::ranges::sort(names);
  EXPECT_EQ(std::ranges::adjacent_find(names), names.end())
      << "duplicate entry: "
      << (std::ranges::adjacent_find(names) == names.end()
              ? ""
              : std::ranges::adjacent_find(names)->str());
  for (const char *expected : {"getline",
                               "asprintf",
                               "popen",
                               "pclose",
                               "opendir",
                               "closedir",
                               "readdir",
                               "mmap",
                               "munmap",
                               "getaddrinfo",
                               "freeaddrinfo",
                               "dlopen",
                               "dlclose",
                               "pthread_create",
                               "pthread_mutex_lock",
                               "read",
                               "write",
                               "open",
                               "stat",
                               "localtime_r",
                               "strtok_r",
                               "strlcpy",
                               "inet_ntop",
                               "regcomp",
                               "regfree",
                               "iconv_open",
                               "iconv_close",
                               "posix_memalign",
                               "reallocarray",
                               "realpath",
                               "__errno_location",
                               "__error"})
    EXPECT_TRUE(llvm::is_contained(names, expected)) << expected;
}

TEST(Builtins, PosixEntries) {
  const auto parsed = parse(R"c(
    typedef struct FILE FILE;
    typedef struct DIR DIR;
    typedef long ssize_t;
    struct dirent; struct addrinfo; struct tm; typedef long time_t;
    ssize_t getline(char **, size_t *, FILE *);
    int asprintf(char **, const char *, ...);
    int posix_memalign(void **, size_t, size_t);
    FILE *popen(const char *, const char *);
    int pclose(FILE *);
    DIR *opendir(const char *);
    struct dirent *readdir(DIR *);
    int closedir(DIR *);
    void *mmap(void *, size_t, int, int, int, long);
    int munmap(void *, size_t);
    int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    void freeaddrinfo(struct addrinfo *);
    char *strtok_r(char *, const char *, char **);
    struct tm *localtime_r(const time_t *, struct tm *);
    struct tm *localtime(const time_t *);
    void *reallocarray(void *, size_t, size_t);
    int pthread_create(void *, const void *, void *(*)(void *), void *);
    ssize_t read(int, void *, size_t);
    ssize_t write(int, const void *, size_t);
  )c");
  ASSERT_TRUE(parsed.ast);

  const auto *getlineSummary = builtinSummary(*parsed.fn("getline"));
  ASSERT_NE(getlineSummary, nullptr);
  EXPECT_EQ(getlineSummary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(getlineSummary->borrowKind(2), core::BorrowKind::Mutable);
  ASSERT_EQ(getlineSummary->stores.size(), 1U);
  EXPECT_EQ(getlineSummary->stores.begin()->dest,
            SummaryPath::param(0).deref());
  EXPECT_EQ(getlineSummary->stores.begin()->value, ValueSource::fresh("free"));

  const auto *asprintfSummary = builtinSummary(*parsed.fn("asprintf"));
  ASSERT_NE(asprintfSummary, nullptr);
  EXPECT_EQ(asprintfSummary->stores.begin()->dest,
            SummaryPath::param(0).deref());
  EXPECT_EQ(asprintfSummary->stores.begin()->value, ValueSource::fresh("free"));
  EXPECT_EQ(builtinSummary(*parsed.fn("posix_memalign"))->stores,
            asprintfSummary->stores);

  EXPECT_TRUE(builtinSummary(*parsed.fn("popen"))
                  ->returns.contains(ValueSource::fresh("pclose")));
  EXPECT_TRUE(builtinSummary(*parsed.fn("pclose"))->frees(0));
  EXPECT_TRUE(builtinSummary(*parsed.fn("opendir"))
                  ->returns.contains(ValueSource::fresh("closedir")));
  EXPECT_TRUE(builtinSummary(*parsed.fn("closedir"))->frees(0));
  EXPECT_EQ(
      builtinSummary(*parsed.fn("readdir"))->returns,
      std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(0))})
      << "the entry lives in the stream";
  EXPECT_TRUE(builtinSummary(*parsed.fn("mmap"))
                  ->returns.contains(ValueSource::fresh("munmap")));
  EXPECT_TRUE(builtinSummary(*parsed.fn("munmap"))->frees(0));

  const auto *gaiSummary = builtinSummary(*parsed.fn("getaddrinfo"));
  ASSERT_NE(gaiSummary, nullptr);
  EXPECT_EQ(gaiSummary->borrowKind(2), core::BorrowKind::Shared);
  EXPECT_EQ(gaiSummary->stores.begin()->dest, SummaryPath::param(3).deref());
  EXPECT_EQ(gaiSummary->stores.begin()->value,
            ValueSource::fresh("freeaddrinfo"));
  EXPECT_TRUE(builtinSummary(*parsed.fn("freeaddrinfo"))->frees(0));

  // RFC 0006, *Alias exactness*: a pointer *into* the argument is an
  // interior copy; the argument itself is an exact one.
  const auto *strtokSummary = builtinSummary(*parsed.fn("strtok_r"));
  ASSERT_NE(strtokSummary, nullptr);
  EXPECT_EQ(
      strtokSummary->returns,
      std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(0))});
  EXPECT_EQ(strtokSummary->stores.begin()->dest, SummaryPath::param(2).deref());

  EXPECT_EQ(builtinSummary(*parsed.fn("localtime_r"))->returns,
            std::set<ValueSource>{ValueSource::copy(SummaryPath::param(1))});
  EXPECT_TRUE(builtinSummary(*parsed.fn("localtime"))->returns.empty())
      << "static storage: unknown";

  // RFC 0006, *Outcome-conditional summaries*: `reallocarray` consumes its
  // argument only when it returns non-null.
  const auto *reallocarraySummary = builtinSummary(*parsed.fn("reallocarray"));
  ASSERT_NE(reallocarraySummary, nullptr);
  EXPECT_TRUE(reallocarraySummary->consumes(0));
  EXPECT_FALSE(
      reallocarraySummary->consumesUnconditionally(SummaryPath::param(0)));
  EXPECT_TRUE(reallocarraySummary->outcomes.at(core::Outcome::NonNull)
                  .at(SummaryPath::param(0))
                  .moved);
  EXPECT_TRUE(reallocarraySummary->outcomes.at(core::Outcome::Null).empty());
  EXPECT_TRUE(reallocarraySummary->returns.contains(ValueSource::null()));

  const auto *createSummary = builtinSummary(*parsed.fn("pthread_create"));
  ASSERT_NE(createSummary, nullptr);
  EXPECT_EQ(createSummary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_FALSE(createSummary->borrowKind(3).has_value())
      << "the start routine's argument is out of scope (threads)";

  EXPECT_EQ(builtinSummary(*parsed.fn("read"))->borrowKind(1),
            core::BorrowKind::Mutable);
  EXPECT_EQ(builtinSummary(*parsed.fn("write"))->borrowKind(1),
            core::BorrowKind::Shared);
}

} // namespace
} // namespace weavec::analysis
