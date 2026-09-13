/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
static int get(int flag,const char*p){flag=1;if(flag)return *p;return 0;}
int main(void){return get(0,0);}
