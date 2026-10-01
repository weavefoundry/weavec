// Held-out repro (RFC 0031 Motivation, §11.1): http-parser test.c:2681-2690 (parse_pause)
// copies the pausing settings into a local 's', points the global 'current_pause_parser' at
// it, and runs the parser; the pause callbacks write 'settings_dontcall' through the global
// while 's' is live. When parse_pause returns, the global still points to the dead 's', but
// it is only read by those callbacks, and the next parse_pause overwrites it first.
// v0.11.0 reports "'current_pause_parser' may outlive 's', which it points to" (a false
// definite lifetime-too-short error at test.c:2687, which stops the build).
// intended: no finding. NOTE: the global is still dangling at parse_pause's exit, which the
// first bullet of RFC 0031 §5.7 ("left in a cell reachable from a ... global at the exit")
// would report; the RFC's second bullet names this case as a false error, so the case pins
// the intended CLEAN and §5.7 must reconcile the two.
// CLEAN
// ASAN
#include <stddef.h>
#include <stdlib.h>
struct parser { int paused; int calls; };
struct settings { int (*on_begin)(struct parser *); };
static struct parser parser;
static struct settings *current_pause_parser;
static int dontcall_cb(struct parser *p) { (void)p; abort(); }
static struct settings settings_dontcall = { dontcall_cb };
static int pause_begin_cb(struct parser *p) {
  p->paused = 1;
  p->calls++;
  *current_pause_parser = settings_dontcall;
  return 0;
}
static struct settings settings_pause = { pause_begin_cb };
static size_t execute(struct parser *p, const struct settings *s, const char *buf, size_t len) {
  size_t i;
  (void)buf;
  for (i = 0; i < len; i++) {
    if (p->paused) return i;
    if (s->on_begin(p) != 0) return i;
  }
  return len;
}
size_t parse_pause(const char *buf, size_t len) {
  size_t nparsed;
  struct settings s = settings_pause;
  current_pause_parser = &s;
  nparsed = execute(&parser, current_pause_parser, buf, len);
  return nparsed;
}
int main(void) {
  const char *buf = "GET / HTTP/1.1\r\n\r\n";
  size_t len = 18;
  do {
    size_t n = parse_pause(buf, len);
    buf += n;
    len -= n;
    parser.paused = 0;
  } while (len > 0);
  return parse_pause(NULL, 0) == 0 && parser.calls == 18 ? 0 : 1;
}
