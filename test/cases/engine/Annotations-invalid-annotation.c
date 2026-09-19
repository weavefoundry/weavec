// Engine pin converted from test/Annotations/invalid-annotation.c; markers are the v0.10.0 golden diagnostics.
// Unknown weavec.* annotations are reported; foreign annotations are ignored.
#include "Inputs/prelude.h"

__attribute__((annotate("weavec.ownd"))) void typo(void) {} // BUG: invalid-annotation

__attribute__((annotate("gsl.owner"))) void foreign(void) {}
