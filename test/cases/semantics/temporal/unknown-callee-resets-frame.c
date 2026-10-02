// RFC 0031 §6.3, §4.6: an unknown callee's effects name the entry state.
// STAGE: S7
// `close_func` resets the block chain of the frame `ls->fs` names, hands
// `ls` to code it cannot see (which may do anything to every object `ls`
// reaches), and pops the frame. The frame is then no longer reachable from
// `ls` at the exit, but the caller still holds it: the summary must say its
// bytes may have been rewritten, and must resolve `*ls->fs` before it
// forgets the cells of `*ls`. Otherwise the caller keeps `fs->bl` pointing
// at the local block `bl` and reports that it outlives it (Lua's
// `mainfunc`).
// CLEAN
typedef struct Block {
  struct Block *previous;
} Block;

struct Lex;

typedef struct Frame {
  Block *bl;
  struct Frame *prev;
} Frame;

typedef struct Lex {
  Frame *fs;
} Lex;

void collect(Lex *ls);

static void leave(Frame *fs) {
  Block *bl = fs->bl;
  fs->bl = bl->previous;
}

static void open_frame(Lex *ls, Frame *fs, Block *bl) {
  fs->prev = ls->fs;
  ls->fs = fs;
  bl->previous = fs->bl;
  fs->bl = bl;
}

static void close_frame(Lex *ls) {
  Frame *fs = ls->fs;
  leave(fs);
  collect(ls);
  ls->fs = fs->prev;
}

void parse(Lex *ls, Frame *fs) {
  Block bl;
  open_frame(ls, fs, &bl);
  close_frame(ls);
}
