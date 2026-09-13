/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int number;struct {int n;} pair;};int main(void){union value v={.pair.n=7};return v.pair.n;}
