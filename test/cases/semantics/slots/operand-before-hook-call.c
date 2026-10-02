// RFC 0031 §5.4: a call through a hook joins its functions' states.
// STAGE: S7
// In `0 != settings->on_begin(p)` the `0` is evaluated before the call,
// whose two functions are each applied to a copy of the state and joined
// (http-parser's `CALLBACK_NOTIFY`). The join renumbered every symbol, so
// the comparison's own result took the number of its operand `0`: its
// condition named itself, and refining the branch recursed until the
// stack ran out. A join inside an expression now keeps the numbers of the
// values the state had before it.
// CLEAN
// RUN-INPUT:
#include <stddef.h>

struct parser {
  int state;
};

struct settings {
  int (*on_begin)(struct parser *);
};

static int accept_all(struct parser *p) {
  p->state = 1;
  return 0;
}

static int reject_all(struct parser *p) {
  p->state = 2;
  return 1;
}

static int run(const struct settings *settings, struct parser *p) {
  int errors = 0;
  for (int i = 0; i < 4; ++i)
    if (settings->on_begin && 0 != settings->on_begin(p))
      ++errors;
  return errors;
}

int main(int argc, char **argv) {
  (void)argv;
  struct settings settings = {accept_all};
  if (argc > 5)
    settings.on_begin = reject_all;
  struct parser p = {0};
  return run(&settings, &p) == 0 && p.state == 1 ? 0 : 1;
}
