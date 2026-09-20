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
#include "weavec/Analysis/KindTable.h"

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
  EXPECT_EQ(builtin->source, SummarySource::Library);
  ASSERT_TRUE(builtin->library);
  EXPECT_EQ(builtin->library->entry->name, "calloc");
  EXPECT_EQ(builtin->summary->freshReturnFamily(), "free");

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

  // RFC 0014: type candidates order inference, while effects require a
  // concrete function-pointer value at the call site.
  EXPECT_EQ(store.candidatesFor(*calls[1]).size(), 2U);
  EXPECT_FALSE(store.lookupIndirect(*calls[1]));
  EXPECT_FALSE(store.lookupIndirect(*calls[2]));

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

TEST(Summaries, PrivateStorageHasStableInvisibleForeignAdapters) {
  const auto owner = parse("static void (*hook)(void *); static int hidden;");
  const auto foreign = parse("int unrelated;");
  ASSERT_TRUE(owner.ast);
  ASSERT_TRUE(foreign.ast);
  GlobalTable local;
  GlobalTable remote;
  const clang::VarDecl *hook = nullptr;
  for (const auto *decl :
       owner.ast->getASTContext().getTranslationUnitDecl()->decls())
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl)) {
      if (var->getName() == "hook")
        hook = var;
      else if (var->getName() == "hidden")
        EXPECT_TRUE(local.portableName(local.idFor(*var)));
    }
  ASSERT_NE(hook, nullptr);
  const auto id = local.idFor(*hook);
  const auto name = local.portableName(id);
  ASSERT_TRUE(name);
  const auto &ctx = foreign.ast->getASTContext();
  const auto before = std::distance(ctx.getTranslationUnitDecl()->decls_begin(),
                                    ctx.getTranslationUnitDecl()->decls_end());
  const auto proxy = remote.importName(*name, ctx, local.interfaces);
  ASSERT_TRUE(proxy);
  EXPECT_TRUE(remote.declFor(*proxy)->isImplicit());
  EXPECT_TRUE(remote.declFor(*proxy)->getType()->isFunctionPointerType());
  EXPECT_EQ(remote.importName(*name, ctx, local.interfaces), proxy);
  EXPECT_EQ(remote.portableName(*proxy), name);
  EXPECT_EQ(remote.callbackName(*proxy), local.callbackName(id));
  EXPECT_EQ(before, std::distance(ctx.getTranslationUnitDecl()->decls_begin(),
                                  ctx.getTranslationUnitDecl()->decls_end()));
  EXPECT_EQ(local.importName(*name, owner.ast->getASTContext()), id);
  EXPECT_FALSE(remote.importName("@weavec-hook:broken", ctx));
  EXPECT_FALSE(remote.importName("@weavec-hook:00", ctx));
  EXPECT_FALSE(remote.importName("missing", ctx));
}

// -- The library table (RFC 0030 §8) ------------------------------------------

/// The summary the `LibrarySpec` row governing `function` states, or
/// nothing when no row governs it.
static std::optional<core::FunctionSummary>
libraryOf(const clang::FunctionDecl *function) {
  if (function == nullptr)
    return std::nullopt;
  const auto match =
      governingLibraryEntry(*function, core::LibrarySpec::shipped());
  if (!match)
    return std::nullopt;
  return librarySummaryOf(*match, *function);
}

TEST(LibrarySummaries, TableCoversTheAllocatorList) {
  for (const char *expected :
       {"malloc",  "calloc",         "realloc",       "free",
        "strdup",  "strndup",        "aligned_alloc", "fopen",
        "fclose",  "strchr",         "memcpy",        "strtol",
        "getline", "asprintf",       "popen",         "pclose",
        "opendir", "closedir",       "readdir",       "mmap",
        "munmap",  "getaddrinfo",    "freeaddrinfo",  "pthread_create",
        "read",    "write",          "strtok_r",      "regcomp",
        "regfree", "posix_memalign", "reallocarray",  "realpath"})
    EXPECT_NE(core::LibrarySpec::shipped().find(expected), nullptr) << expected;
}

