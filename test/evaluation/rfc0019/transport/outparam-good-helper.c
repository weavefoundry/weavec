/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
int make(char **out) {*out=malloc(4);if(!*out)return 0;(*out)[0]=7;return 1;}
