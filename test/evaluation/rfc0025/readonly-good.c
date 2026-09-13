/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
static int read_if(int enabled, const char *p) { if (!enabled) return 0; return 100 / *p; }
int main(void) { return read_if(0, 0); }
