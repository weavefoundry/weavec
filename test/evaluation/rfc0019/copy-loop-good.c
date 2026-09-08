/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void copy(char *d,const char *s,unsigned n) {for(unsigned i=0;i<n;++i)d[i]=s[i];}
int main(void) {char a[8]={1};char b[8];copy(b,a,8);return b[7];}
