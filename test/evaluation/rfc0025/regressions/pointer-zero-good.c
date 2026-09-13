/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int *pointer;int number;};int main(void){union value v={};return v.pointer?*v.pointer:0;}
