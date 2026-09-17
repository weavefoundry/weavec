typedef void (*writer_fn)(unsigned char *);
int invoke(int enabled, writer_fn write, unsigned char *p);
