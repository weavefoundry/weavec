/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
static int get(int *p){if(!p)return 0;return 100 / *p;}int main(void){return get(0);}
