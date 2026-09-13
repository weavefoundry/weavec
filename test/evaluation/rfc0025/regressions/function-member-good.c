/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int (*function)(void);int number;};static int f(void){return 7;}int main(void){union value v={.function=f};return v.function();}
