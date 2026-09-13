/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include <string.h>
union value {int number;int *pointer;};
int main(void){union value u={.number=7};memset(&u,0,sizeof u);return u.number;}
