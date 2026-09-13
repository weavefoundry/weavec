/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int number;int *pointer;};
int main(void){union value u;int *p=&u.number;*p=7;return u.number;}
