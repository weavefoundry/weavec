# Security policy

## Scope

WeaveC is a static analysis tool and (eventually) a compiler. We consider the
following to be security issues:

- **Soundness bugs**: safe-looking code that WeaveC accepts without
  diagnostics but that is actually memory-unsafe under WeaveC's model
  (false negatives) *when WeaveC claims to have proven it safe*. During the
  pre-1.0 period WeaveC makes no such claim, so these are tracked as normal
  bugs, but please still report them.
- Memory-safety bugs or crashes in WeaveC itself that are triggerable by
  untrusted input (e.g. analysing attacker-controlled source code).
- Vulnerabilities in the build or release process (supply chain).

Crashes on malformed input that do not cross a trust boundary, false
positives, and performance problems are ordinary bugs; please file them as
issues.

## Reporting

Please **do not** open a public issue for security vulnerabilities. Instead,
use GitHub's private vulnerability reporting:

https://github.com/weavefoundry/weavec/security/advisories/new

Include a minimal reproducer, the `weavec --version` output, and your
assessment of impact. You will receive an acknowledgement within 5 business
days and a more detailed response within 14 days indicating the next steps.

## Supported versions

Until 1.0, only the latest release and `main` receive security fixes.

## Disclosure

We follow coordinated disclosure. We will work with you on a fix and a
timeline, credit you in the advisory unless you prefer otherwise, and publish
a GitHub Security Advisory when the fix is released.
