# Recall set

Small C programs in the shape of the [Juliet test suite](https://samate.nist.gov/SARD/test-suites/112)'s
CWE cases, one bug each, that WeaveC must catch (RFC 0011, *Recall check*).
Where the lit suite (`test/Analysis`) pins every message and location and
is the regression suite, this set asks only "was this bug class caught",
is organised by bug class, and is what the README's status table cites.

## Layout

```
test/recall/
  recall.h              the libc surface the cases use (no system headers)
  CWE-121/*.c           stack-based buffer overflow
  CWE-122/*.c           heap-based buffer overflow
  CWE-124/*.c           buffer underwrite
  CWE-126/*.c           buffer over-read
  CWE-127/*.c           buffer under-read
  CWE-401/*.c           memory leak
  CWE-415/*.c           double free
  CWE-416/*.c           use after free
  CWE-457/*.c           use of an uninitialised variable
  CWE-476/*.c           NULL pointer dereference
```

Each case pins the diagnostic that must be reported with a comment on the
line it belongs to:

```c
// CWE-121: stack-based buffer overflow, a constant index one past the end.
#include "recall.h"

void bad(void) {
  char buf[10];
  buf[10] = 'A'; // RECALL: out-of-bounds
}

void good(void) { /* the same shape without the bug: must be clean */ }
```

`// RECALL: <id> @<line>` pins a diagnostic on another line. A case may hold
several `bad_*` functions and should hold a `good` one: the script fails on
any *error* the case did not pin, so the good half guards precision as the
bad half guards recall. Warnings (`leak` on the path a bug diverts, Clang's
own `-Warray-bounds`) are not counted.

## Running

```sh
scripts/recall.py --weavec build/dev/bin/weavec            # the table
scripts/recall.py --weavec build/dev/bin/weavec -v         # every pin
scripts/recall.py --weavec build/dev/bin/weavec --only CWE-122
```

`ctest --preset dev` runs it as the `recall` test, after the unit and lit
tests. The exit status is non-zero when a pinned diagnostic is missing or
an unpinned error appears.

## Adding a case

Keep to one bug class per file and one bug per `bad_*` function, in the
smallest shape that shows it; name the file after the shape
(`loop_off_by_one.c`, `memcpy_too_much.c`). A shape WeaveC does not catch
yet does not belong here until it does: the set states what is caught, and
CI holds it there. Open an RFC (or amend RFC 0011) for the rule that would
catch it.
