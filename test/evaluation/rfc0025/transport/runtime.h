/* RFC 0025 frozen transport. SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
union value { int number; int *pointer; };
struct tagged { unsigned tag; union value data; };
int read_if(int enabled, const int *pointer);
int forward(int enabled, const int *pointer);
int dispatch(int (*reader)(int, const int *), int enabled, const int *pointer);
int get(struct tagged *value);
void set(union value *value, int *pointer);
union value make(void);
