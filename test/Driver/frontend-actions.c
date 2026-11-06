// RFC 0038 amendment 27: weavec-cc runs Clang's module and inspection
// actions, for C++ and Objective-C builds too (a C++20 module interface, an
// AST print); only the static analyzer, rewriting and plug-ins are not in it.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -std=c++20 -x c++-module --precompile %S/Inputs/module.cppm -o %t/answers.pcm
// RUN: test -f %t/answers.pcm
// RUN: %weavec_cc -fsyntax-only -Xclang -ast-print %s | FileCheck --check-prefix=PRINT %s
// RUN: not %weavec_cc --analyze %s -o %t/report.plist 2>&1 | FileCheck --check-prefix=ANALYZE %s
//
// PRINT: int printed(void)
// ANALYZE: weavec-cc: error: this job's action (static analysis, rewriting, a plug-in) is not in weavec-cc

int printed(void) { return 1; }
