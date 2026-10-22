// External global buffer of unknown size, indexed far.
// XFAIL: the table is defined in a unit built without WeaveC (-fno-weavec): it has no redzone
// UNITS: c13_extern_impl.c
extern char shared_table[];
int main(void) { return shared_table[100000]; } // TRAP
