// RFC 0030 §16, RFC 0035 §7 and *Diagnostics*: the command line of both
// tools.
//
// weavec-cc's flags parse, malformed values are errors, and the flags RFC
// 0030 and RFC 0035 removed are unknown WeaveC flags, as is every unknown
// -fweavec-*.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -fweavec-checks=report -fweavec-budget=0 -fno-weavec-zero-init -fweavec-zero-init -fno-weavec-summary -fweavec-summary -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: %weavec_cc -fweavec-checks=none -fweavec-budget=500000 -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: not %weavec_cc -fweavec-budget=lots -fsyntax-only %s 2>&1 | FileCheck --check-prefix=BUDGET %s
// RUN: not %weavec_cc -fweavec-ledger= -fsyntax-only %s 2>&1 | FileCheck --check-prefix=EMPTY %s
// RUN: not %weavec_cc -fweavec-checks -fsyntax-only %s 2>&1 | FileCheck --check-prefix=NOVALUE %s
// RUN: not %weavec_cc -fweavec-strict -fsyntax-only %s 2>&1 | FileCheck --check-prefix=STRICT %s
// RUN: not %weavec_cc -fweavec-exclusive-borrows -fsyntax-only %s 2>&1 | FileCheck --check-prefix=EXCLUSIVE %s
// RUN: not %weavec_cc -fweavec-report-unannotated -fsyntax-only %s 2>&1 | FileCheck --check-prefix=UNANNOTATED %s
// RUN: not %weavec_cc -fno-weavec-analyze-headers -fsyntax-only %s 2>&1 | FileCheck --check-prefix=HEADERS %s
// RUN: not %weavec_cc -fweavec-require=checked -fsyntax-only %s 2>&1 | FileCheck --check-prefix=REQUIRE %s
// RUN: not %weavec_cc -fweavec-ledger-format=sarif -fsyntax-only %s 2>&1 | FileCheck --check-prefix=FORMAT %s
// RUN: not %weavec_cc -fweavec-link=analyze -fsyntax-only %s 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not %weavec_cc -fweavec-print-prelude -fsyntax-only %s 2>&1 | FileCheck --check-prefix=PRELUDE %s
// RUN: not %weavec_cc -fno-weavec-runtime -fsyntax-only %s 2>&1 | FileCheck --check-prefix=RUNTIME %s
// RUN: %weavec_cc --help-weavec | FileCheck --check-prefix=HELP %s
//
// WeaveC's flags reach every cc1 job as -Xclang arguments.
// RUN: %weavec_cc -### -fweavec-checks=verify -fno-weavec-zero-init -fweavec-ledger=%t/ledgers/ -fweavec-budget=7 -c %s -o %t/x.o 2>&1 | FileCheck --check-prefix=JOBS %s
//
// A file receives one ledger; an invocation that would write two to it is
// an error, while a directory takes one per unit.
// RUN: not %weavec_cc -fweavec-ledger=%t/one.json -c %s %S/Inputs/rfc0014-callback-helper.c 2>&1 | FileCheck --check-prefix=TWO %s
//
// weavec: its options, and the removed ones are unknown arguments.
// RUN: %weavec --budget=0 --no-zero-init %s -- 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: not %weavec --strict-externs %s -- 2>&1 | FileCheck --check-prefix=TOOL-STRICT %s
// RUN: not %weavec --exclusive-borrows %s -- 2>&1 | FileCheck --check-prefix=TOOL-EXCLUSIVE %s
// RUN: not %weavec --report-unannotated %s -- 2>&1 | FileCheck --check-prefix=TOOL-UNANNOTATED %s
// RUN: not %weavec --analyze-headers %s -- 2>&1 | FileCheck --check-prefix=TOOL-HEADERS %s
// RUN: not %weavec --require=checked %s -- 2>&1 | FileCheck --check-prefix=TOOL-REQUIRE %s
// RUN: not %weavec --ledger=%t/one.json %s -- 2>&1 | FileCheck --check-prefix=TOOL-LEDGER %s
// RUN: not %weavec --ledger-format=sarif %s -- 2>&1 | FileCheck --check-prefix=TOOL-FORMAT %s
// RUN: not %weavec --no-runtime %s -- 2>&1 | FileCheck --check-prefix=TOOL-RUNTIME %s
// RUN: not %weavec --dump-record=%t/x.o.weavec 2>&1 | FileCheck --check-prefix=TOOL-RECORD %s
//
// The -W flags know the ids RFC 0030 adds and RFC 0035 keeps, refuse to
// disable the ones that are always errors, and treat an id RFC 0030 removed,
// and the require levels' ids RFC 0035 deleted, as unknown.
// RUN: %weavec -Wweavec-allocation-failure -Wno-weavec-leak -Werror=weavec-contradicted-assumption %s -- 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: %weavec_cc -Wno-weavec-allocation-failure -Werror=weavec-leak -fsyntax-only %s 2>&1 | FileCheck --allow-empty --check-prefix=ACCEPTED %s
// RUN: not %weavec_cc -Wno-weavec-contradicted-assumption -fsyntax-only %s 2>&1 | FileCheck --check-prefix=CONTRADICTED %s
// RUN: not %weavec -Wno-weavec-checking-incomplete %s -- 2>&1 | FileCheck --check-prefix=REMOVED-INCOMPLETE %s
// RUN: not %weavec_cc -Werror=weavec-checking-failed -fsyntax-only %s 2>&1 | FileCheck --check-prefix=REMOVED-FAILED %s
// RUN: not %weavec -Wno-weavec-unresolved-operation %s -- 2>&1 | FileCheck --check-prefix=UNRESOLVED %s
// RUN: not %weavec_cc -Wno-error=weavec-unchecked-operation -fsyntax-only %s 2>&1 | FileCheck --check-prefix=UNCHECKED %s

