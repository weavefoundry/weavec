// RFC 0049: a header with no include of weavec.h, for
// test/Driver/safe-fix-include.cpp.
#pragma once

inline int second(const int *p) { return p[1]; }
