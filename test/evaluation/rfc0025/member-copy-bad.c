/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
int main(void){union value a={.number=7},b=a;return *b.pointer;}
