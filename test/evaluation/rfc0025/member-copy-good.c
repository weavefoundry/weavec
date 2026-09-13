/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
int main(void){int n=7;union value a={.pointer=&n},b=a;return *b.pointer!=7;}