// ACCEPTED-NOT: error:
// BUDGET: weavec-cc: error: invalid value 'lots' in '-fweavec-budget=lots'; expected a number
// EMPTY: weavec-cc: error: missing value for '-fweavec-ledger'
// NOVALUE: weavec-cc: error: missing value for '-fweavec-checks'
// STRICT: weavec-cc: error: unknown WeaveC flag '-fweavec-strict'
// EXCLUSIVE: weavec-cc: error: unknown WeaveC flag '-fweavec-exclusive-borrows'
// UNANNOTATED: weavec-cc: error: unknown WeaveC flag '-fweavec-report-unannotated'
// HEADERS: weavec-cc: error: unknown WeaveC flag '-fno-weavec-analyze-headers'
// REQUIRE: weavec-cc: error: unknown WeaveC flag '-fweavec-require=checked'
// FORMAT: weavec-cc: error: unknown WeaveC flag '-fweavec-ledger-format=sarif'
// LINK: weavec-cc: error: unknown WeaveC flag '-fweavec-link=analyze'
// PRELUDE: weavec-cc: error: unknown WeaveC flag '-fweavec-print-prelude'
// RUNTIME: weavec-cc: error: unknown WeaveC flag '-fno-weavec-runtime'
// HELP: WeaveC flags of weavec-cc
// HELP: -fweavec-checks=trap|report|verify|none
// HELP: -fweavec-ledger=<path>
// HELP: -fweavec-diagnose
// HELP-NOT: -fweavec-print-prelude
// HELP-NOT: -fweavec-require
// JOBS: "-cc1"
// JOBS-SAME: "-fweavec-checks=verify" "-fno-weavec-zero-init" "-fweavec-ledger={{.*}}ledgers/" "-fweavec-budget=7"
// TWO: weavec-cc: error: '-fweavec-ledger={{.*}}one.json' would receive 2 ledgers; name a directory (ending in '/') to get one ledger per unit
// TOOL-STRICT: Unknown command line argument '--strict-externs'
// TOOL-EXCLUSIVE: Unknown command line argument '--exclusive-borrows'
// TOOL-UNANNOTATED: Unknown command line argument '--report-unannotated'
// TOOL-HEADERS: Unknown command line argument '--analyze-headers'
// TOOL-REQUIRE: Unknown command line argument '--require=checked'
// TOOL-LEDGER: Unknown command line argument '--ledger={{.*}}one.json'
// TOOL-FORMAT: Unknown command line argument '--ledger-format=sarif'
// TOOL-RUNTIME: Unknown command line argument '--no-runtime'
// TOOL-RECORD: Unknown command line argument '--dump-record={{.*}}x.o.weavec'
// CONTRADICTED: weavec-cc: error: '-Wno-weavec-contradicted-assumption': 'contradicted-assumption' is an error and cannot be disabled; use -Wno-error=weavec-contradicted-assumption to make it a warning
// REMOVED-INCOMPLETE: weavec: error: unknown WeaveC diagnostic 'checking-incomplete' (removed by RFC 0030)
// REMOVED-FAILED: weavec-cc: error: unknown WeaveC diagnostic 'checking-failed' (removed by RFC 0030)
// UNRESOLVED: weavec: error: unknown WeaveC diagnostic 'unresolved-operation' (removed by RFC 0035)
// UNCHECKED: weavec-cc: error: unknown WeaveC diagnostic 'unchecked-operation' (removed by RFC 0035)

int main(void) { return 0; }
