/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
int main(int argc,char**argv){(void)argv;union value v;if(argc>1)v.number=1;return v.number;}
