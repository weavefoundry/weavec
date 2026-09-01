# Notes for AI coding agents

Read this before making changes; it summarises the conventions that matter
most and where to find the rest.

## Project in one paragraph

WeaveC is a Clang/LLVM-based tool that adds inferred ownership and borrowing
checks to C. Three C++ libraries: `weavec::Core` (the ownership model, **no
Clang/LLVM includes allowed**), `weavec::Analysis` (Clang AST → core facts;
the only layer that includes both), `weavec::Frontend` (Clang
`FrontendAction`, diagnostics bridging). `tools/weavec` is a libTooling CLI.
Full picture: `docs/architecture.md`; model semantics:
`docs/design/ownership-model.md`.

## Build and test

```sh
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"   # or /usr/lib/llvm-23
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

- Unit tests: `build/dev/unittests/WeaveC{Core,Analysis}Tests`.
- Integration tests: `lit -v build/dev/test` (FileCheck-based; see
  `test/README.md`).
- Format: `scripts/format.sh`; check: `scripts/check-format.sh`.

## Rules that reviewers will enforce

1. Never add `clang/` or `llvm/` includes under `include/weavec/Core` or
   `lib/Core`.
2. Every new diagnostic gets a stable id in `weavec::core::diag`
   (`include/weavec/Core/Diagnostic.h`), an entry in `docs/annotations.md`,
   a unit test, and a lit test that pins the exact message.
3. Annotation spellings live in exactly two places that must agree:
   `include/weavec/Analysis/Annotations.h` (`spelling::`) and
   `resources/include/weavec.h`.
4. Warnings are errors in CI (`WEAVEC_WARNINGS_AS_ERRORS=ON`). Do not add
   suppressions without a comment.
5. Follow the existing file header block and naming (`CamelCase` types and
   constants, `camelBack` functions/variables, `static` free functions rather
   than anonymous namespaces); `.clang-tidy` enforces it.
6. Update `CHANGELOG.md` under `[Unreleased]` for user-visible changes.
7. Do not commit generated files (`build/`, `compile_commands.json`).

## Where things are

| Task                                 | Look at                                                |
| ------------------------------------ | ------------------------------------------------------ |
| Add a checker rule                   | `lib/Analysis/FunctionAnalysis.cpp`                    |
| Change how diagnostics are rendered  | `lib/Frontend/ClangDiagnosticSink.cpp`                 |
| Add a CLI flag                       | `tools/weavec/main.cpp`, `FrontendOptions`             |
| Add an annotation                    | `Annotations.h`, `weavec.h`, `docs/annotations.md`     |
| Change build flags / warnings        | `cmake/WeaveCWarnings.cmake`, `cmake/WeaveCLLVM.cmake` |
| Add a CI job                         | `.github/workflows/ci.yml`                             |
