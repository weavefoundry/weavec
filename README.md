# WeaveC

[![CI](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml/badge.svg)](https://github.com/weavefoundry/weavec/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0%20WITH%20LLVM--exception-blue.svg)](LICENSE)

A mostly source-compatible C compiler that brings Rust-style memory safety to existing C code through inferred ownership and borrowing. It automatically proves memory safety where possible, requires lightweight annotations only when necessary, and isolates truly unsafe operations behind explicit `unsafe` boundaries. The goal is to let teams incrementally make large C codebases memory-safe without rewriting them in Rust or abandoning the C ecosystem.

WeaveC is built on Clang/LLVM rather than implementing a compiler from scratch, using Clang for parsing, semantic analysis, diagnostics, optimization, code generation, and platform support. WeaveC adds its own ownership inference, borrow checking, lifetime analysis, and memory-safety rules on top, allowing the project to focus on its core innovation while remaining compatible with the existing C toolchain and ecosystem.

WeaveC itself is written in modern C++, which provides the most direct and complete access to Clang/LLVM's APIs and infrastructure. The core ownership, borrowing, lifetime, and inference logic should be kept as modular as possible so it remains cleanly separated from the Clang integration layer and can potentially be reused or extended in the future.

> **Status:** early. Every function body is checked by a sound dataflow ([RFC 0002](docs/rfcs/0002-intraprocedural-checking.md)): use-after-free and double-free through any alias and across loops, use-after-move, conflicting borrows, and pointers that outlive what they point to. Calls are modelled by inferred signatures ([RFC 0003](docs/rfcs/0003-signature-inference.md)): every function in the translation unit gets a summary of what it frees, writes, stores and returns, so `node_free(n); n->v` is caught without annotations; the C standard library and POSIX are covered by a shipped table, and annotations are checked against the bodies that carry them. Unsafe code has a boundary ([RFC 0004](docs/rfcs/0004-unsafe-boundaries.md)): pointers cast from integers or declared `WEAVEC_RAW` are *raw* and may only be dereferenced or released inside a `WEAVEC_UNSAFE` region, which is analysed rather than skipped; calls through function pointers are checked from the pointer type's annotations or from the functions assigned to it. Programs are analysed whole ([RFC 0005](docs/rfcs/0005-whole-program-analysis.md)): every translation unit exports the summaries of what it defines, so `other_free(o); o->v` is caught when `other_free` lives in another file, callbacks registered in one file are checked in the file that calls them, and `annotation-required` fires only for code defined nowhere in the program. `weavec-cc` is a drop-in `cc` that does this as part of a normal build. The checker is precise where C idioms need it ([RFC 0006](docs/rfcs/0006-precision.md)): a borrow ends at the pointer's last use, not at the end of its scope; `if (p == sentinel) return; free(p);` knows the two are distinct; `free(a[i])` in a loop does not poison `a[j]`; and a function that frees its argument only when it returns `0` (or `NULL`) is summarised per outcome, so `if (rc != 0) free(p);` is clean while `if (rc == 0) use(p);` is reported. The other half of the ownership contract is checked too ([RFC 0007](docs/rfcs/0007-resource-lifecycle.md)): a resource that is never released is a `leak`, reported where it is lost (`if (c) return -1;` after a `malloc`, an overwrite, a discarded `strdup`, a `free(b)` that drops an owned `b->data`), and releasing it with the wrong function (`free` on a `FILE *`, through any wrapper, across files) is a `mismatched-release`. Pointers are checked for validity, not only ownership ([RFC 0008](docs/rfcs/0008-pointer-validity.md)): dereferencing a `malloc` result, a `strchr` result or any other pointer that may be null without testing it is a `null-dereference` (through calls too: a function that dereferences its parameter requires callers to prove it non-null), a pointer used before it is assigned is a `use-of-uninitialized`, `free` of a stack object, a string literal or the middle of an allocation is an `invalid-release`, and a callee that frees a value and then reinitialises the place (`realloc` in place, `free` then `= NULL`) still kills every copy of the old value the caller kept. The checker also knows *why* ([RFC 0009](docs/rfcs/0009-value-conditional-behaviour.md)): it tracks what is known about integers, so `if (c) free(p); ... if (!c) use(p);` and `switch (op) { case FREE: free(p); }` followed by another `switch` on `op` are clean; a callee's behaviour is summarised *per argument* (`l_alloc(ud, p, n, 0)` frees `p` and returns null, `l_alloc(ud, p, n, 64)` does not; `if (!b->noalloc) free(b->data)` frees only for callers that did not set the flag); and a function whose every path ends in `abort`, `exit`, `longjmp` or another such function is inferred `noreturn`, so `if (bad) die(); use(p);` is checked on the good path only. Objects with more than one owner are understood ([RFC 0010](docs/rfcs/0010-shared-ownership.md)): a reference count is inferred from the `obj_ref`/`obj_unref` pair that keeps it (`o->rc++`; `if (--o->rc == 0) free(o)`, in any spelling from `o->rc--` to `__atomic_fetch_sub`), so `b = obj_ref(a); obj_unref(b); use(a)` is clean while one `obj_unref` too many is a `double-free`, a use after the last one is a `use-after-free`, and a reference taken and dropped is a `leak`; a callee that stores its argument only on success (`if (bag_put(b, s) < 0) free(s);`) is summarised per outcome, and one that keeps its argument in a node of its own (`table_set(t, o)`) is known to have kept it. Shipped summaries for libraries beyond libc and a Clang plugin packaging are next. See [docs/roadmap.md](docs/roadmap.md).

## Quick look

```c
#include <stdint.h>
#include <stdlib.h>
#include <weavec.h>

struct buffer *WEAVEC_OWNED buffer_new(size_t n);
size_t buffer_len(const struct buffer *WEAVEC_BORROWED b);

struct node { int v; struct node *next; };
static void node_free(struct node *n) { free(n); }   // inferred: consumes n

int example(struct node *n) {
  struct node *m = n;
  node_free(m);
  return n->v;       // error: use of 'n' after it was freed [weavec::use-after-free]
}

int *escape(void) {
  int x = 0;
  return &x;         // error: returned pointer may outlive 'x' [weavec::lifetime-too-short]
}

struct node *WEAVEC_OWNED from_handle(uintptr_t h) {
  WEAVEC_UNSAFE { return (struct node *)h; }   // asserts ownership, at one greppable point
}

int handle(uintptr_t h) {
  struct node *r = (struct node *)h;             // r is raw: no one knows who owns it
  return r->v;       // error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
}
```

```
$ weavec example.c --
example.c:13:10: error: use of 'n' after it was freed [weavec::use-after-free]
   13 |   return n->v;
      |          ^
example.c:12:3: note: freed here (through 'm')
   12 |   node_free(m);
      |   ^
example.c:18:10: error: returned pointer may outlive 'x', which it points to [weavec::lifetime-too-short]
   18 |   return &x;
      |          ^
example.c:17:7: note: 'x' is declared here
   17 |   int x = 0;
      |       ^
example.c:27:10: error: dereference of raw pointer 'r' outside an unsafe region [weavec::unsafe-operation]
   27 |   return r->v;
      |          ^
example.c:26:20: note: 'r' is raw: cast from an integer here
   26 |   struct node *r = (struct node *)h;
      |                    ^
example.c:27:10: note: move this operation into a WEAVEC_UNSAFE block or function, or assert the pointer's ownership first
```

Annotations (`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`, `WEAVEC_UNSAFE`, `WEAVEC_NULLABLE`, `WEAVEC_NONNULL`, `WEAVEC_RETAINS`, `WEAVEC_RELEASES`, `WEAVEC_REFCOUNT`, `WEAVEC_OWNED_BY(f)`) expand to nothing on other compilers, so annotated code remains plain, portable C. See [docs/annotations.md](docs/annotations.md).

## Using it as the compiler

`weavec-cc` is Clang's driver with WeaveC inside. Point a build at it and it compiles as `clang` would, analyses each file as it compiles it, and checks the whole program when it links:

```sh
$ CC=weavec-cc make
weavec-cc -c node.c -o node.o          # node.o and node.o.weavec (its summaries)
weavec-cc -c main.c -o main.o
weavec-cc node.o main.o -o prog        # reads the sidecars, analyses the program, then links
main.c:8:3: error: 'n' is freed twice [weavec::double-free]
    8 |   node_free(n);
      |   ^
main.c:7:3: note: previously freed here
    7 |   node_free(n);
      |   ^
1 error generated.
```

A bug inside one file is reported when that file is compiled; a bug that needs two files (`node_free` is defined in `node.c`) is reported when they are linked, and an error stops the link. Flags: `-fno-weavec` (compile only), `-fweavec-strict` (every call into unknown code is a raw operation), `-fno-weavec-link` (skip the link-time step), `-Wno-weavec-annotation-required`, `-Wno-error=weavec-use-after-free` (lower an error to a warning while migrating), `-Werror=weavec`.

The tooling form analyses a compilation database without building: `weavec --whole-program -p build/` (all sources) or `weavec --whole-program a.c b.c -- -Iinclude`. Without `--whole-program`, `weavec file.c --` checks one file as before.

## Building

Requirements: CMake ≥ 3.24, Ninja, a C++20 compiler, and an LLVM/Clang development install (23.x recommended; ≥ 20 supported). Tests additionally need `lit` and LLVM's `FileCheck`.

```sh
# macOS
brew install llvm ninja lit
export WEAVEC_LLVM_PREFIX="$(brew --prefix llvm)"

# Ubuntu / Debian
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 23 all
sudo apt-get install -y ninja-build && pip install lit
export WEAVEC_LLVM_PREFIX=/usr/lib/llvm-23

# Everyone
cmake --preset dev          # configure into build/dev
cmake --build --preset dev  # build
ctest --preset dev          # run unit + integration tests
```

Other presets: `dev-asan`, `dev-tidy`, `release`, `relwithdebinfo`. The full list is in [`CMakePresets.json`](CMakePresets.json); the developer guide is [docs/development.md](docs/development.md).

## Repository layout

```
include/weavec/   Public C++ headers
  Core/           Ownership lattice, lifetimes, borrows, moves, diagnostics (no Clang/LLVM)
  Analysis/       Clang AST -> core facts; the checkers
  Frontend/       Clang FrontendAction / libTooling integration
lib/              Implementations, mirroring include/
tools/weavec/     The analysis tool (libTooling; --whole-program for a compilation database)
tools/weavec-cc/  The drop-in compiler driver (Clang's driver with WeaveC inside)
resources/        weavec.h, the C-facing annotation header (installed to lib/weavec/include)
unittests/        GoogleTest unit tests
test/             lit + FileCheck integration tests
docs/             Architecture, RFCs (docs/rfcs/), roadmap
cmake/            Build-system modules
```

The layering rule is strict: `Core` must not include anything from `clang/` or `llvm/`. `Analysis` is the only layer that knows about both worlds. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome; please read [CONTRIBUTING.md](CONTRIBUTING.md) first. Security issues should be reported privately as described in [SECURITY.md](SECURITY.md).

## License

Apache License 2.0 with LLVM Exceptions, the same license as LLVM itself. See [LICENSE](LICENSE).
