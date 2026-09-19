// Exported function: parameter of unknown provenance is dereferenced/indexed with no requirement.
int first_after(const char *s, int i) { return s[i + 100]; } // BUG: out-of-bounds // NOT-PROVEN: spatial
