/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include <stdlib.h>
union value {int number;int *pointer;};
int main(void){int *p=malloc(sizeof *p);if(!p)return 0;*p=7;union value a={.pointer=p},b=a;free(p);return *b.pointer;}
