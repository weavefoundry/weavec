/* RFC 0025 frozen transport. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "runtime.h"
int main(void){union value v;set(&v,0);return *v.pointer;}
