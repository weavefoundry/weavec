# Contributing to WeaveC

Thank you for your interest in WeaveC. This document covers the mechanics of
contributing; the developer guide in [docs/development.md](docs/development.md)
covers building, testing and tooling in depth.

## Ground rules

- Be kind and constructive. We follow the [Code of Conduct](CODE_OF_CONDUCT.md).
- Discuss significant changes in an issue before opening a large PR. Changes
  to what WeaveC guards, the rules that remove guards, the runtime or the
  annotation go through an RFC; see
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

5. Before pushing, run the sanitizer build at least once for changes to the
   guard passes (`cmake --workflow --preset ci-debug`), run a runtime change
   on both Darwin and Linux, and make sure `scripts/check-format.sh` passes.
6. Open a pull request. The template will ask for a summary, design notes and a
   test plan. CI must be green and one maintainer approval is required.

## What to work on

Issues labelled `good first issue` are self-contained and well specified.
Issues labelled `help wanted` are larger but scoped. If you want to take one,
leave a comment so work is not duplicated.

The [roadmap](docs/roadmap.md) lists the larger pieces of planned work and
links each milestone to the RFC that specifies it.

## Proposing larger changes

Anything that changes what WeaveC guarantees needs an RFC before code:
changing what is guarded or how (frames, globals, the sweep, library-call
guards and the library table), adding or changing a rule that removes a
guard, changing the runtime's allocator, quarantine, sweep or shadow
encoding, adding a run-time report kind or changing a report's format, or
adding an annotation to `weavec.h`. Copy
[`docs/rfcs/0000-template.md`](docs/rfcs/0000-template.md) to
`docs/rfcs/NNNN-short-title.md` with the next unused number, fill it in
(the *Soundness* and *Unresolved questions* sections are the ones reviewers
read first), and open a PR containing only the RFC. Discussion
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
| Types                   | `CamelCase`            | `ModuleContext`, `LedgerRow`    |
| Functions and methods   | `camelBack`            | `layoutFrame`, `checkEscape`    |
| Variables and members   | `camelBack`            | `tracked`, `ledger`             |
| Enumerators             | `CamelCase`            | `LedgerRow::Outcome::Guarded`   |
| Compile-time constants  | `CamelCase`            | `EnforcementLedgerVersion`, `LocalAlign` |
| Macros                  | `UPPER_CASE`           | `WEAVEC_UNSAFE`                 |
| Header guards           | `WEAVEC_<PATH>_H`      | `WEAVEC_CORE_LIBRARYSPEC_H`     |

The runtime (`runtime/`) is C and follows the same naming, with its
internal functions prefixed `weavecRt` and its entry points
`__weavec_rt_<name>`.

Additional rules:

- **Layering.** `lib/Core` must never include Clang or LLVM headers
  (`scripts/check-hygiene.py` checks it). The runtime includes no WeaveC
  header but its own `runtime/weavec_rt.h`, and declares there every entry
  point the passes call.
- **Reports.** A run-time report's kind and line format are user-visible:
  scripts and the case runner match them, so changing one is a breaking
  change.
- **Comments** explain intent and invariants, not what the code does. Each
  file starts with the standard LLVM-style header block.
- **Warnings** are errors in CI. Do not suppress warnings without a comment
  explaining why.
- **Tests.** New guard behaviour needs both a unit test (in `unittests/`,
  or `runtime/test/rt_test.c` for the runtime) exercising the core logic and
  an end-to-end test: a lit test (in `test/`) when the exact IR or report
  text matters, or a case under `test/cases/` whose markers pin where the
  program must stop and what stays guarded (see `test/cases/README.md`).
  New tests are named by feature (`test/cases/<area>/…`,
  `test/Guards/<feature>.c`), not by RFC number;
  existing `rfcNNNN-` names may stay. False-trap fixes need a regression
  case under `test/cases/` (a `// CLEAN` file).

## Commit messages

Use Conventional Commits for commits and squash-merge PR titles, with an
optional area scope. Explain *why* in the body when it is not obvious:

```
fix(runtime): check what strtoumax read, not a terminator

The number parsers read only the number they parse and the character that
stops it, so requiring a terminated string trapped on correct input.

Fixes #42.
```

Use `feat` for new behavior, `fix` for bug fixes, and `perf` for performance
improvements. These trigger releases. Use `!` or a `BREAKING CHANGE:` footer
for incompatible changes. `docs`, `ci`, `build`, `test`, `refactor`, `style`
and `chore` normally do not trigger a release. Optional scopes include `core`,
`frontend`, `runtime`, `driver` and `cmake`.

Python Semantic Release generates `CHANGELOG.md` from the commit history;
do not edit it manually. Document behavior and migration guidance in the
relevant guides and commit/PR descriptions.

## Reporting bugs

Use the bug report template. A minimal reproducer that avoids system headers
(see `test/Inputs/prelude.h` for the idiom) is the single most helpful thing
you can provide.
