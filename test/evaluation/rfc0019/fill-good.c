/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
int main(void) {char a[8];for(unsigned i=0;i<8;++i)a[i]=1;return a[7];}
