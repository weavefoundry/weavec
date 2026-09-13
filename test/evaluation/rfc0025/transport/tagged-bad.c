/* RFC 0025 frozen transport. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "runtime.h"
int main(void){struct tagged v={.tag=1,.data.number=7};return get(&v);}
