// RFC 0018 fixed checked-code evaluation: guarded local array.
int main(int argc, char **argv) { char b[4] = {0}; if (argc < 0 || argc >= 4) return 0; return b[argc]; }
