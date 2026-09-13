// RUN: not %weavec_cc -fweavec-checked-function=main -c %s -o %t.o 2>&1 | FileCheck %s
// RFC 0026/0018: an external buffer mutator is deferred at compile time, but
// a later local assignment still determines validity before the link step.
// CHECK: error: {{.*}}null{{.*}}[weavec::null-dereference]

typedef __SIZE_TYPE__ size_t;
struct buffer { unsigned char *data; size_t length, capacity; };
void mutate(struct buffer *);
int main(void) {
  struct buffer b = {0};
  mutate(&b);
  b.data = 0;
  return b.data[0];
}
