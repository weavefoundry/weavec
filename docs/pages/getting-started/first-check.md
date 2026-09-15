---
title: Your first check
description: Find a use-after-free through an alias, understand the compiler diagnostic, fix it, and try a checked function.
---

This tutorial starts with one C file. You need a working [WeaveC installation](/getting-started/installation/).

## Find a lifetime bug

Save this as `example.c`:

<!-- example:use-after-free.c -->

Run the analyzer. The `--` separator introduces flags for Clang:

```sh
weavec example.c -- -std=c17
```

The command fails with `[weavec::use-after-free]`. The diagnostic identifies the read of `n->value`, and its note points to the earlier release through `alias`.

There are two pointer variables, but they refer to the same object. `node_free(alias)` invalidates the object that `n` also names. WeaveC infers the helper's release behavior from its body.

## Fix the lifetime

Read the value before releasing its storage:

<!-- example:fixed.c -->

Replace the original contents with this version and rerun the command. Ordinary analysis succeeds. The function still consumes its input; callers must not use their pointer after the call.

## Try checked safety

Ordinary analysis finds bugs, but a clean run is not a complete safety result. To require a supported function's obligations to be discharged, use checked mode.

Save this as `checked.c`:

<!-- example:checked.c -->

```sh
weavec --checked-function=get --checked-report=safety.json checked.c -- -std=c17
```

The guard establishes that the index is within the initialized array on every path reaching the read. The selected check succeeds and writes a report.

Remove the guard and rerun it. The unknown index now leaves a bounds obligation unresolved; checked mode must reject that incomplete proof. It need not claim that every call will actually go out of bounds.

## Continue

- [Read the report](/guides/reports/) to understand entry requirements and trusted operations.
- [Integrate your build](/guides/build-integration/) to use real include paths and defines.
- [Look up a diagnostic](/reference/diagnostics/) for its meaning and resolution.