TEST(LibrarySummaries, Entries) {
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

  const auto fopenSummary = libraryOf(parsed.fn("fopen"));
  ASSERT_TRUE(fopenSummary);
  EXPECT_TRUE(fopenSummary->returnsFresh());
  EXPECT_EQ(fopenSummary->freshReturnFamily(), "fclose");
  EXPECT_TRUE(fopenSummary->mayReturnNull());
  EXPECT_EQ(fopenSummary->borrowKind(0), core::BorrowKind::Shared);

  const auto fcloseSummary = libraryOf(parsed.fn("fclose"));
  ASSERT_TRUE(fcloseSummary);
  EXPECT_TRUE(fcloseSummary->frees(0));
  EXPECT_TRUE(fcloseSummary->requiresParam(0));

  // RFC 0008, *Nullness*: a pointer into an argument may also be null (not
  // found); a whole-argument copy (`memcpy`) is exactly the argument.
  const auto strchrSummary = libraryOf(parsed.fn("strchr"));
  ASSERT_TRUE(strchrSummary);
  EXPECT_EQ(
      strchrSummary->returns,
      (std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(0)),
                             ValueSource::null()}));
  EXPECT_EQ(strchrSummary->borrowKind(0), core::BorrowKind::Shared);
  EXPECT_TRUE(strchrSummary->requiresParam(0)) << "reads through it";

  const auto memcpySummary = libraryOf(parsed.fn("memcpy"));
  ASSERT_TRUE(memcpySummary);
  EXPECT_EQ(memcpySummary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(memcpySummary->borrowKind(1), core::BorrowKind::Shared);
  EXPECT_EQ(memcpySummary->returns,
            std::set<ValueSource>{ValueSource::copy(SummaryPath::param(0))});
  // §8.3: null-if-zero is a requirement the engine lifts for a zero length.
  EXPECT_TRUE(memcpySummary->requiresParam(0));
  ASSERT_TRUE(memcpySummary->requiresExtent.contains(0));
  EXPECT_EQ(memcpySummary->requiresExtent.at(0).begin()->need,
            core::PathAffine::ofPath(SummaryPath::param(2)));

  const auto strtolSummary = libraryOf(parsed.fn("strtol"));
  ASSERT_TRUE(strtolSummary);
  EXPECT_TRUE(strtolSummary->returns.empty());
  ASSERT_EQ(strtolSummary->stores.size(), 1U);
  EXPECT_EQ(strtolSummary->stores.begin()->dest, SummaryPath::param(1).deref());
  auto endPointer = ValueSource::interiorCopy(SummaryPath::param(0));
  endPointer.when.require(SummaryPath::param(1),
                          core::ValueFact::of(core::Outcome::NonNull));
  EXPECT_EQ(strtolSummary->stores.begin()->value, endPointer);
  EXPECT_TRUE(strtolSummary->requiresParam(0));
  EXPECT_FALSE(strtolSummary->requiresParam(1)) << "`endptr` may be null";

  const auto strlenSummary = libraryOf(parsed.fn("strlen"));
  ASSERT_TRUE(strlenSummary);
  EXPECT_EQ(strlenSummary->borrowKind(0), core::BorrowKind::Shared);
  EXPECT_TRUE(strlenSummary->returns.empty());

  const auto bsearchSummary = libraryOf(parsed.fn("bsearch"));
  ASSERT_TRUE(bsearchSummary);
  EXPECT_EQ(
      bsearchSummary->returns,
      (std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(1)),
                             ValueSource::null()}));

  // Static storage is a value the summary cannot name (the engine gives it
  // its hidden state slot at the call).
  const auto getenvSummary = libraryOf(parsed.fn("getenv"));
  ASSERT_TRUE(getenvSummary);
  EXPECT_EQ(
      getenvSummary->returns,
      (std::set<ValueSource>{ValueSource::unknown(), ValueSource::null()}));
  EXPECT_FALSE(getenvSummary->returnsFresh());

  EXPECT_FALSE(libraryOf(parsed.fn("strdup")))
      << "the program's own strdup is not the row's";
}

