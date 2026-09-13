/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {volatile int number;int *pointer;};int main(void){union value v={.number=7};return v.number;}
