/* RFC 0025 frozen transport. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
#include "runtime.h"
int read_if(int enabled,const int *pointer){if(!enabled)return 0;return 100 / *pointer;}
int get(struct tagged *value){if(value->tag==0)return value->data.number;return *value->data.pointer;}
void set(union value *value,int *pointer){value->pointer=pointer;}
union value make(void){union value v={.number=7};return v;}
