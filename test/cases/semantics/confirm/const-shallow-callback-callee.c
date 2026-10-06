// The callee of const-shallow-callback.c, in its own unit.
struct callbacks { int (*cred)(void *payload); void *payload; };
struct options { struct callbacks cb; int other; };

int run_with_callbacks(const struct options *opts) {
  return opts->cb.cred(opts->cb.payload);
}
