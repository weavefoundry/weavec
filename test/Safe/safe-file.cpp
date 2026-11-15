// RFC 0047 §6: WEAVEC_SAFE_FILE makes safe the functions a file defines
// after it, whoever includes the file, without -fweavec-safe; the file that
// includes it is not reached. An unknown `#pragma weavec` is
// `bad-annotation`. Under a compiler that is not WeaveC it is nothing.
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -w -I%S/Inputs -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// RUN: %weavec_cxx -std=c++20 -fsyntax-only -w -fno-weavec -I%S/Inputs -I%S/../../resources/include -DPLAIN %s
#include "safe-file.h"

int here(const int *p) { return p[3]; } // this file is not safe

#ifndef PLAIN
#pragma weavec unsafe // expected-error {{'#pragma weavec' takes 'safe' (WEAVEC_SAFE_FILE) [weavec-safe:bad-annotation]}}
#endif
