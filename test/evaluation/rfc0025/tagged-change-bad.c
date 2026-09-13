/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
struct tagged { unsigned tag; union { int number; int *pointer; } data; };
static int get(struct tagged*v){if(v->tag==0)return v->data.number;return *v->data.pointer;}
int main(void){struct tagged v={0,{.number=7}};v.tag=1;return get(&v);}
