// RFC 0030, section 10.6, gate G8: report mode passes the site's file, line and column.
// The -O0 IR equals that of Inputs/rewrite-oracle-report.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-report.expected.c %t -- -fweavec-checks=report

int load(int *p) { return *p; }
