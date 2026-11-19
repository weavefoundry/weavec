// RFC 0047 §6–§8, RFC 0049 §7–§8: under -fweavec-safe-survey the rules also
// run over the functions that are not safe; their errors are recorded in
// the ledger with their fixes, not printed, and the compile succeeds; the
// ledger records the command and the files the unit read. A header that says WEAVEC_SAFE_FILE is
// safe in every unit that includes it. The audit prints the plan: the files
// ready to mark and those blocked, fewest errors first; the members with a
// bound and the aliased classes.
// RUN: rm -rf %t && mkdir -p %t/ledger
// RUN: %weavec_cxx -std=c++20 -O2 -I%S/Inputs -fweavec-safe-survey -fno-weavec-summary -fweavec-ledger=%t/ledger/ -c %s -o %t/survey.o 2>&1 | count 0
// RUN: %weavec_cxx -std=c++20 -O2 -I%S/Inputs -fweavec-safe-survey -fno-weavec-summary -fweavec-ledger=%t/ledger/ -c %S/Inputs/survey-other.cpp -o %t/other.o 2>&1 | count 0
// RUN: FileCheck %s --check-prefix=LEDGER < %t/ledger/survey.o.ledger.json
// RUN: %weavec_cc --weavec-audit %t/ledger | FileCheck %s --check-prefix=AUDIT
// RUN: %weavec_cc --weavec-audit --format=json %t/ledger | FileCheck %s --check-prefix=JSON
// Without the survey, the ledger has no counts and the audit no plan.
// RUN: rm -rf %t/plain && %weavec_cxx -std=c++20 -O2 -I%S/Inputs -fno-weavec-summary -fweavec-ledger=%t/plain/ -c %s -o %t/plain.o
// RUN: %weavec_cc --weavec-audit %t/plain | FileCheck %s --check-prefix=PLAIN
#include "survey-slice.h"

int first(const char *p) { return p[0] + p[1]; }
int *leak() { return new int; }
int clean(int x) { return x + 1; }
int hits = 0; // a global that is not safe to share: its error is in the plan

// LEDGER: "command": {
// LEDGER-NEXT: "arguments": [
// LEDGER: "-fweavec-safe-survey",
// LEDGER: "cxx": true,
// LEDGER: "errors": [
// LEDGER: "fixes": [
// LEDGER: "evidence": "only its elements 0 to 1 are read",
// LEDGER-NEXT: "fact": "counted_by(2)",
// LEDGER-NEXT: "level": "likely",
// LEDGER: "owner": "first",
// LEDGER: "rule": "index-single",
// LEDGER: "surveyed": true
// LEDGER: "fixes": [],
// LEDGER: "rule": "raw-owner",
// LEDGER: "global": true,
// LEDGER: "rule": "thread-share",
// LEDGER: "kind": "aliased"
// LEDGER: "field": "data_"
// LEDGER-NEXT: "file"
// LEDGER-NEXT: "kind": "counted"
// LEDGER: "files": [
// LEDGER: "hash":
// LEDGER: "function": "first"
// LEDGER: "surveyed": true
// LEDGER: "partial": false
// LEDGER: "version": 12

// AUDIT: conversion: 3 of 7 functions safe (43%); 2 more have no error
// AUDIT-NEXT: fixes: 0 certain, 1 likely ('weavec-cc --weavec-fix <ledger>' applies them)
// AUDIT-NEXT: choices, most errors first:
// AUDIT-NEXT: no fix, fewest errors first:
// AUDIT-NEXT: safe-survey.cpp  2 errors: raw-owner 1, thread-share 1
// AUDIT-NEXT: ready (marked or to mark WEAVEC_SAFE_FILE):
// AUDIT-NEXT: survey-other.cpp  1 function
// AUDIT-NEXT: by rule: index-single 1, raw-owner 1, thread-share 1
// AUDIT: reference fields
// AUDIT-NEXT: survey-slice.h:{{[0-9]+}}: Slice.data_
// AUDIT: aliased classes
// AUDIT-NEXT: survey-slice.h:{{[0-9]+}}: Slice

// JSON-DAG: "version": 8
// JSON-DAG: "conversion": {
// JSON-DAG: "choices": []
// JSON-DAG: "likely": 1
// JSON-DAG: "ready": true
// JSON-DAG: "aliased": [
// JSON-DAG: "unchecked": []

// PLAIN-NOT: conversion:
// PLAIN: functions that are not safe
