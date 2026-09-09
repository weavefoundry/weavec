// RFC 0020: fingerprinting must not consume standard input before codegen.
// Standard input cannot provide replayable checked-object evidence.
// RUN: cat %s | %weavec_cc -x c -c - -o %t.o
// RUN: %weavec_cc -fno-weavec-link %t.o -o %t
// RUN: %t
int main(void) { return 0; }
