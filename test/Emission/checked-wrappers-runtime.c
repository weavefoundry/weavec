// RFC 0034, section 5.2: the checked wrappers fail as the other guards do.
// In trap mode the first failure traps; in report mode each failing site is
// reported once and the call goes on (a sprintf writes only the room it
// has, and returns the length it would have written); in verify mode they
// trap as in trap mode. A correct run fails nothing.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -O1 %s -o %t/trap
// RUN: %t/trap 012 | FileCheck --check-prefix=CLEAN %s
// RUN: not --crash %t/trap 0123456789 2>&1 | FileCheck --check-prefix=TRAP %s
// RUN: %weavec_cc -O1 -fweavec-checks=report %s -o %t/report
// RUN: %t/report 0123456789 overlap 2>&1 | FileCheck --check-prefix=REPORT %s
// RUN: %weavec_cc -O1 -fweavec-checks=verify %s -o %t/verify
// RUN: %t/verify 012 | FileCheck --check-prefix=CLEAN %s
// RUN: not --crash %t/verify 012 overlap 2>&1 | FileCheck --check-prefix=TRAP %s
//
// CLEAN: sprintf 5 <012>
// CLEAN: done
// TRAP-NOT: done
// REPORT: weavec: runtime check failed: object at {{.*}}checked-wrappers-runtime.c:[[@LINE+13]]:3
// REPORT: weavec: runtime check failed: object at {{.*}}checked-wrappers-runtime.c:[[@LINE+15]]:10
// REPORT: sprintf 12 <012345
// REPORT: weavec: runtime check failed: disjoint at {{.*}}checked-wrappers-runtime.c:[[@LINE+16]]:3
// REPORT: shifted
// REPORT: done

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No term names words[1], and the destination's extent is not known here. */
__attribute__((noinline)) static void put(char *dst, char **words) {
  strcpy(dst, words[1]);
}
__attribute__((noinline)) static int show(char *dst, char **words) {
  return sprintf(dst, "<%s>", words[1]);
}
__attribute__((noinline)) static void shift(char *p) {
  strcpy(p, p + 2);
}

int main(int argc, char **argv) {
  char *dst = malloc(8);
  if (dst == NULL || argc < 2)
    return 1;
  setvbuf(stdout, NULL, _IONBF, 0);
  /* Report mode goes on after the overflow; nothing reads dst until
   * sprintf has written it again. */
  put(dst, argv);
  int n = show(dst, argv);
  printf("sprintf %d %s\n", n, dst);
  if (argc > 2) {
    char *p = strdup("././x");
    if (p == NULL)
      return 1;
    shift(p);
    printf("shifted %s\n", p);
    free(p);
  }
  free(dst);
  printf("done\n");
  return 0;
}
