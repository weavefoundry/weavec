// RFC 0018 fixed checked-code evaluation: unavailable contract.
extern void unknown(int *p);
int main(void) { int x=1; unknown(&x); return x; }
