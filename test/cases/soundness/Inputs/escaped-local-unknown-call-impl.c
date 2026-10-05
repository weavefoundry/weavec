struct callbacks { void *payload; };
struct options { int version; struct callbacks callbacks; };
static void *kept;
void clear_errors(void) {}
int clone_with(const struct options *options) {
  kept = options->callbacks.payload;
  *(const char **)kept = "redirected";
  return 0;
}
