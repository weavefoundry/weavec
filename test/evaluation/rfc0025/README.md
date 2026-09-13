# RFC 0025 acceptance populations

These cases exercise [input cases and discriminated
objects](../../../docs/rfcs/0025-case-sensitive-checked-contracts.md). They use
the checked evaluation harness, separately from the ordinary diagnostic recall
population in the parent directory.

| Population | Accepted callers | Rejected callers | Purpose |
| --- | ---: | ---: | --- |
| `manifest.json` | 18 | 20 | Primary cases frozen before checker changes |
| `transport/manifest.json` | 5 | 5 | Separate source definitions and compiler objects |
| `regressions/manifest.json` | 11 | 14 | Additional alias, lifetime, layout and guard regressions |
| `upstream/manifest.json` | 3 | 2 | Clients of the complete, unchanged pinned cJSON source |

Each population has its own `frozen-sha256.json`. A successful closed caller
must have no entry requirements, unresolved obligations, deferral or exhausted
limit, and no additional unsafe or annotation trust. Expected rejections must
produce the intended missing property; a syntax error, crash or timeout cannot
satisfy an expectation. Source files and expectations are not regenerated to
hide a regression.

From the repository root:

```sh
python3 scripts/checked-cases.py --population source \
  --weavec build/dev/bin/weavec --output build/rfc25-source
python3 scripts/checked-cases.py --population transport \
  --weavec build/dev/bin/weavec --output build/rfc25-transport
python3 scripts/checked-cases.py --population regressions \
  --weavec build/dev/bin/weavec --output build/rfc25-regressions
python3 scripts/checked-cases.py --population objects \
  --weavec build/dev/bin/weavec --cc build/dev/bin/weavec-cc \
  --output build/rfc25-objects
python3 scripts/checked-cases.py --population cache \
  --weavec build/dev/bin/weavec --output build/rfc25-cache
python3 scripts/checked-cases.py --population upstream \
  --weavec build/rfc25-release/bin/weavec --output build/rfc25-upstream
```

`--output` names a directory. Its `results.json` retains case results, source
and executable identity, and the underlying logs and reports. The first five
commands are included in CTest. The upstream command is a separate Release
acceptance run and verifies its pinned source before analysis.

The cache population compares uncached, cold and warm results, expands compact
case reports, changes an output's selected union member, corrupts a checkpoint,
and independently selects an incomplete generic helper alongside its successful
caller. A warm hit must perform zero function analyses.

Supplemental unit and lit tests also reject a raw byte write or a helper write
to another union member before the first member read. The checker must not
reintroduce an entry member requirement after that write. A separate positive
case keeps an input member readable after a proved disjoint write.

`baseline-complete-functions.json` freezes the 142 exact complete conditional
contract identities from RFC 0024's 1,619 selected corpus definitions. These
are compared by source and function name; equal totals cannot conceal losses.

The original RFC 0020 `projection.c`, JSON and diagnostic snapshot remain in
their original directory. `projection.json` and `projection.stderr` here pin
the generic report and diagnostics after case inference. For `diamond(1)`,
`middle(!n)` at column 48 is the route to the else-branch arithmetic obligation;
the old generic-only report used column 36. Outcomes, requirements and all
other generic report fields are unchanged. `incremental-evaluation.py` selects
this snapshot for reports with case records. Separate case tests pin the new
additive fields.
