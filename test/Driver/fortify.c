// RFC 0036 §6: an optimising enforcing build for glibc defines
// _FORTIFY_SOURCE=3, so that the C library's fortified forms pass object
// sizes for the guards to check; not without optimisation, not when the
// command line chooses a level, not without checks.
// RUN: %weavec_cc -### --target=x86_64-linux-gnu -O2 -c %s 2>&1 | FileCheck --check-prefix=ON %s
// RUN: %weavec_cc -### --target=x86_64-linux-gnu -O0 -c %s 2>&1 | FileCheck --check-prefix=OFF %s
// RUN: %weavec_cc -### --target=x86_64-linux-gnu -O2 -D_FORTIFY_SOURCE=1 -c %s 2>&1 | FileCheck --check-prefix=CHOSEN %s
// RUN: %weavec_cc -### --target=x86_64-linux-gnu -O2 -fweavec-checks=none -c %s 2>&1 | FileCheck --check-prefix=OFF %s
//
// ON: "-D" "_FORTIFY_SOURCE=3"
// OFF-NOT: _FORTIFY_SOURCE
// CHOSEN: "-D" "_FORTIFY_SOURCE=1"
// CHOSEN-NOT: _FORTIFY_SOURCE=3
int x;