TEST(LibrarySummaries, PosixEntries) {
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

  // `getline` reallocates the buffer it is given: the old one is consumed
  // and replaced by the fresh one it stores.
  const auto getlineSummary = libraryOf(parsed.fn("getline"));
  ASSERT_TRUE(getlineSummary);
  EXPECT_EQ(getlineSummary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(getlineSummary->borrowKind(2), core::BorrowKind::Mutable);
  ASSERT_EQ(getlineSummary->stores.size(), 1U);
  EXPECT_EQ(getlineSummary->stores.begin()->dest,
            SummaryPath::param(0).deref());
  EXPECT_TRUE(getlineSummary->stores.begin()->value.isFresh());
  EXPECT_EQ(getlineSummary->stores.begin()->value.family, "free");
  const core::PlaceEffect replaced =
      getlineSummary->effectOf(SummaryPath::param(0).deref());
  EXPECT_TRUE(replaced.moved && replaced.replaced);

  // `null-on-failure` out values are stored on the success class only.
  const auto asprintfSummary = libraryOf(parsed.fn("asprintf"));
  ASSERT_TRUE(asprintfSummary);
  ASSERT_EQ(asprintfSummary->stores.size(), 1U);
  EXPECT_EQ(asprintfSummary->stores.begin()->dest,
            SummaryPath::param(0).deref());
  EXPECT_TRUE(asprintfSummary->stores.begin()->value.isFresh());
  EXPECT_TRUE(asprintfSummary->storesOn.at(core::Outcome::Zero)
                  .contains(SummaryPath::param(0).deref()));
  EXPECT_TRUE(asprintfSummary->nullOn.at(core::Outcome::Negative)
                  .contains(SummaryPath::param(0).deref()));
  const auto memalign = libraryOf(parsed.fn("posix_memalign"));
  ASSERT_TRUE(memalign);
  ASSERT_EQ(memalign->stores.size(), 1U);
  EXPECT_EQ(memalign->stores.begin()->value.extent,
            core::PathAffine::ofPath(SummaryPath::param(2)));

  EXPECT_EQ(libraryOf(parsed.fn("popen"))->freshReturnFamily(), "pclose");
  EXPECT_TRUE(libraryOf(parsed.fn("pclose"))->frees(0));
  EXPECT_EQ(libraryOf(parsed.fn("opendir"))->freshReturnFamily(), "closedir");
  EXPECT_TRUE(libraryOf(parsed.fn("closedir"))->frees(0));
  EXPECT_EQ(
      libraryOf(parsed.fn("readdir"))->returns,
      (std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(0)),
                             ValueSource::null()}))
      << "the entry lives in the stream; null at the end";
  EXPECT_EQ(libraryOf(parsed.fn("mmap"))->freshReturnFamily(), "munmap");
  EXPECT_TRUE(libraryOf(parsed.fn("munmap"))->frees(0));

  const auto gaiSummary = libraryOf(parsed.fn("getaddrinfo"));
  ASSERT_TRUE(gaiSummary);
  EXPECT_EQ(gaiSummary->borrowKind(2), core::BorrowKind::Shared);
  EXPECT_EQ(gaiSummary->stores.begin()->dest, SummaryPath::param(3).deref());
  EXPECT_EQ(gaiSummary->stores.begin()->value.family, "freeaddrinfo");
  EXPECT_TRUE(libraryOf(parsed.fn("freeaddrinfo"))->frees(0));

  // RFC 0006, *Alias exactness*: a pointer *into* the argument is an
  // interior copy; the argument itself is an exact one.
  const auto strtokSummary = libraryOf(parsed.fn("strtok_r"));
  ASSERT_TRUE(strtokSummary);
  EXPECT_EQ(
      strtokSummary->returns,
      (std::set<ValueSource>{ValueSource::interiorCopy(SummaryPath::param(0)),
                             ValueSource::null()}));
  EXPECT_EQ(strtokSummary->stores.begin()->dest, SummaryPath::param(2).deref());
  EXPECT_FALSE(strtokSummary->requiresParam(0))
      << "a null first argument continues the previous string";

  // Returns its buffer argument, or null on failure (RFC 0008, table
  // nullability).
  EXPECT_EQ(libraryOf(parsed.fn("localtime_r"))->returns,
            (std::set<ValueSource>{ValueSource::copy(SummaryPath::param(1)),
                                   ValueSource::null()}));
  EXPECT_FALSE(libraryOf(parsed.fn("localtime"))->returnsFresh())
      << "static storage";

  // RFC 0006, *Outcome-conditional summaries*: `reallocarray` consumes its
  // argument when it returns non-null, and (§8.2) on the null class only
  // when the size `a1 * a2` is zero.
  const auto reallocarraySummary = libraryOf(parsed.fn("reallocarray"));
  ASSERT_TRUE(reallocarraySummary);
  EXPECT_TRUE(reallocarraySummary->consumes(0));
  EXPECT_FALSE(
      reallocarraySummary->consumesUnconditionally(SummaryPath::param(0)));
  EXPECT_TRUE(reallocarraySummary->outcomes.at(core::Outcome::NonNull)
                  .at(SummaryPath::param(0))
                  .moved);
  const core::PlaceEffect nullClass =
      reallocarraySummary->outcomes.at(core::Outcome::Null)
          .at(SummaryPath::param(0));
  EXPECT_TRUE(nullClass.freed);
  EXPECT_EQ(nullClass.when.integers.size(), 1U) << "a1 * a2 == 0";
  EXPECT_TRUE(reallocarraySummary->returns.contains(ValueSource::null()));

  const auto createSummary = libraryOf(parsed.fn("pthread_create"));
  ASSERT_TRUE(createSummary);
  EXPECT_EQ(createSummary->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_FALSE(createSummary->borrowKind(3).has_value());
  EXPECT_TRUE(createSummary->effectOf(SummaryPath::param(3)).escaped)
      << "the thread keeps its argument";

  EXPECT_EQ(libraryOf(parsed.fn("read"))->borrowKind(1),
            core::BorrowKind::Mutable);
  EXPECT_EQ(libraryOf(parsed.fn("write"))->borrowKind(1),
            core::BorrowKind::Shared);
}

