---
title: Contribute to WeaveC
description: Build the project, understand the architecture, propose design changes, and contribute tests or documentation.
---

WeaveC is developed in the [weavefoundry/weavec repository](https://github.com/weavefoundry/weavec). Contributions can improve the guards and the runtime, drop-in compatibility, test coverage, performance, or the documentation itself.

## Set up the compiler

Follow the [developer guide](/contributing/development/) to install LLVM, configure the `dev` preset, and run unit and integration tests.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The [architecture guide](/internals/architecture/) explains the compiler and the runtime: Core holds the library table, Frontend the guard passes, unsafe regions, the enforcement ledger and the compiler driver, and `runtime/` is the C runtime every enforcing link carries.

## Read the design before changing it

What is guarded and the rules that remove a guard, the shadow encoding, frames and globals, the runtime and its reports, the library table, `weavec.h`, the ledger format and the driver's flags are specified by RFCs. Read the relevant accepted RFC before changing those areas. Propose a new RFC for a new design decision; a bug fix that restores the specified behavior does not need one.

Start with the [RFC process](/rfcs/process/#process) and [RFC library](/rfcs/). Core must remain independent of Clang and LLVM headers, and a rule that removes a guard must be local to a function and monitored by `verify` mode.

## Make a reviewable change

- Add meaningful unit and integration coverage for changed behavior. Name new tests by feature (`test/cases/<area>/…`, `test/Guards/<feature>.c`); existing `rfcNNNN-` names may stay.
- A new or changed run-time report needs a lit test in `test/Guards/` pinning its exact text, and an entry in the command-line reference's list of report kinds.
- Use Conventional Commit titles and update the relevant user guide.
- Run formatting and required checks. Generated build outputs are not source files.
- Leave `CHANGELOG.md` to semantic-release; do not edit it manually.

## Improve these docs

Use the edit link on any page to find its maintained source. Some reference pages are generated from existing repository guides, so their edit link points to that guide. The [website guide](/contributing/website/) covers local preview, content generation, tests, and GitHub Pages deployment.

For larger changes, open an [issue](https://github.com/weavefoundry/weavec/issues) describing the problem and proposed scope before investing in the implementation.
