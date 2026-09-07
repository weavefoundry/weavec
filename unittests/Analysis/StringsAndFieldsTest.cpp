//===- StringsAndFieldsTest.cpp - Tests for RFC 0012 ----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// String facts, sized fields, offset relations, lower bounds and
// `WEAVEC_ASSUME`: the rules of RFC 0012 (*Spatial safety II*), checked
// against the diagnostics the checker produces on small programs and the
// sized-field facts a unit exports.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using weavec::test::analyze;
using weavec::test::analyzeInProgram;
using weavec::test::ids;
using weavec::test::messages;
using weavec::test::notes;

using Strings = std::vector<std::string>;

constexpr const char *Types = R"c(
#define SIZED_BY(n) __attribute__((annotate("weavec.sized_by." #n)))
#define ASSUME(e) weavec_assume_((e) != 0)
__attribute__((annotate("weavec.assume"))) static inline void weavec_assume_(int c) { (void)c; }
size_t strlen(const char *);
char *strcpy(char *, const char *);
char *stpcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strcat(char *, const char *);
char *strdup(const char *);
char *strchr(const char *, int);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int puts(const char *);
int printf(const char *, ...);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
char *fgets(char *, int, void *);
struct buf { char *SIZED_BY(cap) data; size_t cap; };
struct vec { int *items; size_t n; size_t cap; };
#line 0
)c";

// -- String facts (RFC 0012, *String facts*, *Sources*, *String checks*) -----

// A literal's length is known; `strcpy`, `strcat` and `sprintf` need the
// length plus the terminator, and are checked against a declared extent.
TEST(Strings, LiteralsAgainstDeclaredExtents) {
  const auto result = analyze(std::string(Types) + R"c(
    void copies(void) {
      char buf[4];
      strcpy(buf, "hello");
      strcpy(buf, "abc");
      strcat(buf, "d");
      sprintf(buf, "%s!", "abc");
      sprintf(buf, "%d", 7);
      sprintf(buf, "%d!!!", 7);
    }
    void fits(void) {
      char buf[8] = "abc";
      strcat(buf, "defg");
      strcat(buf, "h");
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"3: 'strcpy' accesses 6 bytes of 'buf', which has 4 bytes",
                     "5: 'strcat' accesses 5 bytes of 'buf', which has 4 bytes",
                     "6: 'sprintf' accesses 5 bytes of 'buf', which has 4 "
                     "bytes",
                     "8: 'sprintf' accesses at least 5 bytes of 'buf', which "
                     "has 4 bytes",
                     "13: 'strcat' accesses 9 bytes of 'buf', which has 8 "
                     "bytes"}));
  EXPECT_EQ(notes(result.diagnostics), Strings{"'buf' is declared here"});
}

// `strlen(s)` is a length place: an allocation of `strlen(s)` bytes is one
// short of what `strcpy` needs, whether the length is read in the
// allocation or through a variable (RFC 0012, *Length places*).
TEST(Strings, LengthPlacesRelateAllocationsAndCopies) {
  const auto result = analyze(std::string(Types) + R"c(
    void short_by_one(const char *s) {
      char *d = malloc(strlen(s));
      if (!d) return;
      strcpy(d, s);
      free(d);
    }
    void through_a_variable(const char *s) {
      size_t n = strlen(s);
      char *d = malloc(n);
      if (!d) return;
      strcpy(d, s);
      free(d);
    }
    void exact(const char *s) {
      char *d = malloc(strlen(s) + 1);
      if (!d) return;
      strcpy(d, s);
      d[strlen(s)] = 0;
      d[strlen(s) + 1] = 0;
      free(d);
    }
    void appended(const char *s) {
      char *d = malloc(strlen(s) + 4);
      if (!d) return;
      strcpy(d, s);
      strcat(d, "ab");
      strcat(d, "c");
      free(d);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'strcpy' accesses 'strlen(s)' + 1 bytes of 'd', "
                     "which has 'strlen(s)' bytes",
                     "11: 'strcpy' accesses 'strlen(s)' + 1 bytes of 'd', "
                     "which has 'n' bytes ('strlen(s)' equals 'n')",
                     "19: 'd[strlen(s) + 1]' is out of bounds: it reaches "
                     "'strlen(s)' + 2 bytes into 'd', which has 'strlen(s)' + "
                     "1 bytes"}));
}

// `strdup` gives its result the source's length and one more byte of
// extent; a write past that is out of bounds.
TEST(Strings, StrdupCarriesTheLength) {
  const auto result = analyze(std::string(Types) + R"c(
    void dup(const char *s) {
      char *d = strdup(s);
      if (!d) return;
      d[strlen(s)] = 0;
      d[strlen(s) + 1] = 0;
      strcat(d, "x");
      free(d);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: 'd[strlen(s) + 1]' is out of bounds: it reaches "
                     "'strlen(s)' + 2 bytes into 'd', which has 'strlen(s)' + "
                     "1 bytes",
                     "6: 'strcat' accesses 'strlen(s)' + 2 bytes of 'd', which "
                     "has 'strlen(s)' + 1 bytes"}));
}

