# Contributing to WeaveC

Thank you for your interest in WeaveC. This document covers the mechanics of
contributing; the developer guide in [docs/development.md](docs/development.md)
covers building, testing and tooling in depth.

## Ground rules

- Be kind and constructive. We follow the [Code of Conduct](CODE_OF_CONDUCT.md).
- Discuss significant changes in an issue before opening a large PR. Changes
  to the ownership model, checker rules or annotations go through an RFC; see
  [Proposing larger changes](#proposing-larger-changes).
- Keep PRs small and focused. Several small PRs are easier to review than one
  large one.
- All contributions are licensed under the project's
  [Apache 2.0 with LLVM Exceptions](LICENSE) license.

## Development workflow

1. Fork and clone, then set up the toolchain as described in the
   [README](README.md#building).
2. Create a topic branch from `main`.
3. Install the pre-commit hooks so formatting is handled for you:

   ```sh
   pip install pre-commit && pre-commit install
   ```

4. Build and test with the developer preset:

   ```sh
   cmake --preset dev && cmake --build --preset dev && ctest --preset dev
   ```

5. Before pushing, run the sanitizer build at least once for analysis changes
   (`cmake --workflow --preset ci-debug`) and make sure `scripts/check-format.sh`
   passes.
6. Open a pull request. The template will ask for a summary, design notes and a
   test plan. CI must be green and one maintainer approval is required.

## What to work on

Issues labelled `good first issue` are self-contained and well specified.
Issues labelled `help wanted` are larger but scoped. If you want to take one,
leave a comment so work is not duplicated.

The [roadmap](docs/roadmap.md) lists the larger pieces of planned work and
links each milestone to the RFC that specifies it.

## Proposing larger changes

Anything that changes what WeaveC guarantees needs an RFC before code: adding
to or changing the ownership model in `lib/Core`, adding a checker rule or
changing what an existing rule accepts, adding or changing an annotation in
`weavec.h`, or adding a diagnostic id. Copy
[`docs/rfcs/0000-template.md`](docs/rfcs/0000-template.md) to
`docs/rfcs/NNNN-short-title.md` with the next unused number, fill it in
(the *Soundness*, *Diagnostics* and *Unresolved questions* sections are the
ones reviewers read first), and open a PR containing only the RFC. Discussion
happens on that PR; once there is consensus the RFC is merged as **Accepted**
and implementation proceeds in follow-up PRs that reference it. The full
process and the index of existing RFCs are in
[`docs/rfcs/README.md`](docs/rfcs/README.md).

Driver, CLI, build, CI, documentation and rendering changes, and bug fixes
that bring the implementation in line with an accepted RFC, do not need one.

## Coding standards

WeaveC follows the [LLVM coding standards](https://llvm.org/docs/CodingStandards.html)
with a few deliberate deviations, all enforced by `.clang-format` / `.clang-tidy`:

| Element                 | Convention             | Example                         |
| ----------------------- | ---------------------- | ------------------------------- |
| Namespaces              | `lower_case`           | `weavec::core`                  |
| Types                   | `CamelCase`            | `BorrowState`, `PlaceId`        |
| Functions and methods   | `camelBack`            | `addLoan`, `toCoreLocation`     |
| Variables and members   | `camelBack`            | `placeOf`, `tracker`            |
| Enumerators             | `CamelCase`            | `OwnershipKind::Shared`         |
| Compile-time constants  | `CamelCase`            | `diag::UseAfterFree`, `Prelude` |
| Macros                  | `UPPER_CASE`           | `WEAVEC_OWNED`                  |
| Header guards           | `WEAVEC_<PATH>_H`      | `WEAVEC_CORE_BORROW_H`          |

Additional rules:

- **Layering.** `lib/Core` must never include Clang or LLVM headers. Only
  `lib/Analysis` may depend on both Clang and Core. CI does not yet enforce
  this mechanically, so reviewers will.
- **Diagnostics.** Every user-facing diagnostic has a stable identifier in
  `weavec::core::diag`. Changing an identifier is a breaking change.
- **Comments** explain intent and invariants, not what the code does. Each
  file starts with the standard LLVM-style header block.
- **Warnings** are errors in CI. Do not suppress warnings without a comment
  explaining why.
- **Tests.** New checker behaviour needs both a unit test (in `unittests/`)
  exercising the core logic and a lit test (in `test/`) exercising the
  end-to-end diagnostic. Lit tests that pin behaviour specified by an RFC
  carry its number in the filename (`test/Analysis/rfc0002-*.c`).
  False-positive fixes need a regression test in `test/Analysis/clean.c` or a
  new file.

## Commit messages

Use the imperative mood with an area prefix, and explain *why* in the body when
it is not obvious:

```
[Analysis] Treat realloc as a move of its first argument

realloc may free the original allocation, so subsequent uses of the old
pointer must be flagged the same way as after free().

Fixes #42.
```

Common prefixes: `[Core]`, `[Analysis]`, `[Frontend]`, `[Driver]`, `[Test]`,
`[CMake]`, `[CI]`, `[Docs]`.

## Reporting bugs

Use the bug report template. A minimal reproducer that avoids system headers
(see `test/Inputs/prelude.h` for the idiom) is the single most helpful thing
you can provide.
