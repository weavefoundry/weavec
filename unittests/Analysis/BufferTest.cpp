//===- BufferTest.cpp - Relational buffer boundaries (RFC 0026) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {

TEST(BufferAnalysis, HelperCursorDifferencesSurviveMixedLoopPaths) {
  for (const auto *contents : {"0", "1,2,3,4,5,6,7,8,9,10"}) {
    SCOPED_TRACE(contents);
    const std::string code = R"c(
      unsigned decode(const unsigned char *p,const unsigned char *end,
                      unsigned char **out) {
        if(end-p<2)return 0; (*out)[0]=p[1]; (*out)++; return 2;
      }
      int client(void) {
        const unsigned char bytes[10]={)c" +
                             std::string(contents) + R"c(};
        unsigned char dst[10];
        const unsigned char *p=bytes,*end=bytes+10; unsigned char *out=dst;
        while(p<end) {
          if(*p!='\\') {*out++=*p++;}
          else {unsigned n=decode(p,end,&out);if(!n)break;p+=n;}
        }
        return 0;
      }
      int changed_zero(void) {
        unsigned char bytes[2]={0}; bytes[0]=1;
        unsigned char *p=bytes;
        if(*p) p[2]=0;
        return 0;
      }
    )c";
    const auto result =
        test::analyze(code, {.checkContracts = true, .checked = true});
    ASSERT_TRUE(result.ast);
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_TRUE(result.summary("client")->checked.complete());
    EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
    ASSERT_NE(result.summary("changed_zero"), nullptr);
    EXPECT_FALSE(result.summary("changed_zero")->checked.complete());
  }
}

