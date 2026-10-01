// RFC 0031 *Implementation amendments* (the C library's own globals): a call
// the analysis does not see does not make `stdout` name another stream.
// STAGE: S7
// Each round closes `stdout` again (zlib's minigzip with `-c -d` and two
// files, RFC 0030 G9). `open_input` is external: it may do anything to what
// the program's globals reach, but it does not reassign `stdout`, so the
// second round's `stdout` is the stream the first round closed.
// TOOL
#include <stdio.h>
#include <stdlib.h>

typedef struct input *input;
input open_input(const char *path);
int read_input(input in, void *buf, unsigned len);
int close_input(input in);

static void copy_out(input in, FILE *out) {
  char buf[64];
  int len;
  while ((len = read_input(in, buf, sizeof buf)) > 0)
    fwrite(buf, 1, (unsigned)len, out);
  if (fclose(out))
    exit(1);
  if (close_input(in))
    exit(1);
}

int main(int argc, char **argv) {
  do {
    input in = open_input(argv[argc]);
    if (in == NULL)
      fprintf(stderr, "cannot open\n");
    else
      copy_out(in, stdout); // BUG: double-free possible
  } while (--argc);
  return 0;
}
