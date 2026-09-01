<!--
Thanks for contributing to WeaveC! Please fill in the sections below.
Small, focused PRs are much easier to review than large ones.
-->

## Summary

<!-- What does this change do, and why? Link related issues (e.g. "Fixes #123"). -->

## Design notes

<!--
Anything a reviewer should know: alternatives considered, trade-offs,
interactions with the ownership model, follow-up work.
Delete this section if not applicable.
-->

## Test plan

<!-- How was this verified? New lit/unit tests? Manual runs? -->

- [ ] Unit tests added/updated (`unittests/`)
- [ ] Integration tests added/updated (`test/`)
- [ ] `ninja check-weavec` passes locally

## Checklist

- [ ] Code is formatted (`scripts/format.sh`) and passes clang-tidy
- [ ] Public headers and behaviour changes are documented
- [ ] `CHANGELOG.md` updated for user-visible changes
- [ ] No new dependencies on Clang/LLVM introduced into `lib/Core`
