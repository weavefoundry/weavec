/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
static int get(union value*v){return v->number;}
int main(void){int n=0;union value v={.pointer=&n};return get(&v);}
