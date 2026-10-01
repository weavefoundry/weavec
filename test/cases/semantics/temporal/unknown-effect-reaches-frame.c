// RFC 0031 §6.3: an unknown effect reaches what the caller's memory reaches.
// STAGE: S7
// `churn` hands `ls` to code nobody sees; its summary says only that `*ls`
// was rewritten, because `churn` never loaded `ls->fs`. The unknown code had
// `ls->fs`, which is `fs`, so in `parse` it may have rewritten `fs->bl` too.
// After it, `ls->fs` no longer names `fs` for the analysis, so the store
// `close_frame` makes through it does not reach `fs`, and without the rule
// `fs->bl` would still hold `&bl` at the exit: a definite (and false)
// `lifetime-too-short`. With it, `fs` is rewritten by the unknown code and
// nothing definite is left to say (Lua's `mainfunc`).
// CLEAN
typedef struct Block {
  struct Block *previous;
} Block;

typedef struct Frame {
  Block *bl;
  struct Frame *prev;
} Frame;

typedef struct Lex {
  Frame *fs;
} Lex;

void external(Lex *ls);

static void churn(Lex *ls) { external(ls); }

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
  open_frame(ls, fs, &bl);
  churn(ls);
  close_frame(ls);
}