// RFC 0030 §8: a fortified alias has its row's effects on the arguments its
// `chk` clause maps (the flag and size sit before a `printf` format).
TEST(LibrarySummaries, FortifiedAliasesRemapArguments) {
  const auto parsed = parse(R"c(
    typedef __builtin_va_list va_list;
    void format_all(char *d, unsigned long n, va_list ap) {
      __builtin___sprintf_chk(d, 0, n, "%d", 1);
      __builtin___snprintf_chk(d, n, 0, n, "%d", 1);
      __builtin___vsnprintf_chk(d, n, 0, n, "%d", ap);
      __builtin___memcpy_chk(d, d + 1, n, n);
    }
  )c");
  ASSERT_TRUE(parsed.ast);

  const auto sprintfChk = libraryOf(parsed.fn("__builtin___sprintf_chk"));
  ASSERT_TRUE(sprintfChk);
  EXPECT_TRUE(sprintfChk->requiresParam(0)) << "sprintf writes through it";
  EXPECT_TRUE(sprintfChk->requiresParam(3)) << "the format, shifted";
  EXPECT_FALSE(sprintfChk->requiresParam(1)) << "the flag is dropped";

  // `snprintf(NULL, 0, ...)` measures the output: null-if-zero (§8.3).
  const auto snprintfChk = libraryOf(parsed.fn("__builtin___snprintf_chk"));
  ASSERT_TRUE(snprintfChk);
  EXPECT_TRUE(snprintfChk->requiresParam(0));
  EXPECT_TRUE(snprintfChk->requiresParam(4));
  EXPECT_EQ(snprintfChk->requiresExtent.at(0).begin()->need,
            core::PathAffine::ofPath(SummaryPath::param(1)));

  const auto vsnprintfChk = libraryOf(parsed.fn("__builtin___vsnprintf_chk"));
  ASSERT_TRUE(vsnprintfChk);
  EXPECT_TRUE(vsnprintfChk->requiresParam(4));

  const auto memcpyChk = libraryOf(parsed.fn("__builtin___memcpy_chk"));
  ASSERT_TRUE(memcpyChk);
  EXPECT_EQ(memcpyChk->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(memcpyChk->borrowKind(1), core::BorrowKind::Shared);
  EXPECT_FALSE(memcpyChk->borrowKind(3).has_value()) << "the object size";
}

TEST(Summaries, RecursiveApproximationsRetainEarlierGuardedEffects) {
  const auto parsed = parse("void recursive(void *p, int depth, int kind);");
  ASSERT_TRUE(parsed.ast);
  const auto *function = parsed.fn("recursive");
  ASSERT_TRUE(function);
  SummaryStore store;
  core::PathGuard depth;
  depth.require(SummaryPath::param(1), core::ValueFact::nonZero());
  auto narrower = depth;
  narrower.require(SummaryPath::param(2), core::ValueFact::ofConstant(1));
  const auto make = [](const core::PathGuard &guard) {
    core::FunctionSummary summary;
    const PlaceEffect effect{.freed = true, .when = guard};
    summary.addEffect(SummaryPath::param(0), effect);
    summary.outcomes[core::Outcome::Zero][SummaryPath::param(0)] = effect;
    return summary;
  };
  EXPECT_TRUE(store.setInferred(*function, make(narrower), true));
  EXPECT_TRUE(store.setInferred(*function, make(depth), true));
  for (unsigned round = 0; round < 20; ++round)
    EXPECT_FALSE(
        store.setInferred(*function, make(round % 2 ? depth : narrower), true));
  const auto *summary = store.inferredFor(*function);
  ASSERT_TRUE(summary);
  EXPECT_EQ(*summary, make(depth));
  // A fresh component can still reset to bottom before iteration starts.
  EXPECT_TRUE(store.setInferred(*function, {}));
  EXPECT_TRUE(store.inferredFor(*function)->empty());
}

} // namespace
} // namespace weavec::analysis
