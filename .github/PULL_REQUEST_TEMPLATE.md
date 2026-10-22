<!--
Thanks for contributing to WeaveC! Please fill in the sections below.
Small, focused PRs are much easier to review than large ones.
-->

## Summary

<!-- What does this change do, and why? Link related issues (e.g. "Fixes #123"). -->

## Design notes

<!--
Anything a reviewer should know: alternatives considered, trade-offs,
what it costs at run time or build time, follow-up work. Changes to the
library table, the guard passes, the runtime, the annotation or the
run-time report kinds must link the RFC in docs/rfcs/ that specifies them.
Delete this section if not applicable.
-->

## Test plan

<!-- How was this verified? New lit/unit tests, cases, corpus runs? -->

- [ ] Unit tests added/updated (`unittests/`, `runtime/test/rt_test.c`)
- [ ] Integration tests added/updated (`test/Guards`, `test/Driver`, `test/cases`)
- [ ] `ctest --preset dev` passes locally (a runtime change: on Darwin and Linux)

## Checklist

- [ ] Code is formatted (`scripts/format.sh`) and passes clang-tidy
- [ ] Public headers and behaviour changes are documented
- [ ] Conventional Commit PR title and user-visible changes documented (changelog is generated)
- [ ] `scripts/check-hygiene.py` passes (no Clang/LLVM in `lib/Core`, runtime entry points declared in `weavec_rt.h`, line budgets)
