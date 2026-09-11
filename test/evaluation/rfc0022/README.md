# RFC 0022 evaluation

The 32 source cases in `manifest.json` and five actual Jansson callers in
`real-modules.json` were frozen before checker edits. `frozen-sha256.json`
records their exact content. The preserved baseline binary and its reports
live under `build/rfc22-validation/baseline*`; the binary SHA-256 is
`9505618ba097bbb907f6dd447110bb5bdc06fd4347e4f42146b7eec77b4ad775`.

Positive closed callers require complete checking with zero entry requirements,
no limits, no deferral, and no unsafe or annotation trust. Negative cases require
their intended property. C syntax errors, tool failures, missing reports and
timeouts do not satisfy a negative expectation.

```sh
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0022/manifest.json \
  --json build/rfc22-validation/cases.json
```

Jansson callers compile the actual pinned `src/memory.c` and `src/strbuffer.c`,
including hook setters and default/custom allocator functions. They do not
substitute allocation adapters. Open generic definitions may remain incomplete;
the closed caller must establish the contracts of its actual specializations.
This is the bounded interface guarantee of RFC 0022, not universal acceptance
for every callback with the same C function type.

Compiler transport, callback/layout edits and cold/warm cache comparisons have
separate integration tests. Supplemental unit cases include malformed metadata,
object type/offset distinctions and negative callback alternatives.

The supplemental `adversarial-manifest.json` exercises allocation type
laundering, byte offsets, packed member alignment, unrelated local record tags,
explicit null output edges in both orders, output replacement and mixed
builtin/source callback targets. `transport-manifest.json` contains eight
cross-unit positive/negative cases and is also consumed by the compiler-object
harness. These supplemental populations were added during implementation and
are distinguished from the frozen 32-case population.

```sh
python3 scripts/checked-interfaces.py --weavec build/dev/bin/weavec \
  --output build/rfc22-validation/real
python3 scripts/checked-evaluation.py --weavec build/dev/bin/weavec \
  --manifest test/evaluation/rfc0022/adversarial-manifest.json \
  --json build/rfc22-validation/adversarial.json
python3 scripts/checked-memory-build.py --cc build/dev/bin/weavec-cc \
  --manifest test/evaluation/rfc0022/transport-manifest.json \
  --json build/rfc22-validation/objects.json
```

The original retained-userdata positive fixture left a static registration
pointing to a local at scope exit. Its corrected version clears the registration
after the valid read, preserving the intended distinction from the stale
userdata negative. `frozen-original-sha256.json` retains the original identity;
the baseline result records the original program. This correction follows the
existing lifetime rule and is documented in the RFC.

The supplemental mixed-unmodeled pair records an audited baseline false proof:
a null-returning callback cannot supply a missing checked contract for `getenv`.
The bad client is rejected, while a concrete setter choosing only the safe
callback still permits a complete closed caller. The typedef-alignment pair
retains stronger target alignment before canonicalizing type identity; a
misaligned recovery cannot pass as an ordinary compatible `int` view. Independent
UBSan executions substantiate both counterexamples in the validation record.
