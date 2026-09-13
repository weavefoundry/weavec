/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
static void set(union value*v){v->pointer=0;}
int main(void){union value v;set(&v);return v.number;}
