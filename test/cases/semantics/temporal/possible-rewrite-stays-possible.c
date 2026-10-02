// RFC 0031 §6.3: bytes a callee may have rewritten stay possibly rewritten.
// STAGE: S7
// `refill` reads over the whole of `*ls` on some paths only, so its summary
// says the bytes may be rewritten; `advance` calls it on every path, and
// its own summary must still say "may": on the other paths `ls->fs` keeps
// the frame `open_frame` stored there. If `advance`'s summary said the
// bytes were rewritten, `parse` would lose `ls->fs`, the store `close_frame`
// makes through it would not reach `fs`, and `fs->bl` would still hold
// `&bl` at the exit: a definite (and false) `lifetime-too-short` (Lua's
// `luaX_next`, `statlist` and `mainfunc`). The `read` may have replaced
// `ls->fs` itself, so what is left is a possible finding.
#include <unistd.h>

typedef struct Block {
  struct Block *previous;
} Block;

typedef struct Frame {
  Block *bl;
  struct Frame *prev;
} Frame;

typedef struct Lex {
  int fd;
  int token;
  Frame *fs;
} Lex;

static void refill(Lex *ls) {
  if (ls->token == 0)
    (void)read(ls->fd, ls, sizeof *ls);
}

static void advance(Lex *ls) {
  refill(ls);
  ls->token = 1;
}

static void open_frame(Lex *ls, Frame *fs, Block *bl) {
  fs->prev = ls->fs;
  ls->fs = fs;
  bl->previous = fs->bl;
  fs->bl = bl;
}

static void close_frame(Lex *ls) {
  Frame *fs = ls->fs;
  fs->bl = fs->bl->previous;
  ls->fs = fs->prev;
}

void parse(Lex *ls, Frame *fs) {
  Block bl;
  open_frame(ls, fs, &bl); // BUG: lifetime-too-short possible
  advance(ls);
  close_frame(ls);
}
