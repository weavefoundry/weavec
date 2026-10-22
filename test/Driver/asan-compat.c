// RFC 0036 §5: weavec-cc compiles and preprocesses code as AddressSanitizer
// would, so that code written to be ASan-clean takes its ASan paths;
// -fno-weavec-asan-compat turns that off. On Darwin the SDK's fortification
// stays on (Clang turns it off for ASan, which intercepts the C library).
// RUN: %weavec_cc -fsyntax-only %s
// RUN: %weavec_cc -E -dM %s | FileCheck --check-prefix=MACROS %s
// Standard input gets WeaveC's flags as a named file does.
// RUN: %weavec_cc -E -dM -x c - < /dev/null | FileCheck --check-prefix=MACROS %s
// RUN: %weavec_cc -fno-weavec-asan-compat -E -dM -x c - < /dev/null | FileCheck --check-prefix=STDIN-PLAIN %s
// RUN: not %weavec_cc -fno-weavec-asan-compat -fsyntax-only %s 2>&1 | FileCheck --check-prefix=PLAIN %s
// RUN: not %weavec_cc -fweavec-checks=none -fsyntax-only %s 2>&1 | FileCheck --check-prefix=PLAIN %s
//
// MACROS: #define __SANITIZE_ADDRESS__ 1
// MACROS: #define __WEAVEC__ 1
// STDIN-PLAIN-NOT: __SANITIZE_ADDRESS__
// STDIN-PLAIN: #define __WEAVEC__ 1
// PLAIN: error: not built as for ASan

#if !defined(__has_feature)
#error no __has_feature
#endif
#if !__has_feature(address_sanitizer) || !defined(__SANITIZE_ADDRESS__)
#error not built as for ASan
#endif