// An object with no terminator anywhere in it: `strncpy` that fills the
// buffer, an initialiser as long as the array, `memset` with a non-zero
// byte. A terminator-seeking read of it is reported; a NUL store, a
// zeroing `memset` or a shorter copy make it a string again.
TEST(Strings, UnterminatedObjectsAndTheirReaders) {
  const auto result = analyze(std::string(Types) + R"c(
    void filled(void) {
      char name[8];
      strncpy(name, "0123456789", sizeof name);
      size_t n = strlen(name);
      puts(name);
      printf("%d %s", 1, name);
      (void)n;
    }
    void repaired(void) {
      char name[8];
      strncpy(name, "0123456789", sizeof name);
      name[sizeof name - 1] = 0;
      puts(name);
      char other[8];
      strncpy(other, "0123456789", sizeof other - 1);
      other[7] = 0;
      puts(other);
      char fits[8];
      strncpy(fits, "abc", sizeof fits);
      puts(fits);
    }
    void initialised(void) {
      char a[4] = "abcd";
      char b[3] = {'a', 'b', 'c'};
      char c[4] = "abc";
      puts(a);
      puts(b);
      puts(c);
    }
    void filled_bytes(void) {
      char a[4];
      char b[4];
      memset(a, 'x', sizeof a);
      memset(b, 0, sizeof b);
      puts(a);
      puts(b);
    }
    void unknown_copy(char *src) {
      char name[8];
      strncpy(name, src, sizeof name);
      puts(name);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'strlen' reads past the end of 'name', which is not "
                     "NUL-terminated",
                     "5: 'puts' reads past the end of 'name', which is not "
                     "NUL-terminated",
                     "6: 'printf' reads past the end of 'name', which is not "
                     "NUL-terminated",
                     "26: 'puts' reads past the end of 'a', which is not "
                     "NUL-terminated",
                     "27: 'puts' reads past the end of 'b', which is not "
                     "NUL-terminated",
                     "35: 'puts' reads past the end of 'a', which is not "
                     "NUL-terminated"}));
  EXPECT_EQ(notes(result.diagnostics),
            Strings{"'name' is left without a terminator here"});
}

// The facts follow the object, not the name: a pointer to the array and the
// array itself share them, and a copy of the pointer at an offset sees the
// length from there.
TEST(Strings, FactsAreOnTheObject) {
  const auto result = analyze(std::string(Types) + R"c(
    void aliased(void) {
      char buf[4];
      char *p = buf;
      strcpy(p, "hello");
      strcpy(buf, "abc");
      strcat(p, "d");
    }
    void stepped(void) {
      char buf[8] = "abcdef";
      char *p = buf + 2;
      strcat(p, "gh");
      strcat(p, "i");
    }
    void through_fgets(void *f) {
      char buf[8] = "abcdefg";
      fgets(buf, sizeof buf, f);
      strcat(buf, "x");
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'strcpy' accesses 6 bytes of 'p', which has 4 bytes",
                     "6: 'strcat' accesses 5 bytes of 'p', which has 4 bytes",
                     "11: 'strcat' accesses 9 bytes of 'p', which has 8 "
                     "bytes"}));
}

// -- Sized fields (RFC 0012, *Sized fields*) ----------------------------------

// An annotated field is loaded with the extent its count says; accesses
// through it are checked as any counted allocation is.
TEST(SizedFields, AnnotatedLoads) {
  const auto result = analyze(std::string(Types) + R"c(
    void put(struct buf *b) { b->data[b->cap] = 0; }
    void put_ok(struct buf *b) { if (b->cap > 0) b->data[b->cap - 1] = 0; }
    void fill(struct buf *b) {
      for (size_t i = 0; i <= b->cap; i++) b->data[i] = 0;
    }
    void fill_ok(struct buf *b) {
      for (size_t i = 0; i < b->cap; i++) b->data[i] = 0;
    }
    void copy_lit(struct buf *b) {
      if (b->cap == 4) strcpy(b->data, "hello");
    }
    void copy_unknown(struct buf *b, const char *s) { strcpy(b->data, s); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"1: 'b->data[b->cap]' is out of bounds: 'b->cap' is the "
                     "number of elements of 'b->data'",
                     "4: 'b->data[i]' may be out of bounds: 'i' may equal "
                     "'b->cap', the number of elements of 'b->data'",
                     "10: 'strcpy' accesses 6 bytes of 'b->data', which has 4 "
                     "bytes"}));
  EXPECT_EQ(notes(result.diagnostics), Strings{"'b->data' is declared here"});
}

