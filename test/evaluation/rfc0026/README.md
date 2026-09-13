# Growable-buffer evaluation (RFC 0026)

`manifest.json` and `frozen-sha256.json` identify 28 primary cases. The
`transport` population contains 11 separate-source cases; the compiler-object
runner builds and links those same cases. `upstream` contains three callers of
the complete unchanged pinned Jansson source. These populations have separate
identities and denominators.

Run `scripts/checked-buffers.py --population source --weavec <weavec>
--output <directory>`. The other population names are `transport`, `objects`,
`cache`, `oracle` and `upstream`; objects also require `--cc <weavec-cc>`.
The runner verifies the frozen input hashes before checking. The oracle writes
its complete generated inputs and hashes before invoking the analyzer.

Positive closed callers require zero entry requirements, no unsafe or annotation
trust, and complete analysis. Negatives must report the intended missing
property. Invocation errors, syntax errors and unrelated rejections do not pass.

See [the RFC](../../../docs/rfcs/0026-growable-buffer-contracts.md) and
[validation](../../../docs/validation-rfc0026.md) for scope and evidence.
