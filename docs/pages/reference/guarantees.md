---
title: Safety guarantees and scope
description: Understand exactly what a successful checked result establishes and what remains outside WeaveC's supported model.
---

WeaveC distinguishes finding bugs from establishing a selected function's safety obligations. Choose the mode and interpret its result deliberately.

## Ordinary analysis

Ordinary analysis reports supported ownership, lifetime, validity, bounds, and other errors. It can also report incomplete analysis. A zero exit status does **not** mean that every reachable operation was proved safe.

Turning on strict external checking or promoting warnings does not turn ordinary analysis into checked mode.

## Checked code

A successful selected function has its reachable safety obligations discharged within the supported model, **under its reported entry requirements and trusted boundaries**. Its callers must establish the requirements that apply to them.

The report distinguishes proof from unresolved obligations, demonstrated failures, and dependencies on trusted code or assertions. Read it alongside the command's exit status.

| Result                            | Interpretation                                                                                |
| --------------------------------- | --------------------------------------------------------------------------------------------- |
| Complete under entry requirements | The supported obligations are established if the recorded caller premises hold.               |
| Depends on trust                  | The result also relies on the recorded unsafe operations, external contracts, or assumptions. |
| Incomplete                        | Some obligation lacks sufficient evidence; selected checking fails.                           |
| Failed                            | A supported violation was demonstrated; selected checking fails.                              |
| Deferred during compilation       | A dependency awaits link-time resolution; this is not a complete result.                      |

These descriptions summarize outcomes; consult [report fields](/guides/reports/) for their serialized representation.

## Conditions that still matter

- The analyzed source, headers, compilation flags, target, and linked definitions must match the code that executes.
- The recorded input requirements and trusted contracts must be true.
- The model is bounded and uses Clang's target types, layout, and C integer conversions.
- The execution model is single-threaded. General concurrent safety is outside this guarantee.

The result does not certify unselected code, external libraries, the compiler implementation, or arbitrary C. It adds no runtime checks and changes no pointer ABI.

## Inspect the evidence

Start with [reading a report](/guides/reports/), then review [current checked limits](/reference/checked-limits/) and the relevant [validation record](/internals/validation/).

The authoritative contract is [RFC 0018](/rfcs/0018-checked-code-and-safety-contracts/), with practical checked memory behavior extended by [RFC 0019](/rfcs/0019-practical-checked-memory-contracts/) and subsequent RFCs.
