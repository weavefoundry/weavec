# Scalar cell identity regressions

The original `frozen-sha256.json` and `manifest.json` predate the scalar-cell
fixes. `extended-sha256.json` adds heap equivalents without changing the two
original files or their expected outcomes. Both inventories must be verified
when evaluating `extended-manifest.json`.

The separately frozen `advanced-sha256.json` covers `advanced-read.c` and its
manifest. Its helper writes 7 through a pointer, advances to the second byte,
and returns that byte. The client initializes the second byte to 42, so its
out-of-bounds write executes. The RFC0028 baseline executable (SHA-256
`47b881a29c1bb9b7ee547f144e72ef0342376d9a12a2d4ca72db6417156510a3`)
and RFC0029 candidate39d (`d9374707a644f21517563d6a2c727e45f15da4dd72c3d0a36f9c3be6242c5607`)
accept the selected client. An ordinary Clang build of the unchanged file with
`-std=c11 -O0 -g -fsanitize=address,undefined` reports the executed array index
and stack-buffer-overflow. Candidate40b rejects the selected client. This is a
concrete counterexample, not an expectation changed to match the checker.
