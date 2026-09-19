// External global buffer of unknown size, indexed far.
// UNITS: c13_extern_impl.c
extern char shared_table[];
int main(void) { return shared_table[100000]; } // BUG: out-of-bounds // NOT-PROVEN: spatial
