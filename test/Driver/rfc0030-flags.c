// RFC 0030 §16 and *Diagnostics*: the command line of both tools.
//
// weavec-cc's flags parse, malformed values are errors, and the flags §16
// removes are unknown WeaveC flags, as is every unknown -fweavec-*.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -fweavec-checks=report -fweavec-require=proven -fweavec-budget=0 -fno-weavec-zero-init -fweavec-zero-init -fweavec-ledger-format=sarif -fno-weavec-summary -fweavec-summary -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: %weavec_cc -fweavec-checks=none -fweavec-require=checked -fweavec-budget=500000 -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: not %weavec_cc -fweavec-require=always -fsyntax-only %s 2>&1 | FileCheck --check-prefix=REQUIRE %s
// RUN: not %weavec_cc -fweavec-budget=lots -fsyntax-only %s 2>&1 | FileCheck --check-prefix=BUDGET %s
// RUN: not %weavec_cc -fweavec-ledger-format=xml -fsyntax-only %s 2>&1 | FileCheck --check-prefix=FORMAT %s
// RUN: not %weavec_cc -fweavec-ledger= -fsyntax-only %s 2>&1 | FileCheck --check-prefix=EMPTY %s
// RUN: not %weavec_cc -fweavec-checks -fsyntax-only %s 2>&1 | FileCheck --check-prefix=NOVALUE %s
// RUN: not %weavec_cc -fweavec-strict -fsyntax-only %s 2>&1 | FileCheck --check-prefix=STRICT %s
// RUN: not %weavec_cc -fweavec-exclusive-borrows -fsyntax-only %s 2>&1 | FileCheck --check-prefix=EXCLUSIVE %s
// RUN: not %weavec_cc -fweavec-report-unannotated -fsyntax-only %s 2>&1 | FileCheck --check-prefix=UNANNOTATED %s
// RUN: not %weavec_cc -fno-weavec-analyze-headers -fsyntax-only %s 2>&1 | FileCheck --check-prefix=HEADERS %s
// RUN: %weavec_cc --help-weavec | FileCheck --check-prefix=HELP %s
//
// WeaveC's flags reach every cc1 job as -Xclang arguments.
// RUN: %weavec_cc -### -fweavec-checks=verify -fno-weavec-zero-init -fweavec-require=checked -fweavec-ledger=%t/ledgers/ -fweavec-budget=7 -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=JOBS %s
//
// §16: a file receives one ledger; an invocation that would write two to
// it is an error, while a directory takes one per unit and per link.
// RUN: not %weavec_cc -fweavec-ledger=%t/one.json %s -o %t/prog 2>&1 | FileCheck --check-prefix=TWO %s
// RUN: not %weavec_cc -fweavec-ledger=%t/one.json -c %s %S/Inputs/rfc0014-callback-helper.c 2>&1 | FileCheck --check-prefix=TWO %s
// RUN: %weavec_cc -fweavec-ledger=%t/ledgers/ %s -o %t/prog
// RUN: %weavec_cc -fweavec-ledger=%t/one.json -fno-weavec-link %s -o %t/prog
//
// weavec: the §16 options, and the removed ones are unknown arguments.
// RUN: %weavec --require=proven --budget=0 --no-zero-init --ledger-format=sarif %s -- 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: not %weavec --require=always %s -- 2>&1 | FileCheck --check-prefix=TOOL-REQUIRE %s
// RUN: not %weavec --strict-externs %s -- 2>&1 | FileCheck --check-prefix=TOOL-STRICT %s
// RUN: not %weavec --exclusive-borrows %s -- 2>&1 | FileCheck --check-prefix=TOOL-EXCLUSIVE %s
// RUN: not %weavec --report-unannotated %s -- 2>&1 | FileCheck --check-prefix=TOOL-UNANNOTATED %s
// RUN: not %weavec --analyze-headers %s -- 2>&1 | FileCheck --check-prefix=TOOL-HEADERS %s
// RUN: not %weavec --ledger=%t/one.json %s %S/Inputs/rfc0014-callback-helper.c -- 2>&1 | FileCheck --check-prefix=TOOL-TWO %s
//
// The -W flags know the five ids RFC 0030 adds, refuse to disable the ones
// that are always errors, and treat an id RFC 0030 removed as unknown.
// RUN: %weavec -Wweavec-allocation-failure -Wno-weavec-unanalyzed-input -Werror=weavec-contradicted-assumption -Wno-error=weavec-unresolved-operation -Wno-error=weavec-unchecked-operation %s -- 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: %weavec_cc -Wno-weavec-allocation-failure -Werror=weavec-unanalyzed-input -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: not %weavec -Wno-weavec-unresolved-operation %s -- 2>&1 | FileCheck --check-prefix=UNRESOLVED %s
// RUN: not %weavec_cc -Wno-weavec-unchecked-operation -fsyntax-only %s 2>&1 | FileCheck --check-prefix=UNCHECKED %s
// RUN: not %weavec_cc -Wno-weavec-contradicted-assumption -fsyntax-only %s 2>&1 | FileCheck --check-prefix=CONTRADICTED %s
// RUN: not %weavec -Wno-weavec-checking-incomplete %s -- 2>&1 | FileCheck --check-prefix=REMOVED-INCOMPLETE %s
// RUN: not %weavec_cc -Werror=weavec-checking-failed -fsyntax-only %s 2>&1 | FileCheck --check-prefix=REMOVED-FAILED %s

