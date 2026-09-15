---
title: Adopt WeaveC incrementally
description: Introduce WeaveC to an existing C project, establish a baseline, resolve diagnostics, and grow checked coverage.
---

Start with a component you understand: one with identifiable allocation and cleanup paths and a manageable boundary to external code.

## 1. Reproduce the real build context

Generate a compilation database or pass the same language standard, include paths, and defines as the normal compiler. A parser error caused by missing configuration does not tell you anything about ownership.

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
weavec -p build src/buffer.c
```

See [build integration](/guides/build-integration/) for CMake, Make, and compiler-driver workflows.

## 2. Read the first diagnostic and its notes

Follow the reported allocation, alias, release, or escape. Fix lifetime errors and invalid accesses in the code. Check inferred behavior with `--dump-analysis` when a helper's effect is surprising.

During migration, you can lower a particular ordinary error to a warning:

```sh
weavec -Wno-error=weavec-use-after-free -p build src/buffer.c
```

This changes reporting severity. It does not repair the bug or establish a safety guarantee. Keep the baseline visible in CI and remove temporary overrides as the component improves.

## 3. Resolve interfaces

Supply helper definitions through [whole-program analysis](/guides/whole-program/). For external interfaces, add accurate annotations or record a narrow [unsafe boundary](/guides/unsafe/). Avoid broad trust assertions that obscure the component's actual dependencies.

## 4. Select checked functions

```sh
weavec --whole-program --checked-function=buffer_append \
  --checked-report=build/safety.json -p build
```

Inspect the report's entry requirements and trust ledger. A successful selected function can still depend on caller premises or trusted operations. Expand the selected set once the first component's requirements are understood.

## 5. Keep the result reproducible

Run the same analysis command in CI. Rebuild compiler objects and their sidecars when source, headers, flags, or metadata formats change. Keep both the checked report and the normal test results: they provide different evidence.
