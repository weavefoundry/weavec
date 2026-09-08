/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <string.h>
int main(void) {char a[2];strncpy(a,"ab",2);return (int)strlen(a);}
