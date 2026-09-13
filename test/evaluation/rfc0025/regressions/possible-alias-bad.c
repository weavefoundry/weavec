/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int number;int *pointer;};
int main(int argc,char**argv){(void)argv;int n=7;union value a={.pointer=&n},b={.pointer=&n};union value *p=argc>1?&a:&b;p->number=7;return *a.pointer;}
