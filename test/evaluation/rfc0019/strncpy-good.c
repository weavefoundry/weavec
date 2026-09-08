/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <string.h>
int main(void) {char a[4];strncpy(a,"ab",4);return (int)strlen(a);}
