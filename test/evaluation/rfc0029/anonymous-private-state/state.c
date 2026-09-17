typedef struct { const unsigned char *text; unsigned long offset; } state;
static state last;
void reset(void) { last.text = 0; last.offset = 0; }
