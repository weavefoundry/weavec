/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {double number;int *pointer;};int main(void){union value v={.number=1.5};return v.number>0.0;}
