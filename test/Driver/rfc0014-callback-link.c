// RUN: not %weavec --whole-program %S/Inputs/rfc0014-callback-client.c %S/Inputs/rfc0014-callback-helper.c -- 2>&1 | FileCheck %s --check-prefix=LINK
// RFC 0014: a callback bug across units, in `weavec --whole-program`.
//
// RFC 0031 §6.1: format 30 has no callback inputs. In its own unit,
// `invoke`'s callback is unknown code that may do anything to `userdata`
// and to any global (RFC 0031 *Implementation amendments*, "Globals that
// unknown code may write");
// across units, the call through the callback parameter takes the parameter's
// slot solution (RFC 0030 §9.3), which is `drop`, so the client's read
// after `invoke(drop, p)` is a use after free.
// LINK: error: use of 'p' after it was freed [weavec::use-after-free]
// LINK-NOT: annotation-required
// LINK-NOT: analysis-incomplete
