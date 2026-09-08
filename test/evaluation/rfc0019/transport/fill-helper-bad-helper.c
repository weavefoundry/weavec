/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
void fill(char *p,unsigned n) {for(unsigned i=0;i<n;++i)p[i]=1;}
