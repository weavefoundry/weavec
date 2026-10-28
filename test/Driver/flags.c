// RFC 0035 §7, RFC 0036 §7: weavec-cc's flags parse, malformed values are
// errors, and the flags RFCs 0030, 0035 and 0036 removed are unknown WeaveC
// flags, as is every unknown -fweavec-*. The analysis's -W flags are Clang's
// to judge.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -fweavec-checks=report -fno-weavec-zero-init -fweavec-zero-init -fno-weavec-summary -fweavec-summary -fno-weavec-asan-compat -fweavec-asan-compat -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: %weavec_cc -fweavec-checks=none -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: not %weavec_cc -fweavec-ledger= -fsyntax-only %s 2>&1 | FileCheck --check-prefix=EMPTY %s
// RUN: not %weavec_cc -fweavec-checks -fsyntax-only %s 2>&1 | FileCheck --check-prefix=NOVALUE %s
// RUN: not %weavec_cc -fweavec-checks=maybe -fsyntax-only %s 2>&1 | FileCheck --check-prefix=BADVALUE %s
// RUN: not %weavec_cc -fweavec-diagnose -fsyntax-only %s 2>&1 | FileCheck --check-prefix=DIAGNOSE %s
// RUN: not %weavec_cc -fweavec-budget=7 -fsyntax-only %s 2>&1 | FileCheck --check-prefix=BUDGET %s
// RUN: not %weavec_cc -fweavec-dump-analysis -fsyntax-only %s 2>&1 | FileCheck --check-prefix=DUMP %s
// RUN: not %weavec_cc -fweavec-require=checked -fsyntax-only %s 2>&1 | FileCheck --check-prefix=REQUIRE %s
// RUN: not %weavec_cc -fno-weavec-runtime -fsyntax-only %s 2>&1 | FileCheck --check-prefix=RUNTIME %s
// RUN: %weavec_cc -Wno-weavec-leak -fsyntax-only %s 2>&1 | FileCheck --check-prefix=WARNING %s
// RUN: %weavec_cc --help-weavec | FileCheck --check-prefix=HELP %s
//
// WeaveC's flags reach every cc1 job as -Xclang arguments.
// RUN: %weavec_cc -### -fweavec-checks=verify -fno-weavec-zero-init -fweavec-ledger=%t/ledgers/ -fno-weavec-asan-compat -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=JOBS %s
//
// A file receives one ledger; an invocation that would write two to it is
// an error, while a directory takes one per unit.
// RUN: not %weavec_cc -fweavec-ledger=%t/one.json -c %s %S/Inputs/rfc0014-callback-helper.c 2>&1 | FileCheck --check-prefix=TWO %s

// ACCEPTED-NOT: error:
// EMPTY: weavec-cc: error: missing value for '-fweavec-ledger'
// NOVALUE: weavec-cc: error: missing value for '-fweavec-checks'
// BADVALUE: weavec-cc: error: invalid value 'maybe' in '-fweavec-checks=maybe'; expected trap, report, verify or none
// DIAGNOSE: weavec-cc: error: unknown WeaveC flag '-fweavec-diagnose'
// BUDGET: weavec-cc: error: unknown WeaveC flag '-fweavec-budget=7'
// DUMP: weavec-cc: error: unknown WeaveC flag '-fweavec-dump-analysis'
// REQUIRE: weavec-cc: error: unknown WeaveC flag '-fweavec-require=checked'
// RUNTIME: weavec-cc: error: unknown WeaveC flag '-fno-weavec-runtime'
// WARNING: warning: unknown warning option '-Wno-weavec-leak'
// HELP: WeaveC flags of weavec-cc
// HELP: -fweavec-checks=trap|report|verify|none
// HELP: -fweavec-ledger=<path>
// HELP: -fweavec-asan-compat
// HELP-NOT: -fweavec-diagnose
// JOBS: "-cc1"
// JOBS-SAME: "-fweavec-checks=verify" "-fno-weavec-zero-init" "-fweavec-ledger={{.*}}ledgers/" "-fno-weavec-asan-compat"
// TWO: weavec-cc: error: '-fweavec-ledger={{.*}}one.json' would receive 2 ledgers; name a directory (ending in '/') to get one ledger per unit

int main(void) { return 0; }
