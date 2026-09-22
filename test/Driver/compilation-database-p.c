// RFC 0030 §16 (gate H3): `weavec --whole-program -p <dir>` names no source,
// so it loads the compilation database of <dir> itself and analyses every
// file it lists as one program. Before, the tool dereferenced the database
// `CommonOptionsParser` leaves unloaded without a source, and crashed. A
// directory without a database is an error with a message.
//
// RUN: rm -rf %t && mkdir -p %t/build
// RUN: echo '[{"directory": "%S", "file": "%s", "arguments": ["cc", "-c", "%s", "-I%S/../WholeProgram/Inputs"]}, {"directory": "%S", "file": "%S/../WholeProgram/Inputs/node.c", "arguments": ["cc", "-c", "%S/../WholeProgram/Inputs/node.c", "-I%S/../WholeProgram/Inputs"]}]' > %t/build/compile_commands.json
// RUN: not %weavec --whole-program -p %t/build 2>&1 | FileCheck %s
//
// `--extra-arg` applies to the database loaded here as to any other.
// RUN: not %weavec --whole-program -p %t/build --extra-arg=-DSECOND 2>&1 | FileCheck --check-prefixes=CHECK,SECOND %s
//
// The parents of the directory are searched as for `-p` with a source, so
// the missing database is outside the build tree (which has one).
// RUN: not %weavec --whole-program -p /nonexistent/weavec-lit 2>&1 | FileCheck --check-prefix=MISSING %s
// RUN: not %weavec --whole-program 2>&1 | FileCheck --check-prefix=NONE %s
// RUN: not %weavec --whole-program -- -I%S 2>&1 | FileCheck --check-prefix=NONE %s
// RUN: not %weavec -p %t/build 2>&1 | FileCheck --check-prefix=NOINPUT %s
#include "../Inputs/prelude.h"
#include "node.h"

// MISSING: weavec: error: no compilation database in '/nonexistent/weavec-lit' or any parent directory
// MISSING-NOT: Stack dump
// NONE: weavec: error: no compilation database with sources; give -p <build-dir> or list the files
// NOINPUT: weavec: error: no input files

int double_release(void) {
  struct node *n = node_new();
  node_free(n);
  // CHECK: compilation-database-p.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  node_free(n);
  return 0;
}

#ifdef SECOND
int second_release(void) {
  struct node *n = node_new();
  node_free(n);
  // SECOND: compilation-database-p.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  node_free(n);
  return 0;
}
#endif
