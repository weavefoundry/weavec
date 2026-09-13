/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
int main(void){int n=7;union value v={.pointer=&n};((unsigned char*)&v)[0]=0;return *v.pointer;}
