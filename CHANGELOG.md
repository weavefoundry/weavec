# Changelog

All notable changes to WeaveC are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
follows [Semantic Versioning](https://semver.org/) once it reaches 1.0.

## [Unreleased]

### Added

- Initial project scaffolding: CMake build with presets, LLVM/Clang discovery,
  strict warnings, sanitizer and LTO options, install/export rules and CPack.
- `weavec::Core`: Clang-independent ownership lattice (`OwnershipKind`),
  lifetime constraints, loan tracking (`BorrowState`), move tracking
  (`MoveTracker`) and a frontend-neutral diagnostics interface.
- `weavec::Analysis`: annotation recognition and a first path-insensitive
  local ownership checker reporting `use-after-free`, `double-free`,
  `invalid-annotation` and (opt-in) `annotation-required`.
- `weavec::Frontend`: Clang `ASTFrontendAction`, diagnostics bridging to
  Clang's `DiagnosticsEngine`, resource-directory discovery.
- `weavec` command-line tool built on libTooling (`weavec file.c -- <flags>`),
  with `--report-unannotated` and `--analyze-headers`.
- `weavec.h` annotation header: `WEAVEC_OWNED`, `WEAVEC_BORROWED`,
  `WEAVEC_MUT`, `WEAVEC_UNSAFE`, `WEAVEC_ENABLED`.
- GoogleTest unit tests and lit/FileCheck integration tests.
- GitHub Actions CI (Linux ASan/UBSan + Release, macOS Release, clang-format,
  cmake-format, clang-tidy, CodeQL), Dependabot, issue and PR templates.
- Project documentation: architecture, annotations reference, developer
  guide, roadmap.
- RFC process for changes to the model, checker rules, annotations and
  diagnostics (`docs/rfcs/`), with RFC 0001 (ownership model) and RFC 0002
  (sound intra-procedural checking), both Accepted.

[Unreleased]: https://github.com/weavefoundry/weavec/commits/main
