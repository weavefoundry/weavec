/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
static int get(union value*v){return v->number;}
int main(void){union value v={.number=7};return get(&v)!=7;}
