/* RFC 0025 adversarial regression. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value {int number;int *pointer;};
extern void change(void*);int main(void){union value v={.number=7};change(&v);return v.number;}
