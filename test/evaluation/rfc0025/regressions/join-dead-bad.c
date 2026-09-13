/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include <stdlib.h>
union value {int number;int *pointer;};
int main(int argc,char**argv){(void)argv;int *p=malloc(sizeof *p);if(!p)return 0;*p=7;union value v;if(argc>1)v.number=7;else v.pointer=p;free(p);if(argc>1)return v.number;return *v.pointer;}