// A store into an annotated field is checked against the count, whichever
// of the two is written first; a store of the right size, or of an unknown
// one, is not reported.
TEST(SizedFields, AnnotatedStores) {
  const auto result = analyze(std::string(Types) + R"c(
    void shrink(struct buf *b) { b->data = malloc(4); b->cap = 8; }
    void shrink2(struct buf *b) { b->cap = 8; b->data = malloc(4); }
    void grow(struct buf *b, size_t n) { b->data = malloc(n); b->cap = n; }
    void larger(struct buf *b) { b->data = malloc(16); b->cap = 8; }
    void unknown(struct buf *b, char *p) { b->data = p; b->cap = 8; }
    void null(struct buf *b) { b->data = NULL; b->cap = 8; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"1: 'b->data' is declared WEAVEC_SIZED_BY(cap) but is "
                     "given 4 bytes where 'b->cap' says 8",
                     "2: 'b->data' is declared WEAVEC_SIZED_BY(cap) but is "
                     "given 4 bytes where 'b->cap' says 8"}));
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"annotation-mismatch", "annotation-mismatch"}));
  EXPECT_EQ(notes(result.diagnostics), Strings{"'b->data' is declared here"});
}

// A malformed field annotation is reported once, at the field.
TEST(SizedFields, MalformedAnnotationsAreReported) {
  const auto result = analyze(std::string(Types) + R"c(
    struct bad { int *SIZED_BY(nope) p; int q; char *SIZED_BY(q) s; int SIZED_BY(q) k; };
    int one(struct bad *b) { return b->p[0]; }
    int two(struct bad *b) { return b->p[1] + b->s[0]; }
    void three(struct bad *b) { b->k = 1; b->k = 2; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"1: field 'p' is declared WEAVEC_SIZED_BY(nope) but "
                     "'nope' is not an integer field of 'struct bad'",
                     "1: field 'k' is declared WEAVEC_SIZED_BY(q) but is not a "
                     "pointer"}));
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"invalid-annotation", "invalid-annotation"}));
}

// Inference within a unit: the function that fills the pair witnesses it,
// nothing refutes it, and the reader is analysed once more with the count in
// force (RFC 0012, *Two passes in a unit*). The exports carry the facts.
TEST(SizedFields, InferredWithinAUnit) {
  const auto result = analyze(std::string(Types) + R"c(
    int last(struct vec *v) { return v->items[v->cap]; }
    int last_n(struct vec *v) { return v->items[v->n]; }
    void init(struct vec *v, size_t n) {
      v->items = malloc(n * sizeof *v->items);
      v->cap = n;
      v->n = 0;
    }
    void push(struct vec *v, int x) { if (v->n < v->cap) v->items[v->n++] = x; }
    void reserve(struct vec *v, size_t c) {
      int *p = realloc(v->items, c * sizeof *p);
      if (!p) return;
      v->items = p;
      v->cap = c;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"1: 'v->items[v->cap]' is out of bounds: the access "
                     "exceeds the allocation's converted size"}));
  EXPECT_EQ(notes(result.diagnostics), Strings{"'v->items' is declared here"});
  const SizedFieldFacts &facts = result.analyzer->exports().sizedFields;
  EXPECT_EQ(facts.witnesses,
            (std::set<SizedFieldWitness>{SizedFieldWitness{
                .field = "struct vec.items",
                .count = "struct vec.cap",
                .scale = 4,
                .productType = core::IntegerType{64, false}}}));
  EXPECT_TRUE(facts.unsizedFields.empty());
  EXPECT_EQ(facts.unsizedPairs,
            (std::set<UnsizedPair>{UnsizedPair{.field = "struct vec.items",
                                               .count = "struct vec.n"}}));
  EXPECT_EQ(facts.confirmed("struct vec.items"),
            (std::optional(
                std::pair<std::string, std::int64_t>("struct vec.cap", 4))));
}

