// A callee whose access rests on its parameter's Single default (RFC 0030
// §7.3: `first` relies on `p`).
int first(int *p) { return *p; }
