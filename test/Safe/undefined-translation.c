// RFC 0051 §1: the items of C's list of undefined behavior that Clang
// rejects (or gives one meaning) when it translates a unit, as
// `resources/safe/undefined.md` lists them ("translation"); a sample of
// each kind, in a safe unit.
// RUN: %weavec_cc -std=c23 -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s

// J.2 (8): one identifier with internal and external linkage.
extern int shared;
static int shared; // expected-error {{static declaration of 'shared' follows non-static declaration}}

// J.2 (14), in one unit: incompatible declarations.
int twice(int);
long twice(int); // expected-error {{conflicting types for 'twice'}}

// J.2 (22): a void expression's value.
void nothing(void);
int used(void) { return nothing(); } // expected-error {{returning 'void' from a function with incompatible result type 'int'}}

// J.2 (40): a pointer converted to a floating type.
double address(int *p) { return (double)p; } // expected-error {{pointer cannot be cast to type 'double'}}

// J.2 (57): a block-scope function declared static.
void outer(void) {
  static void inner(void); // expected-error {{function declared in block scope cannot have 'static' storage class}}
}

// J.2 (75): void in a parameter list qualified.
void qualified(const void); // expected-error {{'void' as parameter must not have type qualifiers}}

// J.2 (31): __func__ declared.
void named(void) {
  static const char __func__[] = "x"; // expected-error {{expected identifier or '('}}
}
