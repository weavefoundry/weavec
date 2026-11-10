// RFC 0041 §5, amendment 4: a WEAVEC_BORROWED field may hold a borrowed
// pointer when its struct is a local; a pointer loaded from it is borrowed,
// and so is the local's address and a copy of it.
// RUN: %weavec_cc -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <stddef.h>

typedef struct {
  const unsigned char *WEAVEC_BORROWED WEAVEC_COUNTED_BY(length) content;
  size_t length;
  size_t offset;
} parse_buffer;

static parse_buffer *kept;
static const unsigned char *kept_bytes;

WEAVEC_SAFE int peek(parse_buffer *b) {
  return b->offset < b->length ? b->content[b->offset] : -1;
}

WEAVEC_SAFE const unsigned char *leak(parse_buffer *b) {
  kept_bytes = b->content;        // expected-error {{'b' is borrowed for this call and cannot be stored where it outlives the call}}
  return b->content;              // expected-error {{'b' is borrowed for this call and cannot be returned}}
}

// A pointer loaded from a borrowed field is borrowed as long as the struct
// it came from: through a lifetimebound parameter it may be returned.
WEAVEC_SAFE const unsigned char *at(parse_buffer *b WEAVEC_LIFETIMEBOUND) {
  return b->content + b->offset;
}

static const unsigned char *global_bytes;
WEAVEC_SAFE void from_global(void) {
  extern parse_buffer *shared;
  global_bytes = shared->content; // expected-error {{'content' was loaded from a WEAVEC_BORROWED field, so it is borrowed, and cannot be stored where it outlives the call}}
}

typedef struct { size_t position; int code; } report;
WEAVEC_SAFE size_t integers(const unsigned char *WEAVEC_COUNTED_BY(n) v, size_t n) {
  parse_buffer c = {v, n, 0};
  report r;
  r.position = c.offset;          // an integer carries no pointer
  r.code = 0;
  return r.position;
}

WEAVEC_SAFE int parse(const unsigned char *WEAVEC_COUNTED_BY(n) value, size_t n) {
  parse_buffer buffer = {0};
  buffer.content = value;         // a local's borrowed field
  buffer.length = n;
  parse_buffer other = {value, n, 0};
  int first = peek(&buffer) + peek(&other);
  kept = &buffer;                 // expected-error {{'buffer' holds borrowed pointers (WEAVEC_BORROWED fields), and cannot be stored where it outlives the call}}
  return first;
}

WEAVEC_SAFE void through(parse_buffer *b, const unsigned char *WEAVEC_COUNTED_BY(n) v, size_t n) {
  b->content = v;                 // expected-error {{'v' is borrowed for this call and cannot be stored}}
  b->length = n;
}

WEAVEC_SAFE void barrier(void) { __asm__ volatile("" ::: "memory"); }
