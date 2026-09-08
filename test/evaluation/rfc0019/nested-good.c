/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
struct buffer { char *data; unsigned capacity; };
static int get(struct buffer *b) { return b->data[0]; }
int main(void) { char a[1]={7}; struct buffer b={a,1}; return get(&b); }
