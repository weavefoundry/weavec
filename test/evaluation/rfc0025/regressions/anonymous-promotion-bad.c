/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
struct value {union {int number;int *pointer;};};int main(void){struct value v={.number=7};return v.number;}
