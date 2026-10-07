# Semantics cases

Each case pins one rule of the RFC that added it: RFC 0030's worked examples
and rules (`aliasing/`, `assume/`, `boundary/`, `extents/`, `kinds/`,
`library/`, `slots/`, `temporal/`, `examples/`, ...), RFC 0031's object
domain (`objects/`), RFC 0032's runtime (`runtime/`), RFC 0033's drop-in
rules (`dropin/`) and the zero-initialisation cases (`zero-init/`). The first
comment line names the rule; the prose under it describes the case as that
RFC saw it, and may mention checks or outcomes RFC 0035 removed.

Since RFC 0035 each case is judged by what a default build does (it stops at
each `TRAP` line, and nowhere else) and by what the advisory `weavec`
analysis reports (`BUG`, `CLEAN`, `ALLOW`); `test/cases/README.md` has the
marker grammar and the runner. `STAGE` lines are history: the runner ignores
them.
