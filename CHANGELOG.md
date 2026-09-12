# CHANGELOG


## v0.4.0 (2026-09-12)

### Features

- Infer ownership contracts for linked containers
  ([#26](https://github.com/weavefoundry/weavec/pull/26),
  [`1f75ed5`](https://github.com/weavefoundry/weavec/commit/1f75ed576350a33161a16ab632f5dac64e99517b))


## v0.3.0 (2026-09-11)

### Features

- Check opaque pointers, callbacks, and allocator hooks
  ([#25](https://github.com/weavefoundry/weavec/pull/25),
  [`8ed6a1b`](https://github.com/weavefoundry/weavec/commit/8ed6a1b80866a5adaab6c9cf5e84acd6122e0640))


## v0.2.0 (2026-09-10)

### Continuous Integration

- Standardize GitHub release titles and notes
  ([`90caa95`](https://github.com/weavefoundry/weavec/commit/90caa9547e95c769f1118fbdc4c461bcd3bf604c))

### Features

- Add checked C traversal and buffer contracts
  ([#24](https://github.com/weavefoundry/weavec/pull/24),
  [`0cb8a6c`](https://github.com/weavefoundry/weavec/commit/0cb8a6c8c6f512afdb4ad28450b587b9ec46cd5f))


## v0.1.0 (2026-09-09)

### Continuous Integration

- Publish versioned source releases on GitHub
  ([`158f7fb`](https://github.com/weavefoundry/weavec/commit/158f7fb03c029a81161c5b4c4c33cf2f3bf3e88e))

- Run only security queries in the CodeQL workflow
  ([#6](https://github.com/weavefoundry/weavec/pull/6),
  [`67fd113`](https://github.com/weavefoundry/weavec/commit/67fd113c8c720fea02d19133c95ef9d1f6f4bc26))

### Documentation

- Add RFC process with ownership model and dataflow RFCs
  ([#1](https://github.com/weavefoundry/weavec/pull/1),
  [`94f1d1a`](https://github.com/weavefoundry/weavec/commit/94f1d1af9c4863be4fd12ce51f6657ddf9a2d3a6))

### Features

- Accelerate analysis with validated incremental caching
  ([#21](https://github.com/weavefoundry/weavec/pull/21),
  [`c4414d4`](https://github.com/weavefoundry/weavec/commit/c4414d493b09b5082e0fa86bb976fbe8d9e371b7))

- Add C integer semantics and compositional bounds checks
  ([#18](https://github.com/weavefoundry/weavec/pull/18),
  [`fb90a22`](https://github.com/weavefoundry/weavec/commit/fb90a229a0b1d8c46426bc0622943bf0199b53c3))

- Add checked code and compositional safety contracts
  ([#19](https://github.com/weavefoundry/weavec/pull/19),
  [`ed3f712`](https://github.com/weavefoundry/weavec/commit/ed3f712112b6c9ef2f890002f05aa7c81409928f))

- Add checked memory contracts for buffers and heap
  ([#20](https://github.com/weavefoundry/weavec/pull/20),
  [`b5805f5`](https://github.com/weavefoundry/weavec/commit/b5805f5abbbfd6dd98ea7e0fe38f3eabfe5601fa))

- Add derived pointers, extents, and bounds checking
  ([#12](https://github.com/weavefoundry/weavec/pull/12),
  [`ef2fa59`](https://github.com/weavefoundry/weavec/commit/ef2fa59bd8de8d7502af9573205b8a82039a3bf3))

- Add leak detection and release-family checking
  ([#8](https://github.com/weavefoundry/weavec/pull/8),
  [`6b88a3c`](https://github.com/weavefoundry/weavec/commit/6b88a3c7e7f0d0786e3fdaccd1760ab04511b962))

- Add non-lexical loans and outcome-conditional summaries
  ([#7](https://github.com/weavefoundry/weavec/pull/7),
  [`481f187`](https://github.com/weavefoundry/weavec/commit/481f1875fc4b0f44628888b40e6a213ea891f9ac))

- Add null, uninitialized, and invalid-release checks
  ([#9](https://github.com/weavefoundry/weavec/pull/9),
  [`55d4741`](https://github.com/weavefoundry/weavec/commit/55d4741c3640eab8ad77336e53b8bcd759a4cc51))

- Add raw pointers, unsafe regions, and indirect calls
  ([#4](https://github.com/weavefoundry/weavec/pull/4),
  [`bc454bf`](https://github.com/weavefoundry/weavec/commit/bc454bffb99703216b99357ed7ae466547cb9cb1))

- Add scalar facts, guarded summaries, and noreturn checks
  ([#10](https://github.com/weavefoundry/weavec/pull/10),
  [`3a60ee2`](https://github.com/weavefoundry/weavec/commit/3a60ee275efbd1825d37d9f74e3aee328dbb8dcd))

- Add string facts, sized fields, and offset relations
  ([#13](https://github.com/weavefoundry/weavec/pull/13),
  [`6819eb4`](https://github.com/weavefoundry/weavec/commit/6819eb4ed94bed025545475204ad2280f45c55c9))

- Add whole-program analysis and weavec-cc driver
  ([#5](https://github.com/weavefoundry/weavec/pull/5),
  [`260e27b`](https://github.com/weavefoundry/weavec/commit/260e27b180f8cac44c5c8990e7854f497cc748eb))

- Implement sound intra-procedural ownership checking
  ([#2](https://github.com/weavefoundry/weavec/pull/2),
  [`d8071e4`](https://github.com/weavefoundry/weavec/commit/d8071e4ccf58c688387cb0f1ad54488ee9379cc4))

- Infer function summaries and check calls against them
  ([#3](https://github.com/weavefoundry/weavec/pull/3),
  [`af74fe5`](https://github.com/weavefoundry/weavec/commit/af74fe56a2872bea1e0a1a8849bb7bc1d3e16fc0))

- Model reference counts and per-outcome ownership
  ([#11](https://github.com/weavefoundry/weavec/pull/11),
  [`b9c34bd`](https://github.com/weavefoundry/weavec/commit/b9c34bdad2fb3e3260c47b52b52efac3b6702564))

- Preserve heap state and allocation-time bounds
  ([#14](https://github.com/weavefoundry/weavec/pull/14),
  [`1815b67`](https://github.com/weavefoundry/weavec/commit/1815b67d651428f12165af916b5c06d62d9a3c49))

- Preserve pointer alias relationships across calls
  ([#17](https://github.com/weavefoundry/weavec/pull/17),
  [`910dc8a`](https://github.com/weavefoundry/weavec/commit/910dc8a801e3fd6d46c1ef3deb57bcc88c439bab))

- Preserve pointer identity and resolve callback effects
  ([#15](https://github.com/weavefoundry/weavec/pull/15),
  [`cafee09`](https://github.com/weavefoundry/weavec/commit/cafee09054b59b8549b4fb49150715cd6b4d899e))

- Scaffold WeaveC with layered Clang build, tests, CI, and docs
  ([`cb0535c`](https://github.com/weavefoundry/weavec/commit/cb0535c2fd94fe085a321cb13d3365f5d670fb89))

- Track ownership across arrays and containers
  ([#16](https://github.com/weavefoundry/weavec/pull/16),
  [`a5583dc`](https://github.com/weavefoundry/weavec/commit/a5583dc4999f319dbb089b9f16a93c1c2eef862b))
