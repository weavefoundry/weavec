/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
static int get(int *tag,int *p){*tag=1;if(!*tag)return 0;return *p;}int main(void){int tag=0;return get(&tag,0);}
