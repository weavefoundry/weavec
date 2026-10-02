// RFC 0031 *Implementation amendments*: globals that unknown code may write.
// STAGE: S7
// `parse` calls `notify`, which this unit does not define, so it may write
// any global: `count` among them (http-parser's test counts messages in a
// callback its parser runs). `parse`'s summary said nothing of `count`,
// so `main` kept it at zero and reported `messages[count - 1]` as a
// definite (and false) `out-of-bounds`. The summary now says the function
// runs unseen code (`unknown-globals`), and the call forgets what the
// caller's globals hold.
// TOOL
// CLEAN
struct message {
  int upgrade;
};

struct message messages[4];
int count;

void notify(const char *text);

static int parse(const char *text) {
  notify(text);
  return 0;
}

int main(void) {
  count = 0;
  parse("GET / HTTP/1.1");
  return messages[count - 1].upgrade;
}
