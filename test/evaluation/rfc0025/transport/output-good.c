/* RFC 0025 frozen transport. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "runtime.h"
int main(void){int n=7;union value v;set(&v,&n);return *v.pointer!=7;}