// A store whose extent no sibling counts refutes the field; a count written
// without the pointer refutes the pair; a null store says nothing.
TEST(SizedFields, Refutations) {
  const auto result = analyze(std::string(Types) + R"c(
    void init(struct vec *v, size_t n) {
      v->items = malloc(n * sizeof *v->items);
      v->cap = n;
    }
    void steal(struct vec *v, int *p) { v->items = p; }
    void shrink(struct vec *v) { v->cap = v->n; }
    void clear(struct vec *v) { v->items = NULL; v->cap = 0; }
    int last(struct vec *v) { return v->items[v->cap]; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{})
      << "refuted: no extent is synthesised";
  const SizedFieldFacts &facts = result.analyzer->exports().sizedFields;
  EXPECT_EQ(facts.unsizedFields, (std::set<std::string>{"struct vec.items"}));
  EXPECT_EQ(facts.unsizedPairs,
            (std::set<UnsizedPair>{UnsizedPair{.field = "struct vec.items",
                                               .count = "struct vec.cap"}}));
  EXPECT_FALSE(facts.confirmed("struct vec.items"));
}

// Inference across units: the database's witnesses apply in the first pass.
TEST(SizedFields, InferredThroughTheDatabase) {
  UnitExports other;
  other.source = "vec.c";
  other.sizedFields.witnesses.insert(SizedFieldWitness{
      .field = "struct vec.items", .count = "struct vec.cap", .scale = 4});
  ProgramDatabase database;
  database.add(other);
  const auto result = analyzeInProgram(std::string(Types) + R"c(
    int last(struct vec *v) { return v->items[v->cap]; }
    void copy(struct vec *dst, struct vec *src) {
      dst->items = src->items;
      dst->cap = src->cap;
    }
  )c",
                                       &database);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"1: 'v->items[v->cap]' is out of bounds: 'v->cap' is the "
                     "number of elements of 'v->items'"}));
  // The copy loaded the field with the count's extent and stored it beside
  // an equal count: a witness of its own.
  const SizedFieldFacts &facts = result.analyzer->exports().sizedFields;
  EXPECT_TRUE(facts.witnesses.contains(SizedFieldWitness{
      .field = "struct vec.items", .count = "struct vec.cap", .scale = 4}));
  EXPECT_TRUE(facts.unsizedFields.empty());

  // A refutation elsewhere in the program disables the inference.
  UnitExports refuting;
  refuting.source = "steal.c";
  refuting.sizedFields.unsizedFields.insert("struct vec.items");
  database.add(refuting);
  const auto refuted = analyzeInProgram(std::string(Types) + R"c(
    int last(struct vec *v) { return v->items[v->cap]; }
  )c",
                                        &database);
  ASSERT_TRUE(refuted.ast);
  EXPECT_EQ(messages(refuted.diagnostics), Strings{});
}

// -- Relations (RFC 0012, *Offset relations and lower bounds*) ----------------

// `i <= n - 1` is `i < n` for `a[i]` and "may reach" for `a[i + 1]`; a copy
// `j = i + 1` carries the offset; a lower bound decides a constant extent.
TEST(Relations, OffsetsAndLowerBounds) {
  const auto result = analyze(std::string(Types) + R"c(
    void offsets(size_t n, size_t i) { if (!n || n > (size_t)-1 / 4 || i == (size_t)-1) return;
      int *a = malloc(n * sizeof *a);
      if (!a) return;
      if (i <= n - 1) a[i] = 0;
      if (i <= n - 1) a[i + 1] = 0;
      if (i < n - 1) a[i + 1] = 0;
      size_t j = i + 1;
      if (i < n) a[j] = 0;
      if (j < n) a[j] = 0;
      free(a);
    }
    void lower(size_t i) {
      char buf[8];
      if (i >= 8) buf[i] = 0;
      if (i > 7) buf[i] = 0;
      if (i >= 7) buf[i] = 0;
      if (i < 8) buf[i] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"5: 'a[i + 1]' may be out of bounds: 'i' may reach one below "
               "'n', and 'a' has 'n' * 4 bytes",
               "8: 'a[j]' may be out of bounds: 'j' may equal 'n', the number "
               "of elements of 'a'",
               "14: 'buf[i]' is out of bounds: 'i' is at least 8 in an object "
               "of 8 bytes",
               "15: 'buf[i]' is out of bounds: 'i' is at least 8 in an object "
               "of 8 bytes"}));
}

// -- WEAVEC_ASSUME (RFC 0012, *`WEAVEC_ASSUME`*)
// -------------------------------

// The argument holds from the call on, as on the true edge of `if`; an
// assumption the facts contradict ends the path; the annotation on any other
// function is invalid.
TEST(Assume, StatesAFact) {
  const auto result = analyze(std::string(Types) + R"c(
    struct pair { char *p; size_t len; size_t cap; };
    void put(struct pair *b, char c) {
      ASSUME(b->len < b->cap);
      char *d = malloc(b->cap);
      if (!d) return;
      d[b->len] = c;
      d[b->cap] = c;
      free(d);
    }
    void nonnull(int *p) {
      ASSUME(p != NULL);
      *p = 1;
    }
    void contradiction(int *p) {
      if (p) return;
      ASSUME(p != NULL);
      *p = 1;
    }
    void without(int *p) {
      if (p) return;
      *p = 1;
    }
    __attribute__((annotate("weavec.assume"))) void mine(int c) { (void)c; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: 'd[b->cap]' is out of bounds: 'b->cap' is the number "
                     "of elements of 'd'",
                     "21: dereference of 'p', which is null",
                     "23: 'weavec.assume' is not an annotation for 'mine'"}));
}

} // namespace
} // namespace weavec::analysis
