/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
static union value make(void){union value v={.number=7};return v;}
int main(void){union value v=make();return v.number!=7;}
