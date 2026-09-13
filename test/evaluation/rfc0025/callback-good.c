/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
static int read_if(int enabled, const char *p) { if (!enabled) return 0; return 100 / *p; }
static int invoke(int(*f)(int,const char*),int n,const char*p){return f(n,p);}
int main(void){return invoke(read_if,0,0);}
