/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
struct tagged { unsigned tag; union { int number; int *pointer; } data; };
static int get(struct tagged*v){if(v->tag==0)return v->data.number;return *v->data.pointer;}
int main(int argc,char**argv){(void)argv;int n=7;struct tagged v;if(argc>1){v.tag=0;v.data.number=7;}else{v.tag=1;v.data.pointer=&n;}if(argc>1)return v.data.number!=7;return *v.data.pointer!=7;}
