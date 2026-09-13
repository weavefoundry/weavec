/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include <string.h>
union value { int number; int *pointer; };
int main(void){int n=7;union value a={.pointer=&n},b;memcpy(&b,&a,1);return *b.pointer;}