TEST(BufferAnalysis, IndependentCursorCountsPreserveOnlyProvedRelations) {
  const auto result = test::analyze(R"c(
    void copy_bytes(const unsigned char *src, unsigned char *dst, size_t n) {
      const unsigned char *p=src; unsigned char *q=dst;
      while(p<src+n) {*q++=*p++;}
    }
    int good(void) {
      const unsigned char src[10]={0}; unsigned char dst[10];
      copy_bytes(src,dst,10); return 0;
    }
    int short_output(void) {
      const unsigned char src[10]={0}; unsigned char dst[9];
      copy_bytes(src,dst,10); return 0;
    }
    int uninitialized_input(void) {
      unsigned char src[10], dst[10]; src[0]=0;
      copy_bytes(src,dst,10); return 0;
    }
    int changed_stride(void) {
      const unsigned char src[10]={0}; unsigned char dst[10];
      const unsigned char *p=src; unsigned char *q=dst;
      while(p<src+10) {*q++=*p++;q++;} return 0;
    }
    int unrelated_order(void) {
      const unsigned char src[10]={0}; unsigned char dst[10];
      const unsigned char *p=src; unsigned char *q=dst;
      while(p<src+10) {*q++=*p++;} return p<q;
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"copy_bytes", "good"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_TRUE(result.summary(name)->checked.complete()) << name;
  }
  EXPECT_TRUE(result.summary("good")->checked.requirements.empty());
  for (const auto *name : {"short_output", "uninitialized_input",
                           "changed_stride", "unrelated_order"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_FALSE(result.summary(name)->checked.complete()) << name;
  }
}

TEST(BufferAnalysis, FixedSpanStepsRetainTheirActualRemainingLengthBound) {
  const auto result = test::analyze(R"c(
    unsigned consume(const unsigned char *p,const unsigned char *end) {
      if(end-p<2)return 0; (void)p[0]; return 2;
    }
    int client(unsigned length) {
      if(length>10)return 0;
      const unsigned char src[10]={0};
      const unsigned char *p=src,*end=src+length;
      while(p<end) {unsigned n=consume(p,end);if(!n)break;p+=n;}
      return 0;
    }
    int converted_length(int input) {
      unsigned length=(unsigned)input;
      if(length>10)return 0;
      const unsigned char src[10]={0};
      const unsigned char *p=src,*end=src+length;
      while(p<end) {unsigned n=consume(p,end);if(!n)break;p+=n;}
      return 0;
    }
    int too_far(void) {
      const unsigned char src[10]={0};
      const unsigned char *p=src,*end=src+10;
      while(p<end) {unsigned n=consume(p,end);if(!n)break;p+=n+1;}
      return 0;
    }
    int wrapped_remaining(void) {
      unsigned char data[10]; unsigned position=11;
      if(1U<=10U-position) data[position]=0;
      return 0;
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"consume", "client", "converted_length"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_TRUE(result.summary(name)->checked.complete()) << name;
  }
  EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
  ASSERT_NE(result.summary("too_far"), nullptr);
  EXPECT_FALSE(result.summary("too_far")->checked.complete());
  ASSERT_NE(result.summary("wrapped_remaining"), nullptr);
  EXPECT_FALSE(result.summary("wrapped_remaining")->checked.complete());
  EXPECT_TRUE(std::ranges::any_of(
      result.summary("consume")->checked.establishes, [](const auto &post) {
        return post.kind == core::CheckedRequirementKind::CountWithinSpan;
      }));
}

TEST(BufferAnalysis, AdvertisedCapacityDoesNotReplaceActualInputExtent) {
  for (const unsigned extent : {2U, 4U}) {
    const std::string code = R"c(
      void *memcpy(void *, const void *, size_t);
      struct B {char *data; size_t used, capacity;};
      static void copy(struct B *b, char *out) {
        if(b->used>b->capacity)return;
        if(b->data[b->used])memcpy(out,b->data,b->used+1);
      }
      int client(void) {
        char in[)c" + std::to_string(extent) +
                             R"c(]={'a','b'},out[4]={0};
        struct B b={in,2,2};copy(&b,out);return 0;
      }
    )c";
    const auto result =
        test::analyze(code, {.checkContracts = true, .checked = true});
    ASSERT_TRUE(result.ast);
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_EQ(result.summary("client")->checked.complete(), extent == 4);
    EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
  }
}

TEST(BufferAnalysis, PointerOffsetGuardsPreserveOnlyRepresentableDifferences) {
  const auto result = test::analyze(R"c(
    struct reader {
      const unsigned char *data; size_t capacity, position; unsigned depth;
    };
    int scan(struct reader *r) {
      if (!r || !r->data || r->position>=r->capacity) return 0;
      const unsigned char *cursor=r->data+r->position;
      while ((size_t)(cursor-r->data)<r->capacity) {
        if (*cursor=='x') return 1;
        ++cursor;
      }
      return 0;
    }
    int narrow(struct reader *r) {
      if (!r || !r->data || r->position>=r->capacity) return 0;
      const unsigned char *cursor=r->data+r->position;
      while ((unsigned char)(cursor-r->data)<r->capacity) {
        if (*cursor=='x') return 1;
        ++cursor;
      }
      return 0;
    }
    int client(void) {
      const unsigned char input[4]={'a','b','c','x'};
      struct reader r={input,4,0,0}; return scan(&r);
    }
    int lossy(void) {
      const unsigned char input[300]={0};
      struct reader r={input,300,0,0}; return narrow(&r);
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"scan", "client"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_TRUE(result.summary(name)->checked.complete()) << name;
  }
  EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
  EXPECT_TRUE(std::ranges::any_of(
      result.summary("scan")->checked.requirements,
      [](const auto &requirement) {
        return requirement.kind == core::CheckedRequirementKind::SumFits;
      }));
  ASSERT_NE(result.summary("lossy"), nullptr);
  EXPECT_FALSE(result.summary("lossy")->checked.complete());
}

TEST(BufferAnalysis, PairedReaderCountersRequireActualPreservedEquality) {
  for (const auto &increment : {"++count;", "count+=8;", "count=0;"}) {
    SCOPED_TRACE(increment);
    const std::string code = R"c(
      void *memcpy(void *, const void *, size_t);
      struct reader {
        const unsigned char *data; size_t capacity, position; unsigned depth;
      };
      unsigned char *copy_digits(struct reader *r) {
        size_t i=0, count=0;
        if (!r || !r->data) return 0;
        for (i=0; r->position+i<r->capacity; ++i) {
          switch ((r->data+r->position)[i]) {
          case '0': case '1':
    )c" + std::string(increment) +
                             R"c(
            break;
          default: goto done;
          }
        }
      done: ;
        unsigned char *out=malloc(count+1);
        if (!out) return 0;
        memcpy(out,r->data+r->position,count);
        out[count]=0;
        return out;
      }
      int client(void) {
        unsigned char input[5]={'x','0','1','0','x'};
        struct reader r={input,5,1,0};
        unsigned char *out=copy_digits(&r); if(out)free(out); return 0;
      }
    )c";
    AnalysisOptions options;
    options.checkedFunctions.insert("client");
    const auto result = test::analyze(code, options);
    ASSERT_TRUE(result.ast);
    ASSERT_NE(result.summary("client"), nullptr);
    // A reset count is safe but need not remain equal to the scan index.
    const bool safe = std::string_view(increment) != "count+=8;";
    EXPECT_EQ(result.summary("client")->checked.complete(), safe);
    if (safe)
      EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
  }
}

TEST(BufferAnalysis, StableReaderIndexLoopsProveOnlyStrictUnchangedBounds) {
  const auto result = test::analyze(R"c(
    struct reader {
      const unsigned char *data; size_t capacity, position; unsigned depth;
    };
    int scan(struct reader *r) {
      if (!r || !r->data) return 0;
      for (size_t i=0; r->position+i<r->capacity; ++i) {
        switch ((r->data+r->position)[i]) {
        case '0': case '1': break;
        default: goto done;
        }
      }
    done: return 1;
    }
    int changed_index(struct reader *r) {
      if (!r || !r->data) return 0;
      for (size_t i=0; r->position+i<r->capacity; ++i) {
        i=r->capacity;
        if ((r->data+r->position)[i]) return 1;
      }
      return 0;
    }
    int client(void) {
      const unsigned char input[4]={'x','0','1','x'};
      struct reader r={input,4,1,7}; return scan(&r);
    }
    int changed_client(void) {
      const unsigned char input[4]={'x','0','1','x'};
      struct reader r={input,4,1,7}; return changed_index(&r);
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"scan", "client"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_TRUE(result.summary(name)->checked.complete()) << name;
  }
  EXPECT_TRUE(result.summary("client")->checked.requirements.empty());
  ASSERT_NE(result.summary("changed_index"), nullptr);
  // RFC 0029 permits an additional interval beyond advertised capacity.
  // The generic helper can export that sufficient premise, but the actual
  // four-byte client must reject its access at position + capacity.
  const auto &changed = result.summary("changed_index")->checked;
  EXPECT_TRUE(changed.complete());
  EXPECT_TRUE(std::ranges::any_of(changed.requirements, [](const auto &pre) {
    return pre.kind == core::CheckedRequirementKind::Extent &&
           pre.path == core::SummaryPath::param(0).deref().field("data");
  }));
  ASSERT_NE(result.summary("changed_client"), nullptr);
  EXPECT_FALSE(result.summary("changed_client")->checked.complete());
}

TEST(BufferAnalysis, ExplicitByteCastsPreserveEvaluatedPointerOffsets) {
  const auto result = test::analyze(R"c(
    int memcmp(const void *, const void *, size_t);
    int good(void) {
      unsigned short data[3] = {0, 0, 0};
      return memcmp((const char *)(data + 1), "xx", 2);
    }
    int past_end(void) {
      unsigned short data[3] = {0, 0, 0};
      return memcmp((const char *)(data + 3), "xx", 2);
    }
    int before_start(void) {
      unsigned short data[3] = {0, 0, 0};
      return memcmp((const char *)(data - 1), "xx", 2);
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("good"), nullptr);
  EXPECT_TRUE(result.summary("good")->checked.complete());
  EXPECT_TRUE(result.summary("good")->checked.requirements.empty());
  for (const auto *name : {"past_end", "before_start"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_FALSE(result.summary(name)->checked.complete()) << name;
  }
}

TEST(BufferAnalysis, BoundedStringCursorRequiresInitializedTermination) {
  const auto result = test::analyze(R"c(
    size_t strlen(const char *);
    struct writer { unsigned char *data; size_t capacity, length; int flags; };
    void finish(struct writer *w) {
      if (!w || !w->data) return;
      const unsigned char *next = w->data + w->length;
      w->length += strlen((const char *)next);
    }
    void good(void) {
      unsigned char *p=malloc(4); if(!p)return;
      p[0]=0; p[1]=42; p[2]=0; p[3]=0;
      struct writer w={p,4,1,0}; finish(&w);
      if(w.length>=w.capacity) p[4]=1;
      else p[w.length]=0;
      free(p);
    }
    void uninitialized(void) {
      unsigned char *p=malloc(4); if(!p)return;
      p[0]=42; p[3]=0;
      struct writer w={p,4,1,0}; finish(&w); free(p);
    }
    void beyond_bound(void) {
      unsigned char *p=malloc(4); if(!p)return;
      p[0]=42; p[1]=42; p[2]=42; p[3]=0;
      struct writer w={p,3,1,0}; finish(&w); free(p);
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("good"), nullptr);
  EXPECT_TRUE(result.summary("good")->checked.complete());
  for (const auto *name : {"uninitialized", "beyond_bound"}) {
    ASSERT_NE(result.summary(name), nullptr);
    EXPECT_FALSE(result.summary(name)->checked.complete()) << name;
  }
}

static const std::string BufferPrelude = R"c(
  typedef __SIZE_TYPE__ size_t;
  void *malloc(size_t);
  void *realloc(void *, size_t);
  void free(void *);
  void *memcpy(void *, const void *, size_t);
  struct bytes { unsigned char *data; size_t length, capacity; };
  static int reserve(struct bytes *b, size_t n) {
    if(n <= b->capacity) return 0;
    unsigned char *p = realloc(b->data, n);
    if(!p) return -1;
    b->data=p; b->capacity=n; return 0;
  }
  static int append(struct bytes *b, unsigned char x) {
    if(b->length==b->capacity) {
      if(b->capacity>__SIZE_MAX__-8) return -1;
      if(reserve(b,b->capacity+8)) return -1;
    }
    b->data[b->length]=x; ++b->length; return 0;
  }
  static unsigned char last(struct bytes *b) {
    return b->length ? b->data[b->length-1] : 0;
  }
)c";

static core::CheckedContract bufferCheck(const std::string &body,
                                         const std::string &name = "client") {
  AnalysisOptions options;
  options.checkedFunctions.insert(name);
  const auto unit = test::analyze(BufferPrelude + body, options);
  if (!unit.ast || !unit.summary(name)) {
    ADD_FAILURE() << "buffer fixture could not be analyzed";
    return {};
  }
  return unit.summary(name)->checked;
}

TEST(BufferAnalysis, ZeroBytesEstablishConcreteIntegerCells) {
  const auto contract = bufferCheck(R"c(
void *memset(void *, int, size_t);
int client(void) {
  struct bytes rows[2]; int a[1]={7};
  memset(rows,0,sizeof rows);
  return a[rows[1].length];
}
)c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

TEST(BufferAnalysis, PartialOrReplacedZeroBytesSupplyNoStaleCounter) {
  for (const auto *change : {"rows[1].length=256; memset(&rows[1].length,0,1);",
                             "rows[1].length=1;", "change(&rows[1]);"}) {
    SCOPED_TRACE(change);
    const auto contract = bufferCheck(
        R"c(
void *memset(void *, int, size_t);
static void change(struct bytes *b) { b->length=1; }
int client(void) {
  struct bytes rows[2]; int a[1]={7};
  memset(rows,0,sizeof rows);
)c" + std::string(change) +
        "return a[rows[1].length]; }");
    EXPECT_FALSE(contract.complete());
  }
}

TEST(BufferAnalysis, HeaderWritesPreserveOnlySeparatedTerminatingBytes) {
  for (const bool overwrite : {false, true}) {
    SCOPED_TRACE(overwrite);
    const auto contract = bufferCheck(
        R"c(
size_t strlen(const char *);
static int emit(struct bytes *b) {
  if(b->length>b->capacity || b->capacity-b->length<3) return 0;
  unsigned char *p=b->data+b->length;
  p[0]='x'; p[1]='y'; p[2]=0; b->length+=2;
)c" + std::string(overwrite ? "p[2]='z';" : "") +
        R"c(
  return 1;
}
int client(void) {
  unsigned char output[3]; struct bytes b={output,0,3};
  if(!emit(&b)) return 0;
  return strlen((const char *)b.data)!=2;
}
)c");
    EXPECT_EQ(contract.complete(), !overwrite);
    EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(BufferAnalysis, HelperReturnsKeepActualCapacityAndWrittenPrefix) {
  for (const auto *reserveBody :
       {"return needed<=w->size ? w->data+w->used : 0;",
        "if(needed<=w->size) return w->data+w->used;"
        "unsigned char *p=realloc(w->data,needed);if(!p)return 0;"
        "w->data=p;w->size=needed;return p+w->used;"}) {
    SCOPED_TRACE(reserveBody);
    for (const auto *write : {"*out++ = '{';", "++out;", "out[1] = '{';"}) {
      SCOPED_TRACE(write);
      const auto contract = bufferCheck(R"c(
size_t strlen(const char *);
struct writer { unsigned char *data; size_t size, used, depth; int format; };
static unsigned char *next_slot(struct writer *w, size_t needed) {
  if(!w || !w->data) return 0;
  if(w->size>0 && w->used>=w->size) return 0;
  if(needed>2147483647u) return 0;
  needed += w->used+1;
)c" + std::string(reserveBody) + R"c(
}
static int emit(struct writer *w) {
  unsigned char *out=next_slot(w,2);
  if(!out) return 0;
)c" + std::string(write) + R"c(
  ++w->depth; ++w->used;
  out=next_slot(w,2);
  if(!out) return 0;
  *out++='}'; *out=0; --w->depth;
  return 1;
}
int client(void) {
  struct writer w={0};
  w.data=malloc(256);
  if(!w.data) return 0;
  w.size=256;
  int result=emit(&w);
  if(result) (void)strlen((const char *)w.data);
  free(w.data);
  return 0;
}
)c");
      EXPECT_EQ(contract.complete(),
                std::string_view(write) == "*out++ = '{';");
      EXPECT_TRUE(contract.requirements.empty());
    }
  }
}

TEST(BufferAnalysis, ForwardingImportsAnIncompleteGenericCalleesLayoutOnly) {
  for (const std::string write : {"*out++ = '{';", "out[1] = '{';"}) {
    SCOPED_TRACE(write);
    const auto contract = bufferCheck(R"c(
size_t strlen(const char *);
struct writer { unsigned char *data; size_t size, used, depth; int format; };
static unsigned char *reserve_forwarded(struct writer *w, size_t needed) {
    if (!w || !w->data) return 0;
    if (w->size > 0 && w->used >= w->size) return 0;
    if (needed > 2147483647u) return 0;
    needed += w->used + 1;
    if (needed <= w->size) return w->data + w->used;
    return 0;
}
static int render(struct writer *w) {
  if (w->format) return render(w);
    unsigned char *out = reserve_forwarded(w, 2);
    if (!out) return 0;
)c" + write + R"c(

    ++w->depth;
    ++w->used;
    out = reserve_forwarded(w, 2);
    if (!out) return 0;
    *out++ = '}';
    *out = 0;
    --w->depth;
    return 1;
}
int forward(struct writer *w) { return render(w); }
int client(void) {
    struct writer w = {0};
    w.data = malloc(256);
    if (!w.data) return 0;
    w.size = 256;
    int result = forward(&w);
    if (result) (void)strlen((char *)w.data);
    free(w.data);
    return 0;
}
)c");
    EXPECT_EQ(contract.complete(), write == "*out++ = '{';");
    EXPECT_TRUE(contract.requirements.empty());
  }
}

TEST(BufferAnalysis, GenericReserveSeparatesContentsFromReleasePermission) {
  const auto contract = bufferCheck("", "reserve");
  EXPECT_TRUE(contract.complete());
  bool buffer = false;
  bool release = false;
  bool capacity = false;
  for (const auto &requirement : contract.requirements) {
    buffer |= requirement.kind == core::CheckedRequirementKind::Buffer;
    if (requirement.kind == core::CheckedRequirementKind::Buffer)
      if (const auto shape = core::BufferShape::decode(requirement.family))
        release |= shape->ownsBacking;
    release |= requirement.kind == core::CheckedRequirementKind::Release;
  }
  for (const auto &post : contract.establishes)
    capacity |=
        post.kind == core::CheckedRequirementKind::Buffer &&
        post.end == core::PathAffine::ofPath(core::SummaryPath::param(1)) &&
        post.on == core::Outcome::Zero;
  EXPECT_TRUE(buffer);
  EXPECT_TRUE(release);
  EXPECT_TRUE(capacity);
}

TEST(BufferAnalysis, GenericAppendUsesTheSuccessfulReserveBound) {
  EXPECT_TRUE(bufferCheck("", "append").complete());
  EXPECT_TRUE(bufferCheck("", "last").complete());
}

TEST(BufferAnalysis, RuntimeBackEdgesPreserveTheInitializedPrefix) {
  const auto contract = bufferCheck(R"c(
    int client(unsigned n) {
      if(n>1000000) return 0;
      struct bytes b={0};
      for(unsigned i=0;i<n;++i)
        if(append(&b,7)) {free(b.data); return 0;}
      int result=last(&b); free(b.data); return result;
    }
  )c");
  EXPECT_TRUE(contract.complete());
  EXPECT_TRUE(contract.requirements.empty());
}

TEST(BufferAnalysis, CapacityAndLengthStoresNeverCreateStorageOrContents) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; b.data=malloc(4); if(!b.data)return 0;
      b.capacity=100; b.length=99;
      int result=append(&b,7); free(b.data); return result;
    }
  )c")
                   .complete());
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; if(reserve(&b,100))return 0;
      b.length=100; int result=last(&b); free(b.data); return result;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, UninitializedCapacityCannotBeReadThroughThePredicate) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; if(append(&b,7))return 0;
      int result=b.data[b.capacity-1]; free(b.data); return result;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, ReplacementDoesNotReviveSavedPointers) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; if(append(&b,7))return 0;
      unsigned char *saved=b.data;
      if(reserve(&b,128)){free(b.data);return 0;}
      int result=*saved; free(b.data);return result;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, ConditionalGuaranteesDoNotSurviveIgnoredFailureOrWrites) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; reserve(&b,100);
      b.data[99]=7; free(b.data); return 0;
    }
  )c")
                   .complete());
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; if(reserve(&b,1))return 0;
      reserve(&b,100); b.data[99]=7;free(b.data);return 0;
    }
  )c")
                   .complete());
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; int status=reserve(&b,100);
      free(b.data); b.data=malloc(1);
      if(!b.data)return 0;
      if(!status)b.data[99]=7;
      free(b.data); return 0;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, AliasedCountWritesRetireTheEstablishedPrefix) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; if(append(&b,7))return 0;
      size_t *alias=&b.length; *alias=b.capacity;
      int result=last(&b); free(b.data);return result;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, OpaqueMutationCannotRecoverAnEntryPredicate) {
  EXPECT_FALSE(bufferCheck(R"c(
    void mutate(struct bytes *);
    int client(void) {
      struct bytes b={0}; if(append(&b,7))return 0;
      mutate(&b); int result=last(&b);free(b.data);return result;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, BorrowedBackingStorageDoesNotAcquireReleasePermission) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      unsigned char local[8]={0}; struct bytes b={local,8,8};
      return reserve(&b,16);
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, SmallerAdvertisedCapacityPreservesThePhysicalExtent) {
  const auto result = bufferCheck(R"c(
    int client(void) {
      struct bytes b={malloc(3),0,1}; if(!b.data)return 0;
      b.data[2]=7; int result=last(&b);free(b.data);return result;
    }
  )c");
  EXPECT_TRUE(result.complete());
  EXPECT_TRUE(result.requirements.empty());
}

TEST(BufferAnalysis, FailedPositiveAllocationDoesNotBecomeLiveAtZeroCapacity) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={malloc(3),0,0};
      if(b.data){free(b.data);return 0;}
      b.data[1]=7; return 0;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, WrappedGrowthCannotJustifyAnAdvertisedExtent) {
  EXPECT_FALSE(bufferCheck(R"c(
    int client(void) {
      struct bytes b={0}; size_t n=__SIZE_MAX__;
      b.data=malloc(n+2); if(!b.data)return 0;
      b.capacity=n; b.length=n-1;
      int result=append(&b,7);free(b.data);return result;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, ExactReturnedBackingDoesNotPreserveAnOverwrittenZero) {
  EXPECT_FALSE(bufferCheck(R"c(
    size_t strlen(const char *);
    static unsigned char *clobber(struct bytes *b) {
      b->data[0]=7; return b->data;
    }
    int client(void) {
      struct bytes b={malloc(1),0,1}; if(!b.data)return 0;
      b.data[0]=0; unsigned char *p=clobber(&b);
      int result=(int)strlen((char *)p); free(b.data);return result;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, SuccessOnlyConstructorsActivateAfterTheResultIsTested) {
  const std::string make = R"c(
    static int make(struct bytes *b) {
      b->capacity=8; b->length=0; b->data=malloc(8);
      if(!b->data)return -1;
      b->data[0]=7;b->length=1;return 0;
    }
  )c";
  const auto good = bufferCheck(make + R"c(
    int client(void) {
      struct bytes b; int status=make(&b); if(status)return 0;
      int result=last(&b);free(b.data);return result;
    }
  )c");
  EXPECT_TRUE(good.complete());
  EXPECT_TRUE(good.requirements.empty());
  const auto forwarded = bufferCheck(make + R"c(
    static int forward(struct bytes *b) {return make(b);}
    int client(void) {
      struct bytes b;if(forward(&b))return 0;
      int result=last(&b);free(b.data);return result;
    }
  )c");
  EXPECT_TRUE(forwarded.complete());
  EXPECT_TRUE(forwarded.requirements.empty());
  EXPECT_FALSE(bufferCheck(make + R"c(
    int client(void) {
      struct bytes b; make(&b);int result=last(&b);free(b.data);return result;
    }
  )c")
                   .complete());
  EXPECT_FALSE(bufferCheck(make + R"c(
    int client(void) {
      struct bytes b; int status=make(&b); free(b.data);b.data=0;
      if(status)return 0; return last(&b);
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, DeferredTerminationDoesNotSurviveBackingMutation) {
  const std::string make = R"c(
    size_t strlen(const char *);
    static int make(struct bytes *b) {
      b->capacity=1;b->length=0;b->data=malloc(1);
      if(!b->data)return -1;b->data[0]=0;return 0;
    }
  )c";
  for (const std::string mutation :
       {"b.data[0]=7;", "unsigned char *alias=b.data;*alias=7;",
        "memcpy(b.data,\"x\",1);"}) {
    SCOPED_TRACE(mutation);
    auto source = make;
    source += R"c(
      int client(void) {
        struct bytes b;int status=make(&b);if(!b.data)return 0;
    )c";
    source += mutation;
    source += R"c(
        if(status){free(b.data);return 0;}
        int result=(int)strlen((char *)b.data);free(b.data);return result;
      }
    )c";
    EXPECT_FALSE(bufferCheck(source).complete());
  }
}

TEST(BufferAnalysis, CallbackMutationCannotKeepAnOldBufferPredicate) {
  EXPECT_FALSE(bufferCheck(R"c(
    static void corrupt(struct bytes *b) {b->capacity=100;}
    static void invoke(void (*f)(struct bytes *),struct bytes *b) {f(b);}
    int client(void) {
      struct bytes b={malloc(1),0,1};if(!b.data)return 0;
      invoke(corrupt,&b);b.data[b.capacity-1]=7;free(b.data);return 0;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, ResizeInitializesOnlyTheNewLogicalPrefix) {
  const std::string resize = R"c(
    static int resize(struct bytes *b,size_t n) {
      if(n>b->capacity && reserve(b,n))return -1;
      for(size_t i=b->length;i<n;++i)b->data[i]=0;
      b->length=n;return 0;
    }
  )c";
  EXPECT_TRUE(bufferCheck(resize, "resize").complete());
  const auto client = bufferCheck(resize + R"c(
    int client(unsigned n) {
      if(n>1000000)return 0;struct bytes b={0};
      if(resize(&b,n)){free(b.data);return 0;}
      int result=last(&b);free(b.data);return result;
    }
  )c");
  EXPECT_TRUE(client.complete());
  EXPECT_TRUE(client.requirements.empty());
}

TEST(BufferAnalysis, ResizeRejectsSkippedOrConditionalInitialization) {
  for (const std::string loop :
       {"for(size_t i=b->length+1;i<n;++i)b->data[i]=0;",
        "for(size_t i=b->length;i<n;++i)if(i&1)b->data[i]=0;",
        "for(size_t i=b->length;i<n;i+=2)b->data[i]=0;",
        "for(size_t i=b->length;i<n;++i){if(i==2)break;b->data[i]=0;}"}) {
    SCOPED_TRACE(loop);
    const auto source = "static int resize(struct bytes *b,size_t n){"
                        "if(n>b->capacity && reserve(b,n))return -1;" +
                        loop + "b->length=n;return 0;}" + R"c(
      int client(void) {
        struct bytes b={0};if(resize(&b,4)){free(b.data);return 0;}
        int result=last(&b);free(b.data);return result;
      }
    )c";
    EXPECT_FALSE(bufferCheck(source).complete());
  }
}

TEST(BufferAnalysis, ForwardingFailureCannotHideAnAllocatedBacking) {
  EXPECT_FALSE(bufferCheck(R"c(
    static int make(struct bytes *b) {
      b->capacity=8;b->length=0;b->data=malloc(8);
      if(!b->data)return -1;
      b->data[0]=7;b->length=1;return -1;
    }
    static int forward(struct bytes *b) {return make(b);}
    int client(void) {
      struct bytes b;if(forward(&b))return 0;
      free(b.data);return 0;
    }
  )c")
                   .complete());
}

TEST(BufferAnalysis, ReadersPreserveFullInputAndPartialConsumption) {
  const auto result = test::analyze(R"c(
    typedef __SIZE_TYPE__ size_t;
    struct reader { unsigned depth; const unsigned char *data; size_t size, pos; };
    int read_next(struct reader *r) {
      if (r->pos == r->size) return -1;
      int value = r->data[r->pos];
      ++r->pos;
      if (value == 0) return -1;
      return value;
    }
  )c",
                                    {.checkContracts = true, .checked = true});
  const auto *summary = result.summary("read_next");
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(summary->checked.complete());
  bool input = false;
  bool output = false;
  for (const auto &requirement : summary->checked.requirements)
    if (requirement.kind == core::CheckedRequirementKind::Buffer) {
      const auto shape = core::BufferShape::decode(requirement.family);
      input |= shape && shape->reader && !shape->ownsBacking;
    }
  for (const auto &post : summary->checked.establishes)
    if (post.kind == core::CheckedRequirementKind::Buffer) {
      const auto shape = core::BufferShape::decode(post.family);
      output |= shape && shape->reader && !post.on && post.when.trivial();
    }
  EXPECT_TRUE(input);
  EXPECT_TRUE(output);
}

TEST(BufferAnalysis, ReadersRequireActualInitializedInputAndNoWrites) {
  for (const auto *body :
       {"if(r->pos<r->size) ((unsigned char*)r->data)[r->pos]=0;",
        "if(r->pos<r->size) return r->data[r->size];"}) {
    const auto result = test::analyze(R"c(
      typedef __SIZE_TYPE__ size_t;
      struct reader { unsigned depth; const unsigned char *data; size_t size,pos; };
      int read_next(struct reader *r) {
    )c" + std::string(body) + R"c(return 0;}
      int client(void) {
        const unsigned char input[2] = {1, 2};
        struct reader r = {0, input, 2, 0};
        return read_next(&r);
      }
    )c",
                                      {.checkContracts = true,
                                       .checked = true});
    ASSERT_NE(result.summary("read_next"), nullptr);
    const auto &contract = result.summary("read_next")->checked;
    if (std::string(body).find("=0") != std::string::npos) {
      // C permits casting const away when the actual object is writable.
      // The reader's initialized input cannot supply that permission.
      bool writable = false;
      for (const auto &requirement : contract.requirements)
        writable |=
            requirement.kind == core::CheckedRequirementKind::Writable &&
            requirement.path ==
                core::SummaryPath::param(0).deref().field("data");
      EXPECT_TRUE(writable || !contract.complete());
    } else {
      // An actual allocation may exceed its advertised readable prefix.
      // Reading past that prefix exports an additional caller obligation.
      EXPECT_TRUE(std::ranges::any_of(
          contract.requirements, [](const auto &requirement) {
            return requirement.kind == core::CheckedRequirementKind::Extent &&
                   requirement.path ==
                       core::SummaryPath::param(0).deref().field("data");
          }));
    }
    ASSERT_NE(result.summary("client"), nullptr);
    EXPECT_FALSE(result.summary("client")->checked.complete()) << body;
  }
}

} // namespace weavec::analysis
