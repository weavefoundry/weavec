/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include <stdlib.h>
union value { int number; int *pointer; };
int main(void){int*p=malloc(sizeof *p);if(!p)return 0;*p=7;union value v={.pointer=p};int*q=v.pointer;free(p);v.number=0;return *q;}
