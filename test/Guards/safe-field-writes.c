// RFC 0037 §10, RFC 0042: in a safe function, whose fields' bounds hold, a
// library call that writes into an array field of a struct (`memcpy`,
// `read`, not only a string or formatting call) writes no more than the
// field; a write past it within the struct stops. A guarded function
// keeps RFC 0037's scope: a copy from one field across the next is the
// struct's own business there. The limit is of the first written argument
// alone: `strtok_r`'s save pointer, written after a short string field, is
// another object. A wrapper that writes a buffer its caller gives without a
// size (`gcvt`, `wrapper(gcvt, limit)`) is limited to the field too.
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe %s -o %t
// RUN: %t fits | FileCheck %s --check-prefix=FITS
// RUN: not --crash %t copy 2>&1 | FileCheck %s --check-prefix=COPY
// RUN: not --crash %t read 2>&1 | FileCheck %s --check-prefix=READ
// RUN: %t words | FileCheck %s --check-prefix=WORDS
// RUN: not --crash %t digits 2>&1 | FileCheck %s --check-prefix=DIGITS
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: not --crash %t.full copy 2>&1 | FileCheck %s --check-prefix=COPY
// RUN: %weavec_cc -std=c23 -O2 %s -o %t.guarded
// RUN: %t.guarded copy | FileCheck %s --check-prefix=GUARDED
#define _DEFAULT_SOURCE 1
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <weavec.h>

struct record {
  char name[8];
  char secret[8];
};

struct line {
  char text[4];
  long after;
};

struct number {
  char digits[8];
  long after;
  long more[4];
};

int run(const char *mode) {
  if (strcmp(mode, "words") == 0) {
    struct line l = {"a b", 0};
    char *save = NULL;
    int n = 0;
    for (char *w = strtok_r(l.text, " ", &save); w != NULL;
         w = strtok_r(NULL, " ", &save))
      ++n;
    // WORDS: words 2
    printf("words %d\n", n);
    return 0;
  }
  if (strcmp(mode, "digits") == 0) {
    struct number d = {"", 42, {0}};
    // DIGITS: weavec: intra-object-overflow at {{.*}}safe-field-writes.c:[[@LINE+1]]:{{[0-9]+}}: write of 20 bytes
    gcvt(1.0 / 3, 17, d.digits);
    printf("after %ld\n", d.after);
    return 0;
  }
  struct record r = {"", "SECRET!"};
  const char source[16] = "AAAAAAAAAAAAAAA";
  // The count is not a constant, so that the optimizer keeps the call.
  const size_t n = strcmp(mode, "fits") == 0 ? 7 : 12;
  if (strcmp(mode, "read") == 0) {
    const int fd = open("/dev/zero", O_RDONLY);
    if (fd < 0)
      return 1;
    // READ: weavec: intra-object-overflow at {{.*}}safe-field-writes.c:[[@LINE+1]]:{{[0-9]+}}: write of 12 bytes
    const ssize_t got = read(fd, r.name, 12);
    close(fd);
    return got == 12 ? 0 : 1;
  }
  // COPY: weavec: intra-object-overflow at {{.*}}safe-field-writes.c:[[@LINE+1]]:{{[0-9]+}}: write of 12 bytes
  memcpy(r.name, source, n);
  // FITS: secret SECRET!
  // GUARDED: secret AAAAET!
  printf("secret %.7s\n", r.secret);
  return 0;
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  return argc < 2 ? 2 : run(argv[1]);
}
