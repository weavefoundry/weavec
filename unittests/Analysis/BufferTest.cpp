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

} // namespace weavec::analysis
