/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int number;int *pointer;};
int main(void){int n=7;union value u={.pointer=&n};int *p=&u.number;return *p;}
