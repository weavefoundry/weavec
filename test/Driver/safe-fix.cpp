// RFC 0049 §6: `weavec-cc --weavec-fix` over the ledgers of a survey of two
// units that share a header. It writes the certain fixes and the likely
// ones that agree, on every declaration; runs the rules again on the units
// an edit reached; takes out a likely fix that brought an error with no
// fix; stops when a round applies nothing; marks the files left with no
// error WEAVEC_SAFE_FILE; writes its log; and refuses a file that changed
// since its ledger was written, writing nothing and its log. A dry run
// writes nothing.
// RUN: rm -rf %t && mkdir -p %t && cp %S/Inputs/fix-buffer.h %S/Inputs/fix-count.cpp %t/ && cp %s %t/main.cpp
// RUN: cd %t && %weavec_cxx -std=c++20 -fweavec-safe-survey -fweavec-ledger=%t/ledger/ -fsyntax-only main.cpp fix-count.cpp
// RUN: cd %t && %weavec_cxx --weavec-fix --dry-run %t/ledger | FileCheck %s --check-prefix=DRY
// RUN: cmp %S/Inputs/fix-buffer.h %t/fix-buffer.h
// An earlier run's ledger left in the fixer's run directory is not read.
// RUN: mkdir -p %t/weavec-fix/run-1 && echo '{"units": [{"source": "ghost.cpp", "command": {"directory": "/", "arguments": ["cc"]}, "errors": [{"rule": "index-single", "file": "/ghost.cpp", "line": 1, "message": "ghost", "fixes": []}]}]}' > %t/weavec-fix/run-1/ghost.ledger.json
// RUN: cd %t && %weavec_cxx --weavec-fix -j2 %t/ledger | FileCheck %s --check-prefix=FIX
// RUN: FileCheck %s --check-prefix=HEADER < %t/fix-buffer.h
// RUN: FileCheck %s --check-prefix=COUNT < %t/fix-count.cpp
// RUN: FileCheck %s --check-prefix=MAIN < %t/main.cpp
// RUN: FileCheck %s --check-prefix=LOG < %t/weavec-fix/weavec-fix.json
// What the fixer wrote compiles (the marked files safe, main.cpp's two
// functions left to decide guarded), and runs.
// RUN: cd %t && %weavec_cxx -std=c++20 -O1 main.cpp fix-count.cpp -o %t/prog && %t/prog | FileCheck %s --check-prefix=OUT
// A second survey and fixer apply nothing.
// RUN: cd %t && %weavec_cxx -std=c++20 -fweavec-safe-survey -fweavec-ledger=%t/again/ -fsyntax-only main.cpp fix-count.cpp
// RUN: cd %t && %weavec_cxx --weavec-fix %t/again | FileCheck %s --check-prefix=AGAIN
// A file edited after the survey is not written.
// RUN: mkdir -p %t/s && cd %t/s && echo 'int pair(const int *p) { return p[0] + p[1]; }' > stale.cpp
// RUN: cd %t/s && %weavec_cxx -std=c++20 -fweavec-safe-survey -fweavec-ledger=%t/s/ledger/ -fsyntax-only stale.cpp && echo "// edited" >> stale.cpp
// RUN: cd %t/s && not %weavec_cxx --weavec-fix %t/s/ledger 2>&1 | FileCheck %s --check-prefix=STALE
// RUN: FileCheck %s --check-prefix=STALELOG < %t/s/weavec-fix/weavec-fix.json
#include "fix-buffer.h"
#include <cctype>
#include <cstdio>

// Writes its string in place: no string, so the bound below that would
// pass it here is taken out again.
void upcase(char *s) {
  for (; *s != '\0'; ++s)
    *s = static_cast<char>(std::toupper(*s));
}
std::size_t measure(char *p) {
  std::size_t n = 0;
  while (p[n] != '\0')
    ++n;
  upcase(p);
  return n;
}

int main() {
  const char text[] = "a b c";
  Buffer whole(text, sizeof text - 1);
  Buffer other(text, sizeof text - 1);
  char word[] = "word";
  std::printf("%zu %d %zu\n", count(whole.data(), whole.size(), ' '),
              whole.same(other) ? 1 : 0, measure(word));
  return 0;
}

// DRY: --- {{.*}}fix-buffer.h
// DRY: -  const char *data_;
// DRY-NEXT: +  const char *WEAVEC_BORROWED WEAVEC_COUNTED_BY(size_) data_;
// DRY: +std::size_t count(const char *WEAVEC_COUNTED_BY(n) text, std::size_t n, char c);
// DRY: -std::size_t measure(char *p) {
// DRY-NEXT: +std::size_t measure(char *WEAVEC_NULL_TERMINATED p) {

// FIX: weavec: fix: round 1: {{[0-9]+}} edits in 3 files (0 certain, {{[0-9]+}} likely), 1 withdrawn;
// FIX: weavec: fix: round {{[0-9]+}}: nothing to apply
// FIX: weavec: fix: marked {{[1-9]}} files WEAVEC_SAFE_FILE
// FIX: weavec: fix: {{[0-9]+}} edits in 2 files; 6 errors before, 2 after: 1 have a choice or a fix withdrawn, 1 have no fix
// FIX: --weavec-audit {{.*}}weavec-fix/ledger' prints what is left

// HEADER: #include <weavec.h>
// HEADER: Buffer(const char *WEAVEC_COUNTED_BY(n) d, std::size_t n)
// HEADER: const char *WEAVEC_COUNTED_BY(size_) data() const
// HEADER: const char *WEAVEC_BORROWED WEAVEC_COUNTED_BY(size_) data_;
// HEADER: std::size_t count(const char *WEAVEC_COUNTED_BY(n) text, std::size_t n, char c);

// COUNT: #include "fix-buffer.h"
// COUNT-NEXT: #include <weavec.h>
// COUNT-NEXT: WEAVEC_SAFE_FILE
// COUNT: std::size_t count(const char *WEAVEC_COUNTED_BY(n) text, std::size_t n, char c) {

// MAIN: std::size_t measure(char *p) {

// LOG: "schema": "weavec-fix"
// LOG: "withdrawn": [
// LOG: "fact": "null_terminated"
// LOG-NEXT: "fix": "{{.*}}measure{{.*}}#0|bound"

// OUT: 2 1 4

// AGAIN: weavec: fix: round 1: nothing to apply

// STALE: weavec: fix: '{{.*}}stale.cpp' changed since its ledger was written; build again with -fweavec-safe-survey
// STALELOG: "status": 1
