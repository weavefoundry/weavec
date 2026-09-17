# RFC 0029 upstream lifetime audit

This additional population preserves the discovery made at candidate70a. The
original `upstream-extended/failed-parse.c` and its accepted expectation remain
immutable. That client leaves cJSON's global error pointer referring to an
automatic input after its lifetime ends, violating RFC 0001's stored-borrow rule.
The stack case here must reject for that lifetime obligation. Its separate
static-storage counterpart must complete with no entry requirements and no
annotation or unsafe trust. Upstream cJSON sources are unchanged.

`oracle.c` retrieves and reads the retained error pointer after the parsing
helper returns. ASan diagnoses stack-use-after-return with automatic storage;
compiling with `-DSTATIC_INPUT` runs clean. This corroborates the escaped borrow,
not arbitrary parser safety. The original client itself does not dereference
the dangling error pointer, so it is not claimed to execute that invalid read.

Sources use repository-relative includes for reproducible validation; the
original absolute-path development probes and their hashes remain under
`build/rfc29-validation/upstream-lifetime-audit`.
