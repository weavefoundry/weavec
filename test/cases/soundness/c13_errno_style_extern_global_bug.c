// External global buffer of unknown size, indexed far.
// The table is defined in a unit built without WeaveC (-fno-weavec): it has no
// redzone, and the read faults past its mapping; RFC 0040 section 5.6: the
// fault's report names the line.
// UNITS: c13_extern_impl.c
extern char shared_table[];
int main(void) { return shared_table[100000]; } // TRAP
