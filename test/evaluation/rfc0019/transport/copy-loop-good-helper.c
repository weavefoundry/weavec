/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
void copy(char *d,const char *s,unsigned n) {for(unsigned i=0;i<n;++i)d[i]=s[i];}