// ACCEPTED-NOT: error:
// REQUIRE: weavec-cc: error: invalid value 'always' in '-fweavec-require=always'; expected none, checked or proven
// BUDGET: weavec-cc: error: invalid value 'lots' in '-fweavec-budget=lots'; expected a number of block transfers
// FORMAT: weavec-cc: error: invalid value 'xml' in '-fweavec-ledger-format=xml'; expected json or sarif
// EMPTY: weavec-cc: error: missing value for '-fweavec-ledger'
// NOVALUE: weavec-cc: error: missing value for '-fweavec-checks'
// STRICT: weavec-cc: error: unknown WeaveC flag '-fweavec-strict'
// EXCLUSIVE: weavec-cc: error: unknown WeaveC flag '-fweavec-exclusive-borrows'
// UNANNOTATED: weavec-cc: error: unknown WeaveC flag '-fweavec-report-unannotated'
// HEADERS: weavec-cc: error: unknown WeaveC flag '-fno-weavec-analyze-headers'
// HELP: WeaveC flags of weavec-cc
// HELP: -fweavec-checks=trap|report|verify|none
// HELP: -fweavec-ledger=<path>
// HELP: -fweavec-print-prelude
// JOBS: "-cc1"
// JOBS-SAME: "-fweavec-checks=verify" "-fno-weavec-zero-init" "-fweavec-require=checked" "-fweavec-ledger={{.*}}ledgers/" "-fweavec-budget=7"
// TWO: weavec-cc: error: '-fweavec-ledger={{.*}}one.json' would receive 2 ledgers; name a directory (ending in '/') to get one ledger per unit and per link
// TOOL-REQUIRE: for the --require option: Cannot find option named 'always'!
// TOOL-STRICT: Unknown command line argument '--strict-externs'
// TOOL-EXCLUSIVE: Unknown command line argument '--exclusive-borrows'
// TOOL-UNANNOTATED: Unknown command line argument '--report-unannotated'
// TOOL-HEADERS: Unknown command line argument '--analyze-headers'
// TOOL-TWO: weavec: error: '--ledger={{.*}}one.json' would receive 2 ledgers; name a directory (ending in '/') to get one ledger per source
// UNRESOLVED: weavec: error: '-Wno-weavec-unresolved-operation': 'unresolved-operation' is an error and cannot be disabled; use -Wno-error=weavec-unresolved-operation to make it a warning
// UNCHECKED: weavec-cc: error: '-Wno-weavec-unchecked-operation': 'unchecked-operation' is an error and cannot be disabled; use -Wno-error=weavec-unchecked-operation to make it a warning
// CONTRADICTED: weavec-cc: error: '-Wno-weavec-contradicted-assumption': 'contradicted-assumption' is an error and cannot be disabled; use -Wno-error=weavec-contradicted-assumption to make it a warning
// REMOVED-INCOMPLETE: weavec: error: unknown WeaveC diagnostic 'checking-incomplete' (removed by RFC 0030)
// REMOVED-FAILED: weavec-cc: error: unknown WeaveC diagnostic 'checking-failed' (removed by RFC 0030)

int main(void) { return 0; }
