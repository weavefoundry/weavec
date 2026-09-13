/* RFC 0025 frozen acceptance. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
struct value { unsigned tag; const char *text; };
static int get(const struct value*v){if((v->tag & 255u)!=1u)return 0;return 100/v->text[0];}
int main(void){struct value v={257,0};return get(&v);}
