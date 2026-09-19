---
title: Contribute to WeaveC
description: Build the project, understand the architecture, propose design changes, and contribute tests or documentation.
---

WeaveC is developed in the [weavefoundry/weavec repository](https://github.com/weavefoundry/weavec). Contributions can improve diagnostics, supported C patterns, test coverage, performance, or the documentation itself.

## Set up the compiler

Follow the [developer guide](/contributing/development/) to install LLVM, configure the `dev` preset, and run unit and integration tests.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The [architecture guide](/internals/architecture/) explains the three libraries: Core owns the model and the ledger, Analysis bridges Clang to Core behind the engine seam, and Frontend orchestrates check insertion, the driver, program analysis, and diagnostics.

## Read the design before changing it

Ownership semantics, checker rules, annotations, diagnostic identifiers, and serialized cross-unit contracts are specified by RFCs. Read the relevant accepted RFC before changing those areas. Propose a new RFC for a new design decision; a bug fix that restores the specified behavior does not need one.

Start with the [RFC process](/rfcs/process/#process) and [RFC library](/rfcs/). Core must remain independent of Clang and LLVM headers.

## Make a reviewable change

- Add meaningful unit and integration coverage for changed behavior. Name new tests by feature (`test/cases/<area>/…`, `test/Emission/<feature>-*.c`); existing `rfcNNNN-` names may stay.
- Every new diagnostic needs a stable ID, an annotation-reference entry, resolution guidance in `docs/data/diagnostic-remedies.json`, a unit test, and a lit test pinning its exact message.
- Use Conventional Commit titles and update the relevant user guide.
- Run formatting and required checks. Generated build outputs are not source files.
- Leave `CHANGELOG.md` to semantic-release; do not edit it manually.

## Improve these docs

Use the edit link on any page to find its maintained source. Some reference pages are generated from existing repository guides, so their edit link points to that guide. The [website guide](/contributing/website/) covers local preview, content generation, tests, and GitHub Pages deployment.

For larger changes, open an [issue](https://github.com/weavefoundry/weavec/issues) describing the problem and proposed scope before investing in the implementation.
