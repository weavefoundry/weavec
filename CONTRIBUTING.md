# Contributing to WeaveC

Thank you for your interest in WeaveC. This document covers the mechanics of
contributing; the developer guide in [docs/development.md](docs/development.md)
covers building, testing and tooling in depth.

## Ground rules

- Be kind and constructive. We follow the [Code of Conduct](CODE_OF_CONDUCT.md).
- Discuss significant changes (new analyses, annotation syntax, architectural
  changes) in an issue or discussion before opening a large PR.
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

The [roadmap](docs/roadmap.md) lists the larger pieces of planned work.

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
  end-to-end diagnostic. False-positive fixes need a regression test in
  `test/Analysis/clean.c` or a new file.

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
