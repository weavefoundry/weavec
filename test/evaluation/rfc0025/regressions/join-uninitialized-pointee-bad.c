/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int number;int *pointer;};
int main(int argc,char**argv){(void)argv;int n;union value v;if(argc>1)v.number=7;else v.pointer=&n;if(argc>1)return v.number;return *v.pointer;}
