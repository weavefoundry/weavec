/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include <string.h>
union scalar {int number;unsigned other;};
int main(void){union scalar a={.number=7},b;memcpy(&b,&a,sizeof b);return b.other;}
