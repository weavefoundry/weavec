/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int number;int *pointer;};
static int init(union value *v,int flag){if(flag){v->number=7;return 1;}return 0;}int main(int argc,char**argv){(void)argv;union value v;if(init(&v,argc))return v.number;return 0;}
