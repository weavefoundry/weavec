/* RFC 0025 frozen transport. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "runtime.h"
int forward(int enabled,const int *pointer){return read_if(enabled,pointer);}
int dispatch(int (*reader)(int,const int *),int enabled,const int *pointer){return reader(enabled,pointer);}
