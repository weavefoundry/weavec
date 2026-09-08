/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
struct buffer { char *data; unsigned capacity; };
int get(struct buffer *b) { return b->data[0]; }
